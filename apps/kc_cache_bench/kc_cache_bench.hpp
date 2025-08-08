// kc_cache_bench.hpp

#ifndef KC_CACHE_BENCH_HPP
#define KC_CACHE_BENCH_HPP

#include "bench_utils.hpp"
#include "lock.hpp"
#include <chrono>
#include <atomic>
#include <memory>

class SoftwareMutex;

struct per_thread_args
{
    int thread_id;
    per_thread_stats stats;
    std::shared_ptr<std::atomic<bool>> start_flag;
    std::shared_ptr<std::atomic<bool>> end_flag;
};


class kc_cache_bench
{
public:
    // static SoftwareMutex *db_lock;


    /**
     * @brief Run a contention benchmark.
     *
     * @param num_threads               Number of competing threads.
     * @param run_time                  Duration to run the benchmark.
     * @param csv                       If true, output in CSV format.
     * @param no_output                 If true, suppress printing of per-thread stats.
     * @return int                      0 on success, 1 on error (e.g. counter mismatch).
     */

    static void run(int num_threads,
                   double run_time,
                   int capsiz,
                   bool csv,
                   bool rusage,
                   bool no_output);

};

#endif
