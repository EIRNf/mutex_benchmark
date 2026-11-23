// max_contention_bench.hpp

#ifndef MAX_CONTENTION_BENCH_HPP
#define MAX_CONTENTION_BENCH_HPP

#include "lock.hpp"

#include <vector>

// Exit status when the shared counter shows lost updates: two threads were in
// the critical section at once.
constexpr int kExitLostUpdates = 2;

struct MaxBenchOptions {
    int num_threads = 1;
    double run_time_sec = 0.0;     // measured window
    double warmup_sec = 0.0;       // uncounted run before the measured window
    bool csv = false;
    bool rusage = false;           // print getrusage() over the measured window instead
    bool thread_level = false;     // count acquisitions (throughput) instead of timing each
    bool no_output = false;
    bool low_contention = false;   // stagger thread starts by stagger_ms
    int stagger_ms = 0;

    // Added work. All zero = the roof: a thread reacquires as soon as it releases.
    long long critical_ns = 0;     // fixed length, inside the critical section
    long long noncritical_ns = 0;  // uniform random in [0, 2 * mean], after unlocking
    int legacy_critical_delay = 1;     // lock-level only: uniform [0, N) busy-loop iterations
    int legacy_noncritical_delay = 1;  // lock-level only: uniform [0, N) busy-loop iterations

    std::vector<int> cpus;         // thread i runs on cpus[i % size]; empty = not pinned
};

/**
 * Run the max-contention benchmark on an initialized lock (then destroy it).
 *
 * @return 0 on success, kExitLostUpdates if the shared counter disagrees with
 *         the number of completed critical sections, 1 if pinning failed.
 */
int max_contention_bench(const MaxBenchOptions& options, SoftwareMutex* lock);

#endif
