// CMCSLock: A NUMA-aware cohort-based MCS lock (Dice-Marathe-Shavit style).
// Threads acquire a per-cohort local MCS lock; the cohort head additionally
// holds a global MCS lock. Within a cohort the critical section (and the
// implicit global-lock ownership) is handed off locally up to `max_batch`
// times before the cohort yields the global lock, bounding inter-cohort
// unfairness while promoting intra-node locality.
//
// Rewritten 2026-07-12. The previous version kept the GLOBAL queue node
// thread_local: a local batch handoff passed the critical section to a
// cohort sibling without transferring global-queue ownership, so the
// sibling's eventual global release operated on its own never-enqueued
// node — its tail CAS failed and it spun forever on a next pointer nobody
// would write (reproduced: hang at 12T+, the first configuration where
// threads actually shared a cohort, since num_nodes was
// hardware_concurrency()). The batch-cap path could also wake the local
// successor with the same signal as a CS grant while simultaneously
// granting the global successor — a latent double grant. Fixes:
//   - the global queue node lives in the Cohort and its ownership passes
//     with the local lock (only the current cohort head touches it);
//   - local grants carry a status: UNLOCKED_CS (you own CS + global) vs
//     ACQUIRE_GLOBAL (you are the new cohort head; enqueue the cohort's
//     global node first) — same structure as this repo's HMCS;
//   - num_nodes defaults to 2 simulated cohorts so the cohort logic is
//     actually exercised at benchmark thread counts (the old default made
//     every thread its own cohort for n <= cores, hiding the bugs).

#include "lock.hpp"
#include <atomic>
#include <cstddef>
#include <string>

class CMCSLock : public virtual SoftwareMutex {
private:
    static constexpr uint64_t ST_WAIT           = 0;  // spin: not granted yet
    static constexpr uint64_t ST_CS             = 1;  // granted: CS + global held
    static constexpr uint64_t ST_ACQUIRE_GLOBAL = 2;  // granted local: get global

    struct LocalQNode {
        std::atomic<LocalQNode*> next{nullptr};
        std::atomic<uint64_t>    status{ST_WAIT};
    };
    struct GlobalQNode {
        std::atomic<GlobalQNode*> next{nullptr};
        std::atomic<bool>         locked{false};
    };
    struct alignas(std::hardware_destructive_interference_size) Cohort {
        std::atomic<LocalQNode*> local_tail{nullptr};
        GlobalQNode global_node;   // owned by the current cohort head
        int batch_count = 0;       // written only under the cohort's lock
    };

    std::atomic<GlobalQNode*> global_tail{nullptr};
    Cohort* cohorts = nullptr;
    size_t num_nodes = 2;   // simulated NUMA cohorts
    int max_batch = 10;     // local handoffs before yielding the global lock

    static thread_local LocalQNode local_qnode;

    size_t get_numa_node(size_t thread_id) const {
        return thread_id % num_nodes;
    }

    // Enqueue the cohort's global node into the global MCS queue and wait.
    // Caller must be the cohort head (exclusive owner of cohort.global_node).
    void acquire_global(Cohort& cohort) {
        cohort.global_node.next.store(nullptr, std::memory_order_relaxed);
        GlobalQNode* gpred =
            global_tail.exchange(&cohort.global_node, std::memory_order_acq_rel);
        if (gpred) {
            cohort.global_node.locked.store(true, std::memory_order_relaxed);
            gpred->next.store(&cohort.global_node, std::memory_order_release);
            unsigned spins = 0;
            while (cohort.global_node.locked.load(std::memory_order_acquire)) {
                LockSpinWait(spins);
            }
        }
        cohort.batch_count = 0;
    }

    // Standard MCS release of the cohort's global node.
    void release_global(Cohort& cohort) {
        GlobalQNode* g = &cohort.global_node;
        GlobalQNode* gsucc = g->next.load(std::memory_order_acquire);
        if (!gsucc) {
            GlobalQNode* expected = g;
            if (global_tail.compare_exchange_strong(
                    expected, nullptr,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return;
            }
            unsigned spins = 0;
            while (!(gsucc = g->next.load(std::memory_order_acquire))) {
                LockSpinWait(spins);
            }
        }
        gsucc->locked.store(false, std::memory_order_release);
    }

public:
    void init(size_t num_threads) override {
        (void)num_threads;
        cohorts = new Cohort[num_nodes];
    }

    void lock(size_t thread_id) override {
        Cohort& cohort = cohorts[get_numa_node(thread_id)];

        local_qnode.next.store(nullptr, std::memory_order_relaxed);
        local_qnode.status.store(ST_WAIT, std::memory_order_relaxed);
        LocalQNode* pred =
            cohort.local_tail.exchange(&local_qnode, std::memory_order_acq_rel);

        if (pred) {
            pred->next.store(&local_qnode, std::memory_order_release);
            unsigned spins = 0;
            uint64_t st;
            while ((st = local_qnode.status.load(std::memory_order_acquire))
                   == ST_WAIT) {
                LockSpinWait(spins);
            }
            if (st == ST_CS) {
                return;   // predecessor passed CS + global ownership
            }
            // ST_ACQUIRE_GLOBAL: we are the new cohort head.
        }
        acquire_global(cohort);
    }

    void unlock(size_t thread_id) override {
        Cohort& cohort = cohorts[get_numa_node(thread_id)];

        // Batch handoff: pass CS + global ownership within the cohort.
        LocalQNode* succ = local_qnode.next.load(std::memory_order_acquire);
        if (succ && cohort.batch_count < max_batch) {
            cohort.batch_count++;
            succ->status.store(ST_CS, std::memory_order_release);
            return;
        }

        // Yield the global lock (batch cap reached, or no local successor).
        release_global(cohort);

        if (!succ) {
            LocalQNode* expected = &local_qnode;
            if (cohort.local_tail.compare_exchange_strong(
                    expected, nullptr,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return;
            }
            unsigned spins = 0;
            while (!(succ = local_qnode.next.load(std::memory_order_acquire))) {
                LockSpinWait(spins);
            }
        }
        // The successor becomes the cohort head and must re-acquire the
        // global lock through the cohort's (now released) global node.
        succ->status.store(ST_ACQUIRE_GLOBAL, std::memory_order_release);
    }

    void destroy() override {
        delete[] cohorts;
        cohorts = nullptr;
    }

    std::string name() override {
        return "cohortMCS";
    }
};

thread_local CMCSLock::LocalQNode CMCSLock::local_qnode;
