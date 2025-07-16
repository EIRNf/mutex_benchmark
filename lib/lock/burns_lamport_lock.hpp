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
        this->in_contention = (volatile bool*)malloc(sizeof(volatile bool) * num_threads);
        memset((void*)in_contention, 0, sizeof(volatile bool) * num_threads);
        this->fast = false;
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
        if (!fast) { 
            fast = true; 
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
        fast = false;
    }

    void destroy() override {
        free((void*)in_contention);
    }

    std::string name() override {
        return "burns_lamport";
    }
    
private:
    volatile bool fast;
    volatile bool *in_contention;
    size_t num_threads;
};

#endif // LOCK_BURNSLAMPORTLOCK_HPP