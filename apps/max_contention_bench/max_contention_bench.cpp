// max_contention_bench: every thread acquires one shared lock in a loop.
//
// Thread-level mode (--thread-level; the throughput and rusage captures) counts
// each thread's acquisitions. Lock-level mode (the latency capture) times each
// acquisition, from just before lock() to just after unlock().
//
// With no added work (no --critical-ns / --noncritical-ns) a thread reacquires
// the lock as soon as it releases it: the "roof", the most contention any
// workload can put on the lock. Added work is calibrated busy looping:
// --critical-ns is a fixed length inside the critical section, and
// --noncritical-ns is drawn per acquisition, uniformly from [0, 2 * N], so its
// mean is N. The roof's loop is compiled without any delay code.
//
// A run has three phases: the threads start (pinned to --cpus, if given) and
// wait for each other; they run uncounted for --warmup seconds; then they run
// for run_time_s seconds, the measured window, which is all that is reported.
//
// CSV output (--csv) starts with one "# key=value ..." metadata line.
//   thread-level: a header row, then one row per thread:
//     thread_id,seconds,iterations,reacquires,cpu_pinned,cpu_start,cpu_end,node_end
//     (seconds = the measured window; reacquires = acquisitions right after the
//     same thread's own release; cpu_* and node_end are -1 where unknown)
//   lock-level: one row per measured acquisition, no header:
//     thread_id,iteration,seconds
//   --rusage: one line, utime,stime,maxrss,minflt,majflt, over the measured
//     window (maxrss is the process peak), and no metadata line.
//
// Exit status: 0 = ok; 1 = bad arguments or pinning failed; 2 = lost updates
// (the shared counter disagrees with the completed critical sections).

#include <time.h>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>
#include <sys/time.h>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "max_contention_bench.hpp"
#include "bench_utils.hpp"
#include "bench_harness.hpp"
#include "bench_cli.hpp"
#include "cxl_utils.hpp"
#include "lock.hpp"

namespace {

enum Phase : int { kWaiting = 0, kWarmup = 1, kMeasure = 2, kStop = 3 };

// Per-thread generator for random work lengths (xorshift64*). rand() takes a
// lock inside glibc, which would put a second lock into every measurement.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545F4914F6CDD1Dull;
    }
};

// Busy work in busy_sleep() iterations: `fixed` every time, plus a uniform
// draw from [0, random_bound) when random_bound > 1.
struct Work {
    uint64_t fixed = 0;
    uint64_t random_bound = 0;

    bool any() const { return fixed > 0 || random_bound > 1; }

    void run(Rng& rng) const {
        uint64_t loops = fixed;
        if (random_bound > 1) loops += rng.next() % random_bound;
        if (loops) busy_sleep(loops);
    }
};

// What the critical section touches; one cache line, as before.
struct alignas(128) SharedState {
    volatile uint64_t counter = 0;   // +1 per critical section
    volatile int last = -1;          // the thread that held the lock last
};

// One thread's results, on its own cache lines; written once at the end of
// each phase, never inside the loop.
struct alignas(128) ThreadResult {
    uint64_t warmup_iterations = 0;  // completed before the measured window
    uint64_t iterations = 0;         // completed in total
    uint64_t warmup_reacquires = 0;
    uint64_t reacquires = 0;
    int cpu_pinned = -1;
    int cpu_start = -1;              // where the thread ran when measuring began
    int cpu_end = -1;                // ... and when it ended
    int node_end = -1;
    bool pin_failed = false;
    std::vector<double> lock_times;  // lock-level mode, measured window only
};

void where_am_i(int* cpu, int* node) {
#ifdef __linux__
    unsigned c = 0, n = 0;
    if (syscall(SYS_getcpu, &c, &n, nullptr) == 0) {
        *cpu = static_cast<int>(c);
        if (node) *node = static_cast<int>(n);
        return;
    }
#endif
    *cpu = -1;
    if (node) *node = -1;
}

bool pin_current_thread(int cpu) {
#ifdef __linux__
    if (cpu < 0 || cpu >= CPU_SETSIZE) return false;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

// busy_sleep() iterations per nanosecond on the calling thread: the fastest of
// several timed runs (the first runs also bring the core up to speed).
double calibrate_loops_per_ns() {
    constexpr size_t kLoops = size_t{1} << 22;
    double best_ns = 1e30;
    for (int trial = 0; trial < 9; ++trial) {
        auto begin = std::chrono::steady_clock::now();
        busy_sleep(kLoops);
        auto end = std::chrono::steady_clock::now();
        best_ns = std::min(best_ns, std::chrono::duration<double, std::nano>(end - begin).count());
    }
    return static_cast<double>(kLoops) / best_ns;
}

uint64_t ns_to_loops(long long ns, double loops_per_ns) {
    return static_cast<uint64_t>(std::llround(static_cast<double>(ns) * loops_per_ns));
}

double elapsed_seconds(const timespec& begin, const timespec& end) {
    double seconds = static_cast<double>(end.tv_sec - begin.tv_sec)
                   + static_cast<double>(end.tv_nsec - begin.tv_nsec) / 1e9;
    return seconds < 0.0 ? 0.0 : seconds;
}

// Records the counts at the end of warmup. A thread that sees the phase move
// past warmup while it is blocked in lock() counts that acquisition as warmup.
inline void end_warmup(ThreadResult& r, uint64_t iterations, uint64_t reacquires) {
    r.warmup_iterations = iterations;
    r.warmup_reacquires = reacquires;
    where_am_i(&r.cpu_start, nullptr);
}

// Thread-level loop. kAddedWork = false is the roof: no delay code at all.
template <bool kAddedWork>
void count_acquisitions(int i, SoftwareMutex* lock, SharedState& shared,
                        const std::atomic<int>& phase, const Work& critical,
                        const Work& noncritical, ThreadResult& r) {
    Rng rng(static_cast<uint64_t>(i) + 1);
    uint64_t iterations = 0, reacquires = 0;
    int seen = kWarmup;
    while (true) {
        int now = phase.load(std::memory_order_acquire);
        if (now != seen) {
            if (seen == kWarmup) end_warmup(r, iterations, reacquires);
            if (now == kStop) break;
            seen = now;
        }
        lock->lock(i);
        // Critical section
        if (shared.last == i) ++reacquires;
        shared.last = i;
        ++iterations;
        shared.counter = shared.counter + lock->criticalSection(i);
        if constexpr (kAddedWork) critical.run(rng);
        Fence();
        lock->unlock(i);
        if constexpr (kAddedWork) noncritical.run(rng);
    }
    r.iterations = iterations;
    r.reacquires = reacquires;
}

// Lock-level loop: times each acquisition (lock, critical section, unlock).
void time_acquisitions(int i, SoftwareMutex* lock, SharedState& shared,
                       const std::atomic<int>& phase, const Work& critical,
                       const Work& noncritical, ThreadResult& r) {
    Rng rng(static_cast<uint64_t>(i) + 1);
    uint64_t iterations = 0, reacquires = 0;
    int seen = kWarmup;
    timespec begin{}, end{};
    while (true) {
        int now = phase.load(std::memory_order_acquire);
        if (now != seen) {
            if (seen == kWarmup) end_warmup(r, iterations, reacquires);
            if (now == kStop) break;
            seen = now;
        }
        clock_gettime(CLOCK_MONOTONIC, &begin);
        lock->lock(i);
        // Critical section
        if (shared.last == i) ++reacquires;
        shared.counter = shared.counter + 1;
        shared.last = i;
        Fence();
        critical.run(rng);
        lock->unlock(i);
        clock_gettime(CLOCK_MONOTONIC, &end);
        if (seen == kMeasure) r.lock_times.push_back(elapsed_seconds(begin, end));
        // Noncritical section
        noncritical.run(rng);
        ++iterations;
    }
    r.iterations = iterations;
    r.reacquires = reacquires;
}

void subtract_timeval(timeval& a, const timeval& b) {
    a.tv_sec -= b.tv_sec;
    a.tv_usec -= b.tv_usec;
    if (a.tv_usec < 0) {
        a.tv_sec -= 1;
        a.tv_usec += 1000000;
    }
}

// Resource use between two getrusage() snapshots (maxrss stays the peak).
rusage rusage_between(const rusage& before, const rusage& after) {
    rusage delta = after;
    subtract_timeval(delta.ru_utime, before.ru_utime);
    subtract_timeval(delta.ru_stime, before.ru_stime);
    delta.ru_minflt -= before.ru_minflt;
    delta.ru_majflt -= before.ru_majflt;
    delta.ru_inblock -= before.ru_inblock;
    delta.ru_oublock -= before.ru_oublock;
    delta.ru_nvcsw -= before.ru_nvcsw;
    delta.ru_nivcsw -= before.ru_nivcsw;
    delta.ru_nsignals -= before.ru_nsignals;
    return delta;
}

}  // namespace

int max_contention_bench(const MaxBenchOptions& o, SoftwareMutex* lock) {
    // Placement policy (shared by all benchmark apps): only lock memory is
    // NUMA-placed in a -Dhardware_cxl build (get_mutex()/ALLOCATE). Benchmark
    // bookkeeping uses ordinary memory; to bind the whole process to a node,
    // run under `numactl --membind` (placement = "process" in the configs).
    lock->init(o.num_threads);
    const std::string lock_name = lock->name();

    auto shared = std::make_unique<SharedState>();
    std::vector<ThreadResult> results(o.num_threads);
    std::atomic<int> phase{kWaiting};
    std::atomic<int> ready{0};

    double loops_per_ns = 0.0;
    if (o.critical_ns > 0 || o.noncritical_ns > 0) loops_per_ns = calibrate_loops_per_ns();
    Work critical, noncritical;
    if (o.critical_ns > 0) {
        critical.fixed = ns_to_loops(o.critical_ns, loops_per_ns);
    } else if (!o.thread_level && o.legacy_critical_delay > 1) {
        critical.random_bound = static_cast<uint64_t>(o.legacy_critical_delay);
    }
    if (o.noncritical_ns > 0) {
        noncritical.random_bound = 2 * ns_to_loops(o.noncritical_ns, loops_per_ns) + 1;
    } else if (!o.thread_level && o.legacy_noncritical_delay > 1) {
        noncritical.random_bound = static_cast<uint64_t>(o.legacy_noncritical_delay);
    }
    const bool added_work = critical.any() || noncritical.any();

    std::vector<std::thread> threads;
    threads.reserve(o.num_threads);
    for (int i = 0; i < o.num_threads; ++i) {
        threads.emplace_back([&, i]() {
            ThreadResult& r = results[i];
            if (!o.cpus.empty()) {
                r.cpu_pinned = o.cpus[static_cast<size_t>(i) % o.cpus.size()];
                r.pin_failed = !pin_current_thread(r.cpu_pinned);
            }
            ready.fetch_add(1, std::memory_order_acq_rel);
            if (o.thread_level) {
                if (o.low_contention && o.stagger_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(i * o.stagger_ms));
                }
                while (phase.load(std::memory_order_acquire) == kWaiting) {}
                if (added_work) {
                    count_acquisitions<true>(i, lock, *shared, phase, critical, noncritical, r);
                } else {
                    count_acquisitions<false>(i, lock, *shared, phase, critical, noncritical, r);
                }
            } else {
                while (phase.load(std::memory_order_acquire) == kWaiting) {}
                if (o.low_contention && o.stagger_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(i * o.stagger_ms));
                }
                time_acquisitions(i, lock, *shared, phase, critical, noncritical, r);
            }
            where_am_i(&r.cpu_end, &r.node_end);
        });
    }

    while (ready.load(std::memory_order_acquire) < o.num_threads) std::this_thread::yield();
    bool pin_failed = std::any_of(results.begin(), results.end(),
                                  [](const ThreadResult& r) { return r.pin_failed; });

    rusage before{}, after{};
    auto measure_begin = std::chrono::steady_clock::now();
    auto measure_end = measure_begin;
    if (!pin_failed) {
        if (o.warmup_sec > 0) {
            phase.store(kWarmup, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::duration<double>(o.warmup_sec));
        }
        if (o.rusage) getrusage(RUSAGE_SELF, &before);
        measure_begin = std::chrono::steady_clock::now();
        phase.store(kMeasure, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::duration<double>(o.run_time_sec));
    }
    phase.store(kStop, std::memory_order_release);
    measure_end = std::chrono::steady_clock::now();
    if (o.rusage && !pin_failed) getrusage(RUSAGE_SELF, &after);

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    if (pin_failed) {
        for (int i = 0; i < o.num_threads; ++i) {
            if (results[i].pin_failed) {
                fprintf(stderr, "could not pin thread %d to CPU %d\n", i, results[i].cpu_pinned);
            }
        }
        destroy_and_delete_lock(lock);
        return 1;
    }

    const double measured_sec = std::chrono::duration<double>(measure_end - measure_begin).count();
    uint64_t completed = 0, measured_iterations = 0, measured_reacquires = 0;
    for (const auto& r : results) {
        completed += r.iterations;
        measured_iterations += r.iterations - r.warmup_iterations;
        measured_reacquires += r.reacquires - r.warmup_reacquires;
    }
    const uint64_t counter = shared->counter;

    destroy_and_delete_lock(lock);

    if (o.rusage && !o.no_output) {
        rusage window = rusage_between(before, after);
        print_rusage(&window, o.csv);
    }

    if (!o.no_output && !o.rusage) {
        if (o.csv) {
            printf("# max_contention_bench mode=%s lock=%s threads=%d warmup_s=%g measured_s=%.6f "
                   "critical_ns=%lld noncritical_ns=%lld legacy_critical_delay=%d "
                   "legacy_noncritical_delay=%d loops_per_us=%.3f pinned=%d "
                   "acquisitions=%" PRIu64 " reacquires=%" PRIu64 "\n",
                   o.thread_level ? "thread-level" : "lock-level", lock_name.c_str(),
                   o.num_threads, o.warmup_sec, measured_sec,
                   o.critical_ns, o.noncritical_ns,
                   o.thread_level ? 0 : o.legacy_critical_delay,
                   o.thread_level ? 0 : o.legacy_noncritical_delay,
                   loops_per_ns * 1000.0, o.cpus.empty() ? 0 : 1,
                   measured_iterations, measured_reacquires);
        } else {
            printf("%s, %d threads: warmup %g s, measured %.6f s, added work: %s\n",
                   lock_name.c_str(), o.num_threads, o.warmup_sec, measured_sec,
                   added_work ? "yes" : "none (roof)");
        }
        if (o.thread_level) {
            if (o.csv) printf("thread_id,seconds,iterations,reacquires,cpu_pinned,cpu_start,cpu_end,node_end\n");
            for (int i = 0; i < o.num_threads; ++i) {
                const ThreadResult& r = results[i];
                uint64_t iterations = r.iterations - r.warmup_iterations;
                uint64_t reacquires = r.reacquires - r.warmup_reacquires;
                if (o.csv) {
                    printf("%d,%f,%" PRIu64 ",%" PRIu64 ",%d,%d,%d,%d\n", i, measured_sec,
                           iterations, reacquires, r.cpu_pinned, r.cpu_start, r.cpu_end, r.node_end);
                } else {
                    printf("Thread %d: %" PRIu64 " iterations completed in %f seconds "
                           "(%" PRIu64 " reacquires; cpu pinned %d, start %d, end %d, node %d)\n",
                           i, iterations, measured_sec, reacquires,
                           r.cpu_pinned, r.cpu_start, r.cpu_end, r.node_end);
                }
            }
        } else {
            for (int i = 0; i < o.num_threads; ++i) {
                const ThreadResult& r = results[i];
                if (!o.csv) {
                    printf("Thread %d: %zu iterations completed in %f seconds\n",
                           i, r.lock_times.size(), measured_sec);
                }
                for (size_t k = 0; k < r.lock_times.size(); ++k) {
                    if (o.csv) {
                        printf("%d,%zu,%.10f\n", i, k, r.lock_times[k]);
                    } else {
                        printf("    #%d: iteration %zu took %.9f seconds\n", i, k, r.lock_times[k]);
                    }
                }
            }
        }
        if (!o.csv) {
            double percentage = measured_iterations
                ? 100.0 * static_cast<double>(measured_reacquires) / static_cast<double>(measured_iterations)
                : 0.0;
            printf("Unfairness: %" PRIu64 "/%" PRIu64 " of all lock passes were from a mutex back to itself. (%f%%)\n",
                   measured_reacquires, measured_iterations, percentage);
        }
    }

    if (counter != completed) {
        fflush(stdout);
        fprintf(stderr,
            "LOST UPDATES: %s: the shared counter is %" PRIu64 " but %" PRIu64
            " critical sections completed; the lock let threads in together\n",
            lock_name.c_str(), counter, completed);
        return kExitLostUpdates;
    }
    return 0;
}

int main(int argc, char* argv[]) {
    BenchCliOptions opts;
    opts.num_positional = 3;
    opts.accept_rusage = true;
    opts.accept_no_output = true;
    opts.accept_low_contention = true;
    opts.accept_critical_delay = true;
    opts.accept_noncritical_delay = true;
    opts.accept_warmup = true;
    opts.accept_work_ns = true;
    opts.accept_cpus = true;

    BenchCliArgs args;
    if (!parse_bench_cli(argc, argv, opts, args)) {
        fprintf(stderr,
            "Usage: %s <mutex_name> <num_threads> <run_time_s> "
            "[--csv] [--thread-level] [--rusage] [--no-output] [--low-contention] "
            "[--stagger-ms ms] [--warmup s] [--critical-ns N] [--noncritical-ns N] "
            "[--cpus A,B,...] [--critical-delay N] [--noncritical-delay N]\n",
            argv[0]
        );
        return 1;
    }
    if (args.num_threads < 1 || !(args.run_time_sec > 0.0)) {
        fprintf(stderr, "num_threads must be >= 1 and run_time_s > 0\n");
        return 1;
    }
    if (args.thread_level && (args.critical_delay > 0 || args.noncritical_delay > 0)) {
        fprintf(stderr,
            "--critical-delay/--noncritical-delay only apply to lock-level mode; "
            "use --critical-ns/--noncritical-ns with --thread-level\n");
        return 1;
    }
    if ((args.critical_delay > 0 && args.critical_ns > 0)
        || (args.noncritical_delay > 0 && args.noncritical_ns > 0)) {
        fprintf(stderr, "use either the -delay (iterations) or the -ns form of a delay, not both\n");
        return 1;
    }
#ifndef __linux__
    if (!args.cpus.empty()) {
        fprintf(stderr, "--cpus (thread pinning) needs Linux\n");
        return 1;
    }
#endif

    MaxBenchOptions options;
    options.num_threads = args.num_threads;
    options.run_time_sec = args.run_time_sec;
    options.warmup_sec = args.warmup_sec;
    options.csv = args.csv;
    options.rusage = args.rusage;
    options.thread_level = args.thread_level;
    options.no_output = args.no_output;
    options.low_contention = args.low_contention;
    options.stagger_ms = args.stagger_ms;
    options.critical_ns = args.critical_ns;
    options.noncritical_ns = args.noncritical_ns;
    // 1 means none: the bound is exclusive (uniform [0, N)).
    options.legacy_critical_delay = args.critical_delay > 0 ? args.critical_delay : 1;
    options.legacy_noncritical_delay = args.noncritical_delay > 0 ? args.noncritical_delay : 1;
    options.cpus = args.cpus;

    SoftwareMutex *lock = get_mutex(args.mutex_name.c_str(), args.num_threads);
    if (lock == nullptr) {
        fprintf(stderr, "Failed to initialize lock.\n");
        return 1;
    }
    return max_contention_bench(options, lock);
}
