#include "bench_harness.hpp"
#include "bench_utils.hpp"
#include "cxl_utils.hpp"

#include <cstdio>
#include <cstring>
#include <cerrno>

void destroy_and_delete_lock(SoftwareMutex *lock) {
    lock->destroy();
#if defined(__linux__) && defined(hardware_cxl)
    numa_delete(lock);
#else
    delete lock;
#endif
}
