#include "bench_cli.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

namespace {
bool is_flag(const char* s) {
    return s[0] == '-';
}

bool parse_non_negative_double(const char* flag, const char* text, double& out) {
    char* end = nullptr;
    errno = 0;
    double value = strtod(text, &end);
    if (end == text || *end != '\0' || errno != 0 || !(value >= 0.0)) {
        fprintf(stderr, "%s expects a number >= 0, got '%s'\n", flag, text);
        return false;
    }
    out = value;
    return true;
}

bool parse_non_negative_ll(const char* flag, const char* text, long long& out) {
    char* end = nullptr;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (end == text || *end != '\0' || errno != 0 || value < 0) {
        fprintf(stderr, "%s expects an integer >= 0, got '%s'\n", flag, text);
        return false;
    }
    out = value;
    return true;
}

// "0,2,4" -> {0, 2, 4}
bool parse_cpu_list(const char* flag, const char* text, std::vector<int>& out) {
    out.clear();
    const char* p = text;
    while (*p != '\0') {
        char* end = nullptr;
        errno = 0;
        long value = strtol(p, &end, 10);
        if (end == p || errno != 0 || value < 0 || value > 1048575 || (*end != ',' && *end != '\0')) {
            fprintf(stderr, "%s expects comma-separated CPU numbers, got '%s'\n", flag, text);
            return false;
        }
        out.push_back(static_cast<int>(value));
        p = (*end == ',') ? end + 1 : end;
    }
    if (out.empty()) {
        fprintf(stderr, "%s expects at least one CPU number\n", flag);
        return false;
    }
    return true;
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
        } else if (opts.accept_warmup && strcmp(arg, "--warmup") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", arg);
                return false;
            }
            if (!parse_non_negative_double(arg, argv[++i], out.warmup_sec)) return false;
        } else if (opts.accept_work_ns && strcmp(arg, "--critical-ns") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", arg);
                return false;
            }
            if (!parse_non_negative_ll(arg, argv[++i], out.critical_ns)) return false;
        } else if (opts.accept_work_ns && strcmp(arg, "--noncritical-ns") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", arg);
                return false;
            }
            if (!parse_non_negative_ll(arg, argv[++i], out.noncritical_ns)) return false;
        } else if (opts.accept_cpus && strcmp(arg, "--cpus") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", arg);
                return false;
            }
            if (!parse_cpu_list(arg, argv[++i], out.cpus)) return false;
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
