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
