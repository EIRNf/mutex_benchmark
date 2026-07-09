#ifndef __REGION_LAYOUT_HPP_
#define __REGION_LAYOUT_HPP_

#pragma once

#include <cstddef>
#include <new>

// A small "reserve then resolve" builder for locks that carve one raw
// ALLOCATE()'d buffer into several typed sub-regions. Replaces hand-rolled
// offset/size arithmetic -- which has repeatedly caused ALLOCATE/FREE size
// mismatches across lib/lock/*.cpp, since the size formula and the field
// offsets were always computed independently and could silently drift --
// with one declarative reservation list and a single stored total size used
// for both ALLOCATE and FREE.
//
// Usage (in a lock's init()):
//   RegionLayout layout;
//   auto next_ticket = layout.reserve<std::atomic_size_t>();
//   auto now_serving = layout.reserve<std::atomic_size_t>();
//   _region = (volatile char*)ALLOCATE(layout.total_size());
//   next_ticket_ptr = RegionLayout::resolve(next_ticket, _region);
//   now_serving_ptr = RegionLayout::resolve(now_serving, _region);
//   ...
//   // in destroy():
//   FREE((void*)_region, _region_size);  // store layout.total_size() at init() time
//
// Handles are cheap value types (just a byte offset, or offset+stride for
// the cache-line-strided variant) and don't need to survive past init() --
// only the resolved pointers and the total size need to be kept as members.
class RegionLayout {
public:
    template <typename T>
    struct Handle {
        std::size_t offset;
    };

    template <typename T>
    struct StridedHandle {
        std::size_t offset;
        std::size_t stride;
    };

    // Reserve one T, naturally aligned.
    template <typename T>
    Handle<T> reserve() {
        return reserve_array<T>(1);
    }

    // Reserve `count` contiguous Ts, naturally aligned. Resolve with
    // resolve<T>() and index the returned pointer normally (ptr[i]).
    template <typename T>
    Handle<T> reserve_array(std::size_t count) {
        std::size_t offset = align_up(size_, alignof(T));
        size_ = offset + sizeof(T) * count;
        return Handle<T>{offset};
    }

    // Reserve `count` Ts, each padded out to one
    // std::hardware_destructive_interference_size stride (for per-thread
    // cache-line-strided layouts, e.g. MCS/Hopscotch-style queue nodes).
    // Resolve per-index with resolve_strided<T>().
    template <typename T>
    StridedHandle<T> reserve_strided_array(std::size_t count) {
        constexpr std::size_t stride = std::hardware_destructive_interference_size;
        static_assert(sizeof(T) <= stride, "T does not fit within one cache-line stride");
        std::size_t offset = align_up(size_, stride);
        size_ = offset + stride * count;
        return StridedHandle<T>{offset, stride};
    }

    // Reserve `bytes` raw bytes, conservatively aligned -- for embedding a
    // sub-lock's own get_cxl_region_size(n)-sized region (the existing
    // TryLock::region_init() composition convention used by
    // BurnsLamportMutex/LamportLock/SpinLock).
    Handle<unsigned char> reserve_bytes(std::size_t bytes,
                                         std::size_t alignment = alignof(std::max_align_t)) {
        std::size_t offset = align_up(size_, alignment);
        size_ = offset + bytes;
        return Handle<unsigned char>{offset};
    }

    std::size_t total_size() const { return size_; }

    template <typename T>
    static T* resolve(Handle<T> handle, volatile char* base) {
        return reinterpret_cast<T*>(const_cast<char*>(base) + handle.offset);
    }

    template <typename T>
    static T* resolve_strided(StridedHandle<T> handle, volatile char* base, std::size_t index) {
        return reinterpret_cast<T*>(const_cast<char*>(base) + handle.offset + index * handle.stride);
    }

private:
    static std::size_t align_up(std::size_t value, std::size_t alignment) {
        return (value + alignment - 1) / alignment * alignment;
    }

    std::size_t size_ = 0;
};

#endif // __REGION_LAYOUT_HPP_
