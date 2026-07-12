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

Series of commands to run experiments

This command runs each iteration for 5 seconds with an expected total of 8 threads 3 times (8 5 3)
It iterates with increasing threads starting at thread 1 up to 8 with an interval of 1 thread (--iter-threads 1 8 1)
with the max benchmark (-bench max) with the software_cxl lock (-s software_cxl) set defined in constants file.

```python
python3 -m scripts.main 8 5 3 --iter-threads 1 8 1 --bench max -s software_cxl  
```


This command runs with the max benchmark (-bench max) with the software_cxl lock (-s hardware_cxl) set defined in constants file.

```python
python3 -m scripts.main 8 5 3 --iter-threads 1 8 1 --bench max -s hardware_cxl  
```

To graph both sets of experiments together with replacing the underlying data files
```python
python3 -m scripts.main 8 5 3 --iter-threads 1 8 1 --bench max -s combined_cxl --skip-experiment  
```

If the -hcxl flag is added to the run command, the allocation function will be changed from malloc to mmap, mbind to nodemask = 1UL. This can be modified in the lib/utils/cxl_utils.cpp file.
```python
python3 -m scripts.main 8 5 3 --iter-threads 1 8 1 --bench max -s software_cxl  -hcxl
```

You can select different the min benchmark as following: 
```python
python3 -m scripts.main 8 5 3 --iter-threads 1 8 1 --bench min -s software_cxl  
```
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
