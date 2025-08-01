#ifndef LOCK_SPINLOCK_HPP
#define LOCK_SPINLOCK_HPP

#pragma once

#include "../utils/cxl_utils.hpp"
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

            this->lock_ = (std::atomic_flag*)ALLOCATE(sizeof(std::atomic_flag));
        }

        static size_t get_cxl_region_size(size_t num_threads) {
            (void)num_threads;

            return sizeof(std::atomic_flag);
        }

        void region_init(size_t num_threads, volatile char *_cxl_region) override {
            (void)num_threads; // This parameter is not used

            this->lock_ = (std::atomic_flag*)_cxl_region;
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
            FREE((void*)this->lock_, 1);
        }

        std::string name() override {
            return "spin";
        }
        
    private:
        std::atomic_flag *lock_ = ATOMIC_FLAG_INIT;
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