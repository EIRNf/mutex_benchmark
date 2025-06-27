#include "lock.hpp"
#include <mutex>
#include <stdexcept>

class CPPMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads; // This parameter is not used in this implementation
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
        mutex_.lock();
    }
    void unlock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
        mutex_.unlock();
    }
    void destroy() override {}
    std::string name(){return "cpp_std";};
    
private:
    std::mutex mutex_;
};
