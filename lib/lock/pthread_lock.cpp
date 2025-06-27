#include "lock.hpp"
#include <pthread.h>
#include <stdexcept>

class Pthread : public virtual SoftwareMutex {
public:
    Pthread() {
       
    }
    ~Pthread() {
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


    std::string name(){return "pthread";};
    
private:
    pthread_mutex_t mutex_;
};
