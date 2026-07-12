// HCLH NUMA aware queue lock
// Threads spin locally in per-core queues
// They only escalate to parent queues if
// they're the first in the cohort 

#include "lock.hpp"
#include <atomic>
#include <thread>
#include <vector>
#include <cstdint>
#include <cassert>
#include <set>
#include <unistd.h>
#ifdef __linux__
#include <numa.h>
#else
static inline int numa_available() { return -1; }
static inline int numa_max_node() { return 0; }
#endif

//syscall only on linux
#ifdef __linux__
#include <sys/syscall.h>
#endif

// static inline int numa_available() { return -1; }
// static inline int numa_max_node() { return 0; }

namespace hclh {


// status values from the HCLH paper
enum StatusValues : uint64_t {
    WAIT           = UINT64_MAX,
    COHORT_START   = 0x1,
    ACQUIRE_PARENT = UINT64_MAX - 1
};

// Per-thread CLH node
struct QNode {
    std::atomic<QNode*> next;
    std::atomic<bool>   successor_must_wait;
    std::atomic<bool>   tail_when_spliced;
    std::atomic<size_t> cluster_id;

    QNode() : next(nullptr), successor_must_wait(false), tail_when_spliced(false), cluster_id(0) {}

    void init(size_t cid) {
        next.store(nullptr, std::memory_order_relaxed);
        successor_must_wait.store(true, std::memory_order_relaxed);
        tail_when_spliced.store(false, std::memory_order_relaxed);
        cluster_id.store(cid, std::memory_order_relaxed);
    }
};

// Represents a hierarchical node at each level (core, socket, root)
struct HNode {
    std::atomic<QNode*> tail;  
    HNode*              parent;    
    QNode               node;      
    uint64_t            threshold; 
    int                 level;     

    HNode(int lvl = 1, uint64_t thresh = 4)
      : tail(nullptr), parent(nullptr), threshold(thresh), level(lvl) {}
    uint64_t GetThreshold() const { return threshold; }
};

class HCLHMutex : public SoftwareMutex {
private:
    static constexpr int THREADS_PER_CORE_DEFAULT = 4;
    static constexpr int CORES_PER_SOCKET_DEFAULT = 8;

    std::vector<std::vector<std::vector<HNode*>>> hierarchy;
    HNode* root;

    std::vector<QNode*> local_nodes;
    std::vector<HNode*> thread_to_leaf;
    size_t num_threads;

    // this doesn't really do anything on a non-NUMA system
    inline size_t detect_numa_node() const {
#ifdef __linux__
        unsigned cpu, node;
        if (syscall(__NR_getcpu, &cpu, &node, nullptr)==0) return static_cast<size_t>(node);
#endif
        return 0;
    }

    // construct a 3-level NUMA aware hierarchy, same as HMCS
    // this is will be different on a real NUMA system
    void buildHierarchy() {
        int numa_nodes = (numa_available()!=-1 ? numa_max_node()+1 : 1);
        int cores = CORES_PER_SOCKET_DEFAULT;
        int threads_per_core = THREADS_PER_CORE_DEFAULT;

        root = new HNode(3, numa_nodes);

        hierarchy.assign(numa_nodes, {});
        for (int s=0; s<numa_nodes; ++s) {
            hierarchy[s].assign(cores, {});
            HNode* sock = new HNode(2, cores);
            sock->parent = root;

            for (int c=0; c<cores; ++c) {
                HNode* core = new HNode(1, threads_per_core);
                core->parent = sock;
                hierarchy[s][c].assign(threads_per_core, core);
            }
        }
    }

    void mapThreadsToLeaves() {
        thread_to_leaf.resize(num_threads);
        int numa_nodes = hierarchy.size();
        int cores = numa_nodes>0 ? hierarchy[0].size() : 1;
        int tpc = cores>0 ? hierarchy[0][0].size() : 1;
        for(size_t i=0;i<num_threads;++i) {
            int s = detect_numa_node() % numa_nodes;
            int c = (i/threads_per_core()) % cores;
            int t = i % tpc;
            thread_to_leaf[i] = hierarchy[s][c][t];
        }
    }

    int threads_per_core() const {
        return hierarchy.empty()||hierarchy[0].empty()?1:int(hierarchy[0][0].size());
    }

    void acquire(HNode* L, QNode* I) {
        I->init(L->level);
        QNode* pred = L->tail.exchange(I, std::memory_order_acq_rel);

        // if first in queue --> acquire the parent
        if (!pred) {
            I->tail_when_spliced.store(false, std::memory_order_relaxed);
            if (L->parent) acquire(L->parent, &L->node);
            return;
        }

        pred->next.store(I, std::memory_order_release);
        { unsigned spins = 0; while (I->successor_must_wait.load(std::memory_order_acquire)) LockSpinWait(spins); }

        //acquire the parent if we were told to do so
        if (I->tail_when_spliced.load(std::memory_order_acquire) && L->parent) {
            acquire(L->parent, &L->node);
        }
    }

    void release(HNode* L, QNode* I) {
        QNode* succ = I->next.load(std::memory_order_acquire);

        // pass within the cohort if we haven't hit the threshold
        if (succ && I->cluster_id.load(std::memory_order_acquire) < L->GetThreshold()) {
            succ->successor_must_wait.store(false, std::memory_order_release);
            return;
        }

        // escalate to the parent level
        if (L->parent) release(L->parent, &L->node);

        // wakes up the successor and flag, otherwise cleans up tail
        succ = I->next.load(std::memory_order_acquire);
        if (succ) {
            succ->tail_when_spliced.store(true, std::memory_order_release);
            succ->successor_must_wait.store(false, std::memory_order_release);
        } else {
            QNode* expected = I;
            if (L->tail.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) return;
            { unsigned spins = 0; do { succ = I->next.load(std::memory_order_acquire); if (succ) break; LockSpinWait(spins); } while (true); }
            succ->tail_when_spliced.store(true, std::memory_order_release);
            succ->successor_must_wait.store(false, std::memory_order_release);
        }
    }

public:
    std::string name() override { return "hclh-numa-full"; }

    void init(size_t n) override {
        num_threads = n;
        local_nodes.resize(n);
        for (size_t i=0;i<n;++i) local_nodes[i] = new QNode();
        buildHierarchy();
        mapThreadsToLeaves();
    }

    void lock(size_t tid) override {
        assert(tid < num_threads);
        acquire(thread_to_leaf[tid], local_nodes[tid]);
    }

    void unlock(size_t tid) override {
        assert(tid < num_threads);
        release(thread_to_leaf[tid], local_nodes[tid]);
    }

    void destroy() override {
        for (auto q: local_nodes) delete q;
        local_nodes.clear(); thread_to_leaf.clear();
        std::set<HNode*> uniq;
        for (auto& sock: hierarchy) for (auto& core: sock) for (auto* node: core) uniq.insert(node);
        for (auto* n: uniq) delete n;
        delete root; hierarchy.clear();
    }
};

} 
