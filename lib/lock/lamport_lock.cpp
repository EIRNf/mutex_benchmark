#include "lock.hpp"
#include "../utils/cxl_utils.hpp"
#include <stdexcept>
#include <iostream>


class LamportLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        // size_t x_size = 
        _cxl_region = (volatile char*)ALLOCATE(sizeof(size_t) * 2 + sizeof(bool) * num_threads);

        this->x = (volatile size_t*)&_cxl_region[0];
        this->y = (volatile size_t*)&_cxl_region[sizeof(size_t)];
        this->b = (volatile bool*)&_cxl_region[sizeof(size_t) * 2];
        this->num_threads = num_threads;
    }

    void lock(size_t thread_id) override {
    start:
        b[thread_id] = true; //trying to grab the lock
        *x = thread_id+1; //first confirmation
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (*y!=0){
            b[thread_id] = false; //no longer going for the lock
            while (*y!=0){} //wait for whoever was trying to get it to get it
            goto start; //restart
        }

        *y = thread_id + 1; //second confirmation
        std::atomic_thread_fence(std::memory_order_seq_cst);

        if (*x!=thread_id+1){ //someone started going for the lock
            b[thread_id] = false; //not longer going for the lock
            for (size_t j=0; j<num_threads; j++){while(b[j]){}} //wait for contention to go down
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (*y!=thread_id+1){ //while waiting, someone messed with second confirmation
                while(*y!=0){} //wait for the person to unlock
                goto start;
            }
        }
    }
    
    void unlock(size_t thread_id) override {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        *y=0;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        b[thread_id] = false;
    }

    void destroy() override {
        FREE((void*)_cxl_region, sizeof(size_t) * 2 + sizeof(bool) * num_threads);
    }

    std::string name(){return "lamport";}
private:
    volatile char *_cxl_region;
    volatile bool* b;
    volatile size_t *x;
    volatile size_t *y;

    size_t num_threads;
};
