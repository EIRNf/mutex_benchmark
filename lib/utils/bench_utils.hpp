#ifndef __BENCH_UTILS_HPP_
#define __BENCH_UTILS_HPP_

#pragma once

#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
#include <memory>
#include <atomic>
#include <chrono>
#include <vector>


struct per_thread_stats {
    int thread_id;
    int num_iterations;

    std::chrono::nanoseconds run_time;
    struct timespec start_time;
    struct timespec end_time;
    // Vector reallocation could waste some thread time.
    std::vector<double> lock_times;

};


struct run_stats {
    int num_threads;
    struct per_thread_stats **thread_stats;
    struct rusage usage;
};


// Use getrusage  to record resource usage

void record_rusage(bool csv);
void print_rusage(struct rusage *usage, bool csv);

void init_lock_timer(struct per_thread_stats *stats);
void start_lock_timer(struct per_thread_stats *stats);
void end_lock_timer(struct per_thread_stats *stats);
void destroy_lock_timer(struct per_thread_stats *stats);

// void start_timer(struct per_thread_stats *stats);
// void end_timer(struct per_thread_stats *stats);

// void report_thread_stats(struct per_thread_stats *stats, bool csv = false, bool thread_level = true);
void report_run_latency(struct run_stats *stats);

void report_thread_latency(struct per_thread_stats *stats, bool csv, bool thread_level);

#endif // __BENCH_UTILS_HPP_