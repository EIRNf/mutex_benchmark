#ifndef LOCK_SPINLOCK_HPP
#define LOCK_SPINLOCK_HPP

#pragma once

#include "../utils/cxl_utils.hpp"
#include "../utils/region_layout.hpp"
#include "lock.hpp"
#include "trylock.hpp"
#include <atomic>
#include <time.h>
#include <stdexcept>

#ifdef cxl
    class SpinLock : public virtual TryLock {
    public:
        void init(size_t num_threads) override {
            (void)num_threads; // This parameter is not used

            this->_cxl_region_size = get_cxl_region_size(num_threads);
            volatile char *region = (volatile char*)ALLOCATE(_cxl_region_size);
            this->region_init(num_threads, region);
        }

        static size_t get_cxl_region_size(size_t num_threads) {
            (void)num_threads;

            RegionLayout layout;
            layout.reserve<std::atomic_flag>();
            return layout.total_size();
        }

        void region_init(size_t num_threads, volatile char *_cxl_region) override {
            (void)num_threads; // This parameter is not used

            RegionLayout layout;
            auto handle = layout.reserve<std::atomic_flag>();
            this->_cxl_region_size = layout.total_size();
            this->lock_ = RegionLayout::resolve(handle, _cxl_region);
        }

        void lock(size_t thread_id) override {
            (void)thread_id; // This parameter is not used

            while (lock_->test_and_set(std::memory_order_acquire)) {
                // Busy wait
                spin_delay_sched_yield();
            }
        }

        bool trylock(size_t thread_id) override {
            (void)thread_id;

            return !lock_->test_and_set(std::memory_order_acquire);
        }

        void unlock(size_t thread_id) override {
            (void)thread_id; // This parameter is not used

            lock_->clear(std::memory_order_release);
        }

        void destroy() override {
            // Previously hardcoded as FREE(lock_, 1) -- a magic number that
            // happened to match sizeof(std::atomic_flag) on this platform.
            // Now reuses the same size the region was allocated with.
            FREE((void*)this->lock_, _cxl_region_size);
        }

        std::string name() override {
            return "spin";
        }

    private:
        // A pointer, so it must be null-initialized -- ATOMIC_FLAG_INIT
        // ({false}) is for initializing an std::atomic_flag object, not a
        // pointer to one; using it here previously made this branch fail to
        // compile at all under -Dcxl.
        std::atomic_flag *lock_ = nullptr;
        size_t _cxl_region_size = 0;
    };
#else
    class SpinLock : public virtual TryLock {
    public:
        void init(size_t num_threads) override {
            (void)num_threads; // This parameter is not used
        }

        static size_t get_cxl_region_size(size_t num_threads) {
            (void)num_threads;

            return 0;
        }

        void region_init(size_t num_threads, volatile char *_cxl_region) override {
            (void)_cxl_region;
            (void)num_threads; // This parameter is not used
        }

        void lock(size_t thread_id) override {
            (void)thread_id; // This parameter is not used

            while (lock_.test_and_set(std::memory_order_acquire)) {
                // Busy wait
                spin_delay_sched_yield();
            }
        }

        bool trylock(size_t thread_id) override {
            (void)thread_id;

            return !lock_.test_and_set(std::memory_order_acquire);
        }

        void unlock(size_t thread_id) override {
            (void)thread_id; // This parameter is not used

            lock_.clear(std::memory_order_release);
        }

        void destroy() override {}

        std::string name() override {
            return "spin";
        }
        
    private:
        volatile std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    };
#endif // cxl

#endif // LOCK_SPINLOCK_HPP