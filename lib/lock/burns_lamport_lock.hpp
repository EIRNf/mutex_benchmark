#ifndef LOCK_BURNSLAMPORTLOCK_HPP
#define LOCK_BURNSLAMPORTLOCK_HPP

#pragma once

#include "lock.hpp"
#include "trylock.hpp"
#include "../utils/region_layout.hpp"
#include <atomic>
#include <time.h>
#include <stdexcept>
#include <string.h>

class BurnsLamportMutex : public virtual TryLock {
public:
    void init(size_t num_threads) override {
        _cxl_region_size = get_cxl_region_size(num_threads);
        _cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);
        this->region_init(num_threads, _cxl_region);
    }

    static size_t get_cxl_region_size(size_t num_threads) {
        return compute_layout(num_threads).total_size;
    }

    void region_init(size_t num_threads, volatile char *_cxl_region_in) override {
        Layout layout = compute_layout(num_threads);
        this->_cxl_region = _cxl_region_in;
        this->_cxl_region_size = layout.total_size;

        this->fast = RegionLayout::resolve(layout.fast, _cxl_region_in);
        *this->fast = false;
        this->in_contention = RegionLayout::resolve(layout.in_contention, _cxl_region_in);
        memset((void*)in_contention, 0, sizeof(bool) * num_threads);
        this->num_threads = num_threads;
    }

    bool trylock(size_t thread_id) override {
        in_contention[thread_id] = true;
        Fence();
        for (size_t higher_priority_thread = 0; higher_priority_thread < thread_id; higher_priority_thread += 1) {
            if (in_contention[higher_priority_thread]) {
                in_contention[thread_id] = false;
                return false;
            }
        }
        for (size_t lower_priority_thread = thread_id + 1; lower_priority_thread < num_threads; lower_priority_thread += 1) {
            while (in_contention[lower_priority_thread]) {
                // Busy wait for lower-priority thread to give up.
            }
        }
        // Fence between the doorway (in_contention scan) and the fast-flag
        // read-modify-write, and again before publishing our doorway exit.
        // Without these, a weakly-ordered CPU (ARM) can reorder the *fast
        // read above the final in_contention reads (two threads both see
        // fast == false and both become leader) or sink the *fast write
        // below the in_contention[thread_id] = false store. The hardened
        // sibling of this algorithm, BnWakerLock::trylock() in
        // bitonic_networks.hpp, has carried both fences all along.
        Fence();
        bool leader;
        if (!*fast) {
            *fast = true;
            leader = true;
        } else {
            leader = false;
        }
        Fence();
        in_contention[thread_id] = false;
        return leader;
    }

    void lock(size_t thread_id) override {
        while (!trylock(thread_id)) {
            // Busy wait
        }
    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used
        *fast = false;
    }

    void destroy() override {
        FREE((void*)_cxl_region, _cxl_region_size);
    }

    std::string name() override {
        return "burns_lamport";
    }

private:
    struct Layout {
        RegionLayout::Handle<bool> fast;
        RegionLayout::Handle<bool> in_contention;
        size_t total_size;
    };

    // Single source of truth for this lock's region layout -- both
    // get_cxl_region_size() and region_init() build the same layout here,
    // so the size passed to ALLOCATE/FREE can never drift from the offsets
    // actually used to place `fast`/`in_contention`.
    static Layout compute_layout(size_t num_threads) {
        RegionLayout builder;
        auto fast_handle = builder.reserve<bool>();
        auto in_contention_handle = builder.reserve_array<bool>(num_threads);
        return Layout{fast_handle, in_contention_handle, builder.total_size()};
    }

    volatile char *_cxl_region;
    size_t _cxl_region_size;
    volatile bool *fast;
    volatile bool *in_contention;
    size_t num_threads;
};

#endif // LOCK_BURNSLAMPORTLOCK_HPP
