#ifndef NET_ELEVATOR_LOCK_HPP
#define NET_ELEVATOR_LOCK_HPP

#pragma once

#include "lock.hpp"
#include "../utils/cxl_utils.hpp"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <new>
#include <atomic>


// =============================================================================
// NetElevator Lock
//
// An elevator-style lock that uses a bitonic counting network for floor
// assignment, with atomic-toggle balancers.
//
// Entry protocol uses the designated-waker pattern (same as linear/tree
// elevators in this repo): a BurnsLamport trylock determines the waker
// thread, which spins on both its own flag and a special designated-waker
// flag.  Non-waker threads spin only on their own per-thread grant flag.
//
// Design:
//   1. Counting Network Balancer: Each balancer atomically toggles a bit
//      to route threads to alternating output wires. The bitonic network's
//      step property ensures near-uniform distribution across W output
//      wires (floors).
//   2. Elevator sweep unlock: Floors are serviced in sweep order
//      (UP then DOWN). Within each floor, the thread with the smallest
//      (ticket, tid) wins.
//   3. Grant flags: Per-thread cache-line-padded flag + designated-waker
//      flag at slot [num_threads]. Releasing thread writes to winner's
//      grant flag; if no one is waiting, signals the designated-waker flag.
//
// Properties:
//   - Mutual exclusion: guaranteed by single-writer grant flag + waker lock
//   - Deadlock-free: elevator always advances, CS is finite
//   - Starvation-free: bounded overtaking O(W²) per elevator cycle
// =============================================================================

class NetElevatorMutex : public virtual SoftwareMutex {
public:

    // =========================================================================
    // Counting Network Balancer: atomic toggle
    //
    // Each balancer has a single toggle bit. Arriving threads atomically
    // read-and-flip via fetch_xor, getting alternating 0/1 output values.
    // This is linearizable and handles any number of concurrent threads.
    // =========================================================================
    struct Balancer {
        std::atomic<int> state{0};

        void init() {
            state.store(0, std::memory_order_relaxed);
        }

        // Toggle and return old value (0 or 1).
        int traverse() {
            return state.fetch_xor(1, std::memory_order_relaxed) & 1;
        }
    };

    // =========================================================================
    // Designated-waker lock (Burns-Lamport style, embedded)
    //
    // This is a simplified inline BurnsLamport trylock. We embed it to keep
    // all state in one contiguous allocation and avoid TryLock virtual calls.
    // =========================================================================
    struct WakerLock {
        volatile bool* in_contention;  // [num_threads]
        volatile bool  fast;
        size_t num_threads;

        void init(volatile bool* mem, size_t n) {
            in_contention = mem;
            for (size_t i = 0; i < n; i++) in_contention[i] = false;
            fast = false;
            num_threads = n;
        }

        bool trylock(size_t tid) {
            in_contention[tid] = true;
            Fence();
            // Higher-priority (lower id) threads preempt us
            for (size_t h = 0; h < tid; h++) {
                if (in_contention[h]) {
                    in_contention[tid] = false;
                    return false;
                }
            }
            // Wait for lower-priority threads to drop out
            for (size_t l = tid + 1; l < num_threads; l++) {
                while (in_contention[l]) { /* busy wait */ }
            }
            // Ensure all in_contention reads complete before reading fast.
            // Without this, ARM can speculatively load fast before the spin
            // loops finish, seeing a stale value.
            Fence();
            bool leader;
            if (!fast) {
                fast = true;
                leader = true;
            } else {
                leader = false;
            }
            // Ensure fast write is visible before clearing in_contention.
            // Without this, ARM can reorder fast=true after in_contention=false,
            // allowing another thread to exit its spin, read stale fast=false,
            // and also claim leader=true.
            Fence();
            in_contention[tid] = false;
            return leader;
        }

        void unlock() {
            fast = false;
        }
    };

    // =========================================================================
    // Bitonic network stage info (pre-computed)
    // =========================================================================
    struct StageInfo {
        size_t stride;
    };

private:
    size_t num_threads_;
    size_t width_;         // W: number of output wires / floors (power of 2)
    size_t log_width_;     // log₂(W)
    size_t num_stages_;    // Total stages in the bitonic network

    Balancer*     balancers_;     // [num_stages_ * width_/2]
    StageInfo*    stage_infos_;   // [num_stages_]
    WakerLock     waker_lock_;    // Designated-waker trylock

    // Per-thread grant flags, cache-line padded.
    // Slot [num_threads_] is the designated-waker flag.
    // Layout: flags_base_ points to raw memory; get_flag(i) computes address.
    volatile char* flags_base_;

    // Per-thread state (NOT cache-line padded — written only by owner)
    struct ThreadMeta {
        volatile bool   waiting;
        volatile size_t my_floor;
        volatile size_t my_ticket;
    };
    ThreadMeta* thread_meta_;

    // Elevator state (written only by lock holder during unlock)
    size_t current_floor_;
    int    direction_;  // +1 UP, -1 DOWN

    // Per-thread monotonic ticket counters
    size_t* ticket_counters_;

    // Memory region
    volatile char* cxl_region_;
    size_t cxl_region_size_;

    // =========================================================================
    // Helpers
    // =========================================================================

    static constexpr size_t CL = std::hardware_destructive_interference_size;

    inline volatile bool* get_flag(size_t id) {
        return (volatile bool*)&flags_base_[id * CL];
    }

    // ---- Network topology ----

    void precompute_stages() {
        size_t s = 0;
        for (size_t p = 0; p < log_width_; p++) {
            for (size_t j = 0; j <= p; j++) {
                stage_infos_[s].stride = (size_t)1 << (p - j);
                s++;
            }
        }
    }

    inline size_t get_balancer_index(size_t wire, size_t stage_idx) const {
        size_t stride = stage_infos_[stage_idx].stride;
        size_t partner = wire ^ stride;
        size_t lo = (wire < partner) ? wire : partner;
        return lo / (2 * stride) * stride + (lo % stride);
    }

    inline Balancer& get_balancer(size_t stage, size_t bal_idx) {
        return balancers_[stage * (width_ / 2) + bal_idx];
    }

    size_t traverse_network(size_t input_wire) {
        size_t wire = input_wire;
        for (size_t s = 0; s < num_stages_; s++) {
            size_t bi = get_balancer_index(wire, s);
            int out = get_balancer(s, bi).traverse();
            size_t stride = stage_infos_[s].stride;
            if (out == 0) wire = wire & ~stride;
            else          wire = wire |  stride;
        }
        return wire;
    }

    // ---- Elevator sweep ----

    // Find the waiting thread on `floor` with smallest (ticket, tid).
    // Returns thread id, or num_threads_ if none.
    size_t find_floor_winner(size_t floor) {
        size_t best_tid = num_threads_;
        size_t best_ticket = ~(size_t)0;
        for (size_t i = 0; i < num_threads_; i++) {
            if (!thread_meta_[i].waiting || thread_meta_[i].my_floor != floor)
                continue;
            size_t t = thread_meta_[i].my_ticket;
            if (t < best_ticket || (t == best_ticket && i < best_tid)) {
                best_ticket = t;
                best_tid = i;
            }
        }
        return best_tid;
    }

    // Grant the lock to the next waiting thread using elevator sweep.
    // Called by the current lock holder during unlock().
    // If no one is waiting, signals the designated-waker flag.
    void grant_next() {
        // Try current floor first
        size_t winner = find_floor_winner(current_floor_);
        if (winner < num_threads_) {
            *get_flag(winner) = true;
            Fence();
            return;
        }

        // Sweep in current direction, then reverse once if needed
        for (int pass = 0; pass < 2; pass++) {
            size_t limit = width_;
            for (size_t step = 1; step < limit; step++) {
                size_t floor;
                if (direction_ > 0) {
                    floor = current_floor_ + step;
                    if (floor >= width_) break; // past top
                } else {
                    if (step > current_floor_) break; // past bottom
                    floor = current_floor_ - step;
                }

                winner = find_floor_winner(floor);
                if (winner < num_threads_) {
                    current_floor_ = floor;
                    *get_flag(winner) = true;
                    Fence();
                    return;
                }
            }
            // Reverse direction for second pass
            direction_ = -direction_;
        }

        // No one waiting — signal designated-waker flag so the next
        // thread that wins the waker lock can enter immediately.
        *get_flag(num_threads_) = true;
        Fence();
    }

public:
    ~NetElevatorMutex() override {
        destroy();
    }

    void init(size_t num_threads) override {
        num_threads_ = num_threads;

        // Network width W = smallest power of 2 >= max(2, ceil(sqrt(n)))
        size_t target = (size_t)std::ceil(std::sqrt((double)num_threads));
        if (target < 2) target = 2;
        width_ = 1;
        log_width_ = 0;
        while (width_ < target) {
            width_ *= 2;
            log_width_++;
        }

        num_stages_ = log_width_ * (log_width_ + 1) / 2;
        size_t half_w = width_ / 2;

        // --- Compute memory layout ---
        size_t balancer_bytes  = sizeof(Balancer) * num_stages_ * half_w;
        size_t stage_bytes     = sizeof(StageInfo)    * num_stages_;
        size_t flags_bytes     = CL * (num_threads + 1); // +1 for designated waker
        size_t meta_bytes      = sizeof(ThreadMeta)   * num_threads;
        size_t ticket_bytes    = sizeof(size_t)        * num_threads;
        size_t waker_bytes     = sizeof(volatile bool) * num_threads; // in_contention array

        cxl_region_size_ = balancer_bytes + stage_bytes + flags_bytes
                         + meta_bytes + ticket_bytes + waker_bytes + 256;
        cxl_region_ = (volatile char*)ALLOCATE(cxl_region_size_);
        memset((void*)cxl_region_, 0, cxl_region_size_);

        size_t off = 0;

        // Flags first (cache-line aligned at region start)
        flags_base_ = &cxl_region_[off];
        off += flags_bytes;

        balancers_ = (Balancer*)&cxl_region_[off];
        off += balancer_bytes;

        stage_infos_ = (StageInfo*)&cxl_region_[off];
        off += stage_bytes;

        thread_meta_ = (ThreadMeta*)&cxl_region_[off];
        off += meta_bytes;

        ticket_counters_ = (size_t*)&cxl_region_[off];
        off += ticket_bytes;

        volatile bool* waker_mem = (volatile bool*)&cxl_region_[off];

        // --- Initialize ---
        // Use placement new for Balancer because it contains std::atomic
        for (size_t i = 0; i < num_stages_ * half_w; i++)
            new (&balancers_[i]) Balancer();

        precompute_stages();

        for (size_t i = 0; i < num_threads; i++) {
            *get_flag(i) = false;
            thread_meta_[i].waiting = false;
            thread_meta_[i].my_floor = 0;
            thread_meta_[i].my_ticket = 0;
            ticket_counters_[i] = 0;
        }
        // Designated-waker flag starts TRUE (lock is initially free, so the
        // first thread to win the waker lock enters immediately)
        *get_flag(num_threads) = true;

        current_floor_ = 0;
        direction_ = 1;

        waker_lock_.init(waker_mem, num_threads);
    }

    void lock(size_t thread_id) override {
        // Step 1: Traverse counting network → floor assignment
        size_t input_wire = thread_id % width_;
        size_t floor = traverse_network(input_wire);

        // Step 2: Monotonically increasing per-thread ticket
        size_t ticket = ticket_counters_[thread_id]++;

        // Step 3: Register as waiting
        thread_meta_[thread_id].my_floor = floor;
        thread_meta_[thread_id].my_ticket = ticket;
        Fence();
        thread_meta_[thread_id].waiting = true;
        Fence();

        // Step 4: Designated-waker protocol (matches linear/tree elevators)
        volatile bool* my_flag    = get_flag(thread_id);
        volatile bool* waker_flag = get_flag(num_threads_);

        if (waker_lock_.trylock(thread_id)) {
            Fence();
            // We are the designated waker.  Spin on EITHER our own flag
            // (meaning the unlocker granted us) OR the waker flag (meaning
            // the lock was free / the unlocker found nobody waiting).
            unsigned spins = 0;
            while (*my_flag == false && *waker_flag == false) {
                LockSpinWait(spins);
            }
            // Consume the waker flag.
            // CRITICAL: Fence between clearing waker_flag and unlocking the
            // waker lock. Without this, ARM can reorder the store of
            // waker_flag=false AFTER the store of fast=false (inside
            // unlock), allowing another thread to win the waker trylock
            // and see the stale waker_flag==true.
            *waker_flag = false;
            Fence();
            waker_lock_.unlock();
        } else {
            // Not the waker — just spin on our own grant flag
            unsigned spins = 0;
            while (*my_flag == false) {
                LockSpinWait(spins);
            }
        }
        // Consume our grant flag
        *my_flag = false;
    }

    void unlock(size_t thread_id) override {
        // Mark ourselves as no longer waiting
        thread_meta_[thread_id].waiting = false;
        Fence();

        // Grant the next thread using elevator sweep
        grant_next();
    }

    void destroy() override {
        if (cxl_region_) {
            FREE((void*)cxl_region_, cxl_region_size_);
            cxl_region_ = nullptr;
        }
    }

    std::string name() override {
        return "net_elevator";
    }
};

#endif // NET_ELEVATOR_LOCK_HPP
