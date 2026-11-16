#include "bench_harness.hpp"
#include "bench_utils.hpp"
#include "cxl_utils.hpp"

#include <cstdio>
#include <cstring>
#include <cerrno>

#ifdef __linux__
#include <numa.h>
#include <numaif.h>
#endif

void setup_numa_mempolicy_or_fail(int numa) {
#if defined(__linux__) && defined(hardware_cxl)
    if (numa) {
        const int node = cxl_hardware_target_node();
        struct bitmask *mask = cxl_hardware_node_mask(node);
        if (set_mempolicy(MPOL_BIND, mask->maskp, mask->size) != 0) {
            const int error_code = errno;
            numa_bitmask_free(mask);
            cxl_hardware_fail("set_mempolicy for hardware-CXL target failed", error_code);
        }
        numa_bitmask_free(mask);
        fprintf(stderr, "Bound process memory policy to hardware-CXL NUMA node %d\n", node);
    }
#else
    (void)numa;
#endif
}

void destroy_and_delete_lock(SoftwareMutex *lock, int numa) {
    lock->destroy();
#ifdef __linux__
    if (numa) {
        numa_delete(lock);
        return;
    }
#else
    (void)numa;
#endif
    delete lock;
}
