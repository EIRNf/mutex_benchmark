#ifndef WIRE_INDEXED_COUNTING_LOCK_HPP
#define WIRE_INDEXED_COUNTING_LOCK_HPP

#pragma once

#include "lock.hpp"
#include "bitonic_networks.hpp"
#include "counting_lock_common.hpp"
#include "../utils/cxl_utils.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

// =============================================================================
// DESIGN A — Wire-Indexed Counting Lock            (lw_bitonic_*, lw_periodic_*)
//
// The only design in this family whose SERVICE ORDER actually comes from the
// counting network: every other design (B/C/F/G/D) orders through a global
// fetch_add ticket and uses the network purely to shape arrivals.
//
// Protocol
//   lock():   traverse network -> (wire, per-pair round); register in the
//             per-wire slot for that round {tid, round, &local_grant,
//             WAITING}; then poll two grant paths:
//               (a) the holder's unlock() grants us through spin_addr, or
//               (b) the lock is free-floating (*waker_flag_) and we claim it
//                   through the waker lock, leaving a SERVED breadcrumb so
//                   the sweep can advance the wire head without skipping
//                   earlier, not-yet-registered rounds.
//   unlock(): predict the successor's wire from the step property
//             (next_to_serve_ mod W) — O(1) on a hit; otherwise sweep all W
//             wire heads for the minimum (round, wire-distance) waiter;
//             if none is visible, set the waker flag.
//
// Concurrency-control invariants (added 2026-07-14 after the token-
// destruction race was found):
//   * The WAITING -> SERVED (waiter self-serve) and WAITING -> EMPTY
//     (unlocker grant, try_grant()) transitions are CAS-arbitrated: exactly
//     one wins, so a waiter can never absorb both a grant and the waker-flag
//     token. Absorbing both destroyed a lock token and hung every remaining
//     waiter — the historical lw_* hang.
//   * The grant is published with a release store through spin_addr paired
//     with acquire polls of local_grant; the waker-flag store is fenced on
//     the release side.
//   * Wire heads and next_to_serve_ are holder-only state (single writer).
//
// Fairness: approximates network (≈ arrival) order via the step property;
// NOT strict FIFO under concurrency, and the waker-flag path serves
// invisible waiters out of order by design.
//
// Complexity
//   lock:   O(log^2 W) balancer traversals + O(1) registration
//   unlock: O(1) predicted / O(W) sweep (re-swept only when a chosen waiter
//           self-serves concurrently)
//   space:  O(W * rounds_cap * CL) — dominated by the deliberately oversized
//           per-wire slot arrays (see rounds_cap_ comment in init()).
//
// KNOWN RESIDUAL (2026-07-14): ~1-3% of short 4T lock-level runs still hang.
// Evidence so far: sync-layer agnostic (seen with lamport AND bakery
// balancers), NOT caused by duplicate (wire, round) emission (a temporary
// duplicate-registration detector never fired across a reproduced hang), and
// at hang time every waiter is polling with the flag down and no grant
// pending — i.e., one more token-leak window exists in the outer protocol.
// Repro: loop `max_contention_bench lw_periodic_bakery 4 0.25 --csv
// --no-output` ~30x. Until closed, two aliases stay routed to fallback
// designs (see linearizable_counting_lock.hpp Section 5).
// =============================================================================


template <typename Sync, template <typename> class NetworkT>
class WireIndexedCountingLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        num_threads_ = num_threads;

        // Width = smallest power of 2 >= ceil(sqrt(n)), minimum 2
        size_t target = (size_t)std::ceil(std::sqrt((double)num_threads));
        if (target < 2) target = 2;
        width_ = 1;
        while (width_ < target) width_ *= 2;

        // Per-wire capacity. Must exceed the maximum number of rounds that
        // can be live (WAITING or SERVED-awaiting-cleanup) on one wire at
        // once. Unserved tokens total at most num_threads (one per thread),
        // and in the worst case all of them land on a single wire, so the
        // original heuristic 4n/W + 4 could overflow and let a registration
        // silently clobber a still-waiting round. The bound needed for
        // correctness is n plus SERVED breadcrumbs awaiting sweep cleanup;
        // 64n (floor 1024) is deliberately far above it so slot-wrap can be
        // ruled out when debugging — at CL bytes per slot this costs
        // W * 64n * 64B (~8 MB at n=16, W=4), acceptable for a benchmark.
        rounds_cap_ = std::max((size_t)1024, 64 * num_threads);

        network_.build(width_, num_threads);

        // ── Memory layout (single ALLOCATE region) ──
        // [waker_flag : CL] [wire_heads : W*CL] [wire_slots : W*rounds_cap*CL]
        // [thread_meta : n*CL] [waker_lock_mem : n*bool]
        size_t waker_flag_bytes = CL;
        size_t wire_heads_bytes = width_ * CL;
        size_t wire_slots_bytes = width_ * rounds_cap_ * CL;
        size_t meta_bytes       = num_threads * CL;
        size_t waker_bytes      = sizeof(volatile bool) * num_threads;

        region_size_ = waker_flag_bytes + wire_heads_bytes +
                       wire_slots_bytes + meta_bytes + waker_bytes;
        region_ = (volatile char*)ALLOCATE(region_size_);
        memset((void*)region_, 0, region_size_);

        size_t off = 0;
        waker_flag_      = (volatile bool*)&region_[off];      off += waker_flag_bytes;
        wire_heads_base_ = &region_[off];                      off += wire_heads_bytes;
        wire_slots_base_ = &region_[off];                      off += wire_slots_bytes;
        meta_base_       = &region_[off];                      off += meta_bytes;
        volatile bool* waker_mem = (volatile bool*)&region_[off];

        // Init wire heads to round 0
        for (size_t w = 0; w < width_; w++) {
            get_wire_head(w)->next_round = 0;
        }

        // Init all wire slots as empty
        for (size_t w = 0; w < width_; w++) {
            for (size_t r = 0; r < rounds_cap_; r++) {
                auto* s = get_wire_slot(w, r);
                s->state = SLOT_EMPTY;
                s->tid = 0;
                s->round = 0;
                s->spin_addr = nullptr;
            }
        }

        *waker_flag_ = true;   // Lock starts free
        next_to_serve_ = 0;    // Global step-property counter
        waker_lock_.init(waker_mem, num_threads);
        initialized_ = true;
    }

    void lock(size_t thread_id) override {
        volatile bool local_grant = false;

        // 1. Traverse counting network → (wire, per-pair round)
        size_t lv = 0;
        int wire_i = network_.traverse((int)(thread_id % width_),
                                       thread_id, &lv);
        size_t wire  = (size_t)wire_i;
        size_t round = lv >> 1;

        // Store for unlock
        auto* meta = get_meta(thread_id);
        meta->wire  = wire;
        meta->round = round;

        // 2. Register in per-wire slot (rounds_cap_ >= 2n guarantees the
        //    slot for `round` cannot still be live for round - rounds_cap_).
        auto* slot = get_wire_slot(wire, round % rounds_cap_);
        slot->tid       = thread_id;
        slot->round     = round;
        slot->spin_addr = &local_grant;
        Fence();
        slot->state = SLOT_WAITING;
        Fence();

        // 3. Wait for one of two grant paths:
        //    (a) the current holder's unlock() grants us via spin_addr, or
        //    (b) the lock is free-floating (*waker_flag_ set because an
        //        unlock found no visible waiter) and we claim it through
        //        the waker lock.
        //    Every waiter keeps re-polling the flag. The previous protocol
        //    tried the waker lock exactly once and then spun passively on
        //    local_grant; a waiter whose round was invisible to the sweep
        //    (see unlock()) could then starve forever — the observed
        //    lw_* hang at every thread count >= 2.
        // local_grant is read with acquire (paired with try_grant()'s
        // release store through spin_addr) so critical-section accesses
        // cannot be reordered above the grant observation.
        unsigned spins = 0;
        for (;;) {
            if (__atomic_load_n(&local_grant, __ATOMIC_ACQUIRE)) return;
            if (*waker_flag_ && waker_lock_.trylock(thread_id)) {
                Fence();
                if (__atomic_load_n(&local_grant, __ATOMIC_ACQUIRE)) {
                    // Granted while acquiring the waker lock: do NOT consume
                    // the flag (that would destroy a lock token that belongs
                    // to some other, still-invisible waiter).
                    waker_lock_.unlock();
                    return;
                }
                if (*waker_flag_) {
                    // Lock is free — but the current unlocker may STILL be
                    // mid-grant on this very slot (it saw us WAITING before
                    // the flag was set by an earlier unlock, or is sweeping
                    // concurrently). Self-serving AND being granted absorbs
                    // two lock tokens into one CS entry, permanently
                    // destroying one — every remaining waiter then spins
                    // with no flag and no grant (the observed lw_* hang).
                    // Arbitrate through an atomic WAITING -> SERVED
                    // transition; the granter's WAITING -> EMPTY CAS in
                    // try_grant() is the other half of this arbitration.
                    int expected = SLOT_WAITING;
                    if (__atomic_compare_exchange_n(
                            (volatile int*)&slot->state, &expected, SLOT_SERVED,
                            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                        // We own the WAITING->SERVED transition: consume the
                        // free token. The SERVED breadcrumb (not a head
                        // advance) keeps earlier, not-yet-registered rounds
                        // on this wire visible to unlock()'s sweep.
                        *waker_flag_ = false;
                        Fence();
                        waker_lock_.unlock();
                        return;
                    }
                    // Lost the race: unlock() already claimed our slot and
                    // its grant is imminent (or done). Leave the flag for
                    // the waiter it belongs to and take the granted token.
                    waker_lock_.unlock();
                    unsigned gspins = 0;
                    while (!__atomic_load_n(&local_grant, __ATOMIC_ACQUIRE)) {
                        LcSpinWait(gspins);
                    }
                    return;
                }
                waker_lock_.unlock();
            }
            LcSpinWait(spins);
        }
    }

    void unlock(size_t /*thread_id*/) override {
        // Step-property prediction:
        //   Global token ordering: wire 0 r0, wire 1 r0, ..., wire W-1 r0,
        //                          wire 0 r1, wire 1 r1, ...
        //   next_to_serve_ tracks position in this ordering.
        size_t next = next_to_serve_;
        next_to_serve_ = next + 1;
        size_t pred_wire = next % width_;

        // First: check predicted wire — O(1) on prediction hit
        catch_up_head(pred_wire);
        {
            size_t r = get_wire_head(pred_wire)->next_round;
            auto* s = get_wire_slot(pred_wire, r % rounds_cap_);
            if (s->state == SLOT_WAITING && s->round == r &&
                try_grant(pred_wire, r, s)) {
                return;
            }
        }

        // Prediction missed — sweep all W wires for the minimum
        // (round, wire_distance) waiter.  This handles step-property
        // violations under concurrency. Re-swept if a chosen waiter
        // self-serves concurrently (try_grant loses its CAS): that waiter
        // consumed the flag token, so another visible waiter may still need
        // this unlock's token.
        for (;;) {
            size_t best_wire  = width_;
            size_t best_round = ~(size_t)0;
            WireSlot* best_slot = nullptr;

            for (size_t w = 0; w < width_; w++) {
                catch_up_head(w);
                size_t r = get_wire_head(w)->next_round;
                auto* s = get_wire_slot(w, r % rounds_cap_);

                // Prefetch next wire's head
                if (w + 1 < width_)
                    __builtin_prefetch(get_wire_head(w + 1), 0, 1);

                if (s->state == SLOT_WAITING && s->round == r) {
                    // Compare (round, wire_dist) lexicographically
                    size_t wd = (w + width_ - pred_wire) % width_;
                    if (r < best_round ||
                        (r == best_round && wd < ((best_wire + width_ - pred_wire) % width_))) {
                        best_round = r;
                        best_wire  = w;
                        best_slot  = s;
                    }
                }
            }

            if (best_slot == nullptr) break;
            if (try_grant(best_wire, best_round, best_slot)) {
                return;
            }
        }

        // No visible waiter — set waker flag. Waiters poll this flag in
        // lock(), so an invisible waiter (registered at a round above its
        // wire head) claims the free lock through the waker lock instead
        // of being lost. The fence must precede the store: the consumer
        // enters its critical section on seeing the flag, so this unlock's
        // CS writes have to be visible first.
        Fence();
        *waker_flag_ = true;
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
        // Network-agnostic base name; WireIndexedBitonicLock /
        // WireIndexedPeriodicLock below override with the exact benchmark
        // names. (The old body claimed to detect the network type but
        // unconditionally said "bitonic".)
        return std::string("lw_counting_") + Sync::sync_name();
    }

private:
    static constexpr size_t CL = std::hardware_destructive_interference_size;

    // Slot lifecycle: EMPTY -> WAITING (registration) -> EMPTY (granted by
    // unlock) or SERVED (self-served via waker flag; cleaned up by the next
    // sweep's catch_up_head, which uses it to advance the wire head without
    // skipping earlier rounds).
    static constexpr int SLOT_EMPTY   = 0;
    static constexpr int SLOT_WAITING = 1;
    static constexpr int SLOT_SERVED  = 2;

    // Per-wire slot: one waiter registration at a (wire, round) position
    struct WireSlot {
        volatile int    state;
        volatile size_t tid;
        volatile size_t round;      // For verification against wrap-around
        volatile bool*  spin_addr;
    };

    // Per-wire head: tracks the next round to serve on this wire
    struct WireHead {
        size_t next_round;
    };

    // Per-thread metadata: holder's (wire, round) for unlock
    struct ThreadMeta {
        size_t wire;
        size_t round;
    };

    NetworkT<Sync> network_;
    size_t num_threads_ = 0;
    size_t width_       = 0;
    size_t rounds_cap_  = 0;

    volatile char*  region_     = nullptr;
    size_t          region_size_ = 0;
    volatile bool*  waker_flag_ = nullptr;
    volatile char*  wire_heads_base_ = nullptr;
    volatile char*  wire_slots_base_ = nullptr;
    volatile char*  meta_base_  = nullptr;

    BnWakerLock     waker_lock_;
    size_t          next_to_serve_ = 0;   // Protected by lock ownership
    bool            initialized_ = false;

    // CL-padded accessors
    WireHead* get_wire_head(size_t wire) const {
        return (WireHead*)&wire_heads_base_[wire * CL];
    }

    WireSlot* get_wire_slot(size_t wire, size_t slot_idx) const {
        return (WireSlot*)&wire_slots_base_[(wire * rounds_cap_ + slot_idx) * CL];
    }

    ThreadMeta* get_meta(size_t tid) const {
        return (ThreadMeta*)&meta_base_[tid * CL];
    }

    // Advance a wire's head past rounds completed via the waker-flag path.
    // Rounds are assigned contiguously per wire, so each head round is
    // eventually either granted here (head advanced at grant time) or
    // marked SERVED by its self-serving owner (head advanced here). Only
    // the lock holder calls this, so head/slot mutation is single-writer.
    void catch_up_head(size_t w) {
        auto* head = get_wire_head(w);
        for (;;) {
            auto* s = get_wire_slot(w, head->next_round % rounds_cap_);
            if (s->state == SLOT_SERVED && s->round == head->next_round) {
                s->state = SLOT_EMPTY;
                head->next_round++;
            } else {
                break;
            }
        }
    }

    // Try to grant the lock to the waiter registered at (w, r). Claims the
    // slot with an atomic WAITING -> EMPTY transition, which arbitrates
    // against the waiter's own WAITING -> SERVED self-serve CAS in lock():
    // exactly one of the two paths wins, so a waiter can never absorb both
    // a grant and the waker-flag token (which destroyed a token and hung
    // every remaining waiter). Returns false if the waiter self-served.
    // The spin_addr write must be the very last touch: the moment it flips,
    // the waiter may return and the stack frame spin_addr points into dies.
    bool try_grant(size_t w, size_t r, WireSlot* s) {
        // Acquire-load the state before reading spin_addr so the registrant's
        // spin_addr store (published before its WAITING store) is visible.
        if (__atomic_load_n((volatile int*)&s->state, __ATOMIC_ACQUIRE) != SLOT_WAITING) {
            return false;
        }
        volatile bool* addr = s->spin_addr;
        int expected = SLOT_WAITING;
        if (!__atomic_compare_exchange_n(
                (volatile int*)&s->state, &expected, SLOT_EMPTY,
                false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return false; // waiter self-served via the waker flag
        }
        get_wire_head(w)->next_round = r + 1;
        // Release store: publishes this unlock's CS writes (and the head
        // advance) to the waiter's acquire poll of local_grant.
        __atomic_store_n(addr, true, __ATOMIC_RELEASE);
        return true;
    }
};

// Specialization for name() — detect network type at compile time
// (We use partial specialization via a helper)

template <typename Sync>
class WireIndexedBitonicLock
    : public WireIndexedCountingLock<Sync, BitonicNetwork> {
public:
    std::string name() override {
        return std::string("lw_bitonic_") + Sync::sync_name();
    }
};

template <typename Sync>
class WireIndexedPeriodicLock
    : public WireIndexedCountingLock<Sync, PeriodicNetwork> {
public:
    std::string name() override {
        return std::string("lw_periodic_") + Sync::sync_name();
    }
};


#endif // WIRE_INDEXED_COUNTING_LOCK_HPP
