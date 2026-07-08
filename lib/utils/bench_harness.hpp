#ifndef __BENCH_HARNESS_HPP_
#define __BENCH_HARNESS_HPP_

#pragma once

#include "../lock/lock.hpp"

// Binds the process-wide NUMA memory policy to node 2 (best-effort, warns to
// stderr on failure). No-op unless `numa` is non-zero and the build targets
// Linux. Extracted from max_contention_bench.cpp, where it was previously
// the only caller; min_/grouped_contention_bench never called this and
// still don't, to avoid changing their behavior.
void setup_numa_mempolicy_or_warn(int numa);

// Runs lock->destroy() and frees the lock object: routes through
// numa_delete() when `numa` is non-zero (matching the NUMA-aware allocation
// numa_new<T>() performs in get_mutex()), or a plain `delete` otherwise.
// Centralizes lock cleanup that had drifted across the three benchmark
// apps (some previously never deleted the lock in non-NUMA builds; grouped
// previously always deleted, never routing through numa_delete under NUMA).
void destroy_and_delete_lock(SoftwareMutex *lock, int numa);

#endif // __BENCH_HARNESS_HPP_
