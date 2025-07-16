#include "lock.hpp"
#include <stdexcept>


#ifdef __APPLE__

#include <os/lock.h>
class System : public virtual SoftwareMutex {
public:
    System() {
       
    }
    ~System() {
        
    }
    void init(size_t num_threads) override {
        (void)num_threads; // This parameter is not used in this implementation
        lock_ = OS_UNFAIR_LOCK_INIT;
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
        os_unfair_lock_lock(&lock_);
    }
    void unlock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
        os_unfair_lock_unlock(&lock_);
    }
    void destroy() override {}
    std::string name(){return "system_mac";};
    
private:
    os_unfair_lock lock_;
};

#else

#include <pthread.h>
class System : public virtual SoftwareMutex {
public:
    System() {
       
    }
    ~System() {
        pthread_mutex_destroy(&mutex_);
    }
    void init(size_t num_threads) override {
        (void)num_threads; // This parameter is not used in this implementation

        // Initialization is done in the constructor, so this can be empty
         if (pthread_mutex_init(&mutex_, nullptr) != 0) {
            throw std::runtime_error("Failed to initialize pthread mutex");
        }
    }

    void lock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
        pthread_mutex_lock(&mutex_);
    }
    void unlock(size_t thread_id) override {
        (void)thread_id; // This parameter is not used in this implementation
        pthread_mutex_unlock(&mutex_);
    }
    void destroy() override {
        pthread_mutex_destroy(&mutex_);
    }


    std::string name(){return "system_pthread";};
    
private:
    pthread_mutex_t mutex_;
};
#endif
