// TODO: this lock heavily relies on having a lock guard structure, which is not possible under our current implementation.
// This implementation is not as efficient as it should be; the lock() method makes more memory accesses than is strictly necessary because it 
// doesn't store anything locally.

#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>
#include <time.h>
#include <assert.h>

// TODO: explicit memory ordering.
// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.

// CLH variant that does not use malloc or a prev pointer
class HopscotchLocalMutex : public virtual SoftwareMutex {
public:
    struct Node {
        volatile bool successor_must_wait;
    };

    void init(size_t num_threads) override {
        assert(sizeof(Node) == 1);
        (void)num_threads;

        // Initialize
        // The lock starts off unlocked by setting tail to a pointer
        // to some true value so that the next successor can immediately lock.
        // This means that predecessor is never a null pointer.
        tail = &default_node;
    }

    inline Node *my_node() {
        return &nodes[which_node];
    }

    void lock(size_t thread_id) override {
        (void)thread_id;

        Node *node = my_node();
        node->successor_must_wait = true;
        Node *predecessor = tail.exchange(node, std::memory_order_relaxed);
        while (predecessor->successor_must_wait);
    }
    
    void unlock(size_t thread_id) override {
        (void)thread_id;

        my_node()->successor_must_wait = false;
        // The Hopscotch part, flipping to the other node to avoid reusing a node slot prematurely.
        which_node ^= 1;
    }

    void destroy() override {
    }

    std::string name() override {
        return "hopscotch_local";
    }
private:
    // static Node default_node; no pointer, just stored in _cxl_region
    // static std::atomic<Node*> tail;
    // static thread_local Node *node;

    // TODO: if a thread leaves, will its still-used thread locals be reclaimed
    // and break the algorithm?
    // Would this algorithm be faster if the nodes were all contiguous in memory?
    // alignas(2) static thread_local Node my_nodes[2];

    std::atomic<Node*> tail;
    static Node default_node;
    // This can be stored locally because nothing else depends on it.
    alignas(2) static thread_local Node nodes[2];
    static thread_local bool which_node;
};
HopscotchLocalMutex::Node HopscotchLocalMutex::default_node = { false };
alignas(2) thread_local HopscotchLocalMutex::Node HopscotchLocalMutex::nodes[2] = { false, false };
thread_local bool HopscotchLocalMutex::which_node = 0;