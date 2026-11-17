
#include <time.h>
#include <string.h>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>
#include <string>
#include <errno.h>
#include <sys/mman.h>
#include <string.h>

#include "max_contention_bench.hpp"
#include "bench_utils.hpp"
#include "bench_harness.hpp"
#include "bench_cli.hpp"
#include "cxl_utils.hpp"
#include "lock.hpp"

int max_contention_bench(
    int num_threads, 
    double run_time, 
    bool csv, 
    bool rusage,
    bool thread_level, 
    bool no_output, 
    int max_critical_delay_iterations, 
    int max_noncritical_delay_iterations, 
    bool low_contention,
    int stagger_ms,
    SoftwareMutex* lock
) {

    // Placement policy (shared by all benchmark apps): only lock memory is
    // NUMA-placed in a -Dhardware_cxl build (get_mutex()/ALLOCATE). Benchmark
    // bookkeeping uses ordinary memory; to bind the whole process to a node,
    // run under `numactl --membind` (placement = "process" in the configs).
    lock->init(num_threads);

    volatile int* counter = new int(0);
    volatile int* last = new int(-1);
    volatile int* total_unfair = new int(0);
    auto start_flag = std::make_shared<std::atomic<bool>>(false);
    auto end_flag = std::make_shared<std::atomic<bool>>(false);

    std::vector<per_thread_args> thread_args(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        thread_args[i].thread_id            = i;
        thread_args[i].stats.run_time       = run_time;
        thread_args[i].stats.num_iterations = 0;
        thread_args[i].lock                 = lock;
        thread_args[i].start_flag           = start_flag;
        thread_args[i].end_flag             = end_flag;
    }


    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (int i = 0; i < num_threads; ++i) {
        if (thread_level) {
            threads.emplace_back([&, i]() {
                thread_args[i].stats.thread_id = i;
                if (low_contention && stagger_ms > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(i * stagger_ms)
                    );

                }
                while (!*start_flag) {}
                while (!*end_flag) {
                    lock->lock(i);
                    // Critical section
                    if (*last == i) {
                        // lock->unlock(i);
                        // continue;
                        *total_unfair = *total_unfair + 1;
                    }
                    *last = i;
                    thread_args[i].stats.num_iterations++;
                    (*counter) += lock->criticalSection(i);
                    Fence();
                    lock->unlock(i);
                }
            });
        } else {
            // Lock level
            threads.emplace_back([&, i]() {
                thread_args[i].stats.thread_id = i;
                init_lock_timer(&thread_args[i].stats);

                while (!*start_flag) {}

                if (low_contention && stagger_ms > 0) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(i * stagger_ms)
                    );
                }

                while (!*end_flag) {
                    // Lock
                    start_lock_timer(&thread_args[i].stats);
                    lock->lock(i);

                    // Critical section
                    if (*last == i) {
                        // lock->unlock(i);
                        // continue;
                        *total_unfair = *total_unfair + 1;
                    }
                    *counter = *counter + 1;
                    *last = i;
                    Fence();
                    busy_sleep(rand() % max_critical_delay_iterations);

                    // Unlock
                    lock->unlock(i);
                    end_lock_timer(&thread_args[i].stats);

                    // Noncritical section
                    busy_sleep(rand() % max_noncritical_delay_iterations);
                    thread_args[i].stats.num_iterations++;
                }
            });
        }
    }

    *start_flag = true;
    std::this_thread::sleep_for(std::chrono::duration<double>(run_time));
    *end_flag = true;

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    if (rusage && !no_output){
        record_rusage(csv);
    }

    int expectedCounter=0;
    for (auto& targs : thread_args){
        expectedCounter+=targs.stats.num_iterations;
    }

    if (expectedCounter != *counter)
    {
        fprintf(stderr,
            "%s was not correct: expectedCounter %i did not match real counter %i",
            lock->name().c_str(), expectedCounter, *counter
        );
    }

    if (!csv) {
        double unfair_percentage = 100.0 * (double)*total_unfair / (double)expectedCounter;
        printf("Unfairness: %d/%d of all lock passes were from a mutex back to itself. (%f%%)\n", 
            *total_unfair, expectedCounter, unfair_percentage);
    }

    delete counter;
    delete last;
    delete total_unfair;

    destroy_and_delete_lock(lock);

    if (!no_output && !rusage) {
        for (auto& targs : thread_args) {
            report_thread_latency(&targs.stats, csv, thread_level);
            if (!thread_level) destroy_lock_timer(&targs.stats);
        }
    }

    return 0;
}

int main(int argc, char* argv[]) {
    BenchCliOptions opts;
    opts.num_positional = 3;
    opts.accept_rusage = true;
    opts.accept_no_output = true;
    opts.accept_low_contention = true;
    opts.accept_critical_delay = true;
    opts.accept_noncritical_delay = true;

    BenchCliArgs args;
    if (!parse_bench_cli(argc, argv, opts, args)) {
        fprintf(stderr,
            "Usage: %s <mutex_name> <num_threads> <run_time_s> "
            "[--csv] [--thread-level] [--rusage] [--no-output] [--low-contention] "
            "[--stagger-ms ms] [--critical-delay N] [--noncritical-delay N]\n",
            argv[0]
        );
        return 1;
    }

    // 1 is the default because it's the exclusive maximum (rand() % delay)
    // so it can't be 0.
    int max_critical_delay    = args.critical_delay > 0    ? args.critical_delay    : 1;
    int max_noncritical_delay = args.noncritical_delay > 0 ? args.noncritical_delay : 1;

    SoftwareMutex *lock = get_mutex(args.mutex_name.c_str(), args.num_threads);
    if (lock == nullptr) {
        fprintf(stderr, "Failed to initialize lock.\n");
        return 1;
    }

    int result = max_contention_bench(
        args.num_threads,
        args.run_time_sec,
        args.csv,
        args.rusage,
        args.thread_level,
        args.no_output,
        max_critical_delay,
        max_noncritical_delay,
        args.low_contention,
        args.stagger_ms,
        lock
    );

    return result;
}
