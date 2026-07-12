//Cohort TAS Lock: A Hierarchical Test-and-Set NUMA aware spin lock
//Each thread maps to a node. Threads first spin to acqurie a local lock
//Once local lock is acquired, they content to a global lock


#include "lock.hpp"
#include <atomic>
#include <sched.h>
#include <cstdlib>
#include <string>

#ifndef NUMA_NODES
#define NUMA_NODES 2 //Default to 2 NUMA nodes if not defined.
#endif


struct cohort_tas {

    // global test-and-set lock
    std::atomic_flag glock = ATOMIC_FLAG_INIT;

    //array of local locks, one per NUMA node
    struct local_t {
        std::atomic_flag llock = ATOMIC_FLAG_INIT;
    } lnodes[NUMA_NODES];
};

static cohort_tas* init_ctas() {
    cohort_tas* c = (cohort_tas*)std::malloc(sizeof(cohort_tas));

    //makes sure the global lock is cleared
    c->glock.clear(std::memory_order_relaxed);

    //makes sure the local lock is cleared
    for (int i = 0; i < NUMA_NODES; i++) {
        c->lnodes[i].llock.clear(std::memory_order_relaxed);
    }

    return c;
}

static void destroy_ctas(cohort_tas* c) {
    std::free(c);
}

static void ctas_acquire(cohort_tas* c, size_t thread_id) {
    //find which NUMA node the thread belongs to 
    int node = int(thread_id % NUMA_NODES);
    
    // acquire the local cohort lock
    unsigned spins = 0;
    while (c->lnodes[node].llock.test_and_set(std::memory_order_acquire)) {
        LockSpinWait(spins);
    }

    // acquire global lock
    unsigned gspins = 0;
    while (c->glock.test_and_set(std::memory_order_acquire)) {
        LockSpinWait(gspins);
    }
}

static void ctas_release(cohort_tas* c, size_t thread_id) {
    //find which NUMA node the thread belongs to
    int node = int(thread_id % NUMA_NODES);
    c->glock.clear(std::memory_order_release);
    c->lnodes[node].llock.clear(std::memory_order_release);
}

class CohortTASLock : public SoftwareMutex {
    cohort_tas* lock_;
    
public:
    CohortTASLock() : lock_(nullptr) {}
    ~CohortTASLock() override {}

    void init(size_t num_threads) override {
        (void)num_threads;
        lock_ = init_ctas();
    }

    void lock(size_t thread_id) override {
        ctas_acquire(lock_, thread_id);
    }

    void unlock(size_t thread_id) override {
        ctas_release(lock_, thread_id);
    }

    void destroy() override {
        destroy_ctas(lock_);
        lock_ = nullptr;
    }

    std::string name() override {
        return "CohortTASLock";
    }
};
