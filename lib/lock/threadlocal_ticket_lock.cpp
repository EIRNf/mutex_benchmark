#include "lock.hpp"
#include <stdexcept>
#include <atomic>

class ThreadlocalTicketMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        this->num_threads = num_threads;
        // For some reason this needs two casts to work.
        local_nodes_lock = (volatile std::atomic<std::atomic_bool**>)(std::atomic_bool**)malloc(sizeof(std::atomic_bool *) * num_threads);
        for (size_t i = 0; i < num_threads; i++) {
            local_nodes_lock[i] = nullptr; // TODO use atomic_init
        }
    }

    void lock(size_t thread_id) override {
        // This code handles the case where this is the first time the lock is locked.
        // TODO: possible deadlock if we set the pointer when the unlocker has already passed us in the loop
        // and we never got woken up!!!
        // Solution 1: check next_thread_id == -1 (or some boolean) every wait iteration (bad, concurrent memory access)
        // Solution 2: spinlock for modifying internal mutex state (bad, concurrent memory access)
        // Solution 3: each new locking thread changes the ticket number, which notifies the unlocker that it needs to recheck. (doesn't work still)

        if (next_thread_id == -1) {
            ssize_t expected = -1;
            if (next_thread_id.compare_exchange_strong(expected, thread_id, std::memory_order_acquire)) {
                // Successfully locked.
                return;
            }
        }
        
        this->has_priority = false;
        std::atomic_bool **local_nodes = lock_internal();
        local_nodes[thread_id] = &(this->has_priority);
        unlock_internal(local_nodes);
        while (!this->has_priority) {
            // Busy wait
        }
    }

    void unlock(size_t thread_id_) override {
        ssize_t thread_id = thread_id_;

        std::atomic_bool **local_nodes = lock_internal();
        local_nodes[thread_id] = nullptr;
        this->has_priority = false;

        // We don't need to worry about interleaving accesses to next_thread_id here
        // because it's only used by the unlocker
        // We start at next_thread_id and go around to prevent starvation, which would happen if thread_id == 0 was repeatedly taking the lock
        // and this loop always started at 0 and went to the end.
        for (next_thread_id = (next_thread_id + 1) % num_threads; next_thread_id != thread_id; next_thread_id = (next_thread_id + 1) % num_threads) {
            if (local_nodes[next_thread_id] != nullptr) {
                *local_nodes[next_thread_id] = true;
                unlock_internal(local_nodes);
                return;
            }
        }

        // No successor found.
        // See deadlock case above.
        unlock_internal(local_nodes);
        next_thread_id = -1;
    }

    inline std::atomic_bool **lock_internal() {
        std::atomic_bool **local_nodes_ptr;
        do {
            local_nodes_ptr = this->local_nodes_lock.exchange(nullptr, std::memory_order_acquire);
        } while (local_nodes_ptr == nullptr);
        return local_nodes_ptr;
    }

    inline void unlock_internal(std::atomic_bool **local_nodes_ptr) {
        (void)local_nodes_ptr;
        this->local_nodes_lock = local_nodes_ptr;
    }

    void destroy() override {
        free((void*)local_nodes_lock);
    }

    std::string name() override {
        return "djikstra";
    }

private:
    // This variable is also a spinlock set to nullptr when in use.
    // static volatile std::atomic_flag internal_lock;
    static volatile std::atomic<std::atomic_bool**> local_nodes_lock;
    static std::atomic<ssize_t> next_thread_id;
    static thread_local std::atomic_bool has_priority;
    static ssize_t num_threads;
};
volatile std::atomic<std::atomic_bool**> ThreadlocalTicketMutex::local_nodes_lock;
thread_local std::atomic_bool ThreadlocalTicketMutex::has_priority = false;
std::atomic<ssize_t> ThreadlocalTicketMutex::next_thread_id = -1;
ssize_t ThreadlocalTicketMutex::num_threads;
// volatile std::atomic_flag ThreadlocalTicketMutex::internal_lock;