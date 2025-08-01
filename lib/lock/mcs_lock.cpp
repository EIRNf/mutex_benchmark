#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>

// TODO: explicit memory ordering.
// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.

struct QNode {
    std::atomic<struct QNode volatile*> next;
    std::atomic_bool locked;
};

class MCSMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads; // Unused

        this->tail = nullptr;
        this->local_nodes = (volatile QNode*)malloc(sizeof(QNode) * num_threads);
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // Unused

        // Initialize thread_local node
        local_nodes[thread_id].next = nullptr;
        // Load old tail node of queue while also adding ourself to the queue
        volatile QNode *old_tail = tail.exchange(&local_nodes[thread_id], std::memory_order_acquire);
        if (old_tail == nullptr) {
            // We're the only one in the queue; we successfully acquired the lock.
            return;
        } else {
            // Edit the tail to add ourself in.
            local_nodes[thread_id].locked = true;
            old_tail->next = &local_nodes[thread_id];
            while (local_nodes[thread_id].locked) {
                // spin_delay_exponential(); // Busy wait
            }
        }

    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // Unused
        if (local_nodes[thread_id].next == nullptr) {
            volatile QNode *expected = &local_nodes[thread_id];
            if (tail.compare_exchange_strong(expected, nullptr)) {
                return;
            }
        }

        while (local_nodes[thread_id].next == nullptr) {
            // spin_delay_exponential(); // Busy wait
        }

        local_nodes[thread_id].next.load()->locked.store(false);
    }

    void destroy() override {

    }

    std::string name() override {
        return "mcs";
    }
private:
    volatile struct std::atomic<struct QNode volatile*> tail;
    volatile QNode* local_nodes;
};
