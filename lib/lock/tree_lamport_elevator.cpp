#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <math.h>
#include <string.h>

#include "lamport_lock.cpp"

class TreeLamportElevatorMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        // TODO organize this function.
        this->num_threads = num_threads;
        this->leaf_depth = depth(leaf(0));

        this->val = (std::atomic_size_t*)malloc(sizeof(std::atomic_size_t) * (num_threads * 2 + 1));
        for (size_t i = 0; i < num_threads*2; i++) {
            this->val[i] = num_threads; // num_threads represents Not A Thread
        }
        val[num_threads * 2] = num_threads; // optimization mentioned in paper

        this->flag = (std::atomic_bool*)malloc(sizeof(std::atomic_bool) * (num_threads + 1));
        memset((void*)this->flag, 0, sizeof(volatile bool) * (num_threads + 1));
        flag[num_threads] = true;

        // Initialize ring queue
        // If we make the buffer length a power of 2, we can use a 
        // binary & operation instead of modulus to cycle the indices.
        size_t num_threads_rounded_up_to_power_of_2 = 1;
        while (num_threads_rounded_up_to_power_of_2 < num_threads) {
            num_threads_rounded_up_to_power_of_2 *= 2;
        }
        this->queue = (size_t *)malloc(sizeof(size_t ) * num_threads_rounded_up_to_power_of_2);
        this->queue_index_mask = num_threads_rounded_up_to_power_of_2 - 1;
        this->queue_ring_start = 0;
        this->queue_ring_end = 0;

        this->designated_waker_lock.init(num_threads);
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
        queue[(queue_ring_end++)&queue_index_mask] = x;
    }

    inline size_t dequeue() {
        return queue[(queue_ring_start++)&queue_index_mask];
    }

    inline bool queue_empty() {
        // If num_threads is a power of 2 and the queue is totally full,
        // this condition will be true even though the queue is not empty.
        // Hopefully that doesn't happen.
        // I don't think it's possible for the unlocking thread to be in the queue.
        return queue_ring_start == queue_ring_end;
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
        for (size_t j = depth(leaf(thread_id))-1; j != -1; j--) { // doesn't have to be "signed" i think
            size_t k = val[sibling(path_climbing(node, j))];
            if (val[leaf(k)] < num_threads) {
                val[leaf(k)] = num_threads;
                enqueue(k);
            }
        }
        if (!queue_empty()) {
            int k = dequeue();
            flag[k]= true;
        } else {
            flag[num_threads] = true;            

            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
    }

    void destroy() override {
        free((void*)this->val);
        free((void*)this->queue);
        free((void*)this->flag);
        this->designated_waker_lock.destroy();
    }

    std::string name() override {
        return "tree_cas_elevator";
    }
private:
    LamportLock designated_waker_lock;
    std::atomic_size_t *val; // TODO: test atomic_int performance instead
    std::atomic_bool *flag;
    size_t num_threads;
    size_t leaf_depth;

    size_t  *queue;
    size_t queue_index_mask;
    size_t queue_ring_start;
    size_t queue_ring_end;
};