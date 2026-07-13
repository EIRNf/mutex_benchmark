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
| **run** | `--bench {max,min,grouped}` | which C++ binary executes |
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
  --alloc cxl-hardware
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
