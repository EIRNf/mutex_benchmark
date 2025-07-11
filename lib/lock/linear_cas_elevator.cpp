#include "lock.hpp"
#include <stdexcept>
#include <atomic>

#include "spin_lock.hpp"
// TODO: explicit memory ordering.
// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.

class LinearCASElevatorMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        this->num_threads = num_threads;
        this->thread_n_given_lock = (volatile bool*)malloc(sizeof(volatile bool) * (num_threads + 1));
        this->thread_n_is_waiting = (volatile bool*)malloc(sizeof(volatile bool) * num_threads);
        for (size_t i = 0; i < num_threads; i++) {
            this->thread_n_given_lock[i] = false;
        }
        this->thread_n_given_lock[num_threads] = true;
        for (size_t i = 0; i < num_threads; i++) {
            this->thread_n_is_waiting[i] = false;
        }
    }

    void lock(size_t thread_id) override {
        thread_n_is_waiting[thread_id] = true;
        if (designated_waker_lock.trylock(thread_id)) {
            // This thread is the designated waker if 
            // a) the unlocker fucks up and doesn't realize anyone is waiting or
            // b) (more likely) if this thread is the first to get to the lock
            while (thread_n_given_lock[thread_id] == false && thread_n_given_lock[num_threads] == false) {
                // Busy wait
            }
            thread_n_given_lock[num_threads] = false;
            designated_waker_lock.unlock(thread_id);
        } else {
            while (thread_n_given_lock[thread_id] == false) {
                // Busy wait
            }
        }
        thread_n_given_lock[thread_id] = false;
        thread_n_is_waiting[thread_id] = false;
    }

    void unlock(size_t thread_id) override {
        // Cycle around the thread list to find the next successor
        // Start at 1 because we don't loop back to ourself.
        for (size_t offset = 1; offset < num_threads; offset++) {
            size_t next_successor_index = (thread_id + offset) % num_threads;
            if (thread_n_is_waiting[next_successor_index]) {
                thread_n_given_lock[next_successor_index] = true;
                return; // Successfully passed off to successor
            }
        }
        // No successor found.
        thread_n_given_lock[num_threads] = true;
    }

    void destroy() override {
        free((void*)thread_n_given_lock);
        free((void*)thread_n_is_waiting);
    }

    std::string name() override {
        return "linear_cas_elevator";
    }
private:
    SpinLock designated_waker_lock;
    volatile bool *thread_n_given_lock; // Should this be atomic?
    volatile bool *thread_n_is_waiting;
    size_t num_threads;
};