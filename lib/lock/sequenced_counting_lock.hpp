#ifndef SEQUENCED_COUNTING_LOCK_HPP
#define SEQUENCED_COUNTING_LOCK_HPP

#pragma once

#include "lock.hpp"
#include "bitonic_networks.hpp"
#include "counting_lock_common.hpp"
#include "../utils/cxl_utils.hpp"
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>

// =============================================================================
// DESIGN B — Sequenced Counting Lock              (seq_bitonic_*, seq_periodic_*)
//
// NETWORK-ORDERED (since 2026-07-14): the token is the counting network's
// output value (per-wire round * W + wire); there is no shared ticket
// counter. What distinguishes B within the network-ordered designs is its
// service machinery: a token-versioned slot array giving the unlocker an
// O(1) direct handoff onto the successor's own cache line, with the shared
// now_serving_ counter as fallback.
//
// History: from 2026-07 (commit 1d497da) to 2026-07-14 the token was a
// global fetch_add ("sequenced" — the class name is historical), making
// this a ticket lock with a decorative network. The ticket had been
// compensating for handoff fence bugs, not for anything fundamental; the
// network-derived form passes the full oracle battery (see the family's
// COUNTING_LOCKS.md §4b for the argument and validation).
//
// Why the protocol survives non-dense tokens:
//   * Completion is a prefix: a waiter enters only on granted_token == seq
//     or now_serving_ == seq, and both are produced by the unlock of
//     seq - 1, so values complete strictly in order.
//   * The live-value window (emitted-not-done values plus un-emitted holes,
//     each owned by a distinct live thread — holes by the in-flight
//     traversals that will fill them) never exceeds n, so the >= 4n slot
//     array cannot collide and now_serving_'s successor always exists.
//   * The direct-handoff grace spin simply times out more often than under
//     tickets (the successor may still be mid-traversal rather than
//     instructions away); the now_serving_ fallback covers it.
//
// Fairness: n-bounded overtaking in real-time order (quiescently
// consistent), like every network-ordered design; the ticket era was
// strict arrival-FIFO.
//
// Grant safety: grants are token-versioned (granted_token = seq), never a
// pointer to the waiter's stack. An earlier implementation wrote through a
// saved spin_addr after the waiter had already exited via now_serving_,
// re-entered lock(), and reused the same stack address — a delayed grant
// then leaked into the NEXT acquisition and let two threads into the
// critical section. A stale granted_token write cannot match any future
// occupant of the slot (tokens sharing a slot differ by a multiple of
// num_slots >= 4n, while live tokens span at most n).
//
// Memory ordering (2026-07-14): the release fence sits BEFORE the
// now_serving_ store — with it after (as originally written), this holder's
// critical-section writes could reach the successor late. Latent here
// (never observed failing, unlike the same bug class in Design C) because
// the direct-handoff path was already fenced; fixed for uniformity and
// because fence-free callers (LD_PRELOAD injection) exercise it.
//
// Complexity
//   lock:   O(log^2 W) traversal; NO extra atomic RMW beyond the balancers
//   unlock: O(1) — one bounded handoff spin (HANDOFF_SPINS = 64), then the
//           now_serving_ store has already released the lock either way
//   space:  O(num_slots * CL), num_slots = smallest power of two >= 4n
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
        // 1. Traverse the counting network; the token IS the network's
        //    output value (per-wire round * W + wire). Since 2026-07-14
        //    there is no shared ticket counter — ordering comes from the
        //    network (see the header's live-window argument for why seq-1
        //    always exists and why the slot bound below still holds).
        size_t lv = 0;
        int wire_i = network_.traverse((int)(thread_id % width_), thread_id, &lv);
        size_t seq = (lv >> 1) * width_ + (size_t)wire_i;

        // 2. Register in slot array for direct handoff.
        //    Single-writer safety: a slot is shared by tokens seq and
        //    seq +/- num_slots. Live values (emitted-not-done plus holes
        //    owned by in-flight traversals) span at most num_threads, and
        //    num_slots >= 4n, so two live registrants can never collide on
        //    a slot.
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
        // Release: the fence must come BEFORE the now_serving_ store so this
        // holder's critical-section writes are visible when the successor's
        // spin sees the new value. (Same wrong-side-fence class of bug that
        // made the WaitingFilter design lose counter updates; here it was
        // latent because the direct-handoff path below was already fenced.)
        Fence();
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


#endif // SEQUENCED_COUNTING_LOCK_HPP
