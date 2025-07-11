#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>
#include <time.h>

// TODO: explicit memory ordering.
// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.

struct HNode {
    // Nonatomic reads and writes should be acceptable.
    // If a read and a write collide, it can only be while changing a false to a true (true->falses aren't being read)
    // So either:
    // A. Random value is read that is not equal to zero, which is the same as reading a true, which works.
    // B. False is read, which means the thread waits another iteration and tries again, and it works next time.
    volatile bool successor_must_wait;
    volatile bool successor_waiting;
};

class HalfnodeMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads; // Unused

        this->tail = nullptr;
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // Unused

        this->state.successor_must_wait = true;
        struct HNode *predecessor = tail.exchange(&this->state);

        if (predecessor != nullptr) {
            while (predecessor->successor_must_wait) {
                // spin_delay_exponential(); // Wait until the predecessor signals that it is done.
            }
            predecessor->successor_waiting = false;
        }
    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // Unused

        if (tail == &this->state) {
            struct HNode *expected = &this->state;
            if (tail.compare_exchange_strong(expected, nullptr, std::memory_order_release)) {
                return;
            }
        }
        // Signal to successor that this thread is done.
        this->state.successor_waiting = true;
        this->state.successor_must_wait = false;

        while (this->state.successor_waiting) {
            // spin_delay_exponential(); // Wait until the successor reads the unlocked state to proceed.
        }
    }

    void destroy() override {}

    std::string name() override {
        return "halfnode";
    }
private:
    static std::atomic<struct HNode*> tail;
    static thread_local struct HNode state;
};
std::atomic<struct HNode*> HalfnodeMutex::tail;
thread_local struct HNode HalfnodeMutex::state = { false, true };
