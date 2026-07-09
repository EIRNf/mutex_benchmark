
#ifdef inc_umwait
#include "../utils/cxl_utils.hpp"
#include "../utils/region_layout.hpp"

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
        RegionLayout layout;
        auto handle = layout.reserve<std::atomic_uint32_t>();
        this->_region_size = layout.total_size();
        volatile char *region = (volatile char*)ALLOCATE(_region_size);
        this->lock_ = RegionLayout::resolve(handle, region);
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
        // Previously freed with sizeof(std::atomic_flag) (1 byte) -- a
        // copy-paste leftover from a sibling lock -- instead of the actual
        // allocated size. Now reuses the same size the region was
        // allocated with.
        FREE(this->lock_, _region_size);
    }

    std::string name() override {
        return "umwait";
    }
private:
    std::atomic_uint32_t *lock_;
    size_t _region_size = 0;
};
#endif // ifdef __x86_64__