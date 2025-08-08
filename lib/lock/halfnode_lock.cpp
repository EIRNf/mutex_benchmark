// Deprecated

#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>
#include <time.h>
#include <cxl_utils.hpp>

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

        this->states = (HNode*)ALLOCATE(sizeof(HNode)*num_threads);
        for (int i =0; i<num_threads; i++){
            states[i].successor_must_wait=false;
            states[i].successor_waiting=true;
        }
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // Unused

        this->states[thread_id].successor_must_wait = true;
        struct HNode *predecessor = tail.exchange(&this->states[thread_id]);

        if (predecessor != nullptr) {
            while (predecessor->successor_must_wait) {
                // spin_delay_exponential(); // Wait until the predecessor signals that it is done.
            }
            predecessor->successor_waiting = false;
        }
    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // Unused

        if (tail == &this->states[thread_id]) {
            struct HNode *expected = &this->states[thread_id];
            if (tail.compare_exchange_strong(expected, nullptr, std::memory_order_release)) {
                return;
            }
        }
        // Signal to successor that this thread is done.
        this->states[thread_id].successor_waiting = true;
        this->states[thread_id].successor_must_wait = false;

        while (this->states[thread_id].successor_waiting) {
            // spin_delay_exponential(); // Wait until the successor reads the unlocked state to proceed.
        }
    }

    void destroy() override {}

    std::string name() override {
        return "halfnode";
    }
private:
    std::atomic<struct HNode*> tail;
    HNode* states;
};
