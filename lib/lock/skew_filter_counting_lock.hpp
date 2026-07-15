#ifndef SKEW_FILTER_COUNTING_LOCK_HPP
#define SKEW_FILTER_COUNTING_LOCK_HPP

#pragma once

#include "lock.hpp"
#include "bitonic_networks.hpp"
#include "counting_lock_common.hpp"
#include "../utils/cxl_utils.hpp"
#include <atomic>
#include <cmath>
#include <string>


// =============================================================================
// DESIGN D — Skew-Filter Counting Lock            (skew_bitonic_*, skew_periodic_*)
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
//     difference (row increment vs decrement) has no observable effect —
//     which is why ReverseSkew was removed (2026-07-12, see git history).
// Making the filter real — folded multi-balancers whose OUTPUT is the
// ticket (HSW §4.3, Theorem 4.12 bounded toggles) — is open work; see
// LINEARIZABLE_COUNTING_ANALYSIS.md §4b for the options.
//
// vs the rest of the family: this is the plainest ticket lock of the set
// (unlock is a bare fetch_add release); everything above the ticket is
// deliberate, measurable overhead.
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


#endif // SKEW_FILTER_COUNTING_LOCK_HPP
