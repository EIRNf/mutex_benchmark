#include "../utils/cxl_utils.hpp"

#include "lock.hpp"
#include <atomic>
#include <time.h>
#include <stdexcept>

#ifdef cxl
    class ExponentialSpinLock : public virtual SoftwareMutex {
    public:
        void init(size_t num_threads) override {
            (void)num_threads; // This parameter is not used in this implementation
            this->lock_ = (std::atomic_flag*)ALLOCATE(sizeof(std::atomic_flag));
        }

        void lock(size_t thread_id) override {
            (void)thread_id; // This parameter is not used in this implementation

            while (lock_->test_and_set(std::memory_order_acquire)) {
                spin_delay_exponential();
            }
        }

        void unlock(size_t thread_id) override {
            (void)thread_id; // This parameter is not used in this implementation

            lock_->clear(std::memory_order_release);
        }

        void destroy() override {
            FREE(this->lock_, sizeof(std::atomic_flag));
        }
        
        std::string name() override {
            return "exp_spin";
        }
    private:
        std::atomic_flag *lock_;
    };
#else
    class ExponentialSpinLock : public virtual SoftwareMutex {
    public:
        void init(size_t num_threads) override {
            (void)num_threads; // This parameter is not used in this implementation
        }

        void lock(size_t thread_id) override {
            (void)thread_id; // This parameter is not used in this implementation

            while (lock_.test_and_set(std::memory_order_acquire)) {
                spin_delay_exponential();
            }
        }

        void unlock(size_t thread_id) override {
            (void)thread_id; // This parameter is not used in this implementation

            lock_.clear(std::memory_order_release);
        }

        void destroy() override {

        }
        
        std::string name() override {
            return "exp_spin";
        }
    private:
        std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    };
#endif
