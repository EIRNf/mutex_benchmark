
#include "cxl_utils.hpp"
#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <math.h>
#include <string.h>

#include "spin_lock.hpp"

template<class WakerLock>
class TreeElevatorMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        size_t num_threads_rounded_up_to_power_of_2 = 1;
        while (num_threads_rounded_up_to_power_of_2 < num_threads) {
            num_threads_rounded_up_to_power_of_2 *= 2;
        }
        this->num_threads = num_threads;
        this->leaf_depth = depth(leaf(0));

        size_t val_size = sizeof(std::atomic_size_t) * (num_threads * 2 + 1);
        size_t queue_buffer_size = sizeof(size_t) * num_threads_rounded_up_to_power_of_2;
        size_t queue_size = sizeof(Queue);
        size_t waker_lock_size = WakerLock::get_cxl_region_size(num_threads);
        size_t flag_size = sizeof(std::atomic_bool) * (num_threads + 1);
        _cxl_region_size = val_size + queue_buffer_size + queue_size + waker_lock_size + flag_size;
        _cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);


        size_t offset = 0;
        this->val = (std::atomic_size_t*)&_cxl_region[offset];
        offset += val_size;
        this->queue_buffer = (size_t*)&_cxl_region[offset];
        offset += queue_buffer_size;
        this->queue = (Queue*)&_cxl_region[offset];
        offset += queue_size;
        this->designated_waker_lock.region_init(num_threads, (volatile char*)&_cxl_region[offset]);
        offset += waker_lock_size;
        this->flag = (std::atomic_bool*)&_cxl_region[offset];

        for (size_t i = 0; i < num_threads * 2; i++) {
            this->val[i] = num_threads; // num_threads represents Not A Thread
        }
        val[num_threads * 2] = num_threads; // optimization mentioned in paper

        memset((void*)this->flag, 0, flag_size);
        flag[num_threads] = true;

        // Initialize ring queue
        // If we make the buffer length a power of 2, we can use a 
        // binary & operation instead of modulus to cycle the indices.
        this->queue->index_mask = num_threads_rounded_up_to_power_of_2 - 1;
        this->queue->ring_start = 0;
        this->queue->ring_end = 0;
    }

    // depth(1) == 0
    // depth(2) == 1
    inline size_t depth(size_t p) {
        // p should never be 0; val starts at index 1
        // TODO verify
        return (size_t)log2(p);
    }

    inline size_t sibling(size_t p) {
        return p ^ 1;
    }

    inline size_t parent(size_t p) {
        return p >> 1;
    }

    inline size_t leaf(size_t n) {
        return n + num_threads;
    }

    // It's faster to calculate the path variable
    // if given the depth relative to p instead of the 
    // depth of the node
    inline size_t path_climbing(size_t p, size_t climb) {
        return p >> climb;
    }

    inline void enqueue(size_t x) {
        queue_buffer[(queue->ring_end++)&queue->index_mask] = x;
    }

    inline size_t dequeue() {
        return queue_buffer[(queue->ring_start++)&queue->index_mask];
    }

    inline bool queue_empty() {
        // If num_threads is a power of 2 and the queue is totally full,
        // this condition will be true even though the queue is not empty.
        // Hopefully that doesn't happen.
        // I don't think it's possible for the unlocking thread to be in the queue.
        return queue->ring_start == queue->ring_end;
    }

    void lock(size_t thread_id) override {
        // Setup waiting state for this thread
        // TODO set all nodes?
        for (size_t node = leaf(thread_id); node > 1; node = parent(node)) {
            val[node] = thread_id;
        }
        // Contention loop
        if (designated_waker_lock.trylock(thread_id)) {
            // Fence included in algorithm. TODO test
            FENCE();
            while (flag[thread_id] == false && flag[num_threads] == false) {
                // spin_delay_exponential(); // Wait (TODO test spin_delay_exp here)
            }
            flag[num_threads] = false;
            designated_waker_lock.unlock(thread_id);
        } else {
            while (flag[thread_id] == false) {
                // spin_delay_exponential(); // Wait (TODO test spin_delay_exp here)
            }
        }
        val[leaf(thread_id)] = num_threads;
        flag[thread_id] = false;
    }

    void unlock(size_t thread_id) override {
        // Traverse tree from root to lead (excluding root) to find and enqueue waiting nodes.
        // Exclude root because we're enqueueing only siblings and the root does not have a sibling.
        size_t node = leaf(thread_id);
        for (size_t j = leaf_depth; j != -1; j--) { // doesn't have to be "signed" i think
            size_t k = val[sibling(path_climbing(node, j))];
            if (val[leaf(k)] < num_threads) {
                val[leaf(k)] = num_threads;
                enqueue(k);
            }
        }
        if (!queue_empty()) {
            flag[dequeue()] = true;
        } else {
            flag[num_threads] = true;            

            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
    }

    void destroy() override {
        FREE((void*)_cxl_region, _cxl_region_size);
        // free((void*)this->val);
        // free((void*)this->queue);
        // free((void*)this->flag);
        // don't call the destroy method because 
        // it doesn't own any memory to free.
        // this.designated_waker_lock.destroy();
    }

    std::string name() override {
        return "tree_elevator";
    }
private:
    struct Queue {
        size_t index_mask;
        size_t ring_start;
        size_t ring_end;
    };

    // Pointers into _cxl_region
    volatile char *_cxl_region;
    WakerLock designated_waker_lock; // contains pointer (if compiling with --cxl)
    Queue *queue;
    size_t *queue_buffer;
    std::atomic_size_t *val; // TODO: test atomic_int performance instead
    std::atomic_bool *flag;

    // Constants
    size_t num_threads;
    size_t leaf_depth;
    size_t _cxl_region_size;
};