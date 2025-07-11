#include "lock.hpp"
#include <stdexcept>
#include <atomic>

class TicketMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads;
        now_serving = 0;
        next_ticket = 0;
    }

    void lock(size_t thread_id) override {
        (void)thread_id;
        size_t my_ticket = next_ticket.fetch_add(1, std::memory_order_relaxed);
        while (now_serving.load(std::memory_order_acquire) != my_ticket) {
            // Busy wait
        }
    }

    void unlock(size_t thread_id) override {
        (void)thread_id;
        now_serving.fetch_add(1, std::memory_order_release);
    }

    void destroy() override {
        
    }

    std::string name() override {
        return "djikstra";
    }

private:
    volatile std::atomic<size_t> next_ticket;
    volatile std::atomic<size_t> now_serving;
};
