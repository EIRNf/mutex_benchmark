#include "../utils/cxl_utils.hpp"
#include "../utils/region_layout.hpp"
#include "lock.hpp"

#include <atomic>
#include <cstddef>
#include <sched.h>

// CXL-resident exclusive lock corresponding to the supplied fast-exp bridge.
// The benchmark API is blocking, so timeout handling belongs to the external
// store adapter rather than this in-process mutex.
class CXLFastExpLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads;

        RegionLayout layout;
        flag_handle_ = layout.reserve<std::atomic_flag>();
        region_size_ = layout.total_size();
        region_ = static_cast<volatile char *>(ALLOCATE(region_size_));
        if (region_ == nullptr) {
            throw std::bad_alloc();
        }

        flag_ = RegionLayout::resolve(flag_handle_, region_);
        new (flag_) std::atomic_flag{};
        flag_->clear(std::memory_order_release);
    }

    void lock(size_t thread_id) override {
        (void)thread_id;

        uint64_t failed = 0;
        while (flag_->test_and_set(std::memory_order_acquire)) {
            ++failed;
            if ((failed & 63) == 0) {
                sched_yield();
            } else {
                LockSpinHint();
            }
        }
    }

    void unlock(size_t thread_id) override {
        (void)thread_id;
        flag_->clear(std::memory_order_release);
    }

    void destroy() override {
        if (region_ != nullptr) {
            flag_->~atomic_flag();
            FREE(const_cast<char *>(region_), region_size_);
            region_ = nullptr;
            flag_ = nullptr;
            region_size_ = 0;
        }
    }

    std::string name() override {
        return "cxl_fast_exp";
    }

private:
    RegionLayout::Handle<std::atomic_flag> flag_handle_{};
    volatile char *region_ = nullptr;
    std::atomic_flag *flag_ = nullptr;
    size_t region_size_ = 0;
};
