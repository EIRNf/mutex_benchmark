#include "lock.hpp"
#include "../utils/cxl_utils.hpp"
#include <stdexcept>
#include <atomic>

class TicketMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads;
        size_t _cxl_region_size = sizeof(std::atomic_size_t) * 2;
        _cxl_region = (volatile char *)ALLOCATE(_cxl_region_size);
        next_ticket = (std::atomic_size_t*)&_cxl_region[0];
        now_serving = (std::atomic_size_t*)&_cxl_region[sizeof(std::atomic_size_t)];
    }

    void lock(size_t thread_id) override {
        (void)thread_id;
        size_t my_ticket = next_ticket->fetch_add(1, std::memory_order_relaxed);
        while (now_serving->load(std::memory_order_acquire) != my_ticket) {
            spin_delay_sched_yield();
        }
    }

    void unlock(size_t thread_id) override {
        (void)thread_id;
        now_serving->fetch_add(1, std::memory_order_release);
    }

    void destroy() override {
        FREE((void*)_cxl_region, sizeof(std::atomic_size_t) * 2);
    }

    std::string name() override {
        return "djikstra";
    }

private:
    volatile char *_cxl_region;

    std::atomic_size_t *next_ticket;
    std::atomic_size_t *now_serving;
};
