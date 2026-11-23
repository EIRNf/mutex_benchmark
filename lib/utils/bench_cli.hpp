#ifndef __BENCH_CLI_HPP_
#define __BENCH_CLI_HPP_

#pragma once

#include <string>
#include <vector>

// Parsed command-line arguments shared by the three benchmark apps
// (max_/min_/grouped_contention_bench). Not every app uses every field --
// see BenchCliOptions below for which flags each app actually accepts.
struct BenchCliArgs {
    std::string mutex_name;
    int num_threads = 0;
    double run_time_sec = 0.0;
    int num_groups = -1;      // only meaningful when BenchCliOptions.num_positional == 4
    bool csv = false;
    bool thread_level = false;
    bool no_output = false;
    bool rusage = false;
    bool low_contention = false;
    int stagger_ms = 0;
    int critical_delay = -1;      // legacy: busy-loop iterations, lock-level mode only
    int noncritical_delay = -1;   // legacy: busy-loop iterations, lock-level mode only
    double warmup_sec = 0.0;      // --warmup S: run uncounted for S seconds first
    long long critical_ns = 0;    // --critical-ns N: fixed work inside the critical section
    long long noncritical_ns = 0; // --noncritical-ns N: random work between acquisitions, mean N
    std::vector<int> cpus;        // --cpus A,B,...: pin thread i to cpus[i % size] (Linux)
};

// Per-app allow-list: which optional flags this app's parser should accept.
// --csv/-c and --thread-level are always accepted (all three apps already
// support these identically). Everything else -- including --rusage, which
// min_contention_bench does NOT support today -- is gated here. A flag not
// enabled is rejected with "Unrecognized flag" exactly as before; this
// parser never silently accepts-and-ignores a flag an app doesn't support.
struct BenchCliOptions {
    int num_positional;  // 3 for max/min (mutex_name, threads, seconds), 4 for grouped (+ num_groups)
    bool accept_rusage = false;
    bool accept_no_output = false;
    bool accept_low_contention = false;   // also gates --stagger-ms
    bool accept_critical_delay = false;
    bool accept_noncritical_delay = false;
    bool accept_warmup = false;            // --warmup
    bool accept_work_ns = false;           // --critical-ns, --noncritical-ns
    bool accept_cpus = false;              // --cpus
};

// Parses argv[1..argc). Flags may appear anywhere among the positional
// tokens; the first `opts.num_positional` non-flag tokens are collected in
// order as mutex_name, num_threads, run_time_sec[, num_groups]. On any
// failure (wrong positional count, unrecognized/rejected flag, or a
// value-flag missing its argument) prints a message to stderr and returns
// false.
bool parse_bench_cli(int argc, char* argv[], const BenchCliOptions& opts, BenchCliArgs& out);

#endif // __BENCH_CLI_HPP_
