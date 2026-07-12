#include "lock.hpp"
#include "cxl_utils.hpp"
#include "region_layout.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>
#include <new>
#include <string.h>

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
        RegionLayout layout;
        nodes_handle = layout.reserve_strided_array<Node>(num_threads);
        auto tail_handle = layout.reserve<std::atomic<Node*>>();

        _cxl_region_size = layout.total_size();
        this->_cxl_region = (volatile char *)ALLOCATE(_cxl_region_size);
        this->tail = RegionLayout::resolve(tail_handle, _cxl_region);
        *this->tail = nullptr;
        memset((void*)get_node_by_thread_id(0), 0, nodes_handle.stride * num_threads);
    }

    inline Node *get_node_by_thread_id(size_t thread_id) {
        return RegionLayout::resolve_strided(nodes_handle, this->_cxl_region, thread_id);
    }

    void lock(size_t thread_id) override {
        Node *local_node = get_node_by_thread_id(thread_id);

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
        unsigned spins = 0;
        while (local_node->locked) { LockSpinWait(spins); }
    }

    void unlock(size_t thread_id) override {
        Node *local_node = get_node_by_thread_id(thread_id);

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

        local_node->next.load()->locked = false;
    }

    void destroy() override {
        FREE((void*)_cxl_region, _cxl_region_size);
    }

    std::string name() override {
        return "mcs_cachealigned";
    }
private:
    volatile char *_cxl_region;
    size_t _cxl_region_size;

    std::atomic<Node*>* tail;
    RegionLayout::StridedHandle<Node> nodes_handle;
};