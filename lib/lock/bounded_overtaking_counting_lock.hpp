#ifndef BOUNDED_OVERTAKING_COUNTING_LOCK_HPP
#define BOUNDED_OVERTAKING_COUNTING_LOCK_HPP

#pragma once

#include "lock.hpp"
#include "bitonic_networks.hpp"
#include "counting_lock_common.hpp"
#include "../utils/cxl_utils.hpp"
#include <atomic>
#include <cmath>
#include <string>

// =============================================================================
// DESIGN F — Bounded-Overtaking Counting Lock      (bo_bitonic_*, bo_periodic_*)
//
// Built on Design B's ticket skeleton (sequenced_counting_lock.hpp): same
// network-then-fetch_add arrival shaping, same >=4n token-versioned slot
// array. It differs ONLY in what unlock() does when the next ticket holder
// is not ready.
//
// Motivation: every strictly-ordered lock in this family forms a handoff
// chain — waiter k cannot proceed until waiter k-1 runs. If the next
// ticket holder is not ready (preempted, or still inside the network
// between its fetch_add and its registration), the whole chain stalls
// behind one thread. This lock relaxes strict FIFO into K-BOUNDED
// OVERTAKING: an unlocker that finds its immediate successor unregistered
// may grant the lowest *registered* token within a window of K tickets,
// skipping unready holders. Skipped threads detect the skip and re-draw a
// fresh ticket.
//
// The quiescence connection: at (or near) quiescence every drawn ticket is
// registered, so the window scan finds cur+1 immediately and the lock
// degenerates to an exact FIFO ticket lock with zero extra cost. The
// window only opens — and fairness only bends — precisely when strict
// ordering would have stalled the chain.
//
// Protocol
//   lock():
//     1. traverse network (arrival staggering, as Design B; once per
//        acquisition, not per re-draw)
//     2. seq = global_seq_.fetch_add(1)          — contiguous ticket
//     3. slot[seq % S].token = seq               — "I have arrived"
//     4. spin: now_serving_ == seq   -> enter CS
//              now_serving_  > seq   -> we were skipped: goto 2 (re-draw)
//   unlock():  (cur = now_serving_)
//     1. brief grace spin: is slot[cur+1] registered? if yes -> serve cur+1
//     2. else scan cur+2 .. cur+K for the lowest registered token t
//        -> now_serving_ = t   (this write IS the grant; skipped tokens in
//           (cur, t) observe now_serving_ > seq and re-draw)
//     3. else now_serving_ = cur+1 — classic ticket fallback; safe because
//        now_serving_ is monotonic, so cur+1 was never skipped-past and
//        its (current or future) owner still wants it.
//
// Correctness
//   - Mutual exclusion: now_serving_ is written only by the lock holder
//     and a thread enters only on now_serving_ == seq with unique seq —
//     single writer, single equality match per value.
//   - No lost thread: a skipped registrant never blocks forever — it sees
//     now_serving_ > seq (monotonic, jumped over it) and re-draws.
//   - No stale-grant hazard by construction: the grant is the shared
//     now_serving_ write itself; there is no per-thread grant pointer.
//   - Livelock/starvation: overtaking is bounded by K per unlock, but a
//     thread can in principle be skipped repeatedly (each time re-drawing
//     a larger ticket). Under fair scheduling it wins; under adversarial
//     timing this is weaker than FIFO — that is the semantics trade, made
//     explicit. K = num_threads (covers all possible in-flight tickets).
//
// Measured caveat (ELEVATOR_SET_COMPARISON.md §7.7): registration is an
// ARRIVAL signal, not a LIVENESS signal — a registered waiter can itself be
// descheduled, so the window scan adds no scheduling information and Design
// F ties Design B in practice. Design G (heartbeat_counting_lock.hpp) is
// the refinement that adds the missing liveness signal; F is retained as
// its ablation baseline.
//
// Complexity
//   lock:   O(log^2 W) network + 1 fetch_add (+ re-draws when skipped)
//   unlock: O(1) when successor ready (common/quiescent case); O(K) scan
//           when it is not
//   fairness: FIFO at quiescence; K-bounded overtaking per unlock under
//             concurrency; no global starvation bound.
// =============================================================================


template <typename Sync, template <typename> class NetworkT>
class BoundedOvertakingCountingLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        num_threads_ = num_threads;

        size_t target = (size_t)std::ceil(std::sqrt((double)num_threads));
        if (target < 2) target = 2;
        width_ = 1;
        while (width_ < target) width_ *= 2;

        // Slot array: power of 2, >= 4n so live tokens (span <= n, one per
        // thread) can never collide on a slot (same argument as Design B).
        num_slots_ = 1;
        while (num_slots_ < 4 * num_threads) num_slots_ *= 2;
        slot_mask_ = num_slots_ - 1;

        // Overtaking window: all possibly-in-flight tickets.
        window_ = num_threads;

        network_.build(width_, num_threads);

        size_t now_serving_bytes = CL;
        size_t slot_bytes        = num_slots_ * CL;
        region_size_ = CL + now_serving_bytes + slot_bytes;
        region_ = (volatile char*)ALLOCATE(region_size_);

        size_t off = CL;  // leading pad
        now_serving_ = (volatile size_t*)&region_[off];  off += now_serving_bytes;
        slot_base_   = &region_[off];

        global_seq_.store(0, std::memory_order_relaxed);
        *now_serving_ = 0;
        for (size_t i = 0; i < num_slots_; i++) {
            get_slot(i)->token = NO_TOKEN;
        }
        initialized_ = true;
    }

    void lock(size_t thread_id) override {
        // Network traversal once per acquisition (not per re-draw): its
        // role is arrival staggering, which a re-draw does not need.
        network_.traverse((int)(thread_id % width_), thread_id);

        for (;;) {
            size_t seq = global_seq_.fetch_add(1, std::memory_order_relaxed);
            get_slot(seq & slot_mask_)->token = seq;   // register: "arrived"
            Fence();

            unsigned spins = 0;
            for (;;) {
                size_t serving = *now_serving_;
                if (serving == seq) {
                    Fence();
                    return;                       // our turn
                }
                if ((ssize_t)(serving - seq) > 0) {
                    break;                        // skipped: re-draw
                }
                LcSpinWait(spins);
            }
        }
    }

    void unlock(size_t /*thread_id*/) override {
        size_t cur = *now_serving_;
        size_t next = cur + 1;
        auto* nse = get_slot(next & slot_mask_);

        // Grace period: give the immediate successor a moment to register
        // before considering overtaking, so transient timing skew does not
        // cause re-draw churn.
        for (int i = 0; i < HANDOFF_SPINS; i++) {
            if (nse->token == next) {
                Fence();
                *now_serving_ = next;
                Fence();
                return;
            }
            LcSpinHint();
        }

        // Successor not ready: grant the lowest registered ticket in
        // (cur+1, cur+window_]. Registrants cannot abandon tokens in this
        // range concurrently — abandonment requires now_serving_ > token,
        // and we have not advanced now_serving_ yet.
        for (size_t t = next + 1; t <= cur + window_; t++) {
            if (get_slot(t & slot_mask_)->token == t) {
                Fence();
                *now_serving_ = t;   // the grant; skipped tokens re-draw
                Fence();
                return;
            }
        }

        // Nobody registered in the window: classic ticket fallback. Token
        // cur+1 was never skipped-past (now_serving_ is monotonic), so its
        // owner — current or the next thread to draw it — still takes it.
        Fence();
        *now_serving_ = next;
        Fence();
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
        return std::string("bo_counting_") + Sync::sync_name();
    }

private:
    static constexpr size_t CL = std::hardware_destructive_interference_size;
    static constexpr size_t NO_TOKEN = ~(size_t)0;
    static constexpr int HANDOFF_SPINS = 64;

    struct SlotEntry {
        volatile size_t token;   // highest sequence number registered here
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

    bool initialized_ = false;

    SlotEntry* get_slot(size_t idx) const {
        return (SlotEntry*)&slot_base_[idx * CL];
    }
};

template <typename Sync>
class BoBitonicLock
    : public BoundedOvertakingCountingLock<Sync, BitonicNetwork> {
public:
    std::string name() override {
        return std::string("bo_bitonic_") + Sync::sync_name();
    }
};

template <typename Sync>
class BoPeriodicLock
    : public BoundedOvertakingCountingLock<Sync, PeriodicNetwork> {
public:
    std::string name() override {
        return std::string("bo_periodic_") + Sync::sync_name();
    }
};


#endif // BOUNDED_OVERTAKING_COUNTING_LOCK_HPP
