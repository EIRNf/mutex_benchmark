#include "lock.hpp"
#include "cxl_utils.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>
#include <string.h>

// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.

class MCSNonCacheAlignedMutex : public virtual SoftwareMutex {
public:
    struct Node {
        std::atomic<Node*> next;
        // atomic with release/acquire handoff — see the matching comment in
        // mcs_lock.cpp: as a plain volatile bool, CS writes could trail the
        // handoff on arm64 unless the caller fenced inside the CS.
        std::atomic<bool> locked;
    };

    void init(size_t num_threads) override {
        size_t nodes_size = sizeof(Node) * num_threads;
        _cxl_region_size = sizeof(std::atomic<Node*>) + nodes_size;
        this->_cxl_region = (volatile char *)ALLOCATE(_cxl_region_size);
        this->nodes = (Node*)&_cxl_region[0];
        this->tail = (std::atomic<Node*>*)&_cxl_region[nodes_size];
        *this->tail = nullptr;
        memset((void*)this->nodes, 0, nodes_size);
    }

    void lock(size_t thread_id) override {
        Node *local_node = &nodes[thread_id];

        // Initialize thread_local node
        local_node->next = nullptr;
        // acq_rel: the release half publishes our next=nullptr before the
        // node becomes reachable through tail.
        Node *old_tail = tail->exchange(local_node, std::memory_order_acq_rel);
        if (old_tail == nullptr) {
            // We're the only one in the queue; we successfully acquired the lock.
            return;
        }
        // Edit the tail to add ourself in.
        local_node->locked.store(true, std::memory_order_relaxed);
        old_tail->next = local_node;
        unsigned spins = 0;
        while (local_node->locked.load(std::memory_order_acquire)) { LockSpinWait(spins); }
    }

    void unlock(size_t thread_id) override {
        Node *local_node = &nodes[thread_id];

        if (local_node->next == nullptr) {
            Node *expected = local_node;
            if (tail->compare_exchange_strong(expected, nullptr)) {
                return;
            }
        }

        unsigned spins = 0;
        while (local_node->next == nullptr) {
            LockSpinWait(spins);
        }

        // release: hands the critical section's writes to the successor.
        local_node->next.load()->locked.store(false, std::memory_order_release);
    }

    void destroy() override {
        FREE((void*)_cxl_region, _cxl_region_size);
    }

    std::string name() override {
        return "mcs";
    }
private:
    volatile char *_cxl_region;
    size_t _cxl_region_size;

    std::atomic<Node*>* tail;
    Node *nodes;
};