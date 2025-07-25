// CTicket: A NUMA Aware cohort based Ticket lock.
// Threads first acquire a local Ticket lock, which is per NUMA node, before attempting
// to acquire a global Ticket lock. In order to acquire the lock a thread must own local & global lock

#include "lock.hpp"
#include <atomic>
#include <vector>
#include <memory>
#include <thread>

class CohortTicket : public SoftwareMutex {
private:
    
    //created a private ticket lock struct to represent a standard ticket lock for both the local and global lock 
    struct TicketLock {
        std::atomic<size_t> next_ticket{0};
        std::atomic<size_t> now_serving{0};

        //aquires the lock by waiting for its number to be served
        void lock(size_t thread_id) {
            (void)thread_id;
            size_t my_ticket = next_ticket.fetch_add(1, std::memory_order_relaxed);
            while (now_serving.load(std::memory_order_acquire) != my_ticket) {
                std::this_thread::yield(); 
            }
        }

        void unlock(size_t thread_id) {
            (void)thread_id;
            now_serving.fetch_add(1, std::memory_order_release);
        }

        //this returns true if no other threads are waiting for the lock
        //used by the last thread in the cohort to determine if it can release the global lock
        //and transfer ownership to a different cohort
        bool alone() const {
            return next_ticket.load(std::memory_order_relaxed) == 
                   now_serving.load(std::memory_order_relaxed) + 1;
        }
    };

    // represents a cohort of threads, or a NUMA node
    // each cohort has a local ticket lock and a flag to indicate if it holds the global
    struct Cohort {
        TicketLock local_lock;
        std::atomic<bool> holds_global{false};
    };

    size_t num_nodes;
    std::vector<std::unique_ptr<Cohort>> cohorts;
    TicketLock global_lock;

    // Finds which cohort a thread belongs to based on its ID
    // This logic is used to simulate NUMA architecture 
    // In a real NUMA system, this would look different
    size_t get_cohort(size_t thread_id) const {
        return thread_id % num_nodes;
    }

public:
    void init(size_t num_threads) override {
        (void)num_threads; // Unused
        num_nodes = 4; // Number of NUMA nodes can be adjusted 
        cohorts.clear();
        cohorts.reserve(num_nodes);
        for (size_t i = 0; i < num_nodes; ++i) {
            cohorts.emplace_back(std::make_unique<Cohort>());
        }
    }

    void lock(size_t thread_id) override {
        size_t cohort_id = get_cohort(thread_id);
        auto& cohort = *cohorts[cohort_id];

        // Thread must fist acquire the local lock
        cohort.local_lock.lock(thread_id);

        // Only the first thread in the cohort needs global lock
        // Other threads from the same cohort skip this step 
        if (!cohort.holds_global.load(std::memory_order_acquire)) {
            global_lock.lock(thread_id);
            cohort.holds_global.store(true, std::memory_order_release);
        }
    }

    void unlock(size_t thread_id) override {
        size_t cohort_id = get_cohort(thread_id);
        auto& cohort = *cohorts[cohort_id];

        // If the thread is the last in the cohort, it can release the global lock as well
        if (cohort.local_lock.alone()) {
            if (cohort.holds_global.exchange(false, std::memory_order_acq_rel)) {
                global_lock.unlock(thread_id);
            }
        }

        // release the local lock
        cohort.local_lock.unlock(thread_id);
    }

    void destroy() override {
        cohorts.clear();
    }

    std::string name() override {
        return "CohortTicket";
    }
};



