#include "lock.hpp"
#include "../utils/cxl_utils.hpp"
#include <stdexcept>
#include <atomic>
#include <new>

// Ring ticket lock — Anderson-style array queue lock.
//
// Rewritten 2026-07-12. The previous implementation (see git history)
// combined a ring queue with a designated-waker protocol and a racy
// `empty` flag; its own header comment said "This mutex is bad and
// deadlocks", and it failed ~1 in 5 breach-detected runs. The intended
// structure — a ticket counter indexing a ring of per-slot grant flags —
// is exactly Anderson's array-based queue lock (T. Anderson, IEEE TPDS
// 1990), which needs no waker machinery:
//
//   lock():   my = tail.fetch_add(1); spin until ring[my % size] is set;
//             clear it (consume for the next generation).
//   unlock(): set ring[(my + 1) % size].
//
// Each waiter spins on its own cache line (like MCS, without per-node
// pointers). FIFO. Correctness needs size >= num_threads so tickets in
// flight (<= 1 per thread) never alias a slot; init rounds up to a power
// of two >= num_threads + 1.
class RingTicketMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        size_t size = 1;
        while (size < num_threads + 1) size *= 2;
        ring_size_ = size;
        modulo_mask_ = size - 1;

        region_size_ = size * CL;
        region_ = (volatile char*)ALLOCATE(region_size_);
        for (size_t i = 0; i < size; i++) {
            *get_slot(i) = false;
        }
        *get_slot(0) = true;   // lock starts free: ticket 0 may enter
        next_ticket_.store(0, std::memory_order_relaxed);
    }

    void lock(size_t thread_id) override {
        (void)thread_id;
        size_t my = next_ticket_.fetch_add(1, std::memory_order_relaxed);
        volatile bool* slot = get_slot(my & modulo_mask_);
        unsigned spins = 0;
        while (!*slot) {
            LockSpinWait(spins);
        }
        Fence();
        *slot = false;             // consume for the slot's next generation
        my_ticket_ = my;
    }

    void unlock(size_t thread_id) override {
        (void)thread_id;
        Fence();
        *get_slot((my_ticket_ + 1) & modulo_mask_) = true;
        Fence();
    }

    void destroy() override {
        FREE((void*)region_, region_size_);
    }

    std::string name() override {
        return "ring_ticket";
    }

private:
    static constexpr size_t CL = std::hardware_destructive_interference_size;

    volatile bool* get_slot(size_t idx) const {
        return (volatile bool*)&region_[idx * CL];
    }

    std::atomic<size_t> next_ticket_{0};
    volatile char* region_ = nullptr;
    size_t region_size_ = 0;
    size_t ring_size_ = 0;
    size_t modulo_mask_ = 0;
    // The holder's ticket, written under the lock and read at unlock by
    // the same thread. thread_local so the class needn't be singleton.
    static thread_local size_t my_ticket_;
};

thread_local size_t RingTicketMutex::my_ticket_ = 0;
