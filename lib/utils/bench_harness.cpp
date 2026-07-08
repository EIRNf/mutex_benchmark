#include "bench_harness.hpp"
#include "bench_utils.hpp"

#include <cstdio>
#include <cstring>
#include <cerrno>

#ifdef __linux__
#include <numa.h>
#include <numaif.h>
#endif

void setup_numa_mempolicy_or_warn(int numa) {
#ifdef __linux__
    if (numa) {
        unsigned long nodemask[16] = {0};
        nodemask[0] = 1UL << 2;  // Node 2
        unsigned long maxnode = sizeof(nodemask) * 8;

        // Set memory policy for entire process
        if (set_mempolicy(MPOL_BIND, nodemask, maxnode) != 0) {
            fprintf(stderr, "WARNING: Failed to set memory policy to node 2: %s\n", strerror(errno));
        } else {
            fprintf(stderr, "Successfully set process memory policy to node 2\n");
        }
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
