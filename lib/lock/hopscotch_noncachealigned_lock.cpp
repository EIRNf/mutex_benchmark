// Non-cache-aligned variant of HopscotchMutex: identical epoch-based CLH
// handoff (see hopscotch_lock.cpp for the design rationale and the
// double-grant bug the epoch scheme fixes), but the per-thread slots are
// packed contiguously instead of cacheline-strided, deliberately keeping the
// false-sharing behavior this variant exists to measure.

#include "lock.hpp"
#include "cxl_utils.hpp"
#include <string.h>
#include <stdexcept>
#include <atomic>

class HopscotchNonCacheAlignedMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        size_t nodes_size = sizeof(std::atomic<uint64_t>) * num_threads;
        size_t tail_size = sizeof(std::atomic<uint64_t>);
        _cxl_region_size = nodes_size + tail_size;
        _cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);

        this->num_threads = num_threads;
        memset((void*)_cxl_region, 0, _cxl_region_size);
        nodes = (std::atomic<uint64_t>*)&_cxl_region[0];
        tail = (std::atomic<uint64_t>*)&_cxl_region[nodes_size];
        tail->store(0, std::memory_order_relaxed);
    }

    void lock(size_t thread_id) override {
        std::atomic<uint64_t>* me = &nodes[thread_id];
        uint64_t e = (me->load(std::memory_order_relaxed) & kEpochMask) + 1;
        me->store(e, std::memory_order_relaxed);
        uint64_t prev = tail->exchange(pack(thread_id, e), std::memory_order_acq_rel);
        uint64_t pred_epoch = prev & kEpochMask;
        if ((pred_epoch & 1) == 0) {
            return;
        }
        std::atomic<uint64_t>* pred = &nodes[prev >> kSlotShift];
        unsigned spins = 0;
        while (pred->load(std::memory_order_acquire) == pred_epoch) {
            LockSpinWait(spins);
        }
    }

    void unlock(size_t thread_id) override {
        std::atomic<uint64_t>* me = &nodes[thread_id];
        me->store(me->load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    void destroy() override {
        if (_cxl_region != nullptr) {
            FREE((void*)_cxl_region, _cxl_region_size);
            _cxl_region = nullptr;
        }
    }

    std::string name() override {
        return "hopscotch_noncachealigned";
    }

private:
    static constexpr unsigned kSlotShift = 48;
    static constexpr uint64_t kEpochMask = ((uint64_t)1 << kSlotShift) - 1;

    static uint64_t pack(size_t slot, uint64_t epoch) {
        return ((uint64_t)slot << kSlotShift) | epoch;
    }

    volatile char* _cxl_region = nullptr;
    size_t _cxl_region_size = 0;
    size_t num_threads = 0;
    std::atomic<uint64_t>* nodes = nullptr;
    std::atomic<uint64_t>* tail = nullptr;
};
