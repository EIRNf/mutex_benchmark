#include "lock.hpp"
#include <stdexcept>

class DijkstraNonatomicMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        this->unlocking = (volatile bool*)malloc(sizeof(bool) * num_threads);
        this->c = (volatile bool*)malloc(sizeof(bool) * num_threads);
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
        Fence();
        if (k != thread_id) {
            while (!unlocking[k]) {}
            k = thread_id;
            Fence();
            
            goto try_again;
        } 
        c[thread_id] = false;
        Fence();
        for (size_t j = 0; j < num_threads; j++) {
            if (j != thread_id && !c[j]) {
                goto try_again;
            }
        }

    }
    void unlock(size_t thread_id) override {
        unlocking[thread_id] = true;
        c[thread_id] = true;
    }
    void destroy() override {
        free((void*)unlocking);
        free((void*)c);
    }

    std::string name(){return "djikstra";};

private:
    volatile bool *unlocking;
    volatile bool *c;
    volatile size_t k;
    size_t num_threads;
};