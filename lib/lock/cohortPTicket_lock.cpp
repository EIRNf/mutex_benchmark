#ifdef __linux__
#define _GNU_SOURCE
#include <malloc.h>
#endif

#include "lock.hpp"
#include <atomic>
#include <cstdlib>
#include <sched.h>
#include <string>
#include <stdlib.h>

/*
CohortPTicketLock: NUMA-aware hierarchical partitioned ticket spinlock

Partitioned ticket lock has a grant array while normal ticket lock has a single grant
Partitioned ticket lock can scale with much higher node counts than ticket lock
*/

//Default number of NUMA nodes
#ifndef NUMA_NODES
#define NUMA_NODES 2
#endif

// Configuration parameteres
static const int CACHE_ALIGN = 64;
static const int GRANT_SLOTS = 2;
static const int PAUSE_CYCLES = 1000;
static const int THRESHOLD = 64;

// Local Ticket lock 
// Ensures that it is cache aligned to avoid false sharing
struct tkt {
    std::atomic<unsigned long> request{0}, grant{0}, visitors{0};
    std::atomic<int>           state{1};   // 1=GLOBAL_RELEASE, 2=LOCAL_RELEASE, 3=BUSY
    unsigned long              gstate{0};  // Global ticket number

    // Padding to ensure who struct on its own cache 
    // This ensures that updates don't interfere with other threads on other NUMA nodes
    char pad[CACHE_ALIGN
             - sizeof(request)
             - sizeof(grant)
             - sizeof(visitors)
             - sizeof(state)
             - sizeof(gstate)
            ];
};


// allocates cacheline-aligned raw memory
static tkt* init_tkt() {
    tkt* l;
    if (posix_memalign((void**)&l, CACHE_ALIGN, sizeof(tkt)) != 0) {
        return nullptr;
    }
    new (&l->request)  std::atomic<unsigned long>(0);
    new (&l->grant)    std::atomic<unsigned long>(0);
    new (&l->visitors) std::atomic<unsigned long>(0);
    new (&l->state)    std::atomic<int>(1);  // GLOBAL_RELEASE
    l->gstate = 0;
    return l;
}

static void destroy_tkt(tkt* l) { free(l); }


static unsigned long tkt_acquire(tkt* l) {
    auto me = l->request.fetch_add(1, std::memory_order_acquire);
    unsigned spins = 0;
    while (l->grant.load(std::memory_order_acquire) != me) {
        LockSpinWait(spins);
    }
    return me;
}
static void tkt_release(tkt* l) {
    l->grant.fetch_add(1, std::memory_order_release);
}

// global paritioned ticket lock 
// grantline reprsetns a per NUAM node grant slot
struct grantline { std::atomic<int> grant; 
                   char pad[CACHE_ALIGN - sizeof(int)]; 
};

struct ptkt {
    std::atomic<unsigned long> request{0};
    grantline*                 grants;
};

static ptkt* init_ptkt() {
    ptkt* g;
    if (posix_memalign((void**)&g, CACHE_ALIGN, sizeof(ptkt)) != 0) {
        return nullptr;
    }
    new (&g->request) std::atomic<unsigned long>(0);
    if (posix_memalign((void**)&g->grants, CACHE_ALIGN, sizeof(grantline) * GRANT_SLOTS) != 0) {
        free(g);
        return nullptr;
    }
    for (int i = 0; i < GRANT_SLOTS; i++) {
        new (&g->grants[i].grant) std::atomic<int>(0);
    }
    return g;
}
static void destroy_ptkt(ptkt* g) {
    free(g->grants);
    free(g);
}

static unsigned long ptkt_acquire(ptkt* g) {
    auto me = g->request.fetch_add(1, std::memory_order_acquire);
    auto slot = (int)(me & (GRANT_SLOTS - 1));
    unsigned spins = 0;
    while (g->grants[slot].grant.load(std::memory_order_acquire) != (int)me) {
        LockSpinWait(spins);
    }
    return me;
}
static void ptkt_release(ptkt* g, unsigned long me) {
    auto next = me + 1;
    auto slot = (int)(next & (GRANT_SLOTS - 1));
    g->grants[slot].grant.store((int)next, std::memory_order_release);
}

//cohort with local and global ticket lock
struct c_ptkt_tkt {
    ptkt* glock;
    tkt** llocks; 
};

//initializes a cohort parrtitioned ticket lock 
//combines a global partitioned ticket with an array of local ticket locs
static c_ptkt_tkt* init_c_ptkt_tkt() {
    c_ptkt_tkt* c;
    if (posix_memalign((void**)&c, CACHE_ALIGN, sizeof(c_ptkt_tkt)) != 0) {
        return nullptr;
    }
    c->glock = init_ptkt();
    if (c->glock == nullptr) {
        free(c);
        return nullptr;
    }
    if (posix_memalign((void**)&c->llocks, CACHE_ALIGN, sizeof(tkt*) * NUMA_NODES) != 0) {
        destroy_ptkt(c->glock);
        free(c);
        return nullptr;
    }
    for (int i = 0; i < NUMA_NODES; i++) {
        c->llocks[i] = init_tkt();
        if (c->llocks[i] == nullptr) {
            for (int j = 0; j < i; j++) {
                destroy_tkt(c->llocks[j]);
            }
            free(c->llocks);
            destroy_ptkt(c->glock);
            free(c);
            return nullptr;
        }
    }
    return c;
}

static void destroy_c_ptkt_tkt(c_ptkt_tkt* c) {
    for (int i = 0; i < NUMA_NODES; i++) destroy_tkt(c->llocks[i]);
    free(c->llocks);
    destroy_ptkt(c->glock);
    free(c);
}

// acquires the cohort partitioned ticket lock
static void c_acquire(c_ptkt_tkt* c, int node) {
    auto* local = c->llocks[node];
    tkt_acquire(local); //Acquire local ticket lock 

    if (local->state.load(std::memory_order_relaxed) == 1 ) {
        local->gstate   = ptkt_acquire(c->glock);
        local->visitors.store(0, std::memory_order_relaxed);
    } else {
        local->visitors.fetch_add(1, std::memory_order_relaxed);
    }
    local->state.store(3, std::memory_order_relaxed);
}

static void c_release(c_ptkt_tkt* c, int node) {
    auto* local = c->llocks[node];
    auto req   = local->request.load(std::memory_order_relaxed);
    auto gr    = local->grant.load(std::memory_order_relaxed);
    auto vis   = local->visitors.load(std::memory_order_relaxed);
    bool alone = ((req - 1) == gr);

    if (alone || vis >= THRESHOLD) {
        local->state.store(1, std::memory_order_relaxed);
        ptkt_release(c->glock, local->gstate);
    } else {
        local->state.store(2, std::memory_order_relaxed);
    }
    tkt_release(local);
}

class CohortPTicketLock : public SoftwareMutex {
    c_ptkt_tkt* lock_;
public:
    CohortPTicketLock() : lock_(nullptr) {}
    ~CohortPTicketLock() override {}

    void init(size_t num_threads) override {
        (void)num_threads;
        lock_ = init_c_ptkt_tkt();
    }

    void lock(size_t thread_id) override {
        int node = int(thread_id % NUMA_NODES);
        c_acquire(lock_, node);
    }

    void unlock(size_t thread_id) override {
        int node = int(thread_id % NUMA_NODES);
        c_release(lock_, node);
    }

    void destroy() override {
        destroy_c_ptkt_tkt(lock_);
        lock_ = nullptr;
    }

    std::string name() override { return "CohortPTicketLock"; }
};