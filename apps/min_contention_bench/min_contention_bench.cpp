
#include <time.h>
#include <string.h>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>

#include "min_contention_bench.hpp"
#include "bench_utils.hpp"
#include "cxl_utils.hpp"
#include "lock.hpp"

int min_contention_bench(
    int num_threads,
    double run_time,
    bool csv,
    bool thread_level,
    bool no_output,
    bool low_contention,
    int stagger_ms,
    SoftwareMutex* lock
) {
    lock->init(num_threads);
    auto start_flag = std::make_shared<std::atomic<bool>>(false);
    auto end_flag   = std::make_shared<std::atomic<bool>>(false);
    volatile int* counter = (volatile int*)malloc(sizeof(int));
    *counter = 0;

    std::vector<per_thread_args> thread_args(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        thread_args[i].thread_id            = i;
        thread_args[i].stats.run_time       = run_time;
        thread_args[i].stats.num_iterations = 0;
        thread_args[i].lock                 = lock;
        thread_args[i].start_flag           = start_flag;
        thread_args[i].end_flag             = end_flag;
    }

    std::thread thread;
    srand((unsigned)time(0)); 
    int i = (rand()%num_threads); 

    if (thread_level)
    {
        thread = std::thread([&, i]() {
            thread_args[i].stats.thread_id = i;
            if (low_contention && stagger_ms > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(i * stagger_ms)
                );
            }
            while (!*start_flag) {}
            while (!*end_flag) {
                lock->lock(i);
                thread_args[i].stats.num_iterations++;
                (*counter)++; // Critical section
                Fence(); //ensure that counter was updated before unlocking; required for any impl.
                lock->unlock(i);
            }
        });
    } else
    {
        thread = std::thread([&, i]() {
            thread_args[i].stats.thread_id = i;
            init_lock_timer(&thread_args[i].stats);
            if (low_contention && stagger_ms > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(i * stagger_ms)
                );
            }
            while (!*start_flag) {}
            struct timespec delay = {0,0}, rem;
            while (!*end_flag) {
                start_lock_timer(&thread_args[i].stats);
                lock->lock(i);
                (*counter)++;
                lock->unlock(i);
                end_lock_timer(&thread_args[i].stats);
                nanosleep(&delay, &rem);
                thread_args[i].stats.num_iterations++;
            }
        });
    }

    *start_flag = true;
    std::this_thread::sleep_for(std::chrono::duration<double>(run_time));
    *end_flag = true;

    if (thread.joinable()) thread.join();

    lock->destroy();
    free((void*)counter);
    delete lock;

    if (!no_output) {
        report_thread_latency(&thread_args[i].stats, csv, thread_level);
        if (!thread_level) destroy_lock_timer(&thread_args[i].stats);
    }

    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <mutex_name> <num_threads> <run_time_s> <max_noncrit_delay_ns> "
            "[--csv] [--thread-level] [--no-output] [--low-contention] [--stagger-ms ms]\n",
            argv[0]
        );
        return 1;
    }

    const char* mutex_name            = argv[1];
    int         num_threads           = atoi(argv[2]);
    double      run_time_sec          = atof(argv[3]);

    bool csv             = false;
    bool thread_level    = false;
    bool no_output       = false;
    bool low_contention  = false;
    int  stagger_ms      = 0;

    for (int i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "--csv") == 0) {
            csv = true;
        } else if (strcmp(argv[i], "--thread-level") == 0) {
            thread_level = true;
        } else if (strcmp(argv[i], "--no-output") == 0) {
            no_output = true;
        } else if (strcmp(argv[i], "--low-contention") == 0) {
            low_contention = true;
        } else if (strcmp(argv[i], "--stagger-ms") == 0 && i + 1 < argc) {
            stagger_ms = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unrecognized flag: %s\n", argv[i]);
            return 1;
        }
    }

    cxl_mutex_benchmark_init();

    SoftwareMutex *lock = get_mutex(mutex_name, num_threads);
    if (lock == nullptr) {
        fprintf(stderr, "Failed to initialize lock.\n");
        return 1;

    }

    int result = min_contention_bench(
        num_threads,
        run_time_sec,
        csv,
        thread_level,
        no_output,
        low_contention,
        stagger_ms,
        lock
    );

    cxl_mutex_benchmark_exit();
    
    return result;
}