#include "lock.hpp"
#include "../utils/cxl_utils.hpp"
#include "../utils/region_layout.hpp"
#include <stdexcept>

class DijkstraMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        RegionLayout layout;
        auto k_handle         = layout.reserve<std::atomic_size_t>();
        auto unlocking_handle = layout.reserve_array<std::atomic_bool>(num_threads + 1);
        auto c_handle         = layout.reserve_array<std::atomic_bool>(num_threads + 1);

        _cxl_region_size = layout.total_size();
        _cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);

        this->k         = RegionLayout::resolve(k_handle, _cxl_region);
        this->unlocking = RegionLayout::resolve(unlocking_handle, _cxl_region);
        this->c         = RegionLayout::resolve(c_handle, _cxl_region);
        for (size_t i = 0; i < num_threads + 1; i++) {
            unlocking[i] = true;
            c[i] = true;
        }
        *this->k = 0;
        this->num_threads = num_threads;
    }

    void lock(size_t thread_id) override {
        // TODO refactor and remove goto
        unlocking[thread_id+1] = false;
    try_again:
        c[thread_id+1] = true;
        if (*k != thread_id+1) {
            while (!unlocking[*k]) {}
            *k = thread_id+1;
            goto try_again;
        } 
        c[thread_id+1] = false;
        for (size_t j = 1; j <= num_threads; j++) {
            if (j != thread_id+1 && !c[j]) {
                goto try_again;
            }
        }
    }

    void unlock(size_t thread_id) override {
        *k=0;
        unlocking[thread_id+1] = true;
        c[thread_id+1] = true;
    }

    void destroy() override {
        FREE((void*)_cxl_region, _cxl_region_size);
    }

    std::string name() override {return "djikstra";};

private:
    volatile char *_cxl_region;
    size_t _cxl_region_size; // this could just be re-calculated
    std::atomic_bool *unlocking;
    std::atomic_bool *c;
    std::atomic_size_t *k;
    size_t num_threads;
};
