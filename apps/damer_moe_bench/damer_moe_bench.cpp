// damer_moe_bench.cpp
//
// Mixture-of-experts KV-cache movement benchmark. Each event in the plan
// (exported from a Damer-compiled workload by scripts/damer_moe_plan.py) is a
// data movement over an expert's KV-cache slot. The slot is guarded by the
// mutex under test; the lock hold time is the movement itself.
//
//   --movement fused   single pass, as Damer fuses move+transform
//   --movement staged  baseline: separate copy, transform and writeback passes
//   --granularity expert|global   one lock per expert slot, or one lock total

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "bench_cli.hpp"
#include "bench_harness.hpp"
#include "bench_utils.hpp"
#include "cxl_utils.hpp"
#include "lock.hpp"

namespace {

constexpr size_t kTokenBytes = 8192;  // hidden=4096, f16
constexpr size_t kMinBytes = 4096;

struct MoeEvent {
    std::string name;
    size_t bytes = 0;
    int fragments = 1;
    int fanout = 1;
    int priority = 0;
    std::string actions;
    double output_ratio = 1.0;
};

struct Options {
    std::string plan;
    int experts = 8;
    bool global_lock = false;
    bool staged = false;
    bool count_tokens = false;
    double kv_scale = 1.0 / 64.0;
};

struct Slot {
    uint8_t* kv = nullptr;
    uint8_t* replicas[2] = {nullptr, nullptr};
    uint64_t version = 0;
};

bool load_plan(const std::string& path, double scale, std::vector<MoeEvent>& out) {
    std::ifstream in(path);
    if (!in) {
        fprintf(stderr, "Cannot open plan %s (run scripts/damer_moe_plan.py)\n", path.c_str());
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '\t')) f.push_back(cell);
        if (f.size() < 7) {
            fprintf(stderr, "Malformed plan row: %s\n", line.c_str());
            return false;
        }
        MoeEvent e;
        e.name = f[0];
        e.bytes = std::max<size_t>(kMinBytes, (size_t)((double)strtoull(f[1].c_str(), nullptr, 10) * scale));
        e.bytes &= ~(size_t)63;
        e.fragments = std::max(1, atoi(f[2].c_str()));
        e.fanout = std::min(2, std::max(1, atoi(f[3].c_str())));
        e.priority = std::max(1, atoi(f[4].c_str()));
        e.actions = f[5];
        e.output_ratio = std::min(1.0, std::max(0.01, atof(f[6].c_str())));
        out.push_back(e);
    }
    return !out.empty();
}

uint8_t* alloc_buf(size_t bytes) {
    void* p = nullptr;
    if (posix_memalign(&p, 4096, bytes) != 0) return nullptr;
    memset(p, 0x5a, bytes);  // touch under the active NUMA policy
    return static_cast<uint8_t*>(p);
}

// One word-sized transform standing in for quantize/checksum/reduce/filter.
inline uint64_t transform_word(uint64_t x) { return (x * 0x9e3779b97f4a7c15ULL) ^ (x >> 29); }

size_t output_bytes(const MoeEvent& e) {
    return std::max<size_t>(64, (size_t)((double)e.bytes * e.output_ratio)) & ~(size_t)63;
}

void fused_move(const MoeEvent& e, Slot& slot) {
    const size_t out_bytes = output_bytes(e);
    const size_t out_words = out_bytes / 8;
    const size_t in_words = e.bytes / 8;
    const size_t frag_words = std::max<size_t>(8, in_words / e.fragments);
    // Single pass: read each fragment once, transform, and write every replica
    // with only the reduced output.
    const uint64_t* s = reinterpret_cast<const uint64_t*>(slot.kv);
    for (size_t i = 0, o = 0; i < in_words; i += frag_words) {
        size_t n = std::min(frag_words, in_words - i);
        size_t keep = std::min(out_words - o, std::max<size_t>(1, n * out_words / in_words));
        size_t stride = std::max<size_t>(1, n / keep);
        for (int r = 0; r < e.fanout; ++r) {
            uint64_t* d = reinterpret_cast<uint64_t*>(slot.replicas[r]) + o;
            for (size_t k = 0; k < keep; ++k) d[k] = transform_word(s[i + std::min(n - 1, k * stride)]);
        }
        o += keep;
        if (o >= out_words) break;
    }
}

void staged_move(const MoeEvent& e, Slot& slot, uint8_t* scratch) {
    const size_t out_bytes = output_bytes(e);
    const size_t frag = std::max<size_t>(64, (e.bytes / e.fragments) & ~(size_t)63);
    // Stage 1: fragmentwise copy of the full payload into a staging buffer.
    for (size_t off = 0; off < e.bytes; off += frag) {
        memcpy(scratch + off, slot.kv + off, std::min(frag, e.bytes - off));
    }
    // Stage 2: transform pass over the staged payload.
    uint64_t* w = reinterpret_cast<uint64_t*>(scratch);
    for (size_t i = 0; i < e.bytes / 8; ++i) w[i] = transform_word(w[i]);
    // Stage 3: writeback of the (possibly reduced) output per replica.
    for (int r = 0; r < e.fanout; ++r) memcpy(slot.replicas[r], scratch, out_bytes);
}

}  // namespace

int main(int argc, char* argv[]) {
    Options opt;
    std::vector<char*> rest;
    rest.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires an argument\n", flag);
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--plan") opt.plan = need("--plan");
        else if (a == "--experts") opt.experts = std::max(1, atoi(need("--experts")));
        else if (a == "--kv-scale") opt.kv_scale = atof(need("--kv-scale"));
        else if (a == "--granularity") opt.global_lock = std::string(need("--granularity")) == "global";
        else if (a == "--movement") opt.staged = std::string(need("--movement")) == "staged";
        else if (a == "--count") opt.count_tokens = std::string(need("--count")) == "tokens";
        else rest.push_back(argv[i]);
    }
    if (opt.plan.empty()) {
        fprintf(stderr,
            "Usage: %s <mutex_name> <num_threads> <run_time_s> --plan moe_plan.tsv [--csv] "
            "[--experts N] [--granularity expert|global] [--movement fused|staged] "
            "[--kv-scale F] [--count events|tokens]\n", argv[0]);
        return 1;
    }

    BenchCliOptions cli_opts;
    cli_opts.num_positional = 3;
    BenchCliArgs args;
    if (!parse_bench_cli((int)rest.size(), rest.data(), cli_opts, args)) return 1;

    std::vector<MoeEvent> events;
    if (!load_plan(opt.plan, opt.kv_scale, events)) return 1;

    cxl_mutex_benchmark_init();
#ifdef hardware_cxl
    int numa = 1;
#else
    int numa = 0;
#endif
    setup_numa_mempolicy_or_fail(numa);

    const int nthreads = args.num_threads;
    const int nlocks = opt.global_lock ? 1 : opt.experts;
    std::vector<SoftwareMutex*> locks;
    for (int i = 0; i < nlocks; ++i) {
        SoftwareMutex* l = get_mutex(args.mutex_name.c_str(), nthreads);
        if (!l) {
            fprintf(stderr, "Failed to initialize lock.\n");
            return 1;
        }
        l->init(nthreads);
        locks.push_back(l);
    }

    size_t max_bytes = 0;
    for (auto& e : events) max_bytes = std::max(max_bytes, e.bytes);
    std::vector<Slot> slots(opt.experts);
    for (auto& s : slots) {
        s.kv = alloc_buf(max_bytes);
        for (auto& r : s.replicas) r = alloc_buf(max_bytes);
    }
    std::vector<uint8_t*> scratch(nthreads);
    for (auto& s : scratch) s = alloc_buf(max_bytes);

    // Cumulative priority weights for event selection.
    std::vector<uint64_t> cum;
    uint64_t total_w = 0;
    for (auto& e : events) cum.push_back(total_w += (uint64_t)e.priority);

    std::atomic<bool> start{false}, stop{false};
    std::vector<uint64_t> iters(nthreads, 0), tokens(nthreads, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < nthreads; ++t) {
        threads.emplace_back([&, t]() {
            uint64_t rng = 0x9e3779b97f4a7c15ULL * (t + 1);
            auto next = [&]() { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; };
            while (!start.load()) {}
            while (!stop.load(std::memory_order_relaxed)) {
                uint64_t pick = next() % total_w;
                size_t ei = 0;
                while (cum[ei] <= pick) ++ei;
                const MoeEvent& e = events[ei];
                int slot_idx = (int)(next() % opt.experts);
                SoftwareMutex* l = locks[opt.global_lock ? 0 : slot_idx];

                l->lock(t);
                Slot& s = slots[slot_idx];
                if (opt.staged) staged_move(e, s, scratch[t]);
                else fused_move(e, s);
                s.version++;
                Fence();
                l->unlock(t);

                iters[t]++;
                tokens[t] += std::max<size_t>(1, e.bytes / kTokenBytes);
            }
        });
    }

    start = true;
    std::this_thread::sleep_for(std::chrono::duration<double>(args.run_time_sec));
    stop = true;
    for (auto& th : threads) th.join();

    uint64_t total_iters = 0, total_versions = 0, total_tokens = 0;
    for (int t = 0; t < nthreads; ++t) { total_iters += iters[t]; total_tokens += tokens[t]; }
    for (auto& s : slots) total_versions += s.version;
    if (total_iters != total_versions) {
        fprintf(stderr, "%s was not correct: expected %llu slot updates, saw %llu\n",
            args.mutex_name.c_str(), (unsigned long long)total_iters, (unsigned long long)total_versions);
    }

    for (int t = 0; t < nthreads; ++t) {
        uint64_t count = opt.count_tokens ? tokens[t] : iters[t];
        if (args.csv) {
            printf("%d,%f,%llu\n", t, args.run_time_sec, (unsigned long long)count);
        } else {
            printf("Thread %d: %llu %s in %f seconds\n", t, (unsigned long long)count,
                opt.count_tokens ? "tokens" : "events", args.run_time_sec);
        }
    }
    if (!args.csv) {
        printf("Total: %llu events, %llu tokens (%s movement, %s locks)\n",
            (unsigned long long)total_iters, (unsigned long long)total_tokens,
            opt.staged ? "staged" : "fused", opt.global_lock ? "global" : "per-expert");
    }

    for (auto* l : locks) destroy_and_delete_lock(l, numa);
    cxl_mutex_benchmark_exit();
    return 0;
}
