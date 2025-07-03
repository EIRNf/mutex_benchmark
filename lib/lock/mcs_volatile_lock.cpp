#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>

// This file was made to investigate whether adding the "volatile" keyword was slowing MCS down.
// MCSVolatileMutex seems slightly faster than MCSMutex.

// TODO: explicit memory ordering.
// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.

struct VolatileQNode {
    std::atomic<volatile VolatileQNode*> next;
    std::atomic_bool locked;
};

class MCSVolatileMutex : public virtual SoftwareMutex {
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
        volatile VolatileQNode* predecessor = lock_.exchange(&my_node);
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
            // printf("std::atomic_compare_exchange_strong(\n"
            //         "    %p,\n"
            //         "    %p,\n"
            //         "    %p\n"
            //         ")\n",
            //         &lock_,
            //         (volatile QNode**)&my_node,
            //         (volatile QNode*)nullptr
            // );
            volatile VolatileQNode* my_node_p = &my_node;
            if (std::atomic_compare_exchange_strong(
                    &lock_,
                    // We need to get the address of my_node, but the addressing operation
                    // with a temporary variable makes this operation non-atomic and ruins everything.
                    &my_node_p,
                    nullptr
                )) {
                    // printf("%ld: Successfully std::atomic_compare_exchanged, unlocked\n", thread_id);
                    return;
                }
            // If MCSMutex::lock_ is not pointing to this node, but this node's pointer in null,
            // that can only mean there is another node in the process of setting (between lock_.exchange and predecessor->next.store)
            // This almost never happens.
            // printf("Waiting for successor...\n");
            while (my_node.next.load() == nullptr) {
                // printf(".");
                // Busy wait (should this return to the start?)
            }
        }
        my_node.next.load()->locked.store(false);
        // printf("%ld: Unlocked\n", thread_id);
    }

    void destroy() override {

    }

    std::string name() override {
        return "mcs_volatile";
    }
private:
    // Do these need to be volatile?
    static volatile thread_local VolatileQNode my_node;
    static volatile std::atomic<volatile VolatileQNode*> lock_;
};

volatile thread_local VolatileQNode MCSVolatileMutex::my_node;
volatile std::atomic<volatile VolatileQNode*> MCSVolatileMutex::lock_ = nullptr;