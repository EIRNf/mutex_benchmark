#include "../utils/cxl_utils.hpp"
#include "../utils/emucxl_lib.h"
#include "../utils/region_layout.hpp"

#include "lock.hpp"
#include <stdexcept>
#include "../utils/bench_utils.hpp"

class DijkstraNonatomicMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        RegionLayout layout;
        auto k_handle         = layout.reserve<size_t>();
        auto unlocking_handle = layout.reserve_array<bool>(num_threads + 1);
        auto c_handle         = layout.reserve_array<bool>(num_threads + 1);

        this->_cxl_region_size = layout.total_size();
        this->_cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);

        this->k = RegionLayout::resolve(k_handle, _cxl_region);
        *k = 0;
        this->unlocking = RegionLayout::resolve(unlocking_handle, _cxl_region);
        this->c         = RegionLayout::resolve(c_handle, _cxl_region);

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
        FREE((void*)this->_cxl_region, this->_cxl_region_size);
    }

    std::string name() override {
        return "djikstra";
    }

private:
    volatile char *_cxl_region;
    size_t _cxl_region_size;
    volatile bool *unlocking;
    volatile bool *c;
    volatile size_t *k;
    size_t num_threads;
};
