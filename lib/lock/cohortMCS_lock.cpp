// CMCSLock: A NUMA Aware cohort based MCS lock.
// Threads first acquire a local MCS lock, which is per NUMA node, before attempting
// to acquire a global MCS lock. In order to acquire the lock a thread must own local & global lock
// CMCS promotes intra-node locality and fairness between NUMA nodes 


#include "lock.hpp"
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>
#include <string>

struct CMCSQNode {  //per thread node 
    std::atomic<CMCSQNode*> next {nullptr};
    std::atomic<bool> locked {false};
};

struct Cohort { //simulate a NUMA cohort / node. Will be different on an actual NUMA system
    std::atomic<CMCSQNode*> local_tail {nullptr}; //points to the local MCS queue for each node
    std::atomic<int> batch_count {0}; //counts how many threads have executed locally, ensuring fairness between NUMA nodes
};

class CMCSLock : public virtual SoftwareMutex {
private:
    std::atomic<CMCSQNode*> global_tail {nullptr};
    Cohort* cohorts; 
    size_t num_nodes;
    int max_batch = 10; // adjustable. Used to ensure fairness between NUMA nodes

    static thread_local CMCSQNode local_qnode;
    static thread_local CMCSQNode global_qnode;

    size_t get_numa_node(size_t thread_id) {
        return thread_id % num_nodes;
    }

public:
    void init(size_t num_threads) override {
        (void)num_threads;
        num_nodes = std::thread::hardware_concurrency();
        cohorts = new Cohort[num_nodes];
    }

    void lock(size_t thread_id) override {

        //find which NUMA node the thread belongs to 
        size_t node_id = get_numa_node(thread_id);
        Cohort& cohort = cohorts[node_id];

        local_qnode.next = nullptr;
        CMCSQNode* pred = cohort.local_tail.exchange(&local_qnode, std::memory_order_acquire);

        //checks to see if first in the local queue, if so, it can skip to acquire the global lock
        //if no predecessor, it is the first in the local queue
        if (pred) { 
            local_qnode.locked.store(true, std::memory_order_relaxed);
            pred->next.store(&local_qnode, std::memory_order_release);
            while (local_qnode.locked.load(std::memory_order_acquire)) std::this_thread::yield();
        } else {
            global_qnode.next = nullptr;
            CMCSQNode* gpred = global_tail.exchange(&global_qnode, std::memory_order_acquire);
            if (gpred) {
                global_qnode.locked.store(true, std::memory_order_relaxed);
                gpred->next.store(&global_qnode, std::memory_order_release);
                while (global_qnode.locked.load(std::memory_order_acquire)) std::this_thread::yield();
            }
            cohort.batch_count.store(0, std::memory_order_relaxed);
        }
    }

    void unlock(size_t thread_id) override {

        //find which NUMA node the thread belongs to 
        size_t node_id = get_numa_node(thread_id);
        Cohort& cohort = cohorts[node_id];

        //checks to see if a successor in the local queue exists, 
        //and if so, the batch count must be below the max

        CMCSQNode* succ = local_qnode.next.load(std::memory_order_acquire);
        if (succ) {
            cohort.batch_count.fetch_add(1, std::memory_order_relaxed);
            if (cohort.batch_count.load(std::memory_order_relaxed) < max_batch) {
                succ->locked.store(false, std::memory_order_release);
                return;
            }
        }

        //if there is no global successor, CAS nullptr to global_tail
        //if it fails, a global successor is in the process of linking 
        CMCSQNode* gsucc = global_qnode.next.load(std::memory_order_acquire);
        if (!gsucc) {
            CMCSQNode* expected = &global_qnode;
            if (!global_tail.compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_relaxed)) {
                while (!(gsucc = global_qnode.next.load(std::memory_order_acquire))) 
                    {std::this_thread::yield();}
            }
        }

        //pass the global lock to the next successor
        if (gsucc) gsucc->locked.store(false, std::memory_order_release);

        //checks to see if a local successor exists using same CAS logic as above
        if (!succ) {
            CMCSQNode* expected = &local_qnode;
            if (cohort.local_tail.compare_exchange_strong(expected, nullptr, std::memory_order_release, std::memory_order_relaxed)) return;
            while (!(succ = local_qnode.next.load(std::memory_order_acquire))) 
                {std::this_thread::yield();}
        }
        succ->locked.store(false, std::memory_order_release);
    }

    void destroy() override {
        delete[] cohorts;
    }

    std::string name() override {
        return "cohortMCS";
    }


};

thread_local CMCSQNode CMCSLock::local_qnode;
thread_local CMCSQNode CMCSLock::global_qnode;
