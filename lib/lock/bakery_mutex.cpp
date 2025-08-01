#include "lock.hpp"
#include "../utils/cxl_utils.hpp"
#include <stdexcept>
#include <atomic>

class BakeryMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        size_t number_size = sizeof(std::atomic<size_t>) * num_threads;
        size_t choosing_size = sizeof(std::atomic_bool) * num_threads;
        _cxl_region_size = number_size + choosing_size;
        _cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);

        this->number = (volatile std::atomic<size_t>*)&_cxl_region[0];
        this->choosing = (volatile std::atomic_bool*)&_cxl_region[number_size];

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
        size_t my_bakery_number = 1;
        for (size_t i = 0; i < num_threads; i++) {
            if (number[i] + 1 > my_bakery_number) {
                my_bakery_number = number[i] + 1;
            }
        }
        number[thread_id] = my_bakery_number;
        choosing[thread_id] = false;
        // Lock waiting part
        for (size_t j = 0; j < num_threads; j++) {
            while (choosing[j] != 0) {
                // Wait for that thread to be done choosing a number.
            }
            while ((number[j] != 0 && number[j] < number[thread_id]) 
                || (number[j] == number[thread_id] && j < thread_id)) {
                // Stall until our bakery number is the lowest..
            }
        }
    }
    void unlock(size_t thread_id) override {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        number[thread_id] = 0;
    }
    void destroy() override {
        FREE((void*)_cxl_region, _cxl_region_size);
    }

    std::string name() override {
        return "bakery";
    }

private:
    volatile char *_cxl_region;
    volatile std::atomic_bool *choosing;
    // Note: Mutex will fail if this number overflows,
    // which happens if the "bakery" remains full for
    // a long time.
    volatile std::atomic<size_t> *number;
    size_t num_threads;
    size_t _cxl_region_size;
};
