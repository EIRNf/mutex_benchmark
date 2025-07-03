#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>

// TODO: explicit memory ordering.
// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.

struct MQNode {
    std::atomic<MQNode*> next;
    std::atomic_bool locked;
};

class MCSMallocMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads; // Unused

        lock_ = (volatile std::atomic<MQNode*>*)malloc(sizeof(volatile std::atomic<MQNode*>));
        *lock_ = nullptr;
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // Unused

        if (my_node == nullptr) {
            my_node = (MQNode*)malloc(sizeof(MQNode));
        }
        my_node->next = nullptr;
        // my_node.locked may remain uninitialized. 
        // This is fine because if we have the lock we will never
        // access my_node.locked.
        MQNode* predecessor = (*lock_).exchange(my_node);
        // Once the lock._next is swapped, won't be modified
        if (predecessor != nullptr) {
            my_node->locked = true;
            predecessor->next = my_node;
            // printf("%ld: Waiting for unlock...\n", thread_id);
            while (my_node->locked) {
                // printf("%ld: Waiting for node unlock...\n", thread_id);
                // Busy wait
            }
        }
        // printf("%ld: Locked\n", thread_id);
    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // Unused

        if (my_node->next == nullptr) {
            if (std::atomic_compare_exchange_strong(
                    lock_,
                    &my_node,
                    nullptr
                )) {
                    try_free_my_node();
                    // printf("%ld: Successfully std::atomic_compare_exchanged, unlocked\n", thread_id);
                    return;
                }
            // If MCSMutex::lock_ is not pointing to this node, but this node's pointer in null,
            // that can only mean there is another node in the process of setting (between lock_.exchange and predecessor->next.store)
            // This almost never happens.
            while (my_node->next.load() == nullptr) {
                // Busy wait (should this return to the start?)
            }
        }
        my_node->next.load()->locked.store(false);
        try_free_my_node();
        // printf("%ld: Unlocked\n", thread_id);
    }

    inline void try_free_my_node() {
        if (my_node != nullptr) {
            free((void*)my_node);
            my_node = nullptr;
        }
    }

    void destroy() override {
        free((void*)lock_);
    }

    std::string name() override {
        return "mcs";
    }
private:
    // Do these need to be volatile?
    static thread_local MQNode *my_node;
    static volatile std::atomic<MQNode*> *lock_;
};

thread_local MQNode *MCSMallocMutex::my_node = nullptr;
volatile std::atomic<MQNode*> *MCSMallocMutex::lock_ = nullptr;