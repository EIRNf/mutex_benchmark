#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>
#include <time.h>
#include <assert.h>

// TODO: explicit memory ordering.
// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.

// CLH variant that does not use malloc or a prev pointer
class HopscotchMutex : public virtual SoftwareMutex {
public:
    struct Node {
        std::atomic_bool successor_must_wait;
    };

    void init(size_t num_threads) override {
        (void)num_threads; // Unused
        assert(sizeof(struct Node) == 1);

        // The lock starts off unlocked by setting tail to a pointer
        // to some true value so that the next successor can immediately lock.
        // This means that predecessor is never a null pointer.
        tail = &default_node;
        tail.load()->successor_must_wait = false;
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // Unused
        // The Hopscotch part, flipping to the other node to avoid reusing a node prematurely.
        node = (struct Node*)((size_t)node^1);
        node->successor_must_wait = true;
        struct Node *predecessor = tail.exchange(node, std::memory_order_relaxed);
        while (predecessor->successor_must_wait.load(std::memory_order_relaxed));
    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // Unused
        node->successor_must_wait.store(false, std::memory_order_relaxed);
    }

    void destroy() override {
    }

    std::string name() override {
        return "hopscotch";
    }
private:
    static struct Node default_node;
    static std::atomic<struct Node*> tail;
    static thread_local struct Node *node;

    // TODO: if a thread leaves, will its still-used thread locals be reclaimed
    // and break the algorithm?
    // Would this algorithm be faster if the nodes were all contiguous in memory?
    alignas(2) static thread_local struct Node my_nodes[2];
};
struct HopscotchMutex::Node HopscotchMutex::default_node;
std::atomic<struct HopscotchMutex::Node*> HopscotchMutex::tail;

thread_local struct HopscotchMutex::Node HopscotchMutex::my_nodes[2];
thread_local struct HopscotchMutex::Node *HopscotchMutex::node = (struct HopscotchMutex::Node*)&my_nodes;