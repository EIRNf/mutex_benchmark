#include "lock.hpp"
#include <stdexcept>

class PetersonMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        this->level         = (volatile std::atomic<int   >*)ALLOCATE(sizeof(volatile std::atomic<int   >) * num_threads);
        this->last_to_enter = (volatile std::atomic<size_t>*)ALLOCATE(sizeof(volatile std::atomic<size_t>) * num_threads); // Does not need initialization.
        for (size_t i = 0; i < num_threads; i++) {
            this->level[i] = -1;
        }
        this->num_threads = num_threads;
    }

    void lock(size_t thread_id) override {
        for (size_t my_level = 0; my_level < num_threads - 1; my_level++) {
            // printf("%ld: my_level=%ld\n", thread_id, my_level);
            level[thread_id] = my_level;
            last_to_enter[my_level] = thread_id;
            while (true) {
                if (last_to_enter[my_level] != thread_id) {
                    goto next_level;
                }
                bool other_thread_higher = false;
                for (size_t other_thread_id = 0; other_thread_id < num_threads; other_thread_id++) {
                    if (other_thread_id != thread_id && level[other_thread_id] >= level[thread_id]) {
                        other_thread_higher = true;
                    }
                }
                if (!other_thread_higher) {
                    goto next_level;
                }
            }
        next_level:;
        }
        // printf("%ld: Locked.\n", thread_id);
    }

    void unlock(size_t thread_id) override {
        level[thread_id] = -1;
        // printf("%ld: Unlocked.\n", thread_id);
    }

    void destroy() override {
        FREE((void*)level, sizeof(volatile std::atomic<int>) * num_threads);
        FREE((void*)last_to_enter, sizeof(volatile std::atomic<size_t>) * num_threads);
    }

    std::string name() override {
        return "peterson";
    }

private:
    // Could use something smaller than size_t here
    volatile std::atomic<int> *level;
    volatile std::atomic<size_t> *last_to_enter;
    size_t num_threads;
};