
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

#include "min_contention_bench.hpp"
#include "bench_utils.hpp"
#include "lock.hpp"

// Note: the problem with this benchmark is that many mutexes store internal state with thread_local variables that could cause problems because
// if we use the same thread every time, the state gets recycled when it shouldn't be.
// The benchmarking overhead is likely also very significant here.
int min_contention_bench(
    int num_threads, 
    double _run_time, 
    bool csv, 
    bool no_output, 
    SoftwareMutex* lock
) {
    // Create run args structure to hold thread arguments
    // struct run_args args;
    // args.num_threads = num_threads;
    // args.thread_args = new per_thread_args*[num_threads];

    // Create shared memory for the lock
    // This could be a simple pointer or a more complex shared memory structure
    // void* shared_memory = nullptr; // Replace with actual shared memory allocation if needed

    // Initialize the lock
    lock->init(num_threads);

    per_thread_args thread_args;
    thread_args.thread_id            = 0;
    thread_args.stats.run_time       = _run_time;
    thread_args.stats.num_iterations = 0;
    thread_args.lock                 = lock;

    init_lock_timer(&thread_args.stats);
    auto experiment_start_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_time;
    std::chrono::duration<double> run_time = std::chrono::duration<double>(_run_time);

    do {
        // Lock
        start_lock_timer(&thread_args.stats);
        lock->lock(rand() % num_threads);

        // Critical section

        // Unlock
        lock->unlock(rand() % num_threads);
        end_lock_timer(&thread_args.stats);

        // Noncritical section
        thread_args.stats.num_iterations++;
        auto current_time = std::chrono::high_resolution_clock::now();
        elapsed_time = current_time - experiment_start_time;
    } while (elapsed_time < run_time);

    lock->destroy();
    delete lock;

    if (!no_output) {
        report_thread_latency(&thread_args.stats, csv, false);
        destroy_lock_timer(&thread_args.stats);
    }

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <mutex_name> <num_threads> <run_time_s>"
            "[--csv] [--thread-level] [--no-output] [--low-contention] [--stagger-ms ms]\n",
            argv[0]
        );
        return 1;
    }

    const char* mutex_name            = argv[1];
    int         num_threads           = atoi(argv[2]);
    double      run_time_sec          = atof(argv[3]);

    bool csv          = false;
    bool no_output    = false;

    for (int i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "--csv") == 0) {
            csv = true;
        } else if (strcmp(argv[i], "--no-output") == 0) {
            no_output = true;
        } else {
            fprintf(stderr, "Unrecognized flag: %s\n", argv[i]);
            return 1;
        }
    }

    SoftwareMutex *lock = get_mutex(mutex_name, num_threads);
    if (lock == nullptr) {
        return 1;
    }

    int result = min_contention_bench(num_threads, run_time_sec, csv, no_output, lock);
    cleanup_mutex(lock);
    return result;
}
