// lru_workload_bench.hpp

#ifndef LRU_WORKLOAD_BENCH_HPP
#define LRU_WORKLOAD_BENCH_HPP

#include "bench_utils.hpp"
#include "lock.hpp"
#include <chrono>
#include <atomic>
#include <memory>
#include <cstdlib> 
#include <ctime> 

struct per_thread_args {
    int thread_id;
    per_thread_stats stats;
    std::shared_ptr<std::atomic<bool>> start_flag;
    std::shared_ptr<std::atomic<bool>> end_flag;
    SoftwareMutex* lock;
};

/**
 * @brief Run a contention benchmark.
 *
 * @param num_threads               Number of competing threads.
 * @param run_time                  Duration to run the benchmark.
 * @param csv                       If true, output in CSV format.
 * @param thread_level              If true, measure at thread granularity.
 * @param no_output                 If true, suppress printing of per-thread stats.
 * @param max_noncritical_delay_ns  Maximum nanoseconds to sleep in the non-critical section.
 * @param low_contention            If true, stagger thread startup times.
 * @param stagger_ms                Milliseconds between each thread’s startup when low_contention is true.
 * @param lock                      Pointer to the initialized SoftwareMutex.
 * @return int                      0 on success, 1 on error (e.g. counter mismatch).
 */
int lru_workload_bench(
    int num_threads,
    std::chrono::seconds run_time,
    bool csv,
    bool thread_level,
    bool no_output,
    int num_keys,
    SoftwareMutex* lock
);

#endif 
