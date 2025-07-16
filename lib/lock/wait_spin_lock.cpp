#include "lock.hpp"
#include <atomic>
#include <time.h>
#include <stdexcept>
#include <semaphore>
#include <iostream>
#include "../utils/bench_utils.hpp"

class WaitSpinLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        this->num_threads = num_threads; // This parameter is not used in this implementation
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation

        while (true) {
            if (std::atomic_flag_test_and_set_explicit(&lock_, std::memory_order_acquire)) {
                sleep();
            } else{break;}
        }
    }
    void unlock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
        std::atomic_flag_clear_explicit(&lock_, std::memory_order_release);
        wake();

    }
    void destroy() override {}
    
    std::string name() override {
        return "wait_spin";
    }
private:
    volatile std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    struct timespec remaining;
    size_t num_threads;
};