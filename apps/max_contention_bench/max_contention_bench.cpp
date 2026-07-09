
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
#ifdef __linux__
#include <numa.h>
#include <numaif.h>
#endif
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

#ifdef cxl
#elif defined(hardware_cxl)
    int numa = numa_available()+1;
#else
    int numa = 0;
#endif

    // Set process-wide memory policy to node 2 to prevent any allocations on nodes 0 and 1
    setup_numa_mempolicy_or_warn(numa);

    // Create run args structure to hold thread arguments
    // struct run_args args;
    // args.num_threads = num_threads;
    // args.thread_args = new per_thread_args*[num_threads];

    // Create shared memory for the lock
    // This could be a simple pointer or a more complex shared memory structure
    // void* shared_memory = nullptr; // Replace with actual shared memory allocation if needed

    // Initialize the lock

    // Get current memory policy
#ifdef __linux__
    int mode;
    unsigned long nodemask[16] = {0};
    unsigned long maxnode = sizeof(nodemask) * 8;
    int preferred_node = -1;

    // COMMENTED OUT: Using memeater for preallocation instead of mlock
    // The following large allocation and mlock code is disabled since we're using
    // memeater to preallocate memory on the desired NUMA nodes
    /*
    // Allocate and lock 256GB of memory with NUMA policy
    size_t large_alloc_size = 256UL * 1024 * 1024 * 1024; // 256GB
    void* large_mem = nullptr;

    if (get_mempolicy(&mode, nodemask, maxnode, nullptr, 0) == 0) {
        fprintf(stderr, "DEBUG: get_mempolicy succeeded, mode=%d\n", mode);
        fprintf(stderr, "DEBUG: nodemask[0]=0x%lx\n", nodemask[0]);

        if (mode == MPOL_INTERLEAVE) {
            fprintf(stderr, "DEBUG: Using INTERLEAVE mode\n");
            // For interleave mode, allocate with numa_alloc_interleaved
            if (numa_available() >= 0) {
                // Set the interleave mask to match the policy
                struct bitmask *mask = numa_allocate_nodemask();
                for (int node = 0; node <= numa_max_node(); node++) {
                    if (nodemask[node / (sizeof(unsigned long) * 8)] & (1UL << (node % (sizeof(unsigned long) * 8)))) {
                        numa_bitmask_setbit(mask, node);
                        fprintf(stderr, "DEBUG: Adding node %d to interleave mask\n", node);
                    }
                }

                // Use numa_alloc_interleaved_subset to allocate only on specified nodes
                fprintf(stderr, "DEBUG: Set interleave mask, allocating %zu bytes\n", large_alloc_size);

                // numa_alloc_interleaved_subset allocates on the specified nodes only
                large_mem = numa_alloc_interleaved_subset(large_alloc_size, mask);

                numa_free_nodemask(mask);
            }
        } else if (mode == MPOL_BIND || mode == MPOL_PREFERRED) {
            // For bind/preferred mode, allocate on specific node
            for (int node = 0; node <= numa_max_node(); node++) {
                if (nodemask[node / (sizeof(unsigned long) * 8)] & (1UL << (node % (sizeof(unsigned long) * 8)))) {
                    preferred_node = node;
                    fprintf(stderr, "DEBUG: Found node %d in mask\n", node);
                    break;
                }
            }
            if (preferred_node >= 0 && numa_available() >= 0) {
                fprintf(stderr, "DEBUG: Allocating 256GB on NUMA node %d\n", preferred_node);
                large_mem = numa_alloc_onnode(large_alloc_size, preferred_node);
            }
        }

        if (large_mem != nullptr) {
            fprintf(stderr, "DEBUG: Allocated 256GB at %p\n", large_mem);
            // Lock the memory to prevent swapping
            if (mlock(large_mem, large_alloc_size) == 0) {
                fprintf(stderr, "DEBUG: Successfully locked 256GB of memory\n");
                // Touch the memory to force allocation
                memset(large_mem, 0, large_alloc_size);
                fprintf(stderr, "DEBUG: Memory touched\n");
            } else {
                fprintf(stderr, "DEBUG: Failed to lock memory: %s\n", strerror(errno));
            }
        } else {
            fprintf(stderr, "DEBUG: Failed to allocate 256GB: %s\n", strerror(errno));
        }
    }
    */

    // Get memory policy for smaller allocations
    if(numa){
        if (get_mempolicy(&mode, nodemask, maxnode, nullptr, 0) == 0) {
            fprintf(stderr, "DEBUG: get_mempolicy succeeded, mode=%d\n", mode);
            fprintf(stderr, "DEBUG: nodemask[0]=0x%lx\n", nodemask[0]);

            if (mode == MPOL_BIND || mode == MPOL_PREFERRED) {
                for (int node = 0; node <= numa_max_node(); node++) {
                    if (nodemask[node / (sizeof(unsigned long) * 8)] & (1UL << (node % (sizeof(unsigned long) * 8)))) {
                        preferred_node = node;
                        fprintf(stderr, "DEBUG: Found node %d in mask\n", node);
                        break;
                    }
                }
            }
        }
    }
#endif


    lock->init(num_threads);

    // Use NUMA-aware allocation that respects numactl settings
    volatile int* counter;
    volatile int* last;
    volatile int* total_unfair;

    // Force allocate control variables on node 2
#ifdef __linux__
    if (numa) {
        fprintf(stderr, "DEBUG: Allocating control variables on node 2\n");
        counter = (volatile int*)numa_alloc_onnode(sizeof(int), 2);
        last = (volatile int*)numa_alloc_onnode(sizeof(int), 2);
        total_unfair = (volatile int*)numa_alloc_onnode(sizeof(int), 2);
        fprintf(stderr, "DEBUG: Allocated counter=%p, last=%p, total_unfair=%p\n", counter, last, total_unfair);
    } else
#endif
    {
        fprintf(stderr, "DEBUG: NUMA not available, using regular allocation\n");
        counter = new int;
        last = new int;
        total_unfair = new int;
    }

    *counter = 0;
    *last = -1;
    *total_unfair = 0;
    
    // Allocate flags on node 2 to avoid brk() calls
    std::atomic<bool>* start_flag_raw = nullptr;
    std::atomic<bool>* end_flag_raw = nullptr;

#ifdef __linux__
    if (numa) {
        start_flag_raw = (std::atomic<bool>*)numa_alloc_onnode(sizeof(std::atomic<bool>), 2);
        end_flag_raw = (std::atomic<bool>*)numa_alloc_onnode(sizeof(std::atomic<bool>), 2);
        new (start_flag_raw) std::atomic<bool>(false);
        new (end_flag_raw) std::atomic<bool>(false);
    } else
#endif
    {
        start_flag_raw = new std::atomic<bool>(false);
        end_flag_raw = new std::atomic<bool>(false);
    }

#ifdef __linux__
    auto start_flag = std::shared_ptr<std::atomic<bool>>(start_flag_raw, [numa](std::atomic<bool>* p) {
        p->~atomic();
        if (numa) numa_free(p, sizeof(std::atomic<bool>));
        else delete p;
    });
    auto end_flag = std::shared_ptr<std::atomic<bool>>(end_flag_raw, [numa](std::atomic<bool>* p) {
        p->~atomic();
        if (numa) numa_free(p, sizeof(std::atomic<bool>));
        else delete p;
    });
#else
    auto start_flag = std::shared_ptr<std::atomic<bool>>(start_flag_raw);
    auto end_flag = std::shared_ptr<std::atomic<bool>>(end_flag_raw);
#endif

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
                        (*total_unfair)++;
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
                        (*total_unfair)++;
                    }
                    (*counter)++;
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

    // Free NUMA-allocated memory properly
#ifdef __linux__
    if (numa) {
        numa_free((void*)counter, sizeof(int));
        numa_free((void*)last, sizeof(int));
        numa_free((void*)total_unfair, sizeof(int));
    } else
#endif
    {
        delete counter;
        delete last;
        delete total_unfair;
    }

    destroy_and_delete_lock(lock, numa);

    // COMMENTED OUT: Using memeater for preallocation instead
    /*
    // Free the large allocation
    if (large_mem != nullptr) {
        munlock(large_mem, large_alloc_size);
        numa_free(large_mem, large_alloc_size);
        fprintf(stderr, "DEBUG: Freed 256GB allocation\n");
    }
    */


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

    cxl_mutex_benchmark_init();

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

    cxl_mutex_benchmark_exit();

    return result;
}
