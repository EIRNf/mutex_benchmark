#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>

// TODO: explicit memory ordering.
// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.

struct QNode {
    std::atomic<QNode*> next;
    std::atomic_bool locked;
};

class MCSMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads; // Unused
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // Unused

        my_node.next.store(nullptr);
        // my_node.locked may remain uninitialized. 
        // This is fine because if we have the lock we will never
        // access my_node.locked.
        QNode* predecessor = lock_.exchange(&my_node);
        // Once the lock._next is swapped, won't be modified
        if (predecessor != nullptr) {
            my_node.locked.store(true);
            predecessor->next.store(&my_node);
            // printf("%ld: Waiting for unlock...\n", thread_id);
            while (my_node.locked.load()) {
                // printf("%ld: Waiting for node unlock...\n", thread_id);
                // Busy wait
            }
        }
        // printf("%ld: Locked\n", thread_id);
    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // Unused

        if (my_node.next.load() == nullptr) {
            QNode* my_node_p = &my_node;
            if (std::atomic_compare_exchange_strong(
                    &lock_,
                    &my_node_p,
                    nullptr
                )) {
                    // printf("%ld: Successfully std::atomic_compare_exchanged, unlocked\n", thread_id);
                    return;
                }
            // If MCSMutex::lock_ is not pointing to this node, but this node's pointer in null,
            // that can only mean there is another node in the process of setting (between lock_.exchange and predecessor->next.store)
            // This almost never happens.
            while (my_node.next.load() == nullptr) {
                // Busy wait (should this return to the start?)
            }
        }
        my_node.next.load()->locked.store(false);
        // printf("%ld: Unlocked\n", thread_id);
    }

    void destroy() override {

    }

    std::string name() override {
        return "mcs";
    }
private:
    // Do these need to be volatile?
    static thread_local QNode my_node;
    static volatile std::atomic<QNode*> lock_;
};

thread_local QNode MCSMutex::my_node;
volatile std::atomic<QNode*> MCSMutex::lock_ = nullptr;