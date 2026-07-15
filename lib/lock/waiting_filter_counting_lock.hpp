#ifndef WAITING_FILTER_COUNTING_LOCK_HPP
#define WAITING_FILTER_COUNTING_LOCK_HPP

#pragma once

#include "lock.hpp"
#include "bitonic_networks.hpp"
#include "counting_lock_common.hpp"
#include "../utils/cxl_utils.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <type_traits>

// =============================================================================
// DESIGN C — Waiting-Filter Counting Lock          (wf_bitonic_*, wf_periodic_*)
//
// The Herlihy-Shavit-Waarts (1996) §3 "Waiting Network":
// token v waits for predecessor v-1 to set a phase bit at unlock, over
// an n-slot ring with
// phase(v) = floor(v/n) mod 2 preventing ABA across ring laps. The waiting
// filter is exactly what turns non-linearizable network order into a usable
// total order.
//
// Current policy:
//   - bitonic variants use network-derived v (no shared ticket counter)
//   - periodic variants use dense global tickets to avoid predecessor holes
//     observed under lock-shaped schedules.
//
// History: between 2026-07 (commit 1d497da) and 2026-07-14 the token was a
// global fetch_add, introduced because network-derived tokens appeared to
// stall the phase chain. The real culprit was the handoff fence bug below;
// with that fixed, the network-derived form passed every oracle (45/45
// lock-level runs, breach-armed sweeps T∈{2..12} incl. odd counts, preload
// fenced+fence-free, 2s soak) and measured 1.27-1.59x FASTER than the
// ticket form on wf_periodic at T2-T12 (parity on wf_bitonic) — removing
// the fetch_add removes the one serialization hotspot. The ticket era made
// this design a ticket lock with a decorative network; it is again a
// network-ordered lock.
//
// Why the n-cell ring and the v-1 wait are sound WITHOUT dense tickets:
//   * Completed values form a prefix: v cannot enter before v-1 unlocks.
//   * Every value between the done-prefix and the maximum emitted value is
//     held by a distinct live thread — emitted values by their waiting
//     owners, un-emitted holes by the in-flight traversals that will fill
//     them (the counting property guarantees a hole's owner exists once any
//     larger value has been emitted). Each thread holds at most one
//     outstanding value, so the live window never exceeds n:
//     - cell v mod n is never rewritten before v+1 has consumed it, and the
//       mod-2 phase disambiguates adjacent laps → no ABA;
//     - v-1's owner always exists and its traversal never blocks on the
//       lock chain → the wait always terminates.
//
// Fairness: n-bounded overtaking in real-time order (quiescently
// consistent) — a late arriver can draw a smaller value than an
// already-exited earlier arriver, bounded by the in-flight window. This is
// the HSW semantics; it is NOT strict arrival-FIFO (the ticket era was).
//
// vs Design B (sequenced_counting_lock.hpp): both are network-ordered
// (since 2026-07-14); they differ in the successor-signal medium — B grants
// into a token-versioned slot with a shared now_serving_ fallback, C sets a
// phase bit in a fixed n-slot ring that the successor polls. C's unlock
// never touches a shared counter; the price is that ring cells are
// position-fixed (no direct handoff choice).
//
// Memory ordering (2026-07-14 fix): the phase-bit handoff is a release
// store paired with acquire spin loads. The original had a plain store with
// a fence AFTER it — ordering nothing that matters — so critical-section
// writes could reach the successor late; observed as lost counter updates
// in lock-level mode (thread-level mode masked it by fencing inside its
// CS). Release/acquire on the byte (stlrb/ldarb) measured ~2x cheaper here
// than full fences at 1T.
//
// Complexity
//   lock:   O(log^2 W) traversal + phase-bit wait; NO extra atomic RMW
//           (the wait is near-zero at quiescence: the predecessor has
//           usually already unlocked — the Lynch/Shavit/Shvartsman
//           practical-linearizability observation)
//   unlock: O(1) — one release store, no shared-counter write
//   space:  O(n * CL) phase ring + O(n * CL) per-thread values
// =============================================================================


template <typename Sync, template <typename> class NetworkT>
class WaitingFilterCountingLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        num_threads_ = num_threads;

        size_t target = (size_t)std::ceil(std::sqrt((double)num_threads));
        if (target < 2) target = 2;
        width_ = 1;
        while (width_ < target) width_ *= 2;

        network_.build(width_, num_threads);

        // ── Memory layout ──
        // [phase_bits : n * CL]  [thread_values : n * CL]
        size_t phase_bytes = num_threads * CL;
        size_t value_bytes = num_threads * CL;

        region_size_ = phase_bytes + value_bytes;
        region_ = (volatile char*)ALLOCATE(region_size_);
        memset((void*)region_, 0, region_size_);

        size_t off = 0;
        phase_base_ = &region_[off]; off += phase_bytes;
        value_base_ = &region_[off];

        // Init phase_bits to PHASE_UNSET sentinel (0xFF).
        // This is neither 0 nor 1, so no token can accidentally match
        // a predecessor's phase before that predecessor has actually unlocked.
        // Token 0 is special-cased to not wait (no predecessor).
        for (size_t i = 0; i < num_threads; i++) {
            *get_phase_bit(i) = PHASE_UNSET;
            *get_thread_value(i) = 0;
        }
        global_seq_.store(0, std::memory_order_relaxed);

        initialized_ = true;
    }

    void lock(size_t thread_id) override {
        // Bitonic variants use network-derived tokens directly.
        // Periodic variants keep a dense ticket to avoid predecessor holes
        // that can strand the phase chain under lock-shaped schedules.
        size_t lv = 0;
        size_t v = 0;
        if constexpr (std::is_same_v<NetworkT<Sync>, PeriodicNetwork<Sync>>) {
            network_.traverse((int)(thread_id % width_), thread_id, &lv);
            v = global_seq_.fetch_add(1, std::memory_order_acq_rel);
        } else {
            int wire_i = network_.traverse((int)(thread_id % width_), thread_id, &lv);
            v = (lv >> 1) * width_ + (size_t)wire_i;
        }

        *get_thread_value(thread_id) = v;

        // 2. Waiting-filter (Herlihy-Shavit-Waarts Section 3)
        //    Wait for predecessor (value v-1) to set its phase bit.
        //    Token 0 has no predecessor → enters immediately.
        //
        //    Under practical linearizability (Lynch et al., c2 ≤ 2c1):
        //    predecessor has almost always ALREADY set its phase bit
        //    before we check, so this spin is near-zero in practice.
        if (v > 0) {
            size_t pred_slot = (v - 1) % num_threads_;
            uint8_t expected = phase_of(v - 1);
            unsigned spins = 0;
            // Acquire loads: without acquire on the spin read,
            // critical-section loads can be satisfied speculatively before
            // the spin-exit load and read pre-unlock data (observed as lost
            // counter updates in lock-level mode). A per-read acquire
            // (ldarb) is much cheaper than a trailing full fence.
            while (__atomic_load_n((volatile uint8_t*)get_phase_bit(pred_slot),
                                   __ATOMIC_ACQUIRE) != expected) {
                LcSpinWait(spins);
            }
        }
        // Lock acquired.
    }

    void unlock(size_t thread_id) override {
        // O(1): set own phase bit to signal successor.
        // Successor (value v+1) will see this and proceed.
        size_t v = *get_thread_value(thread_id);
        size_t my_slot = v % num_threads_;
        // Release store: the critical section's writes must be visible when
        // the successor sees the bit flip. (The original code had a plain
        // store with a fence AFTER it, which orders nothing that matters —
        // the successor could enter on stale CS data unless the caller
        // happened to fence inside its own CS, as the thread-level
        // benchmark does.)
        __atomic_store_n((volatile uint8_t*)get_phase_bit(my_slot),
                         phase_of(v), __ATOMIC_RELEASE);
    }

    void destroy() override {
        if (!initialized_) return;
        initialized_ = false;
        network_.destroy();
        if (region_) {
            FREE((void*)region_, region_size_);
            region_ = nullptr;
        }
    }

    std::string name() override {
        return std::string("wf_counting_") + Sync::sync_name();
    }

private:
    static constexpr size_t CL = std::hardware_destructive_interference_size;
    static constexpr uint8_t PHASE_UNSET = 0xFF;

    NetworkT<Sync> network_;
    size_t num_threads_ = 0;
    size_t width_       = 0;
    std::atomic<size_t> global_seq_{0};

    volatile char*  region_     = nullptr;
    size_t          region_size_ = 0;
    volatile char*  phase_base_ = nullptr;
    volatile char*  value_base_ = nullptr;

    bool            initialized_ = false;

    // phase(v) = ⌊v/n⌋ mod 2  (Herlihy-Shavit-Waarts)
    // Toggles every n values per slot, preventing ABA on the circular buffer.
    uint8_t phase_of(size_t v) const {
        return (uint8_t)((v / num_threads_) % 2);
    }

    volatile uint8_t* get_phase_bit(size_t slot) const {
        return (volatile uint8_t*)&phase_base_[slot * CL];
    }

    volatile size_t* get_thread_value(size_t tid) const {
        return (volatile size_t*)&value_base_[tid * CL];
    }
};

// Named specializations per network type

template <typename Sync>
class WFBitonicLock
    : public WaitingFilterCountingLock<Sync, BitonicNetwork> {
public:
    std::string name() override {
        return std::string("wf_bitonic_") + Sync::sync_name();
    }
};

template <typename Sync>
class WFPeriodicLock
    : public WaitingFilterCountingLock<Sync, PeriodicNetwork> {
public:
    std::string name() override {
        return std::string("wf_periodic_") + Sync::sync_name();
    }
};


#endif // WAITING_FILTER_COUNTING_LOCK_HPP
