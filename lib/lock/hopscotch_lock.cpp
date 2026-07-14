// CLH variant that does not use malloc or a prev pointer.
//
// Epoch-based redesign (2026-07): the original scheme handed off through a
// per-slot *boolean* with two alternating slots per thread ("hopscotch").
// That is unsound: a waiter that was descheduled while watching a slot
// cannot tell "still locked from my round" from "locked again two rounds
// later", so a slot recycle re-armed under a stale watcher and the next
// release woke BOTH watchers — two threads in the critical section at once
// (reproduced 5/5 as lost counter updates in lock-level mode). The handoff
// was also fully relaxed, so critical-section writes could trail the grant
// on arm64.
//
// Fix: each thread owns ONE slot holding a monotonically increasing epoch.
//   odd value  = enqueued/locked for that use of the slot
//   even value = released
// A locker bumps its slot to the next odd epoch and publishes {slot, epoch}
// packed in the 64-bit tail with an acq_rel exchange; its successor spins
// while nodes[slot] still equals that exact epoch. Since only the owner
// writes its slot and epochs never repeat, the first change a watcher
// observes proves the matching release happened (the owner's release write
// precedes any later write to the slot in program order), so stale watchers
// self-resolve instead of double-granting — one slot per thread is enough,
// and the thread_local slot-parity state (which also made this class
// singleton-only) is gone.

#include "../utils/cxl_utils.hpp"
#include <string.h>
#include "lock.hpp"
#include <stdexcept>
#include <atomic>
#include <new>

class HopscotchMutex : public virtual SoftwareMutex {
public:
    void init(size_t num_threads) override {
        size_t stride = std::hardware_destructive_interference_size;
        size_t nodes_size = stride * num_threads;
        size_t tail_size = sizeof(std::atomic<uint64_t>);
        _cxl_region_size = nodes_size + tail_size;
        _cxl_region = (volatile char*)ALLOCATE(_cxl_region_size);

        this->num_threads = num_threads;
        memset((void*)_cxl_region, 0, _cxl_region_size);
        tail = (std::atomic<uint64_t>*)&_cxl_region[nodes_size];
        // All-zero tail: epoch 0 is even, so the first locker acquires
        // immediately without dereferencing a slot.
        tail->store(0, std::memory_order_relaxed);
    }

    void lock(size_t thread_id) override {
        std::atomic<uint64_t>* me = slot_for(thread_id);
        // Single writer (us): bump our slot to the next odd epoch.
        uint64_t e = (me->load(std::memory_order_relaxed) & kEpochMask) + 1;
        me->store(e, std::memory_order_relaxed);
        // acq_rel: release publishes our slot's odd epoch before the packed
        // value is reachable through tail; acquire makes the predecessor's
        // epoch write visible before we start watching its slot.
        uint64_t prev = tail->exchange(pack(thread_id, e), std::memory_order_acq_rel);
        uint64_t pred_epoch = prev & kEpochMask;
        if ((pred_epoch & 1) == 0) {
            return; // predecessor marker is a released/initial state
        }
        std::atomic<uint64_t>* pred = slot_for(prev >> kSlotShift);
        unsigned spins = 0;
        while (pred->load(std::memory_order_acquire) == pred_epoch) {
            LockSpinWait(spins);
        }
    }

    void unlock(size_t thread_id) override {
        std::atomic<uint64_t>* me = slot_for(thread_id);
        // odd -> even; release hands the critical section to the watcher.
        me->store(me->load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    void destroy() override {
        if (_cxl_region != nullptr) {
            FREE((void*)_cxl_region, _cxl_region_size);
            _cxl_region = nullptr;
        }
    }

    std::string name() override {
        return "hopscotch";
    }

private:
    static constexpr unsigned kSlotShift = 48;
    static constexpr uint64_t kEpochMask = ((uint64_t)1 << kSlotShift) - 1;

    static uint64_t pack(size_t slot, uint64_t epoch) {
        return ((uint64_t)slot << kSlotShift) | epoch;
    }

    inline std::atomic<uint64_t>* slot_for(size_t thread_id) {
        return (std::atomic<uint64_t>*)&_cxl_region[thread_id * std::hardware_destructive_interference_size];
    }

    volatile char* _cxl_region = nullptr;
    size_t _cxl_region_size = 0;
    size_t num_threads = 0;
    std::atomic<uint64_t>* tail = nullptr;
};
