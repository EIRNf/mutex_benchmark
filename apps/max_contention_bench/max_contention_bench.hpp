
#include "bench_utils.hpp"
#include "lock.hpp"

struct per_thread_args {
    int thread_id;

    struct per_thread_stats stats;

    std::shared_ptr<std::atomic<bool>> start_flag; // Shared flag to signal the start of the benchmark
    std::shared_ptr<std::atomic<bool>> end_flag;
    SoftwareMutex *lock; // Pointer to the lock object, type depends on the lock implementation
    
};

struct run_args {
    int num_threads;
    struct per_thread_args **thread_args;
    struct rusage usage;
};

int max_contention_bench(int num_threads, std::chrono::seconds run_time, bool csv, bool rusage, bool thread_level, bool no_output, int max_noncritical_delay_ns, SoftwareMutex* lock);
