
#include "bench_utils.hpp"
#include "lock.hpp"

struct per_thread_args {
    int thread_id;

    struct per_thread_stats stats;

    std::shared_ptr<std::atomic<bool>> start_flag; // Shared flag to signal the start of the benchmark
    SoftwareMutex *lock; // Pointer to the lock object, type depends on the lock implementation
    
};

struct run_args {
    int num_threads;
    struct per_thread_args **thread_args;
    struct rusage usage;
};

void max_contention_bench(int num_threads, int num_iterations);
