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
        local_nodes =  (std::atomic<std::atomic_bool*>*)malloc(sizeof(std::atomic<std::atomic_bool*>) * num_threads);
        for (size_t i = 0; i < num_threads; i++) {
            local_nodes[i] = nullptr; // TODO use atomic_init?
        }
    }

    void lock(size_t thread_id) override {
        (void)thread_id;
        // This code handles the case where this is the first time the lock is locked.
        // TODO: possible deadlock if we set the pointer when the unlocker has already passed us in the loop
        // and we never got woken up!!!
        // Solution 1: check next_thread_id == -1 (or some boolean) every wait iteration (bad, concurrent memory access)
        // Solution 2: spinlock for modifying internal mutex state (bad, concurrent memory access)
        // Solution 3: each new locking thread changes the ticket number, which notifies the unlocker that it needs to recheck. (doesn't work still)

        // TODO: replace this lock.
        lock_internal();
            std::atomic_bool* first_in_line = local_nodes[start&modulo_mask];
            if (first_in_line == nullptr) {
                // technically could just set to any nonzero value.
                local_nodes[end.fetch_add(1)&modulo_mask] = &this->has_priority;
                unlock_internal();
                return;
            }
            // *first_in_line = true; // Un-deadlock the first thread in line.
            this->has_priority = false;
            local_nodes[end.fetch_add(1)&modulo_mask] = &this->has_priority;
        unlock_internal();
        while (!this->has_priority) { // TODO: memory ordering
            spin_delay_exponential(); // Busy wait
        }
    }

    void unlock(size_t thread_id) override {
        (void)thread_id;
        lock_internal();
            // As soon as this happens, start == end if the queue is empty.
            // todo: these don't need to be protected by the lock? (switch to atomic_flag probably)
            size_t my_ring_index = start.fetch_add(1); // what memory order should this be? release?
            local_nodes[my_ring_index&modulo_mask] = nullptr;
            // Technically this could get reset to true by another thread trying to
            // un-deadlock the system, but it doesn't matter because it will be reset if this thread locks again.
            this->has_priority = false; 
            
            // If we naively made this check and then returned from the function if the queue was empty,
            // it would create a race condition where this check happens as local_nodes[start] is set.
            std::atomic_bool *first_node = local_nodes[start&modulo_mask];
        unlock_internal();
        if (first_node != nullptr) {
            *first_node = true;
        }
    }

    // inline std::atomic<std::atomic_bool*>* lock_internal() {
    //     std::atomic<std::atomic_bool*>* local_nodes_ptr;
    //     do {
    //         local_nodes_ptr = this->local_nodes.exchange(nullptr, std::memory_order_acquire);
    //     } while (local_nodes_ptr == nullptr);
    //     return local_nodes_ptr;
    // }

    // inline void unlock_internal(std::atomic<std::atomic_bool*>* local_nodes_ptr) {
    //     this->local_nodes = local_nodes_ptr;
    // }

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
        return "djikstra";
    }

private:
    // static volatile std::atomic_flag internal_lock;
    // This variable is also a spinlock set to nullptr when in use.
    // TODO: which approach is better: nullptr swap or using another variable?
    static std::atomic_flag internal_lock;
    static std::atomic<std::atomic_bool*>* local_nodes;
    static std::atomic<size_t> start;
    static std::atomic<size_t> end;
    static thread_local std::atomic_bool has_priority;
    static size_t num_threads; // Guaranteed to be power of 2.
    static size_t modulo_mask; // num_threads - 1
};
std::atomic_flag RingTicketMutex::internal_lock = ATOMIC_FLAG_INIT;
std::atomic<std::atomic_bool*>* RingTicketMutex::local_nodes;
std::atomic<size_t> RingTicketMutex::start = 0;
std::atomic<size_t> RingTicketMutex::end = 0;
thread_local std::atomic_bool RingTicketMutex::has_priority = false;
size_t RingTicketMutex::num_threads; // Guaranteed to be power of 2.
size_t RingTicketMutex::modulo_mask;
// std::atomic_flag ThreadlocalTicketMutex::internal_lock;