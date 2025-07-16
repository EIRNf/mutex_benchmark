#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <stdio.h>

// TODO: explicit memory ordering.
// NOTE: Because of the limitations of `thread_local`,
// this class MUST be singleton. TODO: This is not yet explicitly enforced.

struct K42Node {
    struct K42Node *next;
    union {
        volatile bool locked;
        struct K42Node *tail;
    };
};

class K42Mutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads; // Unused

    }

    void lock(size_t thread_id) override {
        (void)thread_id; // Unused

    }

    void unlock(size_t thread_id) override {
        (void)thread_id; // Unused

    }

    void destroy() override {

    }

    std::string name() override {
        return "k42";
    }
private:

};