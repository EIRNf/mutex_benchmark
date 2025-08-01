#include "lock.hpp"
#include <stdexcept>
#include "../utils/bench_utils.hpp"

class DijkstraNonatomicMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        this->unlocking = (volatile bool*)malloc(sizeof(bool) * (num_threads+1));
        this->c = (volatile bool*)malloc(sizeof(bool) * (num_threads+1));
        for (size_t i = 0; i <= num_threads; i++) {
            unlocking[i] = true;
            c[i] = true;
        }
        this->k = 0;
        this->num_threads = num_threads;
    }

    void lock(size_t thread_id) override {
        // TODO refactor and remove goto
        unlocking[thread_id+1] = false;
    try_again:
        c[thread_id+1] = true;
        Fence();
        if (k != thread_id+1) {
            while (!unlocking[k]) {}
            k = thread_id+1;

            Fence();
            // goto try_again; //maybe not needed

        } 
        c[thread_id+1] = false;
        Fence();
        for (size_t j = 1; j <= num_threads; j++) {
            if (j != thread_id +1 && !c[j]) {
                goto try_again;
            }
        }
        Fence();

    }
    void unlock(size_t thread_id) override {
        Fence();
        k=0;
        unlocking[thread_id+1] = true;
        c[thread_id+1] = true;
    }
    void destroy() override {
        free((void*)unlocking);
        free((void*)c);
    }

    std::string name() override {return "djikstra";};

private:
    volatile bool *unlocking;
    volatile bool *c;
    volatile size_t k;
    size_t num_threads;
};