#include "lock.hpp"
#include <atomic>
#include <time.h>

class SpinLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads; // This parameter is not used in this implementation
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation

        while (true) {
            if (!std::atomic_flag_test_and_set_explicit(&lock_, std::memory_order_acquire)) {
                break;
            }
            // nanosleep(&nanosleep_timespec, &remaining);
        }
    }
    void unlock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
        std::atomic_flag_clear_explicit(&lock_, std::memory_order_release);
    }
    void destroy() override {}

    std::string name(){return "spin";};
    
private:
    volatile std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    // const struct timespec nanosleep_timespec = { 1, 0 };
    // struct timespec remaining;
};
