#include "bench_utils.hpp"

#include <stdio.h>
#include <cstdlib>
#include <vector>



void record_rusage(bool csv) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        print_rusage(&usage, csv);
    } else {
        perror("getrusage failed");
    }
}

void print_rusage(struct rusage *usage, bool csv) {
    if (!csv){
        printf("User CPU time used: %ld.%06ld seconds\n", 
            usage->ru_utime.tv_sec, usage->ru_utime.tv_usec); //slightly important, not as much
        printf("System CPU time used: %ld.%06ld seconds\n", 
            usage->ru_stime.tv_sec, usage->ru_stime.tv_usec); //important and relevant
        printf("Maximum resident set size: %ld KB\n", usage->ru_maxrss); //not too important?
        printf("Integral shared memory size: %ld KB\n", usage->ru_ixrss); //unmantained
        printf("Integral unshared data size: %ld KB\n", usage->ru_idrss); //unmantained
        printf("Integral unshared stack size: %ld KB\n", usage->ru_isrss); //unmantained
        printf("Page reclaims (soft page faults): %ld\n", usage->ru_minflt); //slightly important
        printf("Page faults (hard page faults): %ld\n", usage->ru_majflt); //slightly important
        printf("Swaps: %ld\n", usage->ru_nswap); //unmantained 
        printf("Block input operations: %ld\n", usage->ru_inblock); //just linux
        printf("Block output operations: %ld\n", usage->ru_oublock); //just linux
        printf("IPC messages sent: %ld\n", usage->ru_msgsnd); //unmantained
        printf("IPC messages received: %ld\n", usage->ru_msgrcv); //unmantained
        printf("Signals received: %ld\n", usage->ru_nsignals); //unmantained
        printf("Voluntary context switches: %ld\n", usage->ru_nvcsw); //just linux
        printf("Involuntary context switches: %ld\n", usage->ru_nivcsw); //just linux
    }
    else{
        printf("%ld.%06ld,%ld.%06ld,%ld,%ld,%ld", usage->ru_utime.tv_sec, usage->ru_utime.tv_usec, usage->ru_stime.tv_sec, usage->ru_stime.tv_usec, usage->ru_maxrss, usage->ru_minflt, usage->ru_majflt);
    }
}

void init_lock_timer(struct per_thread_stats *stats) {
    // Currently unused.
    (void)stats;
}

void start_lock_timer(struct per_thread_stats *stats) {
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
    if (elapsed < 0.0) {
        return 0.0;
    }
    return elapsed;
}

void end_lock_timer(struct per_thread_stats *stats) {
    clock_gettime(CLOCK_MONOTONIC, &stats->end_time);
    double lock_time = get_elapsed_time(stats->start_time, stats->end_time);
    stats->lock_times.push_back(lock_time);
}

void destroy_lock_timer(struct per_thread_stats *stats) {
    (void)stats; // Currently
}

void report_thread_latency(struct per_thread_stats *stats, bool csv, bool thread_level) {
    if (thread_level) {
        if (csv) {
            // Thread ID, Runtime, # Iterations
            printf("%d,%ld,%d\n", stats->thread_id, stats->run_time.count(), stats->num_iterations);
        } else {
            printf("Thread %d: %d iterations completed in %ld seconds\n",
                stats->thread_id, stats->num_iterations, stats->run_time.count());
        }
    } else {
        if (csv) {
            for (int i = 0; i < stats->num_iterations; i++) {
                // Thread ID, Iteration #, Time to lock
                printf("%d,%d,%.10f\n", stats->thread_id, i, stats->lock_times[i]);
            }
        }
        else {
            printf("Thread %d: %d iterations completed in %ld seconds\n",
                stats->thread_id, stats->num_iterations, stats->run_time.count());
            for (int i = 0; i < stats->num_iterations; i++) {
                // Thread ID, Iteration #, Time to lock
                printf("    #%d: iteration %d took %.10f seconds\n", stats->thread_id, i, stats->lock_times[i]);
            }
        }
    }
}

void report_run_latency(struct run_args *stats){
    printf("Run statistics:\n");
    (void)stats;
}


