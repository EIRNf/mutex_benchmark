
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

#include "max_contention_bench.hpp"
#include "bench_utils.hpp"
#include "lock.hpp"

int max_contention_bench(
    int num_threads, 
    double run_time, 
    bool csv, 
    bool thread_level, 
    bool no_output, 
    int max_critical_delay_iterations, 
    int max_noncritical_delay_iterations, 
    bool low_contention,
    int stagger_ms,
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
                    thread_args[i].stats.num_iterations++;
                    (*counter)++; // Critical section
                    Fence(); //ensure that counter was updated before unlocking; required for any impl.
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
                    (*counter)++;
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

    int expected = 0;
    for (auto& targs : thread_args) {
        expected += targs.stats.num_iterations;
    }
    if (*counter != expected) {
        fprintf(stderr,
            "Mutex %s failed; counter = %d, expected = %d\n",
            lock->name().c_str(), *counter, expected
        );
        return 1;
    }

    lock->destroy();
    free((void*)counter);
    delete lock;

    if (!no_output) {
        for (auto& targs : thread_args) {
            report_thread_latency(&targs.stats, csv, thread_level);
            if (!thread_level) destroy_lock_timer(&targs.stats);
        }
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

    bool csv                   = false;
    bool thread_level          = false;
    bool no_output             = false;
    bool low_contention        = false;
    int  stagger_ms            = 0;
    int  max_noncritical_delay = -1;
    int  max_critical_delay    = -1;

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
        } else if (strcmp(argv[i], "--noncritical-delay") == 0 && i + 1 < argc) {
            max_noncritical_delay = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--critical-delay") == 0 && i + 1 < argc) {
            max_critical_delay = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unrecognized flag: %s\n", argv[i]);
            return 1;
        }
    }

    // 1 is the default because it's the exclusive maximum (rand() % delay)
    // so it can't be 0.
    if (max_noncritical_delay <= 0) {
        max_noncritical_delay = 1;
    }
    if (max_critical_delay <= 0) {
        max_critical_delay = 1;
    }

    SoftwareMutex *lock = get_mutex(mutex_name, num_threads);
    if (lock == nullptr) {
        return 1;
    }

    int result = max_contention_bench(
        num_threads,
        run_time_sec,
        csv,
        thread_level,
        no_output,
        max_noncritical_delay,
        max_critical_delay,
        low_contention,
        stagger_ms,
        lock
    );

    cleanup_mutex(lock);

    return result;
}
