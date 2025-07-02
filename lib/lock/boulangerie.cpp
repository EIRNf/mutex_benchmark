#include "lock.hpp"
#include <stdexcept>


class Boulangerie : public virtual SoftwareMutex {
public:
    std::string name() override { return "bakery"; }
    void init(size_t num_threads) override {
        this->num_threads = num_threads;
        this->choosing = (volatile int*)malloc(num_threads * sizeof(int));
        this->number = (volatile int*)malloc(num_threads * sizeof(int));
        for (size_t i = 0; i < num_threads; i++) {
            choosing[i] = 0;
            number[i] = 0;
        }
    }


    void lock(size_t thread_id) override {
        choosing[thread_id] = 1;
        int max_number = 0;
        for (size_t i = 0; i < num_threads; ++i) {
            if (number[i] > max_number) {
                max_number = number[i];
            }
        }
        number[thread_id] = max_number + 1;
        choosing[thread_id] = 0;
        
        //limit the number of threads to check
        size_t limit;
        if (number[thread_id] == 1 && thread_id > 0) {limit = thread_id;}
        else {limit = num_threads;}

        for (size_t j = 0; j < limit; ++j) {
            if (j == thread_id) continue;
            while (choosing[j]) {}
            int prev_j = number[j];
            int curr_j = number[j];
            while (curr_j != 0 && (curr_j < number[thread_id] || (curr_j == number[thread_id] && j < thread_id)) && curr_j == prev_j) {
                    prev_j = curr_j;
                    curr_j = number[j];
            }
        }
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
    volatile int *choosing;
    volatile int *number;
};


