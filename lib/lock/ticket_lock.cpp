#include "lock.hpp"
#include "../utils/cxl_utils.hpp"
#include "../utils/region_layout.hpp"
#include <stdexcept>
#include <atomic>

class TicketMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        (void)num_threads;
        RegionLayout layout;
        auto next_ticket_handle = layout.reserve<std::atomic_size_t>();
        auto now_serving_handle = layout.reserve<std::atomic_size_t>();

        _cxl_region_size = layout.total_size();
        _cxl_region = (volatile char *)ALLOCATE(_cxl_region_size);
        next_ticket = RegionLayout::resolve(next_ticket_handle, _cxl_region);
        now_serving = RegionLayout::resolve(now_serving_handle, _cxl_region);
    }

    void lock(size_t thread_id) override {
        (void)thread_id;
        size_t my_ticket = next_ticket->fetch_add(1, std::memory_order_relaxed);
        unsigned spins = 0;
        while (now_serving->load(std::memory_order_acquire) != my_ticket) {
            LockSpinWait(spins);
        }
    }

    void unlock(size_t thread_id) override {
        (void)thread_id;
        now_serving->fetch_add(1, std::memory_order_release);
    }

    void destroy() override {
        FREE((void*)_cxl_region, _cxl_region_size);
    }

    std::string name() override {
        return "ticket";
    }

private:
    volatile char *_cxl_region;
    size_t _cxl_region_size;

    std::atomic_size_t *next_ticket;
    std::atomic_size_t *now_serving;
};
