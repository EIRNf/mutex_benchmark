#include "lock.hpp"
#include <stdexcept>


class Boulangerie : public virtual SoftwareMutex {
public:
    std::string name() override { return "boulangerie"; }
    void init(size_t num_threads) override {
        this->num_threads = num_threads;
        this->choosing = (volatile bool*)malloc(num_threads * sizeof(volatile bool));
        this->number = (volatile int*)malloc(num_threads * sizeof(volatile int));
        for (size_t i = 0; i < num_threads; i++) {
            choosing[i] = 0;
            number[i] = 0;
        }
    }

    void lock(size_t thread_id) override {
        choosing[thread_id] = 1;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        int max_number = 0;
        for (size_t i = 0; i < num_threads; ++i) {
            if (number[i] > max_number) {
                max_number = number[i];
            }
        }
        number[thread_id] = max_number + 1;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        choosing[thread_id] = 0;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        
        //limit the number of thread to check
        size_t limit;
        if (number[thread_id] == 1 && thread_id > 0) {limit = thread_id;}
        else {limit = num_threads;}

        int prev_j, curr_j;
        for (size_t j = 0; j < limit; ++j) {
            if (j == thread_id) continue;
            while (choosing[j]) {}
            prev_j = number[j];
            curr_j = number[j];
            while (curr_j != 0 && (curr_j < number[thread_id] || (curr_j == number[thread_id] && j < thread_id)) && curr_j == prev_j) {
                    prev_j = curr_j;
                    curr_j = number[j];
            }
        }
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    void unlock(size_t thread_id) override {
        number[thread_id] = 0;
    }

    void destroy() override {
        free((void *)choosing);
        free((void *)number);
    }


private:
    size_t num_threads;
    volatile bool *choosing;
    volatile int *number;
};


