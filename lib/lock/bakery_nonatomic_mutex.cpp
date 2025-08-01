#include "lock.hpp"
#include <stdexcept>
#include "../utils/bench_utils.hpp"
#include "../utils/cxl_utils.hpp"

class BakeryNonAtomicMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        size_t number_size = sizeof(size_t) * num_threads;
        size_t choosing_size = sizeof(bool) * num_threads;
        _cxl_region_size = number_size + choosing_size;
        _cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);

        this->number = (volatile size_t*)&_cxl_region[0];
        this->choosing = (volatile bool*)&_cxl_region[number_size];

        for (size_t i = 0; i < num_threads; i++) {
            choosing[i] = false;
            number[i] = 0;
        }
        this->num_threads = num_threads;
    }

    void lock(size_t thread_id) override {
        // struct timespec nanosleep_timespec = { 0, 10 };
        // Get "bakery number"
        choosing[thread_id] = true;
        Fence();
        size_t my_bakery_number = 1;
        for (size_t i = 0; i < num_threads; i++) {
            if (number[i] + 1 > my_bakery_number) {
                my_bakery_number = number[i] + 1;
            }
        }
        
        number[thread_id] = my_bakery_number;
        Fence();
        choosing[thread_id] = false;
        Fence();

        // Lock waiting part
        for (size_t j = 0; j < num_threads; j++) {
            while (choosing[j] != 0) {
                // Wait for that thread to be done choosing a number.
                // nanosleep(&nanosleep_timespec, &remaining);
            }
            while ((number[j] != 0 && number[j] < number[thread_id]) 
                || (number[j] == number[thread_id] && j < thread_id)) {
                // Stall until our bakery number is the lowest..
                // nanosleep(&nanosleep_timespec, &remaining);
            }
        }
        
        Fence();
    }
    void unlock(size_t thread_id) override {
        number[thread_id] = 0;
    }
    void destroy() override {
        FREE((void*)_cxl_region, _cxl_region_size);
    }

    std::string name() override {
        return "bakery_nonatomic";
    }

private:
    volatile char *_cxl_region;
    volatile bool *choosing;
    // Note: Mutex will fail if this number overflows,
    // which happens if the "bakery" remains full for
    // a long time.
    volatile size_t *number;
    size_t num_threads;
    size_t _cxl_region_size;
};
