#include "lock.hpp"
#include <stdexcept>

class DijkstraMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        this->unlocking = (volatile std::atomic_bool*)malloc(sizeof(std::atomic_bool) * num_threads);
        this->c         = (volatile std::atomic_bool*)malloc(sizeof(std::atomic_bool) * num_threads);
        for (size_t i = 0; i < num_threads; i++) {
            unlocking[i] = true;
            c[i] = true;
        }
        this->k = 0;
        this->num_threads = num_threads;
    }

    void lock(size_t thread_id) override {
        // TODO refactor and remove goto
        unlocking[thread_id] = false;
    try_again:
        c[thread_id] = true;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (k != thread_id) {
            while (!unlocking[k]) {}
            k = thread_id;
            std::atomic_thread_fence(std::memory_order_seq_cst);
            
            goto try_again;
        } 
        c[thread_id] = false;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        for (size_t j = 0; j < num_threads; j++) {
            if (j != thread_id && !c[j]) {
                goto try_again;
            }
        }

    }

    void unlock(size_t thread_id) override {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        unlocking[thread_id] = true;
        c[thread_id] = true;
    }

    void destroy() override {
        free((void*)unlocking);
        free((void*)c);
    }

    std::string name(){return "djikstra";};

private:
    volatile std::atomic_bool *unlocking;
    volatile std::atomic_bool *c;
    volatile std::atomic<size_t> k;
    size_t num_threads;
};
