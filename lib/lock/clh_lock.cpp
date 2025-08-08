#include "../utils/cxl_utils.hpp"

#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>
#include <time.h>
#include <assert.h>

// TODO: allocate once and keep using that.
// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.

class CLHMutex : public virtual SoftwareMutex {
public:
    struct Node {
        std::atomic_bool successor_must_wait;
    };

    void init(size_t num_threads) override {
        (void)num_threads; // Unused

        // The lock starts off unlocked by setting tail to a pointer
        // to some true value so that the next successor can immediately lock.
        // This means that predecessor is never a null pointer.
        tail = (struct Node*)ALLOCATE(sizeof(struct Node*));
        nodes = (Node **)ALLOCATE(sizeof(Node*)*num_threads);
        tail.load()->successor_must_wait = false;
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // Unused

        nodes[thread_id] = (struct Node*)ALLOCATE(sizeof(struct Node)); //why is it allocating every loop?????
        nodes[thread_id]->successor_must_wait = true;
        struct Node *predecessor = tail.exchange(nodes[thread_id], std::memory_order_relaxed);
        while (predecessor->successor_must_wait);
        FREE((void*)predecessor, sizeof(struct Node));
    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // Unused

        nodes[thread_id]->successor_must_wait = false;
    }

    void destroy() override {
        FREE((void*)tail, sizeof(struct Node));
    }

    std::string name() override {
        return "clh";
    }
private:
    std::atomic<struct Node*> tail;
    Node **nodes;
};