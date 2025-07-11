#include <stdexcept>
#include "lock.hpp"

// Does nothing, for testing purposes
class NullMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads; // This parameter is not used in this implementation
    }
    void lock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
    }
    void unlock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
    }
    void destroy() override {}
    std::string name() override {
        return "null";
    }
};
