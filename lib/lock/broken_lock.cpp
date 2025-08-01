#include "../utils/cxl_utils.hpp"

#include "lock.hpp"
#include <atomic>
#include <time.h>
#include <stdexcept>

class BrokenLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads; // This parameter is not used in this implementation
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation

        while (lock_.test_and_set(std::memory_order_acquire)) {
            struct timespec sleep_time = { 1, 0 }, remaining;
            nanosleep(&sleep_time, &remaining);
        }
    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation

        lock_.clear(std::memory_order_release);
    }

    void destroy() override {

    }
    
    std::string name() override {
        return "broken";
    }
private:
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};