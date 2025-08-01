#include "../utils/cxl_utils.hpp"
#include "../utils/emucxl_lib.h"

#include "lock.hpp"
#include <stdexcept>
#include "../utils/bench_utils.hpp"

class DijkstraNonatomicMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        size_t size = (num_threads + 1) * sizeof(bool) * 2 + sizeof(size_t);
        this->_cxl_region = (volatile char*)ALLOCATE(size);

        this->k = (volatile size_t*)&this->_cxl_region[0];
        *k = 0;
        this->unlocking = (volatile bool*)&this->_cxl_region[sizeof(size_t)];

        size_t c_offset = sizeof(bool) * (num_threads + 1) + sizeof(size_t);
        this->c = (volatile bool*)&this->_cxl_region[c_offset];

        for (size_t i = 0; i < num_threads + 1; i++) {
            unlocking[i] = true;
            c[i] = true;
        }
        this->num_threads = num_threads;
    }

    void lock(size_t thread_id) override {
        // TODO refactor and remove goto
        unlocking[thread_id+1] = false;
    try_again:
        c[thread_id+1] = true;
        FENCE();
        if (*k != thread_id+1) {
            while (!unlocking[*k]) {}
            *k = thread_id+1;
            FENCE();
            
            goto try_again;
        } 
        c[thread_id+1] = false;
        FENCE();
        for (size_t j = 1; j < num_threads+1; j++) {
            if (j != thread_id+1 && !c[j]) {
                goto try_again;
            }
        }
        Fence();

    }
    void unlock(size_t thread_id) override {
        *k=0;
        unlocking[thread_id+1] = true;
        c[thread_id+1] = true;
    }
    void destroy() override {
        FREE((void*)this->_cxl_region, this->num_threads * sizeof(bool) * 2);
    }

    std::string name() override {
        return "djikstra";
    }

private:
    volatile char *_cxl_region;
    volatile bool *unlocking;
    volatile bool *c;
    volatile size_t *k;
    size_t num_threads;
};