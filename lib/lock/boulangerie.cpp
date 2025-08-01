#include "../utils/cxl_utils.hpp"

#include "lock.hpp"
#include <stdexcept>

class Boulangerie : public virtual SoftwareMutex {
public:
    std::string name() override { return "boulangerie"; }
    void init(size_t num_threads) override {
        size_t _cxl_region_size = (sizeof(volatile bool) + sizeof(volatile int)) * num_threads;
        this->_cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);
        this->num_threads = num_threads;
        this->number = (volatile int*)&this->_cxl_region[0];
        size_t offset = sizeof(volatile int) * num_threads;
        this->choosing = (volatile bool*)&this->_cxl_region[offset];
        for (size_t i = 0; i < num_threads; i++) {
            choosing[i] = 0;
            number[i] = 0;
        }
    }

    void lock(size_t thread_id) override {
        choosing[thread_id] = 1;
        FENCE();
        int max_number = 0;
        for (size_t i = 0; i < num_threads; ++i) {
            if (number[i] > max_number) {
                max_number = number[i];
            }
        }
        number[thread_id] = max_number + 1;
        FENCE();
        choosing[thread_id] = 0;
        FENCE();
        
        //limit the number of thread to check
        size_t limit;
        if (number[thread_id] == 1 && thread_id > 0) {limit = thread_id;}
        else {limit = num_threads;}

        int prev_j, curr_j;
        for (size_t j = 0; j < limit; ++j) {
            if (j == thread_id) continue;
            while (choosing[j]) {}
            prev_j = number[j];
            curr_j = number[j];
            while (curr_j != 0 && (curr_j < number[thread_id] || (curr_j == number[thread_id] && j < thread_id)) && curr_j == prev_j) {
                    prev_j = curr_j;
                    curr_j = number[j];
            }
        }
        FENCE();
    }

    void unlock(size_t thread_id) override {
        number[thread_id] = 0;
    }

    void destroy() override {
        size_t _cxl_region_size = (sizeof(volatile bool) + sizeof(volatile int)) * this->num_threads;
        FREE((void*)this->_cxl_region, _cxl_region_size);
    }


private:
    volatile char *_cxl_region;
    size_t num_threads;
    volatile bool *choosing;
    volatile int *number;
};


