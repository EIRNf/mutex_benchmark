#include "lock.hpp"
#include <stdexcept>
#include <atomic>

class BakeryMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        this->choosing = (volatile std::atomic_bool*)malloc(sizeof(std::atomic_bool) * num_threads);
        this->number = (volatile std::atomic<size_t>*)malloc(sizeof(std::atomic<size_t>) * num_threads);
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
        size_t my_bakery_number;
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
                // nanosleep(&nanosleep_timespec, &remaining);
            }
            while ((number[j] != 0 && number[j] < number[thread_id]) 
                || (number[j] == number[thread_id] && j < thread_id)) {
                // Stall until our bakery number is the lowest..
                // nanosleep(&nanosleep_timespec, &remaining);
            }
        }
    }
    void unlock(size_t thread_id) override {
        number[thread_id] = 0;
    }
    void destroy() override {
        free((void*)choosing);
        free((void*)number);
    }

    std::string name() override {
        return "bakery";
    }
private:
    volatile std::atomic_bool *choosing;
    // Note: Mutex will fail if this number overflows,
    // which happens if the "bakery" remains full for
    // a long time.
    volatile std::atomic<size_t> *number;
    struct timespec remaining;
    size_t num_threads;
};
