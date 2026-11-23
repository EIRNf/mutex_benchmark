# Lock performance analysis: plan and progress

Goal: a concrete, reproducible picture of where every lock falls in
performance, and groupings derived from measurements rather than from design
intent. The earlier groupings (`scripts/constants.py` series and sets, the
champion tables in `COUNTING_LOCKS.md` and `ELEVATOR_SET_COMPARISON.md`) rest
on evidence that cannot support them (see "Why the old evidence is not
enough"), so the benchmark is fixed first, then measured in stages on the
Linux CXL host, then the groups are rebuilt from the data.

This file is the single record of the plan, the decisions and the progress.
It is updated as the work proceeds; see "Keeping this file current".

## Status

| Phase | What | Status |
|---|---|---|
| 0 | Fix the max benchmark | done on macOS; pinning and the old/new check still to repeat on the CXL host |
| 1 | Inventory of what each lock is | in progress |
| 2 | Correctness check on the target host | not started |
| 3 | Measurements (stages A, B, C) | not started |
| 4 | Analysis | not started |
| 5 | Act on the results | not started |

**Next:** Phase 1 (inventory), which needs no CXL machine. On the CXL host: smoke-test
pinning (`python -m scripts run -o capture=throughput -o locks.include=ticket
-o sweep.var=threads -o sweep.start=1 -o sweep.stop=4 -o sweep.step=1
-o workload.pin=compact -o seconds=0.5`, then check run.json `pinning` and the
`cpu_pinned`/`cpu_end`/`node_end` columns), and repeat the old/new roof check
pinned.

## Keeping this file current

- **With every change that completes a step:** tick its box in the same
  commit and add a line to the progress log (date, what changed, anything
  learned). Commit hashes go into the log in the next update, since a commit
  cannot name itself.
- **After every measurement stage:** add the suite folder, the host and its
  state (governor, turbo, SMT), and a short summary of the results under the
  stage, plus a progress log line.
- **When a decision is made or changed:** add it to the decisions log with
  the date and who made it. Do not rewrite old entries; add a new one.
- **When a finding contradicts an assumption:** add it to "Findings" with the
  evidence (run folder, figure), and update the hypothesis table.
- **At the start of a working session:** read "Status" and "Next", and update
  them if they are stale.
- Keep "Status" and "Next" short; detail belongs in the checklists and logs.

## Why the old evidence is not enough

| Problem | Evidence | Consequence |
|---|---|---|
| Throughput mode ignored `--critical-delay` / `--noncritical-delay` | Delays only existed in latency mode (`max_contention_bench.cpp`, old lines 117 and 124); throughput runs use `--thread-level` (`settings.py`). Ticket at 2 threads: 3.6M, 4.4M, 3.7M iterations with no delay, critical 100000, non-critical 100000 | Every throughput number is the same maximum-contention case; any throughput sweep over a delay measured noise |
| Large noise | The three runs above are one workload and differ by about 20%. The docs report ±30–40% between sessions, from the median of 3 one-second runs | Differences under about 1.5× in the doc tables are not established |
| 4P+4E Mac, threads not pinned | `COUNTING_LOCKS.md` §3, `ELEVATOR_SET_COMPARISON.md` §7.8; no affinity code in the benchmark | Points at 8 threads and above mostly measure scheduling; §7.8's own caveat says so |
| Wait policy differs and is not recorded | Spin-then-yield (`LockSpinWait`) in ticket, MCS, CLH, cohort/hierarchical, elevators; the network locks have their own; bakery, lamport, peterson, knuth, dijkstra spin without yielding | "Families separate by regime" is confounded with yield vs no-yield |
| Hierarchical locks never see the real topology | cohort locks and hbo use `thread_id % N` (N = 2 or 4); HMCS hard-codes 8 threads/core, 16 cores/socket, 8 sockets; HCLH maps every thread to the main thread's socket (`HCLH_lock.cpp:119`) | The hierarchical group has never been tested as hierarchical |
| Two names run another design | `lw_bitonic_cas` is `seq_bitonic_cas`; `lw_bitonic_bakery` is `wf_bitonic_bakery` (`linearizable_counting_lock.hpp:84,87`) | `CHAMPIONS_SET`'s Design A representative is Design B |
| Lost updates did not fail a run | Counter mismatch printed to stderr, exit 0 | A broken lock produced a valid-looking point |
| Latency mode on Linux contained another lock | `rand()` (glibc takes a lock) twice per iteration, plus a clock read and a vector append per acquisition | Latency numbers partly measured glibc |

The max benchmark measures handoff and acquire cost only: one lock, a
critical section touching one shared cache line, preemption only when
threads outnumber cores. With no added work it is the **roof**: the most
contention any workload can put on a lock. That case stays exactly as it
is; runs with added work are a separate, labelled family of measurements.

## Phase 0: fix the max benchmark

Every new behaviour sits behind a config key; with the keys unset, a run is
the roof as before (plus warmup, which every new run gets).

- [x] **Warmup in every run.** `workload.warmup` (seconds, default 0.5).
      Threads run the workload, then counting starts; only the measured
      window is reported. Per-thread counters are thread-local during the
      run (no shared, unpadded stats lines in the critical section).
- [x] **Critical and non-critical work in nanoseconds.**
      `workload.critical_ns` (fixed length) and `workload.noncritical_ns`
      (uniform random, mean = the value), calibrated busy work, in both
      capture modes. With both unset the roof's loop has no delay code.
      The old `critical_delay` / `noncritical_delay` (busy-loop iterations,
      latency mode only) stay for latency runs and are rejected for
      throughput, where they never did anything. `rand()` replaced by a
      per-thread generator.
- [x] **Delays declared.** The run folder name, suite run key, data file
      names, plot titles and run.json all state the added work (e.g.
      `cs200ns-ncs1000ns`); the roof is titled as the roof.
- [x] **Pinning.** (Implemented; the Linux code path is only unit-tested against a
      simulated two-socket sysfs so far. Smoke-test it on the CXL host, see Next.) `workload.pin = none | compact | scatter` (Linux). The
      CPU order comes from sysfs topology: compact fills one socket's
      physical cores, then the next socket, then SMT siblings; scatter
      alternates sockets. Each thread's assigned CPU and the CPU and NUMA
      node it ran on are in the data; the order and topology are in run.json.
- [x] **Correctness.** A counter mismatch (lost update) exits non-zero, so
      the point is recorded as failed with its output kept; run.json lists
      each failed point's reason.
- [x] **Fairness.** Per-thread same-thread reacquire counts in the data. Every
      run that has per-thread counts writes a fairness figure and CSV
      (Jain's index, slowest thread's share, reacquire share).
- [x] **Config keys, flag CLI and README** updated.
- [x] **Hierarchical topology: deferred (TODO).** No environment to test them
      in yet. `TODO(topology)` comments mark each place: cohortTicket,
      cohortMCS, cohortTAS, cohortPTicket, hbo, HMCS, HCLH. Until fixed, the
      hierarchical locks are measured as flat locks and labelled so.
- [x] **Second pass: behaviour is consistent** (macOS, 2026-11-20; see
      Findings for the numbers). Old vs new binary on the roof, interleaved,
      7 reps × 1 s, ticket/mcs/exp_spin/bakery at 1/2/4/8 threads: equal
      within noise except two explained shifts. Delay calibration within 3%
      from 500 ns up. Negative controls: `null` fails every multi-thread
      point (breach abort in throughput mode, exit 2 "LOST UPDATES" in latency
      mode), and run.json gives the reason. Fairness: ticket and mcs have
      Jain = 1.00 and reacquire share 0.00 at 2–8 threads, as a FIFO lock
      should. **Still to repeat on the CXL host, pinned:** this Mac's noise
      (old binary's run-to-run CV up to 45% at 8 threads) is too high for a
      tighter statement.

Follow-ups noted but not in Phase 0: warmup for the min and grouped
benchmarks; latency mode still reads the clock and appends to a vector per
acquisition (a histogram would be cheaper); `capture = rusage` has no
per-thread counts, so it gets no fairness figure.

## Phase 1: inventory of what each lock is

One table in the repo covering all registered names (`bench_utils.cpp`):

- [ ] Implementation behind each name; aliases collapsed.
- [ ] Family or design; controls (`null`, `broken`) marked.
- [ ] Atomic read-modify-write used (none, CAS, fetch-add, exchange,
      test-and-set), from the disassembly of `lock()` / `unlock()`
      (x86: `lock`-prefixed and `xchg`; arm64: `ldxr`/`stxr`, `cas`,
      `ldadd`, `swp`), not from comments.
- [ ] Wait policy (pure spin, spin-then-yield, backoff, sleep) and ordering
      (FIFO, bounded overtaking, unfair).
- [ ] Memory per lock and per thread; singleton-only; allocation in `lock()`.
- [ ] Known issues (lock bug audit, July 2026).

## Phase 2: correctness check on the target host

- [ ] Short repeated runs with the breach and counter checks at every thread
      count used later. Failing or hanging locks are excluded from rankings
      and listed with the failing condition (expect `lw_*`, about 1–3% hangs).

## Phase 3: measurements

Linux CXL server, DRAM first. Record and fix: governor, turbo, SMT, idle
machine, `lscpu`.

Factors: threads (1, 2, 4, … one socket's physical cores, all physical
cores; SMT and oversubscription at 1.5× and 2× cores as separate regimes);
pinning (compact, scatter); contention grid critical 0 / 200 ns / 2 µs ×
non-critical 0 / 1 µs / 10 µs (0 × 0 is the roof); later memory placement and
CXL latency.

Metrics: throughput; Jain's index; slowest-thread share; reacquire share;
CPU time and context switches; acquire-latency percentiles (shortlist only).

Noise control: 3 s measured plus warmup; 5+ repetitions (3 in stage A);
locks interleaved within each repetition; ticket throughout as a drift
reference; medians with bootstrap 95% confidence intervals.

| Stage | Locks | Grid | Time | Status |
|---|---|---|---|---|
| A: screen | all eligible (~85) | 3 contention levels × ~10 thread counts × compact × 3 reps | ~7.5 h | not started |
| B: focus | ~25: one per group from A, outliers, disputed | 6 contention points × 10 thread counts × 2 pinnings × 5 reps | ~15 h | not started |
| C: CXL | ~10 | 2 contention levels × 8 thread counts × DRAM / CXL process at 3 latencies / CXL locks only × 5 reps | ~4 h | not started |

## Phase 4: analysis

- [ ] Per-lock profile: single-thread cost, cost per handoff at saturation,
      scaling shape, fairness, CPU burned, oversubscription collapse point,
      pinning sensitivity.
- [ ] Groups from the data: feature vectors (log throughput relative to the
      median lock at each point, plus fairness), clustering, 2-D projection;
      compared with Phase 1 families (contingency table, adjusted Rand
      index). Every lock outside its family is a finding to explain.
- [ ] Rankings per regime with confidence intervals; overlaps reported as ties.
- [ ] Outliers on the shortlist explained with hardware counters.
- [ ] A new `python -m scripts` command over a suite folder: CSV, figures, and
      a summary map (single-thread cost vs handoff cost at saturation,
      coloured by family, marked by fairness).

## Phase 5: act on the results

- [ ] Rebuild the sets in `constants.py`: series by design, comparison sets by
      measured behaviour.
- [ ] Remove or rename alias names.
- [ ] Update the champion tables in the two docs.

## Claims to confirm or retire

| Claim | Source | Test | Result |
|---|---|---|---|
| RMW-freedom is essentially free | `ELEVATOR_SET_COMPARISON.md` §7.8 point 1 | Pairs differing only in primitive (`linear_bl_elevator` vs `linear_cas_elevator`, `wf_bitonic_bakery` vs `wf_bitonic_cas`), within and across sockets | open |
| Families separate by regime; the 12–16 thread inversion | §7.8 points 2–3 | Oversubscription regime with wait policy recorded; pairs differing only in wait policy | open |
| Network-shaped fair locks beat ticket 1.5–1.9× at saturation | `COUNTING_LOCKS.md` §3 point 2 | Saturated FIFO regime, pinned | open |
| Designs F and G do not separate from B | `COUNTING_LOCKS.md` §3 point 4 | Oversubscription regime | open |
| One champion per series | `CHAMPIONS_SET` comment | Per-series ranks with CIs, per regime | open |
| Hierarchical locks win across sockets | design intent | After the topology fix, scatter pinning | blocked (topology TODO) |
| `software_cxl` is RMW-free | `constants.py` | Phase 1 disassembly | open |

## Decisions log

- **2026-11-20 (user):** plan approved. Phase 0 approved with these notes:
  keep the max benchmark's roof clean and separate; delays must be visible
  in the run title; pin and record assigned cores; hierarchical topology
  fixes are TODOs for now (no environment to test them); warmup for every
  run; track correctness and fairness and plot fairness for every run;
  update the config keys; do a second pass to check behaviour is consistent.
- **2026-11-20:** target host for Phases 2–3 is the Linux CXL server
  (recommended; `lscpu` still needed to set thread counts). Wait policy is
  measured as implemented and recorded per lock, not unified.

## Findings

- **2026-11-20, roof old vs new (macOS 4P+4E, unpinned).** New/old median
  throughput, geometric mean over 16 cells: 1.05 (warmup 0) and 1.07
  (warmup 0.5); bootstrap 95% CIs contain 1 in 10/16 and 12/16 cells. The
  two consistent shifts:
  - **1 thread: +5–8%** (ticket, mcs, bakery; CI excludes 1 for ticket and
    mcs). The old loop updated two shared, unpadded memory counters in every
    critical section (the thread's iteration count, and the "unfair" count,
    which at 1 thread is incremented every time); the new loop keeps both in
    registers. About 3 ns of a 47 ns iteration. Deliberate: the roof no longer
    charges each reacquire an extra shared write.
  - **exp_spin at 8 threads: −15 to −19%** (CI excludes 1 with warmup). With
    warmup (and the start barrier) all 8 threads contend for the whole window;
    the old run counted the start, when fewer had arrived.
  Data: scratchpad `ab/roof.csv` from this session (not kept in the repo).
- **2026-11-20, calibration.** At 1 thread, added work measured / requested:
  critical 0.88–0.93 at 100 ns, 0.99–1.03 at 500 ns–10 µs; non-critical
  1.00–1.02 throughout. 100 ns critical work is partly hidden by the
  out-of-order core; treat sub-200 ns critical work as approximate.
  Saturation: ticket with 2 µs critical work takes 2.2 µs (2 threads) and
  2.5 µs (4 threads) per acquisition: the critical section plus the handoff.
- **2026-11-20, old Mac data re-plotted with the fairness figure** (paper set,
  1–16 threads, 0.1 s): past 8 threads (oversubscribed) bakery,
  boulangerie and knuth starve threads (slowest thread 0.00–0.06 of the
  mean), while the elevators, ticket and mcs stay near 1. Consistent with
  "wait policy, not family" (those three spin without yielding); to confirm
  pinned on Linux.
- **2026-11-20, exp_spin hands the lock back to its last holder in nearly
  every acquisition** (reacquire share 1.00 at 2–8 threads, roof) while Jain's
  index stays 0.93–0.99 over a 0.3 s window: its unfairness is long bursts,
  invisible to the per-thread totals the old analysis used.

## Progress log

- **2026-11-20:** plan written; Phase 0 started.
- **2026-11-20:** Phase 0 implemented (uncommitted): warmup, calibrated work
  in ns, declared work tags, pinning, lost-update exit, failure reasons in
  run.json, fairness figure and CSV for every run, new config keys and flags,
  README, `TODO(topology)` notes, 15 new tests (94 pass). Second pass done on
  macOS; Linux pinning and a pinned old/new check remain for the CXL host.
- **2026-11-23:** Phase 0 committed (user's go-ahead); Phase 1 started.
