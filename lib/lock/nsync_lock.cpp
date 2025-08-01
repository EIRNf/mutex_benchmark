#ifdef inc_nsync
#include "lock.hpp"
#include <stdexcept>
#include "nsync_mu.h"

class NSync : public virtual SoftwareMutex {
public:
    NSync() {
    }
    ~NSync() {}
    void init(size_t num_threads) override {
        (void)num_threads; // This parameter is not used in this implementation
        // Initialization is done in the constructor, so this can be empty
        nsync::nsync_mu_init(&my_mutex);
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
        nsync::nsync_mu_lock(&my_mutex);
    }
    void unlock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
        nsync::nsync_mu_unlock(&my_mutex);
    }
    void destroy() override {}

    std::string name() override {return "nsync";};
    
private:
    nsync::nsync_mu my_mutex; // Global mutex
};
#endif //inc_nsync