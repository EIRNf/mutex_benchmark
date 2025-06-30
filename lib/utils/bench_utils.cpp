#include "bench_utils.hpp"

#include <stdio.h>
#include <cstdlib>

void record_rusage() {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        print_rusage(&usage);
    } else {
        perror("getrusage failed");
    }
}

void print_rusage(struct rusage *usage) {
    printf("User CPU time used: %ld.%06d seconds\n", 
           usage->ru_utime.tv_sec, usage->ru_utime.tv_usec);
    printf("System CPU time used: %ld.%06d seconds\n", 
           usage->ru_stime.tv_sec, usage->ru_stime.tv_usec);
    printf("Maximum resident set size: %ld KB\n", usage->ru_maxrss);
    printf("Integral shared memory size: %ld KB\n", usage->ru_ixrss);
    printf("Integral unshared data size: %ld KB\n", usage->ru_idrss);
    printf("Integral unshared stack size: %ld KB\n", usage->ru_isrss);
    printf("Page reclaims (soft page faults): %ld\n", usage->ru_minflt);
    printf("Page faults (hard page faults): %ld\n", usage->ru_majflt);
    printf("Swaps: %ld\n", usage->ru_nswap);
    printf("Block input operations: %ld\n", usage->ru_inblock);
    printf("Block output operations: %ld\n", usage->ru_oublock);
    printf("IPC messages sent: %ld\n", usage->ru_msgsnd);
    printf("IPC messages received: %ld\n", usage->ru_msgrcv);
    printf("Signals received: %ld\n", usage->ru_nsignals);
    printf("Voluntary context switches: %ld\n", usage->ru_nvcsw);
    printf("Involuntary context switches: %ld\n", usage->ru_nivcsw);
}

void start_timer(struct per_thread_stats *stats) {
    clock_gettime(CLOCK_MONOTONIC, &stats->start_time);
}

void end_timer(struct per_thread_stats *stats) {
    clock_gettime(CLOCK_MONOTONIC, &stats->end_time);
}

void init_lock_timer(struct per_thread_stats *stats) {
    stats->lock_times = (double *)malloc(sizeof(double) * stats->num_iterations);
}

void start_lock_timer(struct per_thread_stats *stats, size_t index) {
    (void)index; // Not used but could be used in the future.
    clock_gettime(CLOCK_MONOTONIC, &stats->start_time);
}

double get_elapsed_time(struct timespec start_time, struct timespec end_time) {
    long seconds = end_time.tv_sec - start_time.tv_sec;
    long nanoseconds = end_time.tv_nsec - start_time.tv_nsec;

    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1e9;
    }

    double elapsed = seconds + nanoseconds / 1e9;
    return elapsed;
}

void end_lock_timer(struct per_thread_stats *stats, size_t index) {
    clock_gettime(CLOCK_MONOTONIC, &stats->end_time);
    stats->lock_times[index] = get_elapsed_time(stats->start_time, stats->end_time);
}

void report_thread_latency(struct per_thread_stats *stats, bool csv, bool thread_level) {
    if (thread_level) {
        double elapsed = get_elapsed_time(stats->start_time, stats->end_time);
        
        if (csv) {
            printf("%d,%d,%.6f\n", stats->thread_id, stats->num_iterations, elapsed);
        } else {
            printf("Thread %d: %d iterations completed in %.6f seconds\n",
                stats->thread_id, stats->num_iterations, elapsed);
        }
    } else {
        double elapsed = get_elapsed_time(stats->start_time, stats->end_time);
        
        if (csv) {
            for (int i = 0; i < stats->num_iterations; i++) {
                // Thread ID, Iteration #, Time to lock
                printf("%d,%d,%.9f\n", stats->thread_id, i, stats->lock_times[i]);
            }
        }
        else {
            printf("Thread %d: %d iterations completed in %.6f seconds\n",
                stats->thread_id, stats->num_iterations, elapsed);
            for (int i = 0; i < stats->num_iterations; i++) {
                // Thread ID, Iteration #, Time to lock
                printf("    #%d: iteration %d took %.9f seconds\n", stats->thread_id, i, stats->lock_times[i]);
            }
        }
    }
}

void report_run_latency(struct run_args *stats){
    printf("Run statistics:\n");
    (void)stats;
}