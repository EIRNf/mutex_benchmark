# mutex_benchmark

Benchmarking different mutex libraries and implementations

## Installation

Install the Boost library [here](https://www.boost.org/doc/libs/1_53_0/doc/html/bbv2/installation.html).
Install nsync [here](https://github.com/google/nsync).

The Python driver needs Python 3.11 or newer (experiment configs are TOML,
read with the standard-library `tomllib`). It builds the benchmarks with
Meson and Ninja; `requirements.txt` installs both, and the driver finds them
next to the Python running it, so a virtualenv works without activating it:

```bash
python3 -m pip install -r requirements.txt
```

## Experiment Running

### Config files (`python -m scripts run`)

An experiment is one TOML file: the benchmark, the locks, an optional sweep,
the memory placement, and how to plot. Examples live in `experiments/`.

```bash
python3 -m scripts run experiments/max_threads.toml
python3 -m scripts run experiments/max_threads.toml -o seconds=10 -o sweep.stop=64
python3 -m scripts run -o capture=throughput -o locks.include=spin,mcs \
  -o sweep.var=threads -o sweep.start=1 -o sweep.stop=8 -o sweep.step=1   # no file
python3 -m scripts run experiments/max_threads.toml --print-config        # check, don't run
```

Each run gets its own folder, `data/runs/<timestamp>-<name>/` (or `--dir DIR`):

| Path | Contents |
|---|---|
| `config.toml` | the fully resolved config; `python -m scripts run <it>` repeats the experiment |
| `run.json` | status (`running`, `complete`, `failed`, `interrupted`), timing, command, git commit and dirty flag, host and `numactl --hardware`, resolved lock list, build flags, and how many data points were written (missing ones are listed); Tigon runs add the submodule commit and `CXLALLOC_BACKEND` (`null` = unset, which Tigon treats as `mmap`) |
| `raw/` | CSVs |
| `plots/` | figures and speedup tables |
| `logs/` | debug logs |

Re-plot a recorded run without re-running or repeating any flags; only
`plot.*` settings can change:

```bash
python3 -m scripts plot data/runs/<run-folder>
python3 -m scripts plot data/runs/<run-folder> -o plot.layout=combined -o plot.speedup=ticket
```

**Builds.** Placement `locks` needs a `-Dhardware_cxl` build, so it builds
into `build-hardware_cxl/`; everything else uses `build/`. Switching placement
therefore never forces a full rebuild. `--no-build` (or
`MUTEX_BENCH_SKIP_BUILD=1`) reuses the existing build, and `--build-dir DIR`
picks another folder. Before running, the build is checked: one configured
for the other placement, or built without a conditionally compiled lock you
selected (`nsync`, `boost`, `umwait`, `futex`), is rejected.

`-o KEY=VALUE` sets any setting by its dotted key and wins over the file.
Values are read as TOML (`-o plot.show=true`, `-o seconds=2.5`); bare words are
strings, and list settings accept commas (`-o locks.include=spin,mcs`). Unknown
keys are rejected with the closest match. `--print-config` prints the fully
resolved config, which is also a good starting point for a new file.

| Key | Meaning (default) |
|---|---|
| `name` | experiment name (file name) |
| `bench` | `max`, `min`, `grouped`, `tigon`, `tigon_ycsb`, `tigon_tpcc` (`max`) |
| `capture` | `latency`, `throughput`, `rusage` (`latency`) |
| `threads`, `seconds`, `repetitions` | fixed thread count, seconds per run, runs per point (10, 1, 10) |
| `[locks]` `sets`, `include`, `exclude` | lock selection, as `-s/-i/-x` (champions set) |
| `[sweep]` `var`, `start`, `stop`, `step` | inclusive sweep of `threads`, `critical_delay` or `noncritical_delay` |
| `[memory]` `placement`, `node` | see below (`none`) |
| `[workload]` `groups`, `critical_delay`, `noncritical_delay`, `stagger_ms` | per-benchmark knobs |
| `[tigon]` `query`, `protocol`, `partitions`, `partition_ratio`, `warmup_seconds`, `read_write_ratio`, `read_only_ratio`, `cross_ratio`, `cross_partitions`, `keys`, `zipf`, `neworder_dist`, `payment_dist` | Tigon workload (Tigon benches only) |
| `[plot]` `layout`, `show`, `scatter`, `max_points`, `variability`, `speedup`, `heatmap`, `heatmap_ref`, `stdev`, `averages` | plotting |
| `[output]` `log_level` | console log level (`INFO`) |

**Memory placement** (`[memory]`, or `--placement`/`--numa-node` on the flag CLI):

| `placement` | What lands on `node` | How |
|---|---|---|
| `none` | nothing pinned | plain build, OS default policy |
| `process` | everything the benchmark allocates: lock state, thread stacks, benchmark and Tigon data | each benchmark runs under `numactl --membind=node` |
| `locks` | lock memory | `-Dhardware_cxl` build; lock objects and `ALLOCATE`d regions are `mbind()`-placed via `MUTEX_BENCH_NUMA_NODE` |

With `locks`, every benchmark app places only lock memory; benchmark
bookkeeping (counters, start/stop flags, thread stacks) stays in ordinary
memory. Locks that allocate internally without `ALLOCATE` are not placed. CSV
and figure names carry `cxl-hw-nodeN` (`locks`) or `membind-nodeN` (`process`).

### Suites (`python -m scripts suite`)

A suite runs several distinct experiments, each into its own run folder, for
example every experiment in several memory environments. Examples live in
`experiments/suites/`.

```bash
python3 -m scripts suite experiments/suites/placement.toml --list   # plan and time estimate
python3 -m scripts suite experiments/suites/placement.toml
python3 -m scripts suite experiments/suites/placement.toml --only max -o seconds=1
python3 -m scripts suite --resume data/suites/<suite-folder>
```

```toml
name   = "placement"                # default: the file name
cgroup = "/sys/fs/cgroup/a/cgroup.procs"   # optional: join this cgroup before building

[defaults]                          # same keys as an experiment file
capture = "throughput"
locks   = { sets = ["paper"] }
sweep   = { var = "threads", start = 1, stop = 41, step = 4 }

[[environments]]                    # optional; every experiment runs in each one
name   = "cxl"
memory = { placement = "process", node = 1 }

[[environments]]
name    = "cxl-slow"
memory  = { placement = "process", node = 1 }
latency = "ffff"                    # CXL latency register value

[latency_control]                   # needed when an environment sets latency
pci      = "0000:a0:00.0"           # this host's CXL device; verify before use
register = "0xe08.l"                # default

[[experiments]]
bench = "max"                       # named after the bench unless `name` is set

[[experiments]]
file = "../tigon_ycsb.toml"         # start from an experiment file
vary = { "tigon.partition_ratio" = ["threads", "half", "quarter"] }  # one experiment per value
environments = ["cxl-slow"]         # optional: run only in these environments
```

A run's settings are layered: `[defaults]`, then the experiment's `file`, its
own keys, its `vary` value, the environment's `memory`, and last the `-o`
overrides. Overrides apply to every run in the suite, even with `--only`, and
every run is validated before anything starts, so a setting that only some
runs accept (such as `tigon.zipf`, YCSB only) belongs in the suite file.

The suite folder, `data/suites/<timestamp>-<name>/` (or `--dir`), holds
`suite.toml` (the file as given), `suite.json` (the plan, each run's status,
and each session), and one run folder per run at `<environment>/<experiment>/`.
The runner:

- joins the suite's `cgroup`, if set (writing its PID to `cgroup.procs` with
  `sudo`), so builds and benchmarks run inside it; a missing cgroup stops the
  suite before anything runs;
- builds each needed build folder once, before the first run (`--no-build`
  reuses them; each run still checks its build);
- runs each experiment as its own `python -m scripts run` process, so a failed
  run is recorded and the suite carries on (`--stop-on-failure` stops instead);
- sets the CXL latency with `setpci` (under `sudo`) before each environment
  that names one, returns to the original value for environments that don't,
  and restores it when the suite ends, including on Ctrl-C or SIGTERM. Each
  run's `run.json` records its environment and latency under `suite`.
  Unattended suites need `sudo` not to prompt mid-run (for example a
  `NOPASSWD` rule for `setpci`).

A suite with two or more environments ends by printing the `compare` command
for its folder (see below).

`--resume` continues a suite from its folder (`suite.json` and the prepared
run configs), never the original file: complete runs are skipped, and an
unfinished run's folder is moved to `<run>.attempt-N` before it reruns.
`--only GLOB` (repeatable) selects runs by key, experiment or environment
name, such as `cxl/*`, `max` or `dram`.

### Comparing runs (`python -m scripts compare`)

`compare` plots a suite's runs against each other. With two or more
environments it compares each experiment across them (DRAM vs CXL for the
same lock); `--by experiment` instead compares the experiments within each
environment, for example Tigon's partition ratios.

```bash
python3 -m scripts compare data/suites/<suite-folder>
python3 -m scripts compare data/suites/<suite-folder> --baseline cxl --only max
python3 -m scripts compare data/suites/<suite-folder> --by experiment --only 'cxl/tigon_ycsb-*'
```

It writes to `<suite-folder>/compare/` (or `--out DIR`), replacing earlier
output:

| File | Contents |
|---|---|
| `<group>-throughput.png` | one panel per lock, one line per environment (or experiment): mean and stdev over repetitions |
| `<group>-vs-<baseline>.png` | per environment: mean throughput divided by the baseline's, per lock and sweep point (below 1 = slower) |
| `<group>.csv` | the numbers behind both: `series`, `lock`, sweep value, `mean`, `std`, `n`, `ratio` |
| `summary-vs-<baseline>.png`, `summary.csv` | geometric mean of those ratios over the sweep, per lock and experiment |

The baseline is the first environment (or experiment) unless `--baseline`
names another. Throughput is each run's total over all threads divided by its
seconds, so runs of different lengths compare fairly. Only throughput sweeps
are compared; runs that have not started are skipped, and runs that stopped
early are included with the points they wrote. Both are reported as notes.

### Flag CLI (`python -m scripts.main`)

The flag CLI builds the same config and runs the same validation. It keeps
its flat layout: CSVs in `--data-folder` (by default a new timestamped folder
under `data/generated/`) with `config.toml` and `run.json` beside them, and
figures in the folder's `../figs`. `--reuse-data` with the default folder
re-plots the newest run that has data. It is organised into four domain groups. Every group has a primary enum
selector; domain-specific flags only apply within their context.

| Group | Primary selector | Controls |
|---|---|---|
| **run** | `--bench {max,min,grouped,tigon,tigon_ycsb,tigon_tpcc}` | which binary/application executes |
| **capture** | `--capture {latency,throughput,rusage}` | what is measured |
| **graph** | `--plot {auto,faceted,combined,none}` | how results are shown |
| **selection** | `-s/-i/-x`, `--placement` | which locks, where memory goes |

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

**Same sweep, RMW-free lock set (`software_cxl`), faceted plot:**
```bash
python3 -m scripts.main 8 5 3 \
  --bench max \
  --capture throughput \
  --sweep threads 1 8 1 \
  --plot faceted \
  -s software_cxl
```

**Hardware-CXL allocator:**
```bash
python3 -m scripts.main 8 5 3 \
  --bench max \
  --capture throughput \
  --sweep threads 1 8 1 \
  -s hardware_cxl \
  --placement locks \
  --numa-node 1
```

`--numa-node N` is required with `--placement locks` or `process`. Choose a memory-bearing
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
  --placement locks \
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
This re-plots the newest run under `data/generated/`. For runs made with
`python -m scripts run`, use `python -m scripts plot <run-folder>` instead.

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
git submodule update --init apps/tigon_db_bench/tigon
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

**DRAM and CXL baselines (`experiments/suites/baseline.toml`):**
```bash
python3 -m scripts suite experiments/suites/baseline.toml --list
python3 -m scripts suite experiments/suites/baseline.toml --only dram -o sweep.stop=8 -o seconds=30
python3 -m scripts suite experiments/suites/baseline.toml --only cxl -o sweep.start=4 -o sweep.step=4
```
Tigon YCSB, max and min on the DRAM node and on the CXL node, plus grouped
(2 groups) on CXL, with the `paper` lock set, 3 repetitions and a 1-64 thread
sweep. Every benchmark runs under `numactl --membind` (`placement =
"process"`), so Tigon's allocations land on the node through its default
`mmap` allocator backend (the bundled archive does not support the native
`cxl` backend). This suite replaces `scripts/run_baseline.sh` and its
`run_dram_baseline.sh` / `run_cxl_baseline.sh` wrappers; the comment at the
top of the file maps each old option.

**DRAM vs CXL matrix (`experiments/suites/matrix.toml`):**
```bash
python3 -m scripts suite experiments/suites/matrix.toml --list
python3 -m scripts suite experiments/suites/matrix.toml -o sweep.stop=64
python3 -m scripts suite experiments/suites/matrix.toml -o locks.sets=champions   # native and Tigon
```
Tigon YCSB (RMW) and TPC-C (mixed) with TwoPL, each with partitions equal to
the swept thread count N, `max(1, N/2)` and `max(1, N/4)`, then max and min,
in a DRAM and a CXL environment. Tigon uses the `tigon` lock set
(`exp_spin`, `ticket`, `mcs`, `cxl_fast_exp`, which passed the two-instance
preload probe) and the native benchmarks the `paper` set. The suite joins the
cgroup of `/sys/fs/cgroup/a/cgroup.procs` before building; remove the
`cgroup` line on hosts without it. To also test adjusted CXL latencies, set
this host's CXL device in `[latency_control]` and uncomment the `cxl-low` /
`cxl-high` environments. Check the PCI address and register for the host
first: the write changes platform PCI configuration. This suite replaces
`start.sh`; the comment at the top of the file maps each old variable and
option.

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
| `--scxl` | (removed; emucxl software-CXL allocator dropped) | |
| `--hcxl` | `--placement locks --numa-node N` | |
| `--alloc cxl-hardware` | `--placement locks` | `--alloc` still accepted as an alias |
| `-m` / `--multithreaded` | (removed; it had no effect) | |
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
