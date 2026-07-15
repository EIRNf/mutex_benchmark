#include "../utils/cxl_utils.hpp"
#include "../utils/bench_utils.hpp"
#include "lock.hpp"
#include <stdexcept>
#include <cstring>
#include <new>

class BakeryNonAtomicMutex : public virtual SoftwareMutex {
public:
    struct ThreadData {
        volatile int number;
        volatile bool choosing;
    };

    void init(size_t num_threads) override {
        _cxl_region_size = std::hardware_destructive_interference_size * num_threads;
        this->_cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);
        this->num_threads = num_threads;
        memset((void*)this->_cxl_region, 0, _cxl_region_size);
    }

    inline ThreadData *get(size_t thread_id) {
        return (ThreadData*)&this->_cxl_region[thread_id * std::hardware_destructive_interference_size];
    }

    void lock(size_t thread_id) override {
        // Get "bakery number"
        ThreadData *td = get(thread_id);
        td->choosing = true;
        Fence();
        size_t my_bakery_number = 1;
        ThreadData *other_thread;
        for (size_t i = 0; i < num_threads; i+=1) {
            other_thread = get(i);
            size_t other_number = static_cast<size_t>(other_thread->number);
            if (other_number + 1 > my_bakery_number) {
                my_bakery_number = other_number + 1;
            }
        }
        
        td->number = my_bakery_number;
        Fence();
        td->choosing = false;
        Fence();

        // Lock waiting part
        for (size_t j = 0; j < num_threads; j++) {
            other_thread = get(j);
            while (other_thread->choosing != 0) {
                // Wait for that thread to be done choosing a number.
                // nanosleep(&nanosleep_timespec, &remaining);
            }
            while ((other_thread->number != 0 && other_thread->number < td->number) 
                || (other_thread->number == td->number && j < thread_id)) {
                // Stall until our bakery number is the lowest..
                // nanosleep(&nanosleep_timespec, &remaining);
            }
        }
        
        Fence();
    }
    void unlock(size_t thread_id) override {
        get(thread_id)->number = 0;
    }

    void destroy() override {
        FREE((void*)_cxl_region, _cxl_region_size);
    }

    std::string name() override {
        return "bakery_nonatomic";
    }

private:
    volatile char *_cxl_region;
    size_t num_threads;
    size_t _cxl_region_size;
};
