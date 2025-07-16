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
#include "../lock/lock.hpp"

// asm fence
#if defined(__x86_64)
    //#define Fence() __asm__ __volatile__ ( "mfence" )
    #define Fence() __asm__ __volatile__ ( "lock; addq $0,128(%%rsp);" ::: "cc" )
#elif defined(__i386)
    #define Fence() __asm__ __volatile__ ( "lock; addl $0,128(%%esp);" ::: "cc" )
#elif defined(__ARM_ARCH)
    #define Fence() __asm__ __volatile__ ( "DMB ISH" ::: )
#else
    #error unsupported architecture
#endif

struct per_thread_stats {
    int thread_id;
    int num_iterations;

    double run_time;
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

void record_rusage();
void print_rusage(struct rusage *usage);

void init_lock_timer(struct per_thread_stats *stats);
void start_lock_timer(struct per_thread_stats *stats);
void end_lock_timer(struct per_thread_stats *stats);
void destroy_lock_timer(struct per_thread_stats *stats);

// void start_timer(struct per_thread_stats *stats);
// void end_timer(struct per_thread_stats *stats);

// void report_thread_stats(struct per_thread_stats *stats, bool csv = false, bool thread_level = true);
void report_run_latency(struct run_stats *stats);

void report_thread_latency(struct per_thread_stats *stats, bool csv, bool thread_level);

void busy_sleep(size_t iterations);
SoftwareMutex *get_mutex(const char *mutex_name, size_t num_threads);
void cleanup_mutex(SoftwareMutex *mutex);

#endif // __BENCH_UTILS_HPP_