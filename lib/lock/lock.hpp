#ifndef LOCK_LOCK_HPP
#define LOCK_LOCK_HPP

#pragma once

#include <atomic>
#include <thread>

#include <vector>
#include <sched.h>
#include <sanitizer/tsan_interface.h>
#include <semaphore>

#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
    // asm fence. ADDED for ThreadSanitizer support. SHOULD NOT WORK, as tsan_acquire and tsan_release require a memory address.
    #if defined(__x86_64)
        #define Fence() {\
        __tsan_acquire(nullptr); \
        __tsan_release(nullptr); \
        __asm__ __volatile__ ( "lock; addq $0,128(%%rsp);" ::: "cc" );\
        }
    #elif defined(__i386)
        #define Fence() {\
            __tsan_acquire(nullptr); \
            __tsan_release(nullptr); \
        __asm__ __volatile__ ( "lock; addl $0,128(%%esp);" ::: "cc" );\
        }
    #elif defined(__ARM_ARCH)
        #define Fence() {\
        __tsan_acquire(nullptr); \
        __tsan_release(nullptr); \
        __asm__ __volatile__ ( "DMB ISH" ::: ); \
        }
    #else
        #error unsupported architecture
    #endif
# else
// asm fence
    #if defined(__x86_64)
        //#define Fence() __asm__ __volatile__ ( "mfence" )
        #define Fence() __asm__ __volatile__ ( "lock; addq $0,128(%%rsp);" ::: "cc" )
    #elif defined(__i386)
        #define Fence() __asm__ __volatile__ ( "lock; addl $0,128(%%esp);" ::: "cc" )
    #elif defined(__ARM_ARCH)
        #define Fence() __asm__ __volatile__ ( "DMB ISH" ::: ) 
    #else
        #error unsupported architecture
    #endif
#endif
#endif


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

    int criticalSection(size_t thread_id) {
        *currentId=thread_id;
        Fence();
        for (int i=0; i<100; i++){
            if (*currentId!= thread_id){
                throw std::runtime_error(name() + " was breached");
            }
        }
        return 1;
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

    inline void spin_delay_sched_yield() {
        sched_yield();
    }

    inline void spin_delay_exponential() {
        // Same as nsync
        static size_t attempts = 0;
        if (attempts < 7) {
            volatile int i;
            for (i = 0; i != 1 << attempts; i+=1);
        } else {
            std::this_thread::yield();
        }
        attempts++;
    }

    inline void spin_delay_linear() {
        static size_t delay = 5;
        volatile size_t i;
        for (i = 0; i != delay; i+=1);
        delay += 5;
    }

private:
    volatile unsigned int* currentId= (volatile unsigned int* )malloc(sizeof(unsigned int *));

};

#endif // LOCK_LOCK_HPP