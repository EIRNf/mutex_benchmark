
#include "bench_utils.hpp"
#include "lock.hpp"

struct per_thread_args {
    int thread_id;

    struct per_thread_stats stats;

    std::shared_ptr<std::atomic<bool>*> start_flags; // Shared flag to signal the start of the benchmark
    std::shared_ptr<std::atomic<bool>*> end_flags;
    SoftwareMutex *lock; // Pointer to the lock object, type depends on the lock implementation
    
};

struct run_args {
    int num_threads;
    struct per_thread_args **thread_args;
    struct rusage usage;
};

void schedule_flags(std::shared_ptr<std::atomic<bool>*> start_flags, std::shared_ptr<std::atomic<bool>*> end_flags, std::chrono::nanoseconds run_time, int num_groups);

int grouped_contention_bench(int num_threads, std::chrono::nanoseconds run_time, int num_groups, bool csv, SoftwareMutex* lock);
