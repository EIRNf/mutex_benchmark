#ifndef LOCK_BURNSLAMPORTLOCK_HPP
#define LOCK_BURNSLAMPORTLOCK_HPP

#pragma once

#include "lock.hpp"
#include "trylock.hpp"
#include <atomic>
#include <time.h>
#include <stdexcept>
#include <string.h>

class BurnsLamportMutex : public virtual TryLock {
public:
    void init(size_t num_threads) override {
        size_t _cxl_region_size = get_cxl_region_size(num_threads);
        _cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);
        this->region_init(num_threads, _cxl_region);
    }

    static size_t get_cxl_region_size(size_t num_threads) {
        return sizeof(size_t) + sizeof(volatile bool) * (num_threads + 1);
    }

    void region_init(size_t num_threads, volatile char *_cxl_region) override {
        this->fast = (volatile bool*)&_cxl_region[0];
        *this->fast = false;
        this->in_contention = (volatile bool*)&_cxl_region[sizeof(volatile bool)];
        memset((void*)in_contention, 0, sizeof(volatile bool) * num_threads);
        this->num_threads = num_threads;
    }

    bool trylock(size_t thread_id) override {
        in_contention[thread_id] = true;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        for (size_t higher_priority_thread = 0; higher_priority_thread < thread_id; higher_priority_thread++) {
            if (in_contention[higher_priority_thread]) {
                in_contention[thread_id] = false;
                return false;
            }
        }
        for (size_t lower_priority_thread = thread_id + 1; lower_priority_thread < num_threads; lower_priority_thread++) {
            while (in_contention[lower_priority_thread]) {
                // Busy wait for lower-priority thread to give up.
            }
        }
        bool leader;
        if (!*fast) { 
            *fast = true; 
            leader = true; 
        } else {
            leader = false;
        }
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
        FREE((void*)_cxl_region, get_cxl_region_size(num_threads));
    }

    std::string name() override {
        return "burns_lamport";
    }
    
private:
    volatile char *_cxl_region;
    volatile bool *fast;
    volatile bool *in_contention;
    size_t num_threads;
};

#endif // LOCK_BURNSLAMPORTLOCK_HPP