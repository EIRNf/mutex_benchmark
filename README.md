# mutex_benchmark

Benchmarking different mutex libraries and implementations

## Installation

Install the Boost library [here](https://www.boost.org/doc/libs/1_53_0/doc/html/bbv2/installation.html).
Install nsync [here](https://github.com/google/nsync).

Install `pandas` and `matplotlib` for Python:

```python
pip install matplotlib pandas
```

## Experiment Running

The CLI is organised into four domain groups. Every group has a primary enum
selector; domain-specific flags only apply within their context.

| Group | Primary selector | Controls |
|---|---|---|
| **run** | `--bench {max,min,grouped,tigon,tigon_ycsb,tigon_tpcc}` | which binary/application executes |
| **capture** | `--capture {latency,throughput,rusage}` | what is measured |
| **graph** | `--plot {auto,faceted,combined,none}` | how results are shown |
| **selection** | `-s/-i/-x`, `--alloc` | which locks, which allocator |

Use `--sweep VAR START STOP STEP` to iterate a variable (`threads`,
`critical-delay`, `noncritical-delay`) over an inclusive range.

---

### Quick-start examples

**Throughput sweep over thread counts (the most common run):**
```bash
python3 -m scripts.main 8 5 3 \
  --bench max \
  --capture throughput \
  --sweep threads 1 8 1 \
  -s champions
```

**Same sweep, software-CXL allocator, faceted plot:**
```bash
python3 -m scripts.main 8 5 3 \
  --bench max \
  --capture throughput \
  --sweep threads 1 8 1 \
  --plot faceted \
  -s software_cxl \
  --alloc cxl-software
```

**Hardware-CXL allocator:**
```bash
python3 -m scripts.main 8 5 3 \
  --bench max \
  --capture throughput \
  --sweep threads 1 8 1 \
  -s hardware_cxl \
  --alloc cxl-hardware \
  --numa-node 1
```

`--numa-node N` is required for hardware-CXL runs. Choose a memory-bearing
NUMA node that is allowed to the process and is backed by the CXL device on
your host; the runner and benchmark validate that the node exists, has memory,
and is allowed, but do not infer which memory technology backs it. The selected
node is included in CSV and figure names.

For allocation placement diagnostics, set `MUTEX_BENCH_PLACEMENT=1` on the
benchmark command. Hardware-CXL allocations then fault one byte per page and
query `move_pages()` to report the observed node counts by allocation site.
This intentionally changes allocation work and is for placement checks, not
performance measurements.

`cxl_fast_exp` is the CXL-resident exclusive lock adapted from the supplied
fast-exp bridge. It is included in `hardware_cxl`; select it directly with
`-i cxl_fast_exp` when comparing CXL placement:
```bash
python3 -m scripts.main 8 5 3 \
  --bench max \
  --capture throughput \
  --sweep threads 1 8 1 \
  -i cxl_fast_exp \
  --alloc cxl-hardware \
  --numa-node 1
```

**Re-plot from existing CSVs (no re-run):**
```bash
python3 -m scripts.main 8 5 3 \
  --bench max \
  --capture throughput \
  --sweep threads 1 8 1 \
  --plot faceted \
  -s combined_cxl \
  --reuse-data
```

**Min benchmark sweep:**
```bash
python3 -m scripts.main 8 5 3 \
  --bench min \
  --capture throughput \
  --sweep threads 1 8 1 \
  -s software_cxl
```

**Tigon setup (Linux x86_64 only).** Tigon is a git submodule at `apps/tigon_db_bench/tigon` (pinned commit). Quick start on a fresh Linux machine:
```bash
git clone -b tigon-benchmark-submodule <repo-url> && cd mutex_benchmark
git submodule update --init apps/tigon_db_bench/tigon   # path required: cxl/emucxl is an unmapped legacy gitlink
sudo apt install build-essential cmake meson ninja-build clang python3 \
  libjemalloc-dev libgoogle-glog-dev libgflags-dev
meson setup build && meson compile -C build mutex_preload
./apps/tigon_db_bench/build.sh      # inits submodule, applies patches, builds bench_ycsb/bench_tpcc
```
Our changes to Tigon (spinlock-to-`pthread_mutex_t` redirection, `cxlalloc` mmap backend) are kept out of the submodule as
`apps/tigon_db_bench/patches/*.patch`; `build.sh` applies them idempotently. The patch also selects standalone CXL
transport and skips the network round-trip probe. The build is refused on non-Linux hosts. Tigon runs use `app` shim
scope automatically so allocator and logging-library mutexes remain untouched.

**Tigon database benchmark sweep (injected locks via LD_PRELOAD):**
```bash
python3 -m scripts.main 8 5 3 \
  --bench tigon_ycsb \
  --tigon-query rmw \
  --tigon-protocol TwoPL \
  --tigon-read-write-ratio 80 \
  --tigon-keys 200000 \
  --capture throughput \
  --sweep threads 1 8 1 \
  -s champions
```

The query is now explicit/configurable for each Tigon run. `--bench tigon` is
an alias for `--bench tigon_ycsb`. YCSB accepts `rmw`, `mixed`, `scan`,
`insert`, and `delete`; TPC-C accepts `mixed`, `first_two`, `neworder`,
`payment`, and `test`. If omitted, the existing defaults are preserved:
YCSB `rmw`, TPC-C `mixed`, and protocol `TwoPL`.
The default YCSB RMW mix has a read/write ratio of 80, meaning 80% reads and
20% updates among generated operations (with a 0% read-only-transaction ratio).

Tigon's workload settings can be changed with `--tigon-*` options. Common
options are `--tigon-partitions N` for a fixed partition count or
`--tigon-partition-ratio {threads,half,quarter}` to derive partitions from
each swept thread count (otherwise partitions track the thread count), and
`--tigon-warmup-seconds N` (otherwise `min(1, run-seconds - 1)`).
YCSB also supports `--tigon-read-write-ratio`, `--tigon-read-only-ratio`,
`--tigon-cross-ratio`, `--tigon-cross-partitions`, `--tigon-keys`, and
`--tigon-zipf`. TPC-C supports `--tigon-neworder-dist` and
`--tigon-payment-dist`. Values and defaults are shown by `--help`.

For example, to run the TPC-C new-order-only workload:
```bash
python3 -m scripts.main 8 5 3 \
  --bench tigon_tpcc \
  --tigon-query neworder \
  --capture throughput \
  --sweep threads 1 8 1 \
  -s champions
```

Every invocation logs its effective workload configuration (including lock,
query, protocol, thread/partition counts, run/warmup duration, and the
workload-specific parameters). The same settings are included in each raw CSV
filename and in the generated figure names, so runs with different Tigon
workloads remain identifiable when using a shared data folder.

The paper comparison lock list is available as `-s paper`:
`bakery`, `boulangerie`, `lamport`, the four linear/tree elevator variants,
`peterson`, `knuth`, `spin`, `exp_spin`, and `ticket`.

**Tigon/CXL environment check and basic lock comparison:**
```bash
scripts/tigon_cxl_experiment.sh
```
This verifies NUMA/DAX/CXL discovery, builds the project and Tigon, runs
`mmap`, `shm`, and `cxl` allocator smoke tests with bounded timeouts, and
compares a small lock set. Results are written to `data/tigon_cxl/` as
`environment.txt`, `smoke.csv`, `results.csv`, logs, and (when matplotlib is
available) `comparison.png`. Use `--cxl-node N` to add NUMA membind placement,
or `--smoke-only` / `--compare-only` to run a single phase.
Each invocation stores those artifacts below a timestamped run directory.

**DRAM baseline for Tigon, max, and min benchmarks:**
```bash
scripts/run_baseline.sh \
  --memory dram \
  --node 0 \
  --threads 8 \
  --seconds 30 \
  --repetitions 3
```
This binds all processes to the selected NUMA node, runs the `paper` lock set,
and writes separate CSVs and faceted plots under `data/dram_baseline/`.
For a quick Tigon-only run, add `--tigon-only --threads 4 --seconds 5`.
Each invocation creates a timestamped run directory below
`data/dram_baseline/`; its raw CSVs and figures stay associated with that run.
Use `--skip 4` to sample 4, 8, ..., 64 threads instead of every thread count.

**CXL NUMA baseline for all applications:**
```bash
scripts/run_baseline.sh \
  --memory cxl \
  --node 1 \
  --threads 8 \
  --seconds 30 \
  --repetitions 3
```
This runs Tigon YCSB, max-contention, min-contention, and grouped-contention
under the selected CXL NUMA node. Tigon uses `CXLALLOC_BACKEND=mmap` so the
NUMA policy places its allocations on CXL memory; the native `cxl` allocator
backend is not used because the bundled archive does not support it.
Each invocation writes its raw CSVs and figures below a timestamped directory
under `data/cxl_baseline/`.
Use `--skip 4` for a faster 4, 8, ..., 64-thread sweep.

The legacy `run_dram_baseline.sh` and `run_cxl_baseline.sh` commands remain
as compatibility wrappers around this unified runner.

**Master benchmark matrix (`start.sh`):**
```bash
THREAD_START=1 MAX_THREADS=64 THREAD_INTERVAL=4 \
RUN_TIME=10 RUNS=1 \
./start.sh
```
Without `CXL_LATENCY_PCI`, the script runs CXL at the host's current latency
setting and does not access or modify PCI configuration. To additionally test
both adjusted latency values, provide the verified PCI BDF:
```bash
export CXL_LATENCY_PCI=<verified-cxl-latency-control-bdf>
THREAD_START=1 MAX_THREADS=64 THREAD_INTERVAL=4 RUN_TIME=10 RUNS=1 ./start.sh
```
The script runs max/min on the DRAM node and the selected CXL mode(s); it also
runs Tigon YCSB and TPC-C for each mode. For each Tigon workload it compares
partitions equal to the swept thread count N, `max(1, floor(N/2))`, and
`max(1, floor(N/4))`. The defaults are the existing YCSB RMW and TPC-C mixed
workloads with TwoPL.
All runs use `numactl --membind` on the selected node; Tigon uses
`--alloc malloc` with `CXLALLOC_BACKEND=mmap`, not the separate
`--alloc cxl-hardware` allocator.

Run settings are environment-overridable variables at the top of `start.sh`,
including `THREAD_START`, `MAX_THREADS`, `THREAD_INTERVAL`, `RUN_TIME`,
`RUNS`, `DRAM_NODE`, `CXL_NODE`, `LOCK_SET`, and `TIGON_LOCKS`. Set
`RUN_NATIVE=0`, `RUN_TIGON=0`, `RUN_DRAM=0`, `RUN_CXL_UNMODIFIED=0`,
`RUN_CXL_LOW=0`, or `RUN_CXL_HIGH=0` to omit a suite or mode. With no
`CXL_LATENCY_PCI`, the unmodified CXL mode is enabled by default and the
adjusted modes are disabled. Supplying the BDF enables the two adjusted modes
by default; the `RUN_CXL_*` variables can override these defaults. Tigon
query, protocol, key count, ratios, and `TIGON_PARTITION_MODES` can also be
overridden.
Pass `--lock-set NAME` to select one of the named sets in
`scripts/constants.py` for both native and Tigon runs; for example:
```bash
./start.sh --lock-set champions
./start.sh --list-runs --lock-set hardware_cxl
```
This option overrides both `LOCK_SET` and `TIGON_LOCKS`. Without it, native
runs use `LOCK_SET` and Tigon uses its default `exp_spin,ticket,mcs,cxl_fast_exp`
list (which passed the two-instance preload probe), or the comma-separated
`TIGON_LOCKS` override.

Each benchmark invocation gets a separate timestamped directory under
`data/master_runs/<application>/<memory-mode>/...`, containing raw CSVs,
figures, logs, and `run_config.txt`. `DRY_RUN=1 ./start.sh` prints the planned
commands and creates the run manifests without launching benchmarks or
writing PCI configuration. To list the entire configured matrix and get a
count and timed-workload duration estimate without creating directories or
checking runtime NUMA/PCI prerequisites, use:
```bash
THREAD_START=1 MAX_THREADS=64 THREAD_INTERVAL=4 RUN_TIME=10 RUNS=1 \
./start.sh --list-runs
```
The listing shows each timestamped output group, its lock/thread/repetition
matrix, Tigon workload and effective partition counts, plus the total number
of benchmark process launches. The duration estimate includes `RUN_TIME` for
each launch and Tigon warmup, but excludes build, startup, plotting, and other
overhead. When adjusted CXL modes are enabled, the script
requires `CXL_LATENCY_PCI`, reads and saves the original register, verifies
each write, and restores the original value on exit. The register write
changes platform PCI configuration; confirm the BDF and offset for the host
before enabling those modes.

**Lock-level latency CDF (no sweep):**
```bash
python3 -m scripts.main 4 1 5 \
  --bench max \
  --capture latency \
  -s champions
```

**Speedup table + variability plot:**
```bash
python3 -m scripts.main 8 5 3 \
  --bench max \
  --capture throughput \
  --sweep threads 1 8 1 \
  -s champions \
  --speedup exp_spin \
  --variability
```

Throughput sweeps save both a line graph and a relative-throughput heatmap.
Each heatmap cell is the mean total iterations per run for that lock divided
by the reference lock's mean at the same sweep value, matching the line graph
and speedup table. Values above 1 are faster; grey cells indicate missing
data or a zero reference. The default reference is `exp_spin`; if it is not
selected, the first lock with data is used with a warning. Use
`--heatmap-ref ticket` to choose another selected lock, or `--speedup ticket`
to use the same reference for both the table and heatmap. If an explicitly
chosen reference has no data, the heatmap is skipped with a warning.
Use `--no-heatmap` to save only the line graph, or `--plot none` to disable
plots. Latency and rusage captures do not generate heatmaps.

**Sweep critical-section delay (single thread count):**
```bash
python3 -m scripts.main 8 1 5 \
  --bench max \
  --capture throughput \
  --sweep critical-delay 0 500 50
```

---

### Migration guide — old flag → new flag

| Old CLI flag | New CLI equivalent | Notes |
|---|---|---|
| `--iter-threads S E N` | `--sweep threads S E N --capture throughput` | sweep VAR is positional first arg |
| `--iter-critical-delay S E N` | `--sweep critical-delay S E N --capture throughput` | |
| `--iter-noncritical-delay S E N` | `--sweep noncritical-delay S E N --capture throughput` | |
| `--thread-level` | `--capture throughput` | |
| `--lock-level` | `--capture latency` (default) | |
| `-r` / `--rusage` | `--capture rusage` | |
| `--faceted` | `--plot faceted` | |
| `--combined` | `--plot combined` | |
| `--skip-plotting` | `--plot none` | |
| `--skip-experiment` | `--reuse-data` | |
| `--scxl` | `--alloc cxl-software` | |
| `--hcxl` | `--alloc cxl-hardware` | |
| `--low-contention --stagger-ms N` | `--stagger-ms N` | implies staggered start |
| `-a` / `--all` | (removed; default is champions set) | |
| `--info/--warning/--error/--critical` | `-l INFO/WARNING/ERROR/CRITICAL` | |

## Performance Measurement (single-lock, no Python pipeline)

The benchmark binary can be driven directly for quick throughput and
correctness measurements (this is what the 2026-07 numbers in
`ELEVATOR_SET_COMPARISON.md` §7.4/§7.6 were produced with):

```bash
# Build
meson setup build && meson compile -C build

# One run: <lock> <threads> <seconds>; --thread-level prints
# "thread_id,run_time,iterations" per thread AND arms the breach-detecting
# critical section (throws + aborts if mutual exclusion is ever violated).
./build/apps/max_contention_bench/max_contention_bench mcs 4 1.0 --thread-level --csv

# Throughput = sum of column 3 / run time:
./build/apps/max_contention_bench/max_contention_bench wf_bitonic_cas 8 1.0 --thread-level --csv \
  | awk -F, 'NF==3 {sum+=$3} END {print sum " ops/s (1s run)"}'

# Full sweep: locks x thread counts x reps, with a hang timeout
# (gtimeout = GNU coreutils; brew install coreutils on macOS).
for name in mcs ticket spin linear_lamport_elevator wf_bitonic_cas bo_bitonic_cas seq_periodic_cas periodic_cas; do
  for T in 1 2 4 8; do
    for rep in 1 2 3; do
      out=$(gtimeout -s KILL 10 ./build/apps/max_contention_bench/max_contention_bench \
            "$name" "$T" 1.0 --thread-level --csv 2>/dev/null)
      rc=$?
      if [ $rc -ne 0 ]; then echo "$T $name $rep FAIL(rc=$rc)"; else
        echo "$T $name $rep $(echo "$out" | awk -F, 'NF==3 {s+=$3} END{print s+0}')"
      fi
    done
  done
done
# Take the median of the 3 reps per (lock, T); always include mcs/ticket in
# the same session as controls — absolute numbers drift +/-40% day to day on
# laptops, but same-session ratios are stable.

# Correctness-only sweep (verdict per run: exit 137 = hang/deadlock,
# "was breached" in output = mutual exclusion violation):
gtimeout -s KILL 6 ./build/apps/max_contention_bench/max_contention_bench \
  seq_bitonic_cas 8 0.4 --thread-level --csv > /tmp/out 2>&1
case $? in 137) echo HANG;; 0) grep -q breached /tmp/out && echo BREACH || echo OK;; *) echo "CRASH/BREACH";; esac

# Fairness spread (bounded-overtaking locks): min vs max per-thread iterations
./build/apps/max_contention_bench/max_contention_bench bo_bitonic_cas 8 1.0 --thread-level --csv \
  | awk -F, 'NF==3 {if(min==""||$3<min)min=$3; if($3>max)max=$3} END{print "min="min" max="max" ratio="max/min}'
```

## Injecting locks into existing applications (LD_PRELOAD / DYLD)

`preload/libmutex_preload.{so,dylib}` transparently replaces the pthread
mutexes of an **unmodified, dynamically linked** application with any lock
from this suite (same names as `get_mutex()` / the `-s` selection sets). No
recompilation of the target: on Linux it is injected with `LD_PRELOAD`, on
macOS with `DYLD_INSERT_LIBRARIES` (dyld interposing).

```bash
meson setup build && meson compile -C build

# Convenience wrapper (picks the right env var per platform):
scripts/with_lock.sh mcs ./my_server --port 8080
scripts/with_lock.sh -S -n 64 -s app wf_bitonic_cas ./sqlite_bench

# Raw environment (Linux):
MUTEX_SHIM_LOCK=mcs MUTEX_SHIM_STATS=1 \
  LD_PRELOAD=$PWD/build/preload/libmutex_preload.so ./my_app
```

### How it works

The shim follows the LiTL approach (*"Multicore Locks: The Case Is Not
Closed Yet"*, USENIX ATC'16): on first sight of a `pthread_mutex_t` it
"claims" it by overwriting its first 16 bytes with a magic word plus a
pointer to a wrapper (glibc mutexes are ≥40 bytes, macOS 64), so the hot
path is one atomic load — no hash map. Claimed mutexes never touch the real
pthread implementation again. Threads receive dense ids in
`[0, MUTEX_SHIM_MAX_THREADS)` on first use (recycled at thread exit, so
thread-pool churn is fine — the pool only limits *concurrent* threads).
`pthread_cond_wait` is re-implemented with a per-mutex *shadow* real mutex so
a waiter never sleeps while holding the injected lock; `signal`/`broadcast`
serialize on the shadow, which closes the lost-wakeup window LiTL accepts.
Recursive/errorcheck mutexes are handled by owner/depth emulation in the
wrapper. Process-shared, robust, and priority-protocol mutexes are detected
at `pthread_mutex_init` and left on the real implementation.

### Environment variables

| Variable | Default | Meaning |
|---|---|---|
| `MUTEX_SHIM_LOCK` | *(unset = inert)* | lock to inject (`mcs`, `ticket`, `wf_bitonic_cas`, ...) |
| `MUTEX_SHIM_MAX_THREADS` | `128` | dense thread-id pool; also `init(N)` for every claimed lock. Aborts loudly if exceeded *concurrently* |
| `MUTEX_SHIM_SCOPE` | `all` | `all` = every claimable mutex; `app` = only mutexes first locked from the main executable's text (`MUTEX_SHIM_TARGET_LIBS=substr,substr` widens this); `region` = only mutexes living in tracked `mmap` regions (`MUTEX_SHIM_REGION_FLAGS=shared\|anon\|all`) or `mbind()`-ed NUMA/CXL ranges (Linux) |
| `MUTEX_SHIM_STATS` / `MUTEX_SHIM_STATS_FILE` | off / stderr | dump claim/acquisition/cond-wait counters at exit — check `claimed_mutexes`/`shim_acquisitions` to confirm interception actually happened |
| `MUTEX_SHIM_FENCE` | `1` | full fence after acquire / before release. The benchmark validates locks under `Fence()`-instrumented critical sections; some locks (e.g. `mcs`, whose handoff is a plain volatile store) rely on that and lose critical-section writes on arm64 without it. Disable only for locks with self-contained acquire/release semantics |
| `MUTEX_SHIM_COND_STRICT` | `1` | `0` reverts to LiTL-level condvar handling (tiny lost-wakeup window, less signal overhead) |
| `MUTEX_SHIM_LOG` / `MUTEX_SHIM_STRICT` | off | per-claim logging / abort instead of degrading on shim errors |

### Caveats & compatibility

- **Semantics**: `pthread_mutex_timedlock` degrades to a blocking lock;
  `trylock` returns `EBUSY` when the wrapper is owned but may block briefly
  when racing (the `SoftwareMutex` interface has no native trylock);
  cross-thread unlock returns `EPERM`. rwlocks, barriers and `pthread_spin_*`
  pass through untouched. Statically linked targets cannot be intercepted.
- **Unsupported locks**: anything with *type-level* `static`/`thread_local`
  state is single-instance-only and breaks under injection (many mutexes per
  process): `clh` (static tail — aborts), `hopscotch`/`hopscotch_nca`
  (thread_local node parity — hangs), `mcs_local`, `hopscotch_local`,
  `threadlocal_ticket`. The linear elevator family (`elevator`,
  `linear_*_elevator`) currently hangs under injection (liveness assumption
  from the fixed benchmark cohort); `tree_*_elevator` and `net_elevator` work.
- **Memory**: every claimed mutex gets its own lock instance sized for
  `MUTEX_SHIM_MAX_THREADS` — with `scope=all` an app with many mutexes can
  allocate a lot (bitonic locks are O(N log² N) per instance). Lower
  `MUTEX_SHIM_MAX_THREADS` or narrow the scope.
- **macOS**: SIP strips `DYLD_*` from *protected* binaries — injecting works
  for locally built / homebrew binaries, not Apple platform binaries or
  hardened-runtime apps. Note macOS apps often use `os_unfair_lock`/GCD
  internally, which is not interceptable; coverage is higher on Linux.
- **Measuring**: use the app's own throughput metric (or `/usr/bin/time`)
  and always record the shim stats; `MUTEX_SHIM_LOCK=system` gives a
  shim-overhead baseline, running without `MUTEX_SHIM_LOCK` gives a
  passthrough baseline.

Smoke tests: `meson test -C build preload_victim_baseline preload_shim_mcs
preload_shim_ticket preload_shim_exp_spin preload_shim_system
preload_shim_scope_app` (`tests/preload/` — a pure-POSIX victim app
exercising static/dynamic/recursive mutexes, trylock, condvar queue +
ping-pong + gate, thread churn beyond the id pool, and destroy paths).
