#ifndef LAMPORT_LOCK_HPP
#define LAMPORT_LOCK_HPP

#include "lock.hpp"
#include <stdexcept>

class LamportLock : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        this->x = (volatile size_t*)malloc(sizeof(size_t));;
        this->y = (volatile size_t*)malloc(sizeof(size_t));;
        this->b=(volatile bool*)malloc(sizeof(bool) * num_threads);
        this->fast=(volatile bool)malloc(sizeof(bool));
        fast=false;
        this->num_threads = num_threads;
    }

    void lock(size_t thread_id) override {
    start:
        b[thread_id] = true; //trying to grab the lock
        *x = thread_id+1; //first confirmation
        Fence();

        if (*y!=0){
            b[thread_id] = false; //no longer going for the lock
            while (*y!=0){} //wait for whoever was trying to get it to get it
            goto start; //restart
        }

        *y = thread_id + 1; //second confirmation
        Fence();

        if (*x!=thread_id+1){ //someone started going for the lock
            b[thread_id] = false; //not longer going for the lock

            for (int j=0; j<(int)num_threads; j++){while(b[j]){}} //wait for contention to go down
            Fence();

            if (*y!=thread_id+1){ //while waiting, someone messed with second confirmation
                while(*y!=0){} //wait for the person to unlock
                goto start;
            }
        }
        Fence();
    }
    
    bool trylock(size_t thread_id){
        b[thread_id] = true; //trying to grab the lock
        *x = thread_id+1; //first confirmation
        Fence();

        if (*y!=0){
            b[thread_id] = false; //no longer going for the lock
            return false; //wait for whoever was trying to get it to get it
        }

        *y = thread_id + 1; //second confirmation
        Fence();

        if (*x!=thread_id+1){ //someone started going for the lock
            b[thread_id] = false; //not longer going for the lock
            Fence();
            for (int j=0; j<(int)num_threads; j++){while(b[j]){}} //wait for contention to go down


            if (*y!=thread_id+1){ //while waiting, someone messed with second confirmation
                return false; //wait for the person to unlock
            }
        }
        bool leader = false;
        if (!fast){
            leader=true;
            fast=leader;
        }
        *y=0;
        b[thread_id]=false;
        return leader;
    }

    void unlock(size_t thread_id) override {
        Fence();
        *y=0;
        Fence();
        b[thread_id] = false;
        fast=false;
    }

    void destroy() override {
        free((void*)b);
        free((void*)x);
        free((void*)y);
    }

    std::string name() override {return "lamport";}
private:
    volatile bool* b;
    volatile size_t *x;
    volatile size_t *y;

    volatile bool fast;

    size_t num_threads;
};

#endif // LAMPORT_LOCK_HPP