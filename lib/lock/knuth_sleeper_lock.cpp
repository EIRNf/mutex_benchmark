#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <new>

#define NOT_IN_CONTENTION 0
#define LOOPING 1
#define HAS_LOCK 2

class KnuthSleeperMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        size_t control_array_size = sizeof(volatile std::atomic_int) * num_threads;
        this->control = (volatile std::atomic_int*)malloc(control_array_size);
        // binary_semaphore is not trivially constructible: raw malloc memory
        // previously held garbage counters (UB — waits that never blocked or
        // never woke). Construct each one in place.
        this->sleeper = (std::binary_semaphore*)malloc(sizeof(std::binary_semaphore)*num_threads);
        for (size_t i = 0; i < num_threads; i++) {
            new (&this->sleeper[i]) std::binary_semaphore(0);
        }
        memset((void*)control, 0, control_array_size);
        this->k = 0;
        this->num_threads = num_threads;
    }

    void lock(size_t thread_id_) override {
        // In order for the loop to work correctly, j
        // must be a signed variable, so thread_id
        // must be signed as well to compare to it.
        // This should not produce instructions.
        ssize_t thread_id = thread_id_;
    beginning:
        control[thread_id] = LOOPING;      
        wake(thread_id);
    restart_loop:
        for (ssize_t j = k; j >= 0; j--) {
            if (j == thread_id) {
                goto end_of_loop;
            }
            if (control[j] != NOT_IN_CONTENTION) {
                // Bounded wait, not acquire(): several threads may park on
                // the same rival's semaphore, and its owner posts a single
                // permit per state change — a plain acquire() stranded all
                // but one of them forever (deterministic deadlock at >=4T).
                // The timeout turns a lost wakeup into a bounded re-check.
                sleeper[j].try_acquire_for(std::chrono::microseconds(50));
                goto restart_loop;
            }
        }
        for (ssize_t j = num_threads - 1; j >= 0; j--) {
            if (j == thread_id) {
                goto end_of_loop;
            }
            if (control[j] != NOT_IN_CONTENTION) {
                // sleeper[j].try_acquire();
                // sleeper[j].acquire();
                goto restart_loop;
            }
        }
    end_of_loop:
        control[thread_id] = HAS_LOCK;        
        wake(thread_id);

        for (ssize_t j = num_threads - 1; j >= 0; j--) {
            if (j != thread_id && control[j] == HAS_LOCK) {
                goto beginning;
            }
        }
        k = thread_id;
    }

    void wake(size_t thread_id){
        if (!sleeper[thread_id].try_acquire()){
        }
        sleeper[thread_id].release();

    }

    void unlock(size_t thread_id) override {
        if (thread_id == 0) {
            k = num_threads - 1;
        } else {
            k = thread_id - 1;
        }
        control[thread_id] = NOT_IN_CONTENTION;      
        wake(thread_id);
    }

    void destroy() override {
        free((void*)control);
        if (sleeper != nullptr) {
            for (size_t i = 0; i < num_threads; i++) {
                std::destroy_at(&sleeper[i]);
            }
            free((void*)sleeper);
            sleeper = nullptr;
        }
    }

    std::string name() override {
        return "knuth";
    }

private:
    volatile std::atomic_int *control;
    std::binary_semaphore *sleeper = nullptr;
    volatile std::atomic_int k;
    size_t num_threads;
};
