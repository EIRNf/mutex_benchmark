// Hierarchical NUMA Aware MCS Lock Implementation with 3 levels: threads --> cores --> sockets --> global

#include "lock.hpp"
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <cstdint>


namespace hmcs {

// Per-thread queue node, just like in MCS
struct QNode {
    std::atomic<QNode*> next;
    std::atomic<uint64_t> status;
    QNode() : next(nullptr), status(0) {}
};

// Each level has its own HNode, leaf, intermediate, root
// consists of pointer to parent, QNode, threshold, and level
struct HNode {
    std::atomic<QNode*> tail;  
    HNode* parent;             
    QNode node;                
    uint64_t threshold;       
    int level;                 
    

    //constructor
    HNode(int lvl = 1, uint64_t thresh = 4) 
        : tail(nullptr), parent(nullptr), threshold(thresh), level(lvl) {}
    
    uint64_t GetThreshold() const { return threshold; }
};

// values from the HMCS paper
enum StatusValues : uint64_t {
    UNLOCKED = 0x0,
    LOCKED = 0x1,
    COHORT_START = 0x1,
    ACQUIRE_PARENT = UINT64_MAX - 1,
    WAIT = UINT64_MAX
};

class HMCSLock : public SoftwareMutex {
private:
    // NUMA architecture, this is adjustable
    static const int MAX_THREADS = 1024; 
    static const int THREADS_PER_CORE = 8;    
    static const int CORES_PER_SOCKET = 16;  
    static const int SOCKETS_PER_NODE = 8;    
    

    std::vector<std::vector<std::vector<HNode*>>> hierarchy; 
    HNode* root; 
    
    std::vector<QNode*> local_nodes;
    std::vector<HNode*> thread_to_leaf;  
    size_t num_threads;

public:
    std::string name() override { return "hmcs"; }

    void init(size_t n) override {

        //scales the hierarchy based on the number of threads
        if (n > MAX_THREADS) {
            num_threads = n;
        } else {
            num_threads = n;
        }

        local_nodes.resize(n);
        thread_to_leaf.resize(n);
        for (size_t i = 0; i < n; ++i) {
            local_nodes[i] = new QNode();
        }
        
        // Builds hierarchy
        buildHierarchy();
        
        // Map threads to their leaf nodes based on NUMA topology
        mapThreadsToLeaves();
    }

    void lock(size_t tid) override {
        assert(tid < num_threads);
        QNode* I = local_nodes[tid];
        HNode* L = thread_to_leaf[tid];
        
        // Lock function
        acquire(L, I);
    }

    void unlock(size_t tid) override {
        QNode* I = local_nodes[tid];
        HNode* L = thread_to_leaf[tid];
        
        // Release function
        release(L, I);
    }

private:

    // buildHierarchy builds a tree structure of locks basaed on
    // threads, cores, sockets, and root to simulate NUMA hierarchy
    // On an actual NUMA system, this isn't needed
    void buildHierarchy() {

        //calculate the actual # of sockets needed
        int actual_sockets = std::min(SOCKETS_PER_NODE, (int)((num_threads + THREADS_PER_CORE * CORES_PER_SOCKET - 1) / (THREADS_PER_CORE * CORES_PER_SOCKET)));
        if (actual_sockets == 0) actual_sockets = 1;
        
        //calculate the acutal # of cores per socket
        int actual_cores_per_socket = std::min(CORES_PER_SOCKET, (int)((num_threads + THREADS_PER_CORE - 1) / THREADS_PER_CORE));
        if (actual_cores_per_socket == 0) actual_cores_per_socket = 1;
        
        // the top of the hierarchy
        root = new HNode(3, actual_sockets);
        
        hierarchy.resize(actual_sockets);
        
        for (int socket = 0; socket < actual_sockets; ++socket) {
            hierarchy[socket].resize(actual_cores_per_socket);
            
            HNode* socket_node = new HNode(2, actual_cores_per_socket);
            socket_node->parent = root;
            
            for (int core = 0; core < actual_cores_per_socket; ++core) {
                hierarchy[socket][core].resize(THREADS_PER_CORE);
                
                HNode* core_node = new HNode(1, THREADS_PER_CORE);
                core_node->parent = socket_node;
                
                for (int thread = 0; thread < THREADS_PER_CORE; ++thread) {
                    hierarchy[socket][core][thread] = core_node;
                }
            }
        }
    }
    
    // function mapsThreadtoLeaves assigns each thread to a leaf HNdode 
    // this isn't needed on an actual NUMA system
    void mapThreadsToLeaves() {
        
        int actual_sockets = hierarchy.size();
        int actual_cores_per_socket = actual_sockets > 0 ? hierarchy[0].size() : 0;
        
        for (size_t i = 0; i < num_threads; ++i) {
            int socket = (i / (THREADS_PER_CORE * actual_cores_per_socket)) % actual_sockets;
            int core = (i / THREADS_PER_CORE) % actual_cores_per_socket;
            int thread = i % THREADS_PER_CORE;
            
            // Ensures we don't exceed the hierarchy
            if (socket < actual_sockets && core < actual_cores_per_socket) {
                thread_to_leaf[i] = hierarchy[socket][core][thread];
            } else {
                thread_to_leaf[i] = hierarchy[0][0][0];
            }
        }
    }


    // if first in level, it acquires the parent recursively
    // if not first in level, it waits for predecessor to pass lock
    void acquire(HNode* L, QNode* I) {
        I->next.store(nullptr, std::memory_order_relaxed);
        I->status.store(WAIT, std::memory_order_relaxed);
        
        QNode* pred = L->tail.exchange(I, std::memory_order_acq_rel);
        
        if (pred == nullptr) {
            I->status.store(COHORT_START, std::memory_order_relaxed);
            
            if (L->parent != nullptr) {
                acquire(L->parent, &(L->node));
            }
            
            return;
        }
        
        pred->next.store(I, std::memory_order_release);
        
        uint64_t my_status;

        // spins until your status changes from WAIT
        unsigned spins = 0;
        do {
            my_status = I->status.load(std::memory_order_acquire);
            if (my_status != WAIT) {
                break;
            }
            LockSpinWait(spins);
        } while (true);
        
        if (my_status == ACQUIRE_PARENT) {
            if (L->parent != nullptr) {
                acquire(L->parent, &(L->node));
            }
            I->status.store(COHORT_START, std::memory_order_relaxed);
        }
    }

    void release(HNode* L, QNode* I) {
        uint64_t count = I->status.load(std::memory_order_acquire);
        
        // If we haven't reached threshold and have a successor, pass within cohort
        QNode* succ = I->next.load(std::memory_order_acquire);
        if (count < L->GetThreshold() && succ != nullptr) {
            succ->status.store(count + 1, std::memory_order_release);
            return;
        }
        
        // if you reached the threshold, or no successor, release the lock
        if (L->parent != nullptr) {
            release(L->parent, &(L->node));
        }
        
        if (succ != nullptr) {
            // Tell successor to acquire parent, else clear the tail
            succ->status.store(ACQUIRE_PARENT, std::memory_order_release);
        } else {
            QNode* expected = I;
            if (L->tail.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
                return;
            }
            
            // if the tail wasn't cleared, there is a successor
            unsigned spins = 0;
            do {
                succ = I->next.load(std::memory_order_acquire);
                if (succ != nullptr) break;
                LockSpinWait(spins);
            } while (true);
            
            succ->status.store(ACQUIRE_PARENT, std::memory_order_release);
        }
    }

public:
    void destroy() override {
        for (auto q : local_nodes) {
            delete q;
        }
        local_nodes.clear();
        thread_to_leaf.clear();
        
        std::set<HNode*> unique_nodes;
        for (const auto& socket : hierarchy) {
            for (const auto& core : socket) {
                for (auto* node : core) {
                    unique_nodes.insert(node);
                }
            }
        }
        for (auto* node : unique_nodes) {
            delete node;
        }
        
        delete root;
        hierarchy.clear();
    }
};

}