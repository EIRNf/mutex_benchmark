// Build with -Dhardware_cxl to place ALLOCATE'd lock memory on the NUMA node
// named by MUTEX_BENCH_NUMA_NODE; otherwise ALLOCATE is plain malloc.


#ifndef __CXL_UTILS_HPP
#define __CXL_UTILS_HPP

#pragma once

#include <stddef.h>

// #ifndef __cpp_lib_hardware_interference_size
// namespace std {
//     const size_t hardware_destructive_interference_size = 64;
// }
// #endif


// defined via macro so they can be changed for the actual hardware
#if defined(hardware_cxl)
    #include <cerrno>
    #include <climits>
    #include <cstdio>
    #include <cstdlib>
    #include <cstring>
    #include <map>
    #include <sys/mman.h>
    #include <unistd.h>
    #include <vector>
    #include <numa.h>
    #include <numaif.h>
    [[noreturn]] inline void cxl_hardware_fail(const char *message, int error_code = 0) {
        std::fprintf(stderr, "hardware_cxl: %s", message);
        if (error_code != 0) {
            std::fprintf(stderr, ": %s", std::strerror(error_code));
        }
        std::fputc('\n', stderr);
        std::fflush(stderr);
        std::abort();
    }

    inline int cxl_hardware_target_node() {
        static const int node = []() {
            const char *value = std::getenv("MUTEX_BENCH_NUMA_NODE");
            if (value == nullptr || *value == '\0') {
                cxl_hardware_fail(
                    "MUTEX_BENCH_NUMA_NODE is required; run with --numa-node N"
                );
            }

            char *end = nullptr;
            errno = 0;
            const long parsed = std::strtol(value, &end, 10);
            if (errno == ERANGE || end == value || *end != '\0' ||
                parsed < 0 || parsed > INT_MAX) {
                cxl_hardware_fail("MUTEX_BENCH_NUMA_NODE must be a non-negative integer");
            }
            if (numa_available() < 0 || parsed > numa_max_node()) {
                cxl_hardware_fail("MUTEX_BENCH_NUMA_NODE is not an available NUMA node");
            }

            long long free_bytes = 0;
            if (numa_node_size64(static_cast<int>(parsed), &free_bytes) <= 0) {
                cxl_hardware_fail("MUTEX_BENCH_NUMA_NODE has no online memory");
            }

            struct bitmask *allowed = numa_get_mems_allowed();
            if (allowed == nullptr) {
                cxl_hardware_fail("could not query the process's allowed memory nodes");
            }
            const bool is_allowed = numa_bitmask_isbitset(
                allowed, static_cast<unsigned int>(parsed)
            ) != 0;
            numa_bitmask_free(allowed);
            if (!is_allowed) {
                cxl_hardware_fail(
                    "MUTEX_BENCH_NUMA_NODE is not allowed by the process memory-node mask"
                );
            }
            return static_cast<int>(parsed);
        }();
        return node;
    }

    inline bool cxl_hardware_placement_enabled() {
        static const bool enabled = []() {
            const char *value = std::getenv("MUTEX_BENCH_PLACEMENT");
            return value != nullptr && std::strcmp(value, "1") == 0;
        }();
        return enabled;
    }

    inline struct bitmask *cxl_hardware_node_mask(int node) {
        struct bitmask *mask = numa_allocate_nodemask();
        if (mask == nullptr) {
            cxl_hardware_fail("could not allocate a NUMA node mask");
        }
        if (static_cast<unsigned int>(node) >= mask->size) {
            numa_bitmask_free(mask);
            cxl_hardware_fail("target NUMA node does not fit in the libnuma node mask");
        }
        numa_bitmask_setbit(mask, static_cast<unsigned int>(node));
        return mask;
    }

    inline void cxl_hardware_report_placement(
        void *region, size_t size, int target_node, const char *owner, int source_line
    ) {
        if (!cxl_hardware_placement_enabled()) {
            return;
        }

        const long page_size_raw = sysconf(_SC_PAGESIZE);
        if (page_size_raw <= 0) {
            cxl_hardware_fail("could not determine page size for placement instrumentation");
        }
        const size_t page_size = static_cast<size_t>(page_size_raw);
        const size_t page_count = (size - 1) / page_size + 1;
        std::vector<void *> pages(page_count);
        std::vector<int> statuses(page_count, -1);
        auto *base = static_cast<unsigned char *>(region);
        auto *touch_base = static_cast<volatile unsigned char *>(region);

        for (size_t i = 0, offset = 0; i < page_count; ++i, offset += page_size) {
            pages[i] = base + offset;
            const unsigned char value = touch_base[offset];
            touch_base[offset] = value;
        }

        if (move_pages(0, page_count, pages.data(), nullptr, statuses.data(), 0) < 0) {
            const int error_code = errno;
            std::fprintf(
                stderr,
                "PLACEMENT source=%s:%d address=%p bytes=%zu pages=%zu "
                "requested_node=%d query=failed errno=%d error=%s\n",
                owner, source_line, region, size, page_count, target_node,
                error_code, std::strerror(error_code)
            );
            std::fflush(stderr);
            return;
        }

        std::map<int, size_t> node_counts;
        std::map<int, size_t> error_counts;
        size_t resident_pages = 0;
        size_t target_pages = 0;
        for (int status : statuses) {
            if (status < 0) {
                ++error_counts[-status];
                continue;
            }
            ++resident_pages;
            ++node_counts[status];
            if (status == target_node) {
                ++target_pages;
            }
        }

        std::fprintf(
            stderr,
            "PLACEMENT source=%s:%d address=%p bytes=%zu pages=%zu "
            "requested_node=%d resident=%zu target_pages=%zu other_pages=%zu "
            "node_counts=",
            owner, source_line, region, size, page_count, target_node,
            resident_pages, target_pages, resident_pages - target_pages
        );
        bool first = true;
        for (const auto &entry : node_counts) {
            std::fprintf(stderr, "%s%d:%zu", first ? "" : ",", entry.first, entry.second);
            first = false;
        }
        if (node_counts.empty()) {
            std::fputs("none", stderr);
        }
        std::fputs(" error_counts=", stderr);
        first = true;
        for (const auto &entry : error_counts) {
            std::fprintf(stderr, "%s%d:%zu", first ? "" : ",", entry.first, entry.second);
            first = false;
        }
        if (error_counts.empty()) {
            std::fputs("none", stderr);
        }
        std::fputc('\n', stderr);
        std::fflush(stderr);
    }

    inline void *cxl_hardware_alloc(
        size_t size, const char *owner = "unknown", int source_line = 0
    ) {
        if (size == 0) {
            cxl_hardware_fail("refusing a zero-sized hardware-CXL allocation");
        }

        const int node = cxl_hardware_target_node();
        void *mapped = mmap(
            nullptr, size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0
        );
        if (mapped == MAP_FAILED) {
            cxl_hardware_fail("mmap for hardware-CXL allocation failed", errno);
        }

        struct bitmask *mask = cxl_hardware_node_mask(node);
        if (mbind(mapped, size, MPOL_BIND, mask->maskp, mask->size, 0) != 0) {
            const int error_code = errno;
            numa_bitmask_free(mask);
            munmap(mapped, size);
            cxl_hardware_fail("mbind for hardware-CXL allocation failed", error_code);
        }
        numa_bitmask_free(mask);
        cxl_hardware_report_placement(mapped, size, node, owner, source_line);
        return mapped;
    }

    inline void cxl_hardware_free(void *region, size_t region_size) {
        if (region != nullptr && munmap(region, region_size) != 0) {
            cxl_hardware_fail("munmap for hardware-CXL allocation failed", errno);
        }
    }

    inline void *_cxl_region_init(size_t region_size) {
        return cxl_hardware_alloc(region_size, "cxl-region");
    }

    inline void _cxl_region_free(void *region, size_t region_size) {
        cxl_hardware_free(region, region_size);
    }

    #define ALLOCATE(size) cxl_hardware_alloc(size, __FILE__, __LINE__)
    #define FREE(ptr, size) cxl_hardware_free(ptr, size)
#else
    #define ALLOCATE(size) malloc(size)
    #define FREE(ptr, size) free(ptr); (void)(size) //
#endif // hardware_cxl

#if defined(__x86_64)
    //#define Fence() __asm__ __volatile__ ( "mfence" )
    #define FENCE() __asm__ __volatile__ ( "lock; addq $0,128(%%rsp);" ::: "cc" )
#elif defined(__i386)
    #define FENCE() __asm__ __volatile__ ( "lock; addl $0,128(%%esp);" ::: "cc" )
#elif defined(__ARM_ARCH)
    #define FENCE() __asm__ __volatile__ ( "DMB ISH" ::: )
#else
    #error unsupported architecture
#endif

#endif // __CXL_UTILS_HPP
