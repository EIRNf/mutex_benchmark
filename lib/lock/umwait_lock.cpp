
#ifdef inc_umwait
#include "../utils/cxl_utils.hpp"

#include "lock.hpp"
#include <atomic>
#include <time.h>
#include <stdexcept>
#include <stdio.h>
#include <assert.h>
#include <mwaitxintrin.h>
#include <cpuid.h>

#define POWER_SAVING 0
#define FAST_WAKEUP 1

#define UNLOCKED 0
#define LOCKED 1

class UMWaitLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        // assert(__builtin_cpu_supports("umwait"));
        (void)num_threads; // This parameter is not used in this implementation
        this->lock_ = (std::atomic_uint32_t*)ALLOCATE(sizeof(std::atomic_uint32_t));
        this->lock_->store(UNLOCKED);
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation

        uint32_t expected;
        while (!lock_->compare_exchange_strong(expected = UNLOCKED, LOCKED, std::memory_order_acquire)) {
            _umonitor(lock_);
            if (*lock_ == LOCKED) {
                _umwait(FAST_WAKEUP, 0);
            }
        }
    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation

        lock_->store(UNLOCKED, std::memory_order_release);
    }

    void destroy() override {
        FREE(this->lock_, sizeof(std::atomic_flag));
    }
    
    std::string name() override {
        return "umwait";
    }
private:
    std::atomic_uint32_t *lock_;
};
#endif // ifdef __x86_64__