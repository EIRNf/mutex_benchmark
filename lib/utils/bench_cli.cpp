#include "bench_cli.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
bool is_flag(const char* s) {
    return s[0] == '-';
}
}  // namespace

bool parse_bench_cli(int argc, char* argv[], const BenchCliOptions& opts, BenchCliArgs& out) {
    const char* positionals[4] = {nullptr, nullptr, nullptr, nullptr};
    int num_collected = 0;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];

        if (strcmp(arg, "--csv") == 0 || strcmp(arg, "-c") == 0) {
            out.csv = true;
        } else if (strcmp(arg, "--thread-level") == 0) {
            out.thread_level = true;
        } else if (opts.accept_rusage && strcmp(arg, "--rusage") == 0) {
            out.rusage = true;
        } else if (opts.accept_no_output && strcmp(arg, "--no-output") == 0) {
            out.no_output = true;
        } else if (opts.accept_low_contention && strcmp(arg, "--low-contention") == 0) {
            out.low_contention = true;
        } else if (opts.accept_low_contention && strcmp(arg, "--stagger-ms") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", arg);
                return false;
            }
            out.stagger_ms = atoi(argv[++i]);
        } else if (opts.accept_critical_delay && strcmp(arg, "--critical-delay") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", arg);
                return false;
            }
            out.critical_delay = atoi(argv[++i]);
        } else if (opts.accept_noncritical_delay && strcmp(arg, "--noncritical-delay") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", arg);
                return false;
            }
            out.noncritical_delay = atoi(argv[++i]);
        } else if (is_flag(arg)) {
            fprintf(stderr, "Unrecognized flag: %s\n", arg);
            return false;
        } else {
            if (num_collected >= opts.num_positional) {
                fprintf(stderr, "Unexpected extra argument: %s\n", arg);
                return false;
            }
            positionals[num_collected++] = arg;
        }
    }

    if (num_collected != opts.num_positional) {
        fprintf(stderr,
            "Expected %d positional argument(s) (mutex_name, num_threads, run_time_s%s), got %d\n",
            opts.num_positional,
            opts.num_positional >= 4 ? ", num_groups" : "",
            num_collected
        );
        return false;
    }

    out.mutex_name   = positionals[0];
    out.num_threads  = atoi(positionals[1]);
    out.run_time_sec = atof(positionals[2]);
    if (opts.num_positional >= 4) {
        out.num_groups = atoi(positionals[3]);
    }

    return true;
}
