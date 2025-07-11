#ifndef LOCK_LOCK_HPP
#define LOCK_LOCK_HPP

#pragma once

#include <atomic>
#include <thread>
#include <vector>

class SoftwareMutex {
public:
    SoftwareMutex() = default;
    virtual ~SoftwareMutex() = default;

    // Initialize the mutex for a given number of threads
    virtual void init(size_t num_threads) = 0;

    // Lock the mutex for the calling thread (thread_id)
    virtual void lock(size_t thread_id) = 0;

    // Unlock the mutex for the calling thread (thread_id)
    virtual void unlock(size_t thread_id) = 0;

    // Cleanup any resources used by the mutex
    virtual void destroy() = 0;

    virtual std::string name() =0;

    inline void spin_delay_exponential() {
        // Same as nsync
        static size_t attempts = 0;
        if (attempts < 7) {
            volatile int i;
            for (i = 0; i != 1 << attempts; i++);
        } else {
            std::this_thread::yield();
        }
        attempts++;
    }

    inline void spin_delay_linear() {
        static size_t delay = 5;
        volatile size_t i;
        for (i = 0; i != delay; i++);
        delay += 5;
    }

    inline void spin_delay_exponential_nanosleep() {
        static struct timespec nanosleep_timespec = { 0, 10 }, remaining;
        nanosleep(&nanosleep_timespec, &remaining);
        nanosleep_timespec.tv_nsec *= 2;
    }
};

#endif // LOCK_LOCK_HPP