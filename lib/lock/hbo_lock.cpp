// hbo lock: A hierarchial test and set back off lock designed for NUMA systems
// This lock optimizes NUMA architecture and reduces cache invalidations and bus traffic
// Threads in the same NUMA node get more priority for acquiring the lock then threas in other NUMA nodes
// Priority is given through different backoff intervals

#include "lock.hpp"
#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <limits>

class hbo_lock : public SoftwareMutex {
private:

    // A special value that indicates that the lock is free
    static const unsigned long FREE = std::numeric_limits<unsigned long>::max(); 

    // Constants for backoff for local and remote spinning
    static const int BACKOFF_BASE = 10;
    static const int BACKOFF_CAP = 1000;
    static const int REMOTE_BACKOFF_BASE = 50;
    static const int REMOTE_BACKOFF_CAP = 5000;
    static const int GET_ANGRY_LIMIT = 10;

    std::atomic<unsigned long> lock_; // Globlal lock holder, represented by ID
    size_t num_threads_;
    std::atomic<unsigned long>* is_spinning_; // Shared state per node for spinning threads
    size_t num_nodes_;
    std::vector<size_t> thread_node_ids_;  // Maps thread IDs to node IDs

    // Identify which NUMA node a thread belongs to 
    size_t get_node_id(size_t thread_id) const {
        return thread_id % num_nodes_;
    }

    // The method for calculating backoff time 
    // This is to reduce contention and give local threads priority
    void backoff(int* b, int cap) {
        for (int i = 0; i < *b; ++i) {
            std::this_thread::yield();
        }
        *b = std::min(*b * 2, cap);
    }

    // The thread firsts tries to acquire the lock using a simple CAS
    //If that fails, it enters a slow path where it spins, either locally or remotely
    // This ensures that the lock is NUMA aware and reduces contention
    void hbo_acquire_slowpath(size_t thread_id, unsigned long tmp) {
        int b;
        int get_angry = 0;
        size_t my_node = get_node_id(thread_id);

    start:
        if (tmp == my_node) {
            // If the lock is held by the same node, we can be more aggressive towards acquiring it
            b = BACKOFF_BASE;
            while (true) {
                backoff(&b, BACKOFF_CAP);
                unsigned long expected = FREE;
                if (lock_.compare_exchange_strong(expected, my_node, std::memory_order_acquire)) {
                    return;
                }
                tmp = lock_.load(std::memory_order_relaxed);

                // If the lock is transfered to another node
                // during this time, we need to restart the process
                if (tmp != my_node) {
                    backoff(&b, BACKOFF_CAP);
                    goto restart;
                }
            }
        } else {
            // If the lock is held by a different node, the thread applies a less aggressive backoff strategy
            b = REMOTE_BACKOFF_BASE;

            is_spinning_[my_node].store(lock_.load(std::memory_order_relaxed), std::memory_order_relaxed);

            while (true) {
                backoff(&b, REMOTE_BACKOFF_CAP);
                unsigned long expected = FREE;
                if (lock_.compare_exchange_strong(expected, my_node, std::memory_order_acquire)) {
                    is_spinning_[my_node].store(FREE, std::memory_order_relaxed);
                    return;
                }
                tmp = lock_.load(std::memory_order_relaxed);

                // If during this time, the lock is transferred to the same node
                // we need to restart the process
                if (tmp == my_node) {
                    is_spinning_[my_node].store(FREE, std::memory_order_relaxed);
                    goto restart;
                }

                // After a certain amount of time, the thread gets "angry"
                // and starts to contend more aggressively
                // this feature ensures fairness and prevents starvation
                if (++get_angry >= GET_ANGRY_LIMIT) {
                    b = BACKOFF_BASE; 
                }
            }
        }

    restart:

        //tries to acquire the lock again
        //If it fails, it goes back to the start
        tmp = lock_.load(std::memory_order_relaxed);
        if (tmp == FREE) {
            unsigned long expected = FREE;
            if (lock_.compare_exchange_strong(expected, my_node, std::memory_order_acquire)) {
                return;
            }
        }
        goto start;
    }

public:
    hbo_lock() : lock_(FREE), num_threads_(0), is_spinning_(nullptr), num_nodes_(4) {}

    ~hbo_lock() {
        delete[] is_spinning_;
    }

    void init(size_t num_threads) override {
        num_threads_ = num_threads;
        thread_node_ids_.resize(num_threads);

        delete[] is_spinning_;
        is_spinning_ = new std::atomic<unsigned long>[num_nodes_]; 

        for (size_t i = 0; i < num_threads_; ++i) {
            thread_node_ids_[i] = get_node_id(i);
        }

        for (size_t i = 0; i < num_nodes_; ++i) {
            is_spinning_[i].store(FREE, std::memory_order_relaxed);
        }
    }

    void lock(size_t thread_id) override {
        size_t my_node = get_node_id(thread_id);
        unsigned long expected = FREE;

        // Try to acquire the lock diretly using a CAS operation
        // If it fails, enter the slow path 
        if (lock_.compare_exchange_strong(expected, my_node, std::memory_order_acquire)) {
            return;
        }

        unsigned long observed = lock_.load(std::memory_order_relaxed);
        hbo_acquire_slowpath(thread_id, observed);
    }

    void unlock(size_t thread_id) override {
        (void)thread_id;
        lock_.store(FREE, std::memory_order_release);
    }

    void destroy() override {
        delete[] is_spinning_;
        is_spinning_ = nullptr;
    }

    std::string name() override {
        return "hbo_lock";
    }
};

