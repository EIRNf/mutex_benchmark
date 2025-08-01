#include "lock.hpp"
#include "../utils/cxl_utils.hpp"
#include <stdexcept>
#include <atomic>
#include <cstring>

#define NOT_IN_CONTENTION 0
#define LOOPING 1
#define HAS_LOCK 2

class KnuthMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        _cxl_region_size = sizeof(std::atomic_int) * (num_threads + 1);
        _cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);

        this->k = (std::atomic_int*)&_cxl_region[0];
        this->control = (volatile std::atomic_int*)&_cxl_region[sizeof(std::atomic_int)];

        memset((void*)control, 0, sizeof(std::atomic_int) * num_threads);
        *this->k = 0;
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
    restart_loop:
        for (ssize_t j = *k; j >= 0; j--) {
            if (j == thread_id) {
                goto end_of_loop;
            }
            if (control[j] != NOT_IN_CONTENTION) {
                goto restart_loop;
            }
        }
        for (ssize_t j = num_threads - 1; j >= 0; j--) {
            if (j == thread_id) {
                goto end_of_loop;
            }
            if (control[j] != NOT_IN_CONTENTION) {
                goto restart_loop;
            }
        }
    end_of_loop:
        control[thread_id] = HAS_LOCK;
        for (ssize_t j = num_threads - 1; j >= 0; j--) {
            if (j != thread_id && control[j] == HAS_LOCK) {
                goto beginning;
            }
        }
        *k = thread_id;
    }

    void unlock(size_t thread_id) override {
        if (thread_id == 0) {
            *k = num_threads - 1;
        } else {
            *k = thread_id - 1;
        }
        control[thread_id] = NOT_IN_CONTENTION;
    }

    void destroy() override {
        FREE((void*)_cxl_region, _cxl_region_size);
    }

    std::string name() override {
        return "knuth";
    }

private:
    volatile char *_cxl_region;
    volatile std::atomic_int *control;
    volatile std::atomic_int *k;
    size_t num_threads;
    size_t _cxl_region_size;
};
