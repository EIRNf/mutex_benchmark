#include "../harness.hpp"

#include "lock.hpp"
#include "trylock.hpp"

#include <atomic>
#include <time.h>
#include <stdexcept>

class SpinLock : public virtual TryLock {
public:
    void init(size_t num_threads) override {
        (void)num_threads; // This parameter is not used

        this->lock_ = (std::atomic_flag*)malloc(sizeof(std::atomic_flag));
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

    void destroy() override {}

    std::string name() override {
        return "spin";
    }
    
private:
    std::atomic_flag *lock_ = ATOMIC_FLAG_INIT;
};