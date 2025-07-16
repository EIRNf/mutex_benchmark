#ifndef LOCK_LOCK_HPP
#define LOCK_LOCK_HPP

#pragma once

#include <atomic>
#include <thread>
#include <semaphore>
#include "../utils/bench_utils.hpp"

class SoftwareMutex {
public:
    SoftwareMutex() = default;
    virtual ~SoftwareMutex() = default;

    // Initialize the mutex for a given number of threads
    virtual void init(size_t num_threads) = 0;

    // Lock the mutex for the calling thread (thread_id)
    virtual void lock(size_t thread_id) = 0;

    // Unlock the mutex for the calling thread (thread_id)
    virtual void unlock(size_t thread_id) = 0;

    // Cleanup any resources used by the mutex
    virtual void destroy() = 0;

    void criticalSection(size_t thread_id) {
        *currentId=thread_id;
        Fence();
        for (int i=0; i<100; i++){
            if (*currentId!= thread_id){
                throw std::runtime_error(name() + " was breached");
            }
        }

    }

    void sleep() {
        sleeper.acquire();
    }

    void wake(){
        if(!sleeper.try_acquire()){
        }
            sleeper.release();
    }

    std::binary_semaphore sleeper{0};

    virtual std::string name() =0;
private:
    volatile unsigned int* currentId= (volatile unsigned int* )malloc(sizeof(unsigned int *));
};

#endif // LOCK_LOCK_HPP