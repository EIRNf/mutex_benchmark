#include "grouped_contention_bench.hpp"
#include <string.h>
#include <stdio.h>
#include "bench_utils.hpp"
#include "bench_harness.hpp"
#include "bench_cli.hpp"
#include "cxl_utils.hpp"
#include "memory.h"
#include "stdio.h"
#include <string>
#include <stdlib.h>

#include "lock.hpp"

#ifdef __linux__
#include <numa.h>
#endif

int grouped_contention_bench(int num_threads, double run_time, int num_groups, bool csv, bool rusage, SoftwareMutex* lock) {

#ifdef hardware_cxl
    int numa = numa_available()+1;
#else
    int numa = 0;
#endif

    // Create run args structure to hold thread arguments
    // struct run_args args;
    // args.num_threads = num_threads;
    // args.thread_args = new per_thread_args*[num_threads];

    // Create shared memory for the lock
    // This could be a simple pointer or a more complex shared memory structure
    // void* shared_memory = nullptr; // Replace with actual shared memory allocation if needed

    // Initialize the lock
    lock->init(num_threads);

    // Create a flag to signal the threads to start
    std::shared_ptr<std::atomic<bool>*> start_flags = std::make_shared<std::atomic<bool>*>((std::atomic<bool>*) malloc(sizeof(std::atomic<bool>*) * num_groups));
    std::shared_ptr<std::atomic<bool>*> end_flags = std::make_shared<std::atomic<bool>*>((std::atomic<bool>*) malloc(sizeof(std::atomic<bool>*) * num_groups));

    for (int i=0; i<num_groups; i++){
        (*start_flags)[i]=false;
        (*end_flags)[i]=false;
    }

    // Create an array of thread arguments
    std::vector<per_thread_args> thread_args(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        thread_args[i].thread_id = i;
        thread_args[i].lock = lock; // Pass the lock to each thread
        thread_args[i].stats.run_time=run_time;
    }

    // Create an array of threads
    std::vector<std::thread> threads(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        thread_args[i].start_flags = start_flags; // Share the start flag with each thread
        thread_args[i].end_flags = end_flags;
        threads[i] = std::thread([&, i]() {

                // Record the thread ID
                thread_args[i].stats.thread_id = thread_args[i].thread_id;
            
                // Each thread will run this function

                int group_num = num_groups*(((double)thread_args[i].thread_id)/((double)num_threads));
                while (!(*thread_args[i].start_flags)[group_num]) {
                    // Wait until the start flag is set
                }
                while(!(*thread_args[i].end_flags)[group_num]){
                    // Perform the locking operations
                    thread_args[i].lock->lock(thread_args[i].thread_id);
                    thread_args[i].stats.num_iterations++;
                    lock->criticalSection(i);
                    thread_args[i].lock->unlock(thread_args[i].thread_id);

                }
                thread_args[i].stats.num_iterations--;
        });
    }

    schedule_flags(start_flags, end_flags, run_time, num_groups);

    // Wait for all threads to finish
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }


    if (rusage){
        record_rusage(csv);
    }

    // Cleanup resources: destroy and free the lock (numa_delete when the
    // lock was NUMA-allocated by get_mutex(), plain delete otherwise).
    destroy_and_delete_lock(lock, numa);

    // Output benchmark results

    if (!rusage){
        for (auto& args : thread_args) {
            report_thread_latency(&args.stats, csv, true); // Report latency for each thread
        }
    }

    // record_rusage(); // Record resource usage
    // report_latency(&args); // Report latency if needed
    return 0;
}

void schedule_flags(std::shared_ptr<std::atomic<bool>*> start_flags, std::shared_ptr<std::atomic<bool>*> end_flags, double run_time, int num_groups){
    for (int i=0; i<num_groups; i++){

        (*start_flags)[i]=true;
        std::this_thread::sleep_for(std::chrono::duration<double>(run_time));
        (*end_flags)[i]=true;

    }
    
}

int main(int argc, char* argv[]) {
    BenchCliOptions opts;
    opts.num_positional = 4;
    opts.accept_rusage = true;

    BenchCliArgs args;
    if (!parse_bench_cli(argc, argv, opts, args)) {
        fprintf(stderr,
            "Usage: %s <mutex_name> <num_threads> <run_time_per_group> <num_groups> "
            "[--csv] [--thread-level] [--rusage]\n",
            argv[0]
        );
        return 1;
    }

    if (args.num_threads % args.num_groups != 0) {
        fprintf(stderr, "Number of threads must be evenly divisible by number of groups\n");
        return 1;
    }

    cxl_mutex_benchmark_init();

    SoftwareMutex *lock = get_mutex(args.mutex_name.c_str(), args.num_threads);
    if (lock == nullptr) {
        fprintf(stderr, "Failed to initialize lock.\n");
        return 1;
    }

    int result = grouped_contention_bench(args.num_threads, args.run_time_sec, args.num_groups, args.csv, args.rusage, lock);

    cxl_mutex_benchmark_exit();

    return result;
}