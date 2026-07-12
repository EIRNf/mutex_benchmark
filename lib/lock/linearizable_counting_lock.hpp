#ifndef LINEARIZABLE_COUNTING_LOCK_HPP
#define LINEARIZABLE_COUNTING_LOCK_HPP

#pragma once

#include "lock.hpp"
#include "bitonic_networks.hpp"
#include "../utils/cxl_utils.hpp"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <new>
#include <atomic>
#include <algorithm>
#include <string>

// =============================================================================
// Linearizable-Window Counting Locks
//
// Two lock designs that exploit counting-network structure for successor
// lookup, reducing unlock cost from O(n) to O(W) or O(1).
//
// Based on:
//   [1] Lynch, Shavit, Shvartsman (1996) — "Counting Networks Are Practically
//       Linearizable"  (PODC 96)
//   [2] Herlihy, Shavit, Waarts (1996) — "Linearizable Counting Networks"
//       (Distributed Computing)
//
// See LINEARIZABLE_COUNTING_ANALYSIS.md for full theoretical analysis.
//
// ═════════════════════════════════════════════════════════════════════════════
//
// DESIGN A — Wire-Indexed Counting Lock  (WireIndexedBitonicLock, etc.)
//
//   Uses per-wire slot arrays indexed by per-pair round.  Unlock predicts
//   the successor's (wire, round) from the step property and checks ONE
//   slot.  Falls back to O(W) wire sweep if prediction misses.
//
//   Lock path:  O(log²W) balancer traversals  (same as standard)
//   Unlock:     O(1) predicted / O(W) fallback
//   Extra cost: NONE — no additional atomic on lock path
//
//   Assumptions:
//     A1  Step property approximately holds under concurrency
//     A2  Per-pair rounds monotonically increase per wire (guaranteed by
//         linearizable balancers: each balancer uses fetch_add)
//     A5  Per-pair rounds across final-layer balancers are INDEPENDENT —
//         prediction may miss when step property is violated
//
// DESIGN B — Sequenced Counting Lock  (SeqBitonicLock, etc.)
//
//   Adds ONE global fetch_add after network traversal.  The sequence number
//   is the linearization point — tokens are contiguous and globally ordered.
//   Unlock checks slot[seq + 1] for guaranteed O(1).
//
//   Lock path:  O(log²W) traversal + 1 fetch_add  (one extra atomic)
//   Unlock:     O(1) guaranteed
//   Extra cost: One shared cache line (global_seq_)
//
//   The counting network distributes arrival times across W/2 balancers,
//   so threads hit global_seq_ at staggered intervals — less contention
//   than a bare ticket lock.
// =============================================================================
// DESIGN C: Waiting-Filter Counting Lock  (Design C)
//
// From Herlihy, Shavit, Waarts (1996), Section 3: "The Waiting Network"
//
// A counting network + n-element phase-bit array that linearizes the output.
// Each token v waits for predecessor v-1 to set its phase bit (at unlock),
// then enters CS.  Unlock sets own phase bit — O(1) always.
//
// Key insight from Lynch, Shavit, Shvartsman (1996):
//   Under practical linearizability (c2 ≤ 2·c1, i.e., bounded timing
//   variation on wires), predecessors have ALREADY completed before
//   successors check, so the phase-bit wait is near-zero in practice.
//   The Waiting-filter is still needed for correctness under all timings
//   (preemption, NUMA/CXL with c2 >> c1).
//
// Complexity:
//   Lock:   O(log²W) network traverse + O(1) phase check (practical)
//           O(log²W) + O(chain) phase check (worst case, chain ≤ n)
//   Unlock: O(1) always — set one phase bit
//   Space:  O(n·CL) phase bits + O(n·CL) per-thread values
//   Extra atomics on lock path: NONE
//
// phase(v) = ⌊v/n⌋ mod 2   — toggles every n values per slot, preventing ABA
// =============================================================================
//
// =============================================================================


// ─── Spin-wait hint ──────────────────────────────────────────────────────────
#if defined(__aarch64__) || defined(_M_ARM64)
  #define LcSpinHint() __asm__ volatile("yield")
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #define LcSpinHint() __asm__ volatile("pause")
#else
  #define LcSpinHint() ((void)0)
#endif

// Long-wait spin: pure spinning collapses once runnable threads outnumber
// the (performance) cores — a spinner burns the cycles its predecessor in
// the handoff chain needs to reach its own unlock. Yielding every ~1K
// iterations is unreachable on the fast path (uncontended waits finish in
// far fewer spins) and restores progress under saturation. Ordering and
// correctness are untouched: who gets the lock next is already fixed by
// the token protocol; this only affects when the waiter's core is ceded.
#define LcSpinWait(spins) \
    do { if (((++(spins)) & 1023) == 0) sched_yield(); else LcSpinHint(); } while (0)


// =============================================================================
// SECTION 1: Wire-Indexed Counting Lock  (Design A)
//
// Per-wire slot arrays enable O(1) predicted / O(W) fallback unlock.
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
        // old heuristic 4n/W + 4 could overflow and let a registration
        // silently clobber a still-waiting round. 2n leaves margin for
        // SERVED breadcrumbs awaiting sweep cleanup.
        rounds_cap_ = std::max((size_t)16, 2 * num_threads);

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
        unsigned spins = 0;
        for (;;) {
            if (local_grant) return;
            if (*waker_flag_ && waker_lock_.trylock(thread_id)) {
                Fence();
                if (local_grant) {
                    // Granted while acquiring the waker lock: do NOT consume
                    // the flag (that would destroy a lock token that belongs
                    // to some other, still-invisible waiter).
                    waker_lock_.unlock();
                    return;
                }
                if (*waker_flag_) {
                    // Lock is free: consume the token and self-serve.
                    *waker_flag_ = false;
                    Fence();
                    // Leave a SERVED breadcrumb instead of advancing the
                    // wire head. The old code set next_round = round + 1
                    // here, which skipped any earlier, not-yet-registered
                    // rounds on this wire and made those waiters permanently
                    // invisible to unlock()'s sweep. The sweep now advances
                    // the head past SERVED rounds itself, in order.
                    slot->state = SLOT_SERVED;
                    Fence();
                    waker_lock_.unlock();
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
            if (s->state == SLOT_WAITING && s->round == r) {
                grant(pred_wire, r, s);
                return;
            }
        }

        // Prediction missed — sweep all W wires for the minimum
        // (round, wire_distance) waiter.  This handles step-property
        // violations under concurrency.
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

        if (best_slot) {
            grant(best_wire, best_round, best_slot);
            return;
        }

        // No visible waiter — set waker flag. Waiters poll this flag in
        // lock(), so an invisible waiter (registered at a round above its
        // wire head) claims the free lock through the waker lock instead
        // of being lost.
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
        std::string net = "bitonic";
        // Detect periodic by checking if NetworkT is PeriodicNetwork
        // (template trick: check build signature)
        return std::string("lw_") + net + "_" + Sync::sync_name();
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

    // Grant the lock to the waiter registered at (w, r). The slot is
    // released before the spin_addr write: the moment *spin_addr flips,
    // the waiter may return and its stack frame (which spin_addr points
    // into) dies — it must be the very last touch.
    void grant(size_t w, size_t r, WireSlot* s) {
        get_wire_head(w)->next_round = r + 1;
        volatile bool* addr = s->spin_addr;
        s->state = SLOT_EMPTY;
        Fence();
        *addr = true;
        Fence();
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


// =============================================================================
// SECTION 2: Sequenced Counting Lock  (Design B)
//
// Global fetch_add provides contiguous token ordering.
// Guaranteed O(1) unlock via slot array indexed by sequence number.
// =============================================================================

template <typename Sync, template <typename> class NetworkT>
class SequencedCountingLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        num_threads_ = num_threads;

        // Width for network: power of 2 >= ceil(sqrt(n))
        size_t target = (size_t)std::ceil(std::sqrt((double)num_threads));
        if (target < 2) target = 2;
        width_ = 1;
        while (width_ < target) width_ *= 2;

        // Slot array: power of 2, at least 4x threads for wrap safety
        num_slots_ = 1;
        while (num_slots_ < 4 * num_threads) num_slots_ *= 2;
        slot_mask_ = num_slots_ - 1;

        network_.build(width_, num_threads);

        // ── Memory layout ──
        // [waker_flag : CL] [now_serving : CL] [slot_entries : num_slots*CL]
        // [padding : CL]
        size_t pad_bytes         = CL;
        size_t now_serving_bytes = CL;
        size_t slot_bytes        = num_slots_ * CL;

        region_size_ = pad_bytes + now_serving_bytes + slot_bytes;
        region_ = (volatile char*)ALLOCATE(region_size_);
        memset((void*)region_, 0, region_size_);

        size_t off = 0;
        off += pad_bytes;  // padding / alignment
        now_serving_  = (volatile size_t*)&region_[off];  off += now_serving_bytes;
        slot_base_    = &region_[off];

        global_seq_.store(0, std::memory_order_relaxed);
        *now_serving_ = 0;

        // NO_TOKEN sentinel: token 0 is a valid sequence number, so slots
        // must be explicitly initialized to a value no thread will ever hold.
        for (size_t i = 0; i < num_slots_; i++) {
            auto* s = get_slot(i);
            s->token         = NO_TOKEN;
            s->granted_token = NO_TOKEN;
        }

        initialized_ = true;
    }

    void lock(size_t thread_id) override {
        // 1. Traverse counting network (distributes contention)
        //    The network output is NOT used for ordering — only for
        //    distributing the contention across W/2 balancers so that
        //    threads arrive at global_seq_ at staggered intervals.
        network_.traverse((int)(thread_id % width_), thread_id);

        // 2. Linearization point: global sequence number
        //    This single fetch_add produces contiguous, globally-ordered tokens.
        size_t seq = global_seq_.fetch_add(1, std::memory_order_acq_rel);

        // 3. Register in slot array for direct handoff.
        //    Single-writer safety: a slot is shared by tokens seq and
        //    seq +/- num_slots; since unserved tokens span at most
        //    num_threads (each thread holds one) and num_slots >= 4n,
        //    two live registrants can never collide on a slot.
        auto* slot = get_slot(seq & slot_mask_);
        slot->token = seq;
        Fence();

        // 4. Spin until it's our turn: either the predecessor hands off
        //    directly into our slot (granted_token == seq), or we observe
        //    the now_serving update. The grant is token-versioned rather
        //    than a pointer to our stack: an earlier implementation had the
        //    unlocker write through a saved `spin_addr` after the waiter had
        //    already exited via now_serving, re-entered lock(), and reused
        //    the same stack address for its next wait flag — a delayed
        //    grant then leaked into the *next* acquisition and let two
        //    threads into the critical section. A stale write of
        //    granted_token = seq cannot match any future occupant of this
        //    slot (their token differs by a multiple of num_slots).
        unsigned spins = 0;
        while (slot->granted_token != seq && *now_serving_ != seq) {
            LcSpinWait(spins);
        }
        Fence();
    }

    void unlock(size_t /*thread_id*/) override {
        // Advance to next token. This alone releases the lock — the
        // successor's spin loop watches now_serving_.
        size_t cur = *now_serving_;
        size_t next_token = cur + 1;
        *now_serving_ = next_token;
        Fence();

        // Try to wake successor directly — O(1) handoff onto the
        // successor's own cache line (reduces traffic on now_serving_).
        // Writing the token value (not a bool) keeps a late write harmless:
        // it can only ever match the exact registration it observed.
        auto* nse = get_slot(next_token & slot_mask_);

        static constexpr int HANDOFF_SPINS = 64;
        for (int i = 0; i < HANDOFF_SPINS; i++) {
            if (nse->token == next_token) {
                nse->granted_token = next_token;
                Fence();
                return;
            }
            LcSpinHint();
        }
        // Successor will see now_serving_ update and proceed
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
        return std::string("seq_counting_") + Sync::sync_name();
    }

private:
    static constexpr size_t CL = std::hardware_destructive_interference_size;
    static constexpr size_t NO_TOKEN = ~(size_t)0;

    struct SlotEntry {
        volatile size_t token;          // registered sequence number
        volatile size_t granted_token;  // set to `token` by the unlocker
    };

    NetworkT<Sync> network_;
    size_t num_threads_ = 0;
    size_t width_       = 0;
    size_t num_slots_   = 0;
    size_t slot_mask_   = 0;

    std::atomic<size_t> global_seq_{0};   // The linearization point

    volatile char*  region_      = nullptr;
    size_t          region_size_ = 0;
    volatile size_t* now_serving_ = nullptr;
    volatile char*  slot_base_   = nullptr;

    bool            initialized_ = false;

    SlotEntry* get_slot(size_t idx) const {
        return (SlotEntry*)&slot_base_[idx * CL];
    }
};

// Named specializations per network type

template <typename Sync>
class SeqBitonicLock
    : public SequencedCountingLock<Sync, BitonicNetwork> {
public:
    std::string name() override {
        return std::string("seq_bitonic_") + Sync::sync_name();
    }
};

template <typename Sync>
class SeqPeriodicLock
    : public SequencedCountingLock<Sync, PeriodicNetwork> {
public:
    std::string name() override {
        return std::string("seq_periodic_") + Sync::sync_name();
    }
};


// =============================================================================
// SECTION 2.5: Bounded-Overtaking Counting Lock  (Design F)
//
// Motivation: every strictly-ordered lock in this file forms a handoff
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
// Protocol (built on the Sequenced/Design-B skeleton; contiguous tokens):
//   lock():
//     1. traverse network (arrival staggering, as Design B)
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
// Correctness:
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
// Complexity:
//   Lock:   O(log²W) network + 1 fetch_add (+ re-draws when skipped)
//   Unlock: O(1) when successor ready (the common/quiescent case);
//           O(K) scan when it is not
//   Fairness: FIFO at quiescence; K-bounded overtaking per unlock under
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


// =============================================================================
// SECTION 3: Waiting-Filter Counting Lock  (Design C)
//
// From Herlihy, Shavit, Waarts (1996), Section 3: "The Waiting Network"
//
// A counting network + n-element phase-bit array that linearizes the output.
// Each token v waits for predecessor v-1 to set its phase bit (at unlock),
// then enters CS.  Unlock sets own phase bit — O(1) always.
//
// Key insight from Lynch, Shavit, Shvartsman (1996):
//   Under practical linearizability (c2 ≤ 2·c1, i.e., bounded timing
//   variation on wires), predecessors have ALREADY completed before
//   successors check, so the phase-bit wait is near-zero in practice.
//   The Waiting-filter is still needed for correctness under all timings
//   (preemption, NUMA/CXL with c2 >> c1).
//
// Complexity:
//   Lock:   O(log²W) network traverse + O(1) phase check (practical)
//           O(log²W) + O(chain) phase check (worst case, chain ≤ n)
//   Unlock: O(1) always — set one phase bit
//   Space:  O(n·CL) phase bits + O(n·CL) per-thread values
//   Extra atomics on lock path: NONE
//
// phase(v) = ⌊v/n⌋ mod 2   — toggles every n values per slot, preventing ABA
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

        initialized_ = true;
    }

    void lock(size_t thread_id) override {
        // 1. Traverse counting network → (wire, round) → global token v
        //    Token value v = round * width + wire  (contiguous at quiescence
        //    by the step property;  unique under concurrency)
        size_t lv = 0;
        int wire_i = network_.traverse((int)(thread_id % width_),
                                       thread_id, &lv);
        size_t wire  = (size_t)wire_i;
        size_t round = lv >> 1;
        size_t v = round * width_ + wire;

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
            while (*get_phase_bit(pred_slot) != expected) {
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
        *get_phase_bit(my_slot) = phase_of(v);
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
        return std::string("wf_counting_") + Sync::sync_name();
    }

private:
    static constexpr size_t CL = std::hardware_destructive_interference_size;
    static constexpr uint8_t PHASE_UNSET = 0xFF;

    NetworkT<Sync> network_;
    size_t num_threads_ = 0;
    size_t width_       = 0;

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


// =============================================================================
// SECTION 4: Skew-Filter Counting Lock  (Design D)
//
// ⚠ IMPLEMENTATION STATUS — the Skew filter here is an OVERHEAD MODEL, not
// the Herlihy-Shavit-Waarts §4.1 construction. lock() runs the filter-shaped
// traversal (one fetch_add per layer against a w×d toggle grid, row walk
// seeded from the network output), but the resulting row is DISCARDED:
// ordering is enforced entirely by an ordinary ticket pair
// (global_seq_ fetch_add + now_serving_ spin). Consequences:
//   - Correct mutual exclusion and linearizability (via the ticket, which
//     is trivially a linearization point) — verified 2026-07 at 2/4/8T.
//   - The filter contributes only cost (layer_depth_ = n-1 extra atomic
//     RMWs per acquisition), useful for measuring what an HSW-style filter
//     topology would add to the lock path.
//   - Skew and ReverseSkew are therefore behaviorally identical; their only
//     difference (row increment vs decrement) has no observable effect.
// Making the filter real — folded multi-balancers whose OUTPUT is the
// ticket (HSW §4.3, Theorem 4.12 bounded toggles) — is open work; see
// LINEARIZABLE_COUNTING_ANALYSIS.md §4b for the options.
//
// ── Original design intent (HSW §4.1, kept for reference) ──
// Architecture: counting_network → Skew-filter → per-wire counters.
// The Skew-filter (d layers) reorders tokens so the combined output is
// linearizable; d ≥ n-1 gives exit(a) < enter(b) ⟹ val(a) < val(b)
// (Theorem 4.8). Skew-layer = chain of balancers b_i with north outputs as
// layer outputs; folding (§4.3) maps the infinite filter onto a w×d grid
// of multi-balancers with bounded counters.
//
// Complexity as implemented:
//   Lock:   O(log²W) network + (n-1) filter fetch_adds + 1 ticket fetch_add
//   Unlock: O(1) — now_serving_.fetch_add
//   Space:  O((W+n)·(n-1)) toggle slots + network
// =============================================================================

template <typename Sync, template <typename> class NetworkT>
class SkewFilterCountingLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        num_threads_ = num_threads;

        // Width = smallest power of 2 >= ceil(sqrt(n)), minimum 2
        size_t target = (size_t)std::ceil(std::sqrt((double)num_threads));
        if (target < 2) target = 2;
        width_ = 1;
        while (width_ < target) width_ *= 2;

        // Filter height: tokens enter on rows 0..width-1 and can move
        // down up to n-1 rows through the filter.
        filter_rows_ = width_ + num_threads;

        // Layer depth d >= n-1 for correctness (Theorem 4.8)
        layer_depth_ = (num_threads > 1) ? (num_threads - 1) : 1;

        network_.build(width_, num_threads);

        // Allocate toggle array from the CXL-aware region allocator so it
        // lands in the same memory domain as every other lock's state
        // (plain new[] here previously put it in ordinary process heap,
        // breaking the single-region invariant under -Dcxl/-Dhardware_cxl).
        size_t toggle_count = filter_rows_ * layer_depth_;
        toggles_bytes_ = toggle_count * sizeof(std::atomic<size_t>);
        toggles_ = (std::atomic<size_t>*)ALLOCATE(toggles_bytes_);
        for (size_t i = 0; i < toggle_count; i++) {
            toggles_[i].store(0, std::memory_order_relaxed);
        }

        global_seq_.store(0, std::memory_order_relaxed);
        now_serving_.store(0, std::memory_order_relaxed);
        initialized_ = true;
    }

    void lock(size_t thread_id) override {
        // 1. Traverse counting network (distributes contention)
        size_t lv = 0;
        network_.traverse((int)(thread_id % width_), thread_id, &lv);

        // 2. Traverse the filter-shaped toggle grid. NOTE: `row` is
        //    deliberately discarded below — see the section comment. This
        //    loop models the cost of an HSW skew filter, not its ordering.
        //    Skew-layer topology (per layer):
        //      toggle even -> north (stay at row r)
        //      toggle odd  -> south (move to row r+1)
        size_t lv_wire = (size_t)(lv & 1);
        size_t lv_round = lv >> 1;
        size_t row = lv_round * width_ + lv_wire;

        for (size_t layer = 0; layer < layer_depth_; layer++) {
            size_t folded = row % filter_rows_;
            size_t idx = folded * layer_depth_ + layer;
            size_t val = toggles_[idx].fetch_add(1, std::memory_order_acq_rel);
            if (val & 1) row = row + 1;
        }

        // 3. Ticket allocation: global sequence number
        //    The counting network + filter distribute contention so
        //    threads reach this point at staggered intervals.
        size_t ticket = global_seq_.fetch_add(1, std::memory_order_relaxed);

        // 4. Spin until now_serving matches our ticket (ticket-lock)
        unsigned spins = 0;
        while (now_serving_.load(std::memory_order_acquire) != ticket) {
            LcSpinWait(spins);
        }
    }

    void unlock(size_t /*thread_id*/) override {
        now_serving_.fetch_add(1, std::memory_order_release);
    }

    void destroy() override {
        if (!initialized_) return;
        initialized_ = false;
        network_.destroy();
        FREE((void*)toggles_, toggles_bytes_);
        toggles_ = nullptr;
    }

    std::string name() override {
        return std::string("skew_counting_") + Sync::sync_name();
    }

private:
    NetworkT<Sync> network_;
    size_t num_threads_  = 0;
    size_t width_        = 0;
    size_t filter_rows_  = 0;
    size_t layer_depth_  = 0;

    std::atomic<size_t>* toggles_   = nullptr;  // filter balancers
    size_t toggles_bytes_ = 0;
    std::atomic<size_t>  global_seq_{0};         // ticket counter
    std::atomic<size_t>  now_serving_{0};        // ticket-lock serving counter

    bool initialized_ = false;
};

// Named specializations per network type

template <typename Sync>
class SkewBitonicLock
    : public SkewFilterCountingLock<Sync, BitonicNetwork> {
public:
    std::string name() override {
        return std::string("skew_bitonic_") + Sync::sync_name();
    }
};

template <typename Sync>
class SkewPeriodicLock
    : public SkewFilterCountingLock<Sync, PeriodicNetwork> {
public:
    std::string name() override {
        return std::string("skew_periodic_") + Sync::sync_name();
    }
};


// =============================================================================
// SECTION 4.5 (REMOVED 2026-07-12): Reverse-Skew-Filter Counting Lock
//
// ReverseSkewFilterCountingLock was removed: as implemented it was
// byte-for-byte behaviorally identical to SkewFilterCountingLock (the only
// difference was the sign applied to a value that both classes discard), so
// benchmarking both produced no information. If the real HSW §4.2
// reverse-skew filter (wait-free, Theorem 4.10) is ever implemented — i.e.
// the filter output actually determines ordering — it deserves a fresh
// class, not a resurrection of this one. See git history for the old code.
// =============================================================================


// =============================================================================
// SECTION 5: Concrete Type Aliases
// =============================================================================

//TODO: DOES NOT WORK
// ── Design A: Wire-Indexed (predictive O(1)/O(W) unlock) ────────────────────
using LWBitonicCASLock      = WireIndexedBitonicLock<BnCASSync>;
using LWBitonicBLLock       = WireIndexedBitonicLock<BnBLSync>;
using LWBitonicLamportLock  = WireIndexedBitonicLock<BnLamportSync>;
using LWBitonicBakeryLock   = WireIndexedBitonicLock<BnBakerySync>;

using LWPeriodicCASLock      = WireIndexedPeriodicLock<BnCASSync>;
using LWPeriodicBLLock       = WireIndexedPeriodicLock<BnBLSync>;
using LWPeriodicLamportLock  = WireIndexedPeriodicLock<BnLamportSync>;
using LWPeriodicBakeryLock   = WireIndexedPeriodicLock<BnBakerySync>;

//TODO: DOES NOT WORK
// ── Design B: Sequenced (guaranteed O(1) unlock) ────────────────────────────
using SeqBitonicCASLock      = SeqBitonicLock<BnCASSync>;

using SeqPeriodicCASLock      = SeqPeriodicLock<BnCASSync>;

// Bounded-overtaking (Design F) — CAS only: it requires a global fetch_add
// ticket, so software-sync balancer variants would be incoherent hybrids
// (the same reason the seq_*/skew_* non-CAS variants were removed).
using BoBitonicCASLock  = BoBitonicLock<BnCASSync>;
using BoPeriodicCASLock = BoPeriodicLock<BnCASSync>;

// ── Design C: Waiting-Filter (O(1) unlock, phase-bit chain) ─────────────────
using WFBitonicCASLock      = WFBitonicLock<BnCASSync>;
using WFBitonicBLLock       = WFBitonicLock<BnBLSync>;
using WFBitonicLamportLock  = WFBitonicLock<BnLamportSync>;
using WFBitonicBakeryLock   = WFBitonicLock<BnBakerySync>;


using WFPeriodicCASLock      = WFPeriodicLock<BnCASSync>;
using WFPeriodicBLLock       = WFPeriodicLock<BnBLSync>;
using WFPeriodicLamportLock  = WFPeriodicLock<BnLamportSync>;
using WFPeriodicBakeryLock   = WFPeriodicLock<BnBakerySync>;

// ── Design D: Skew-Filter (non-blocking linearizable, O(n) avg depth) ───────
using SkewBitonicCASLock      = SkewBitonicLock<BnCASSync>;

using SkewPeriodicCASLock      = SkewPeriodicLock<BnCASSync>;

// ── Design E: Reverse-Skew-Filter (wait-free linearizable, O(n) depth) ──────


#endif // LINEARIZABLE_COUNTING_LOCK_HPP
