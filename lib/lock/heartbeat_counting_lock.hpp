#ifndef HEARTBEAT_COUNTING_LOCK_HPP
#define HEARTBEAT_COUNTING_LOCK_HPP

#pragma once

#include "lock.hpp"
#include "bitonic_networks.hpp"
#include "counting_lock_common.hpp"
#include "../utils/cxl_utils.hpp"
#include <atomic>
#include <cmath>
#include <string>

// =============================================================================
// DESIGN G — Heartbeat-Overtaking Counting Lock    (hb_bitonic_*, hb_periodic_*)
//
// Refines Design F (bounded_overtaking_counting_lock.hpp) with the lesson
// from its measurement: registration is an ARRIVAL signal, but skipping is
// only profitable when the skipped-over thread is not RUNNING — and a
// registered waiter can itself be descheduled. Design F's window scan
// therefore gained no scheduling information and merely tied Seq/WF
// (ELEVATOR_SET_COMPARISON.md §7.7).
//
// Design G gives the unlocker a LIVENESS signal: every waiter stamps a
// per-slot heartbeat counter on each spin iteration. The stalled-successor
// path samples all candidate beats, waits a short grace gap, re-samples,
// and grants the lowest token whose beat ADVANCED — a thread that is
// provably scheduled at this instant. A descheduled thread cannot beat for
// a full scheduling quantum (milliseconds), while a spinning thread beats
// every few nanoseconds, so a ~sub-microsecond gap discriminates cleanly.
//
// (Related in spirit to time-published locks — He, Scherer & Scott,
// "Preemption adaptivity in time-published queue-based spin locks" — which
// also let the lock observe waiter recency to route around preempted
// threads; here the signal is a free-running beat counter in the waiter's
// own cache line and the consumer is a ticket-window scan.)
//
// Selection ladder at unlock (cur = own token):
//   1. successor (cur+1) registers within a brief grace spin -> grant it
//      (fast path: O(1), covers the quiescent/ready case; a thread that
//      just registered is trivially alive)
//   2. lowest token in (cur+1, cur+K] with a FRESH heartbeat -> grant it
//   3. lowest registered token (stale or not) -> grant it  (= Design F)
//   4. nobody registered -> now_serving_ = cur+1  (= classic ticket)
//
// Correctness is inherited from Design F verbatim: F's argument (single
// now_serving_ writer, exact-equality entry, monotonic skip detection,
// re-draw on skip, scan-vs-abandon stability) holds for ANY choice of
// registered token in the window — the heartbeat only changes which one is
// chosen. The heartbeat itself is a single-writer field in the waiter's
// own CL-padded slot; the unlocker only reads it.
//
// Costs vs Design F: one extra volatile store per spin iteration (own
// cache line — no coherence traffic except during the rare scan), a
// holder-only scratch array (no atomics), and a grace gap (~256 spin
// hints) on the stalled path only.
// =============================================================================


template <typename Sync, template <typename> class NetworkT>
class HeartbeatOvertakingCountingLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        num_threads_ = num_threads;

        size_t target = (size_t)std::ceil(std::sqrt((double)num_threads));
        if (target < 2) target = 2;
        width_ = 1;
        while (width_ < target) width_ *= 2;

        num_slots_ = 1;
        while (num_slots_ < 4 * num_threads) num_slots_ *= 2;
        slot_mask_ = num_slots_ - 1;
        window_ = num_threads;

        network_.build(width_, num_threads);

        size_t now_serving_bytes = CL;
        size_t slot_bytes        = num_slots_ * CL;
        size_t scratch_bytes     = window_ * sizeof(size_t);
        region_size_ = CL + now_serving_bytes + slot_bytes + scratch_bytes;
        region_ = (volatile char*)ALLOCATE(region_size_);

        size_t off = CL;
        now_serving_ = (volatile size_t*)&region_[off];  off += now_serving_bytes;
        slot_base_   = &region_[off];                    off += slot_bytes;
        // Scratch for the two-phase freshness scan. Only the current lock
        // holder touches it (successive holders, never concurrently), so it
        // needs no volatile/padding — but it lives in the ALLOCATE region so
        // CXL/NUMA builds keep all lock state in one memory domain.
        scratch_beats_ = (size_t*)&region_[off];

        global_seq_.store(0, std::memory_order_relaxed);
        *now_serving_ = 0;
        for (size_t i = 0; i < num_slots_; i++) {
            get_slot(i)->token = NO_TOKEN;
            get_slot(i)->beat  = 0;
        }
        initialized_ = true;
    }

    void lock(size_t thread_id) override {
        network_.traverse((int)(thread_id % width_), thread_id);

        for (;;) {
            size_t seq = global_seq_.fetch_add(1, std::memory_order_relaxed);
            auto* slot = get_slot(seq & slot_mask_);
            slot->token = seq;
            Fence();

            unsigned spins = 0;
            for (;;) {
                size_t serving = *now_serving_;
                if (serving == seq) {
                    Fence();
                    return;
                }
                if ((ssize_t)(serving - seq) > 0) {
                    break;                        // skipped: re-draw
                }
                slot->beat = slot->beat + 1;      // liveness stamp
                LcSpinWait(spins);
            }
        }
    }

    void unlock(size_t /*thread_id*/) override {
        size_t cur = *now_serving_;
        size_t next = cur + 1;
        auto* nse = get_slot(next & slot_mask_);

        // 1. Fast path: successor registers within the grace spin.
        for (int i = 0; i < HANDOFF_SPINS; i++) {
            if (nse->token == next) {
                Fence();
                *now_serving_ = next;
                Fence();
                return;
            }
            LcSpinHint();
        }

        // Successor absent. Two-phase freshness scan over the window.
        // Phase 1: sample beats of registered candidates.
        size_t lowest_registered = 0;
        bool have_registered = false;
        for (size_t k = 0; k < window_; k++) {
            size_t t = next + 1 + k;
            auto* s = get_slot(t & slot_mask_);
            if (s->token == t) {
                scratch_beats_[k] = s->beat;
                if (!have_registered) {
                    lowest_registered = t;
                    have_registered = true;
                }
            } else {
                scratch_beats_[k] = NO_TOKEN;   // sentinel: not registered
            }
        }

        if (have_registered) {
            // Grace gap: long enough for any scheduled spinner to beat at
            // least once, far shorter than a scheduling quantum.
            for (int i = 0; i < FRESHNESS_GAP_SPINS; i++) {
                // The immediate successor may register mid-scan — prefer it.
                if (nse->token == next) {
                    Fence();
                    *now_serving_ = next;
                    Fence();
                    return;
                }
                LcSpinHint();
            }
            // Phase 2: grant the lowest candidate whose beat advanced.
            for (size_t k = 0; k < window_; k++) {
                if (scratch_beats_[k] == NO_TOKEN) continue;
                size_t t = next + 1 + k;
                auto* s = get_slot(t & slot_mask_);
                if (s->token == t && s->beat != scratch_beats_[k]) {
                    Fence();
                    *now_serving_ = t;
                    Fence();
                    return;
                }
            }
            // 3. Nobody provably live: fall back to the lowest registered
            //    (Design F behavior — its owner runs eventually).
            Fence();
            *now_serving_ = lowest_registered;
            Fence();
            return;
        }

        // 4. Nobody registered at all: classic ticket fallback.
        Fence();
        *now_serving_ = next;
        Fence();
    }

    void destroy() override {
        if (!initialized_) return;
        initialized_ = false;
        network_.destroy();
        scratch_beats_ = nullptr;   // part of region_
        if (region_) {
            FREE((void*)region_, region_size_);
            region_ = nullptr;
        }
    }

    std::string name() override {
        return std::string("hb_counting_") + Sync::sync_name();
    }

private:
    static constexpr size_t CL = std::hardware_destructive_interference_size;
    static constexpr size_t NO_TOKEN = ~(size_t)0;
    static constexpr int HANDOFF_SPINS = 64;
    static constexpr int FRESHNESS_GAP_SPINS = 256;

    struct SlotEntry {
        volatile size_t token;   // registered sequence number
        volatile size_t beat;    // heartbeat: bumped by the waiter each spin
    };

    NetworkT<Sync> network_;
    size_t num_threads_ = 0;
    size_t width_       = 0;
    size_t num_slots_   = 0;
    size_t slot_mask_   = 0;
    size_t window_      = 0;

    std::atomic<size_t> global_seq_{0};

    volatile char*   region_      = nullptr;
    size_t           region_size_ = 0;
    volatile size_t* now_serving_ = nullptr;
    volatile char*   slot_base_   = nullptr;
    size_t*          scratch_beats_ = nullptr;   // holder-only

    bool initialized_ = false;

    SlotEntry* get_slot(size_t idx) const {
        return (SlotEntry*)&slot_base_[idx * CL];
    }
};

template <typename Sync>
class HbBitonicLock
    : public HeartbeatOvertakingCountingLock<Sync, BitonicNetwork> {
public:
    std::string name() override {
        return std::string("hb_bitonic_") + Sync::sync_name();
    }
};

template <typename Sync>
class HbPeriodicLock
    : public HeartbeatOvertakingCountingLock<Sync, PeriodicNetwork> {
public:
    std::string name() override {
        return std::string("hb_periodic_") + Sync::sync_name();
    }
};


#endif // HEARTBEAT_COUNTING_LOCK_HPP
