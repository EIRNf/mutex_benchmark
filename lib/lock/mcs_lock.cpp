#include "lock.hpp"
#include "cxl_utils.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>
#include <string.h>
#include <immintrin.h>

// TODO: explicit memory ordering.
// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.

class MCSMutex : public virtual SoftwareMutex {
public:
    struct Node {
        std::atomic<Node*> next;
        volatile bool locked;
    };

    void init(size_t num_threads) override {
        size_t nodes_size = sizeof(Node) * num_threads;
        _cxl_region_size = sizeof(std::atomic<Node*>) + nodes_size;
        this->_cxl_region = (volatile char *)ALLOCATE(_cxl_region_size);
        this->nodes = (Node*)&_cxl_region[0];
        this->tail = (std::atomic<MCSMutex::Node*>*)&_cxl_region[nodes_size];
        *this->tail = nullptr;
        memset((void*)this->nodes, 0, nodes_size);
    }

    void lock(size_t thread_id) override {
        Node *local_node = &nodes[thread_id];

        // Initialize thread_local node
        local_node->next = nullptr;
        // Load old tail node of queue while also adding ourself to the queue
        Node *old_tail = tail->exchange(local_node, std::memory_order_acquire);
        if (old_tail == nullptr) {
            // We're the only one in the queue; we successfully acquired the lock.
            return;
        }
        // Edit the tail to add ourself in.
        local_node->locked = true;
        old_tail->next = local_node;
        while (local_node->locked);
    }

    void unlock(size_t thread_id) override {
        Node *local_node = &nodes[thread_id];

        if (local_node->next == nullptr) {
            Node *expected = local_node;
            if (tail->compare_exchange_strong(expected, nullptr)) {
                return;
            }
        }

        while (local_node->next == nullptr) {
            // spin_delay_exponential(); // Busy wait
        }

        local_node->next.load()->locked = false;
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

    std::atomic<MCSMutex::Node*>* tail;
    MCSMutex::Node *nodes;
};