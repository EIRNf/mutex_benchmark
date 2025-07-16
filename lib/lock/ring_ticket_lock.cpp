#include "lock.hpp"
#include <stdexcept>
#include <atomic>

// #define RING_START (start&modulo_mask)
// #define RING_END (end&modulo_mask)
#define SENTINEL ((std::atomic_bool*)1)

// This mutex is bad and deadlocks.
class RingTicketMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        // We round up to the nearest power of 2 for extra space and to make
        // it easier to divide.
        // The array size has to be at least 1 more than the number of threads 
        // so that start == end means the array is empty and not all the way full 
        size_t nearest_power_of_2 = 1;
        while (nearest_power_of_2 < num_threads + 1) {
            nearest_power_of_2 *= 2;
        }
        num_threads = nearest_power_of_2;

        this->num_threads = num_threads;
        this->modulo_mask = num_threads - 1;
        // For some reason this needs two casts to work.
        local_nodes =  (std::atomic<volatile bool*>*)malloc(sizeof(std::atomic<volatile bool*>) * num_threads);
        for (size_t i = 0; i < num_threads; i++) {
            local_nodes[i] = nullptr; // TODO use atomic_init?
        }
    }

    void lock(size_t thread_id) override {
        (void)thread_id;
        // printf("%ld: Locking...\n", thread_id);

        this->has_priority = false;
        local_nodes[end.fetch_add(1)&modulo_mask] = &this->has_priority;
        if (trylock_internal()) {
            // We are the designated waker.
            while (!this->has_priority && !empty) { 
                // spin_delay_exponential(); // Busy wait
            }
            empty = false;
            unlock_internal();
        } else {
            while (!this->has_priority) { // TODO: memory ordering
                // spin_delay_exponential(); // Busy wait
            }
        }
        this->has_priority = false;
        // printf("%ld: Locked\n", thread_id);
    }

    void unlock(size_t thread_id) override {
        // printf("%ld: Unlocking...\n", thread_id);
        (void)thread_id;
        // As soon as this happens, start == end if the queue is empty.
        // todo: these don't need to be protected by the lock? (switch to atomic_flag probably)
        size_t my_ring_index = start.fetch_add(1);
        local_nodes[my_ring_index&modulo_mask] = nullptr;
        
        // If we naively made this check and then returned from the function if the queue was empty,
        // it would create a race condition where this check happens as local_nodes[start] is set.
        volatile bool *first_node = local_nodes[start&modulo_mask];
        if (first_node != nullptr) {
            *first_node = true;
        } else {
            empty = true;
        }
        // printf("%ld: Unlocked\n", thread_id);
    }

    inline bool trylock_internal() {
        return !internal_lock.test_and_set();
    }

    inline void lock_internal() {
        while (internal_lock.test_and_set(std::memory_order_acquire));
    }

    inline void unlock_internal() {
        internal_lock.clear(std::memory_order_release);
    }

    void destroy() override {
        free((void*)local_nodes);
    }

    std::string name() override {
        return "ring_ticket";
    }

private:
    // static volatile std::atomic_flag internal_lock;
    // This variable is also a spinlock set to nullptr when in use.
    // TODO: which approach is better: nullptr swap or using another variable?
    static std::atomic_flag internal_lock;
    static std::atomic_bool empty;
    static std::atomic<volatile bool*>* local_nodes;
    static std::atomic<size_t> start;
    static std::atomic<size_t> end;
    static thread_local volatile bool has_priority;
    static size_t num_threads; // Guaranteed to be power of 2.
    static size_t modulo_mask; // num_threads - 1
};
std::atomic_flag RingTicketMutex::internal_lock = ATOMIC_FLAG_INIT;
// Double pointer prevents false sharing?
std::atomic<volatile bool*>* RingTicketMutex::local_nodes;
std::atomic<size_t> RingTicketMutex::start = 0;
std::atomic<size_t> RingTicketMutex::end = 0;
thread_local volatile bool RingTicketMutex::has_priority = false;
size_t RingTicketMutex::num_threads; // Guaranteed to be power of 2.
size_t RingTicketMutex::modulo_mask;
std::atomic_bool RingTicketMutex::empty = true;