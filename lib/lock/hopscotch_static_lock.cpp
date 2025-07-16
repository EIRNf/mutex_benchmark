#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <string.h>

// TODO: explicit memory ordering.
// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.


// CLH variant that does not use malloc or a prev pointer, able to be statically allocated.
class HopscotchStaticMutex : public virtual SoftwareMutex {
public:
    struct Node {
        std::atomic_bool successor_must_wait;
    };

    static size_t get_size(size_t num_threads) {
        return sizeof(HopscotchStaticMutex) + num_threads * 3;
    }

    void init(size_t num_threads) override {
        assert(sizeof(struct Node) == 1);

        // The lock starts off unlocked by setting tail to a pointer
        // to some true value so that the next successor can immediately lock.
        // This means that predecessor is never a null pointer.
        this->head.num_threads = num_threads;
        this->head.tail = &default_node;
        this->default_node.successor_must_wait.store(false);
        for (size_t i = 0; i < num_threads; i++) {
            node_in_use[i] = 0;
        }
        this->head.nodes = &this->_nodes[num_threads];
    }

    void lock(size_t thread_id) override {
        // printf("%ld: Locking...\n", thread_id);
        // The Hopscotch part, flipping to the other node to avoid reusing a node prematurely.
        // This function should really take struct Node *node as an argument,
        // but we can't break the interface.
        node_in_use[thread_id] ^= 1;
        struct Node *node = node_get(thread_id);
        node->successor_must_wait = true;
        struct Node *predecessor = this->head.tail.exchange(node, std::memory_order_relaxed);
        while (predecessor->successor_must_wait.load(std::memory_order_relaxed));
        // printf("%ld: Locked. (node_in_use[thread_id]=%d)\n", thread_id, node_in_use[thread_id]);
    }

    void unlock(size_t thread_id) override {
        // printf("%ld: Unlocking...\n", thread_id);
        node_get(thread_id)->successor_must_wait.store(false, std::memory_order_relaxed);
        // printf("%ld: Unlocked.\n", thread_id);
    }

    void destroy() override {
    }

    inline struct Node *node_get(size_t thread_id) {
        return &this->head.nodes[thread_id * 2 + node_in_use[thread_id]];
    }

    std::string name() override {
        return "hopscotch_static";
    }
private:
    struct {
        size_t num_threads;
        std::atomic<struct Node*> tail;
        struct Node *nodes;
    } head;
    struct Node default_node;
    // Unsized region of size num_threads * 3
    union {
        bool node_in_use[0];
        uint8_t _tail[0];
        struct Node _nodes[0];
    };
};