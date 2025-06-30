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

        // Justine Tunney copied spinlock implementation
        if (std::atomic_exchange_explicit(&lock_, true, std::memory_order_acquire)) {
            for (;;) {
                for (;;)
                    if (!std::atomic_load_explicit(&lock_, std::memory_order_relaxed))
                    break;
                if (!atomic_exchange_explicit(&lock_, true, std::memory_order_acquire))
                    break;
            }
        }
    }
    void unlock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
        std::atomic_store_explicit(&lock_, false, std::memory_order_release);
    }
    void destroy() override {}

    std::string name(){return "spin";};
    
private:
    volatile std::atomic_bool lock_ = false;
    // volatile std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    const struct timespec nanosleep_timespec = { 0, 100 };
    struct timespec remaining;
};
