// TODO: this lock heavily relies on having a lock guard structure, which is not possible under our current implementation.
// This implementation is not as efficient as it should be; the lock() method makes more memory accesses than is strictly necessary because it 
// doesn't store anything locally.

#include "../utils/cxl_utils.hpp"
#include <string.h>
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
class HopscotchMutex : public virtual SoftwareMutex {
public:
    struct Node {
        volatile bool successor_must_wait;
    };

    void init(size_t num_threads) override {
        assert(sizeof(Node) == 1);

        // Create memory region
        size_t tail_size = sizeof(std::atomic<Node*>*);
        size_t nodes_size = sizeof(Node) * (num_threads * 2); // Each thread has 2 node slots, and the default node is in the last slot.
        size_t default_node_size = sizeof(Node);
        _cxl_region_size = tail_size + nodes_size + default_node_size;
        _cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);

        // Set up pointers into region
        size_t offset = 0;
        tail = (std::atomic<Node*>*)&_cxl_region[offset];
        offset += tail_size;
        nodes = (Node*)&_cxl_region[offset];
        offset += nodes_size;
        Node *default_node = (Node*)&_cxl_region[offset];

        // Initialize
        // The lock starts off unlocked by setting tail to a pointer
        // to some true value so that the next successor can immediately lock.
        // This means that predecessor is never a null pointer.
        default_node->successor_must_wait = false;
        *tail = default_node;
        memset((void*)nodes, 0, nodes_size + default_node_size);
    }

    inline Node *my_node(size_t thread_id) {
        return &nodes[thread_id * 2 + which_node];
    }

    void lock(size_t thread_id) override {
        Node *node = my_node(thread_id);
        node->successor_must_wait = true;
        Node *predecessor = tail->exchange(node, std::memory_order_relaxed);
        while (predecessor->successor_must_wait);
    }
    
    void unlock(size_t thread_id) override {
        my_node(thread_id)->successor_must_wait = false;
        // The Hopscotch part, flipping to the other node to avoid reusing a node slot prematurely.
        which_node ^= 1;
    }

    void destroy() override {
    }

    std::string name() override {
        return "hopscotch";
    }
private:
    // static Node default_node; no pointer, just stored in _cxl_region
    // static std::atomic<Node*> tail;
    // static thread_local Node *node;

    // TODO: if a thread leaves, will its still-used thread locals be reclaimed
    // and break the algorithm?
    // Would this algorithm be faster if the nodes were all contiguous in memory?
    // alignas(2) static thread_local Node my_nodes[2];

    volatile char *_cxl_region;
    size_t _cxl_region_size;
    std::atomic<Node*>* tail;
    Node *nodes;
    // This can be stored locally because nothing else depends on it.
    static thread_local bool which_node;
};
thread_local bool HopscotchMutex::which_node = 0;
