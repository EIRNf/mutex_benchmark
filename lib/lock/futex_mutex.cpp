#ifdef inc_futex
#include "lock.hpp"
#include <atomic>
#include <time.h>
#include <stdexcept>
#include <atomic>
#include <syscall.h>
#include <linux/futex.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>

#define UNLOCKED 0
#define LOCKED 1
#define LOCKED_POSSIBLY_WITH_WAITERS 2

// Linux-specific
class FutexLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads; // This parameter is not used

        state = 0;
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used

        // Try to lock immediately
        // TODO: isn't this going to deadlock if this code successfully runs when there are waiters waiting and then this thread unlocks and
        // never wakes anyone up? What prevents this other than the 10ms timeout?
        // std::atomic_compare_exchange_strong(state, UNLOCKED, LOCKED);
        uint32_t expected = UNLOCKED;
        if (state.compare_exchange_strong(expected, LOCKED)) {
            return; // Successfully locked.
        }

        // Wait with futex syscalls.
        do {
            expected = LOCKED;
            if (state == LOCKED_POSSIBLY_WITH_WAITERS || state.compare_exchange_weak(expected, LOCKED_POSSIBLY_WITH_WAITERS, std::memory_order_acquire)) {
                // TODO: use a timeout here?
                long result = syscall(SYS_futex, (uint32_t*)&state, FUTEX_WAIT, LOCKED_POSSIBLY_WITH_WAITERS, NULL, 0);
                if (result == -1 && errno != EAGAIN) {
                    fprintf(stderr, "Futex syscall failed with errno=%d\n", errno);
                    exit(1);
                }
            }
            expected = UNLOCKED;
        } while (!state.compare_exchange_weak(expected, LOCKED_POSSIBLY_WITH_WAITERS, std::memory_order_acquire));
    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used

        uint32_t old_state = state.fetch_sub(1);
        if (old_state != LOCKED) {
            // There are waiters.
            state = UNLOCKED;
            syscall(SYS_futex, (uint32_t*)&state, FUTEX_WAKE, 1, 0, 0);
        }
    }

    void destroy() override {}

    std::string name() override {
        return "futex";
    }
    
private:
    std::atomic<uint32_t> state;
    struct timespec timeout = { 0, 10000000 };
};
#endif