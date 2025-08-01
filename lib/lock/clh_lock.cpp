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
        tail.load()->successor_must_wait = false;
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // Unused

        node = (struct Node*)ALLOCATE(sizeof(struct Node));
        node->successor_must_wait = true;
        struct Node *predecessor = tail.exchange(node, std::memory_order_relaxed);
        while (predecessor->successor_must_wait);
        FREE((void*)predecessor, sizeof(struct Node));
    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // Unused

        node->successor_must_wait = false;
    }

    void destroy() override {
        FREE((void*)tail, sizeof(struct Node));
    }

    std::string name() override {
        return "clh";
    }
private:
    static std::atomic<struct Node*> tail;
    static thread_local struct Node *node;
};
std::atomic<struct CLHMutex::Node*> CLHMutex::tail;
thread_local struct CLHMutex::Node *CLHMutex::node;