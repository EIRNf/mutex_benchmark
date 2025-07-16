#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <cstring>

#define NOT_IN_CONTENTION 0
#define LOOPING 1
#define HAS_LOCK 2

class KnuthSleeperMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        size_t control_array_size = sizeof(volatile std::atomic_int) * num_threads;
        this->control = (volatile std::atomic_int*)malloc(control_array_size);
        this->sleeper = (std::binary_semaphore*)malloc(sizeof(std::binary_semaphore)*num_threads);
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
                // sleeper[j].try_acquire();
                sleeper[j].acquire();
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
    }

    std::string name() override {
        return "knuth";
    }

private:
    volatile std::atomic_int *control;
    std::binary_semaphore *sleeper;
    volatile std::atomic_int k;
    size_t num_threads;
};
