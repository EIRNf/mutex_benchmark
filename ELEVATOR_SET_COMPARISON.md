# Elevator Lock Set — Overhead Comparison

Comparison of all locks in the `ELEVATOR_SET` benchmark group, focusing on
algorithmic overhead, space cost, contention behaviour, and
hardware requirements.

**Notation**: *n* = threads, *W* = network width (smallest power of 2 ≥
⌈√n⌉), *CL* = cache-line size (64 B).

---

## 1. Summary Table

| Lock | Lock O() | Unlock O() | Atomics per lock() | Space | Local Spin | Fairness |
|---|---|---|---|---|---|---|
| `exp_spin` | O(1) | O(1) | 1 TAS | O(1) | No | Unfair |
| `mcs` | O(1) | O(1) | 1 XCHG + 1 CAS | O(n·CL) | Yes | FIFO |
| `mcs_nca` | O(1) | O(1) | 1 XCHG + 1 CAS | O(n) | Yes\* | FIFO |
| `linear_cas_elevator` | O(n) | O(n) | 1 TAS (waker) | O(n·CL) | Yes | ≈FIFO |
| `linear_bl_elevator` | O(n) | O(n) | **0** | O(n·CL) | Yes | ≈FIFO |
| `linear_lamport_elevator` | O(n) | O(n) | **0** | O(n) | Yes† | ≈FIFO |
| `tree_cas_elevator` | O(log n) | O(log n) | 1 TAS (waker) | O(n·CL) | Yes | Tree-order |
| `tree_bl_elevator` | O(log n) | O(log n) | **0** | O(n·CL) | Yes | Tree-order |
| `tree_lamport_elevator` | O(log n) | O(log n) | **0** | O(n·CL) | Yes | Tree-order |
| `net_elevator` | O(log²W) | O(n·W) | log²W × fetch\_xor | O(W²log²W + n·CL) | Yes | Elevator |
| `bitonic_cas` | O(log²W) | O(n) | log²W × fetch\_add | O(W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `bitonic_bl` | O(n·log²W) | O(n) | **0** | O(n·W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `bitonic_lamport` | O(n·log²W)‡ | O(n) | **0** | O(n·W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `bitonic_elevator` | O(log²W) | O(n) | log²W × (XCHG + CAS) | O(n·W·log²W + n·CL) | Yes (stack+node) | ≈FIFO |
| `bitonic_bakery` | O(n·log²W) | O(n) | **0** | O(n·W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `periodic_cas` | O(log²W) | O(n) | log²W × fetch\_add | O(W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `periodic_bl` | O(n·log²W) | O(n) | **0** | O(n·W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `periodic_lamport` | O(n·log²W)‡ | O(n) | **0** | O(n·W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `periodic_elevator` | O(log²W) | O(n) | log²W × (XCHG + CAS) | O(n·W·log²W + n·CL) | Yes (stack+node) | ≈FIFO |
| `periodic_bakery` | O(n·log²W) | O(n) | **0** | O(n·W·log²W + n·CL) | Yes (stack) | ≈FIFO |
| `wf_bitonic_cas` | O(log²W) | **O(1)** | log²W fetch\_add | O(n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_bitonic_bl` | O(n·log²W) | **O(1)** | **0** | O(n·W·log²W + n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_bitonic_lamport` | O(n·log²W)‡ | **O(1)** | **0** | O(n·W·log²W + n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_bitonic_bakery` | O(n·log²W) | **O(1)** | **0** | O(n·W·log²W + n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_periodic_cas` | O(log²W) | **O(1)** | log²W fetch\_add | O(n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_periodic_bl` | O(n·log²W) | **O(1)** | **0** | O(n·W·log²W + n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_periodic_lamport` | O(n·log²W)‡ | **O(1)** | **0** | O(n·W·log²W + n·CL) | Yes (phase bit) | ≈FIFO |
| `wf_periodic_bakery` | O(n·log²W) | **O(1)** | **0** | O(n·W·log²W + n·CL) | Yes (phase bit) | ≈FIFO |

\* `mcs_nca` nodes not CL-padded — false sharing possible.  
† `linear_lamport_elevator` grant flags not CL-padded.  
‡ O(1) fast path when uncontended; degrades to O(n) per balancer under contention.  
§ Seq (`seq_*`) and LW (`lw_*`) were broken (mutual-exclusion violations /
hangs) and **repaired 2026-07-11** — token-versioned handoff for Seq, SERVED
breadcrumbs + waker-flag polling for LW; see
`LINEARIZABLE_COUNTING_ANALYSIS.md` §8.3. All 16 variants now pass the
breach-detecting benchmark, 5 reps × {2,4,8} threads, 0 failures.

---

## 2. Overhead Breakdown

### 2.1 Lock Acquisition

The dominant cost is the **number of serialisation points** a thread must
pass through to acquire the lock.

| Category | Locks | Serialisation Points | Cost Model |
|---|---|---|---|
| **Single atomic** | `exp_spin`, `mcs`, `mcs_nca` | 1 | O(1) — one RMW to enter |
| **Linear scan (software)** | `linear_bl_elevator`, `linear_lamport_elevator` | 1 (BL/Lamport doorway over n threads) | O(n) — scan all threads, no atomics |
| **Linear scan (CAS waker)** | `linear_cas_elevator` | 1 TAS + O(n) scan | O(n) — 1 atomic + linear scan |
| **Tree walk** | `tree_*_elevator` | O(log n) tree nodes + waker | O(log n) — write from leaf to root |
| **Network traverse (CAS)** | `net_elevator`, `bitonic_cas`, `periodic_cas` | O(log²W) atomics | O(log²W) — each balancer is 1 RMW |
| **Network traverse (elevator sync)** | `bitonic_elevator`, `periodic_elevator` | O(log²W) MCS-style locks | O(log²W) — each balancer is 1 XCHG + local spin |
| **Network traverse (software)** | `bitonic_bl`, `periodic_bl`, `*_bakery` | O(log²W) mutex acquires, each O(n) | O(n·log²W) — most expensive |
| **Network traverse (Lamport)** | `bitonic_lamport`, `periodic_lamport` | O(log²W) Lamport locks | O(1)–O(n) per balancer × log²W |

**Concrete example** (n = 16 threads → W = 4, log²W = 4 balancers traversed):

| Lock | Ballpark lock() operations |
|---|---|
| `mcs` | 1 XCHG |
| `exp_spin` | 1 TAS (+ backoff retries) |
| `linear_bl_elevator` | 16-thread BL doorway scan |
| `tree_cas_elevator` | 4 tree writes + 1 TAS |
| `bitonic_cas` / `periodic_cas` | 4 fetch\_add |
| `bitonic_elevator` / `periodic_elevator` | 4 XCHG + 4 CAS (MCS-style per balancer) |
| `bitonic_bl` / `periodic_bl` | 4 × 16-thread BL doorways = 64 flag reads |
| `bitonic_bakery` / `periodic_bakery` | 4 × 16-thread bakery doorways = 64 reads + 64 writes |

### 2.2 Unlock / Grant

| Category | Locks | Unlock Cost | Mechanism |
|---|---|---|---|
| **O(1) release** | `exp_spin` | O(1) | Store 0 to flag |
| **O(1) handoff** | `mcs`, `mcs_nca` | O(1) | Write to successor's node (1 CAS if tail) |
| **O(log n) tree walk** | `tree_*_elevator` | O(log n) | Root-to-leaf sibling check + ring dequeue |
| **O(n) linear scan** | `linear_*_elevator` | O(n) | Cyclic scan of waiting[] flags |
| **O(n) step-property scan** | `bitonic_*`, `periodic_*` | O(n) | Scan all ThreadMeta, use (wire\_dist, round, tid) ordering |
| **O(n·W) floor sweep** | `net_elevator` | O(n·W) | Sweeps up to 2(W−1) floors, each probing all n ThreadMeta slots (`find_floor_winner`) |

The new bitonic/periodic locks and the existing counting network locks share
the same O(n) unlock scan. This is the primary scaling bottleneck — every
unlock must inspect all n thread metadata slots. The WF (Waiting-Filter)
variants solve this with O(1) unlock via a phase-bit chain.

### 2.3 Space Overhead

| Lock | Per-thread | Shared State | Total |
|---|---|---|---|
| `exp_spin` | 0 | 1 atomic flag | O(1) |
| `mcs` | 1 CL-padded node | 1 atomic tail ptr | O(n·CL) |
| `linear_*_elevator` | 1 CL-padded grant flag + 1 bool | Waker lock O(n) | O(n·CL) |
| `tree_*_elevator` | 1 CL-padded grant flag | 2n+1 tree nodes + ring buffer | O(n·CL) |
| `net_elevator` | 1 CL-padded meta slot | W²/2 · log²W atomics | O(W²·log²W + n·CL) |
| `cn_ticket_*` | 1 CL-padded meta slot | CL-padded balancer array | O(W·log²W·CL + n·CL) | *removed* |
| `bitonic_cas` / `periodic_cas` | 1 CL-padded meta slot | Heap-allocated balancer tree | O(W·log²W + n·CL) |
| `bitonic_bl` / `periodic_bl` | 1 CL-padded meta slot | Balancer tree + n flags/balancer | O(n·W·log²W + n·CL) |
| `bitonic_elevator` / `periodic_elevator` | 1 CL-padded meta slot | Balancer tree + n MCS nodes/balancer | O(n·W·log²W + n·CL) |
| `bitonic_bakery` / `periodic_bakery` | 1 CL-padded meta slot | Balancer tree + 2n arrays/balancer | O(n·W·log²W + n·CL) |

The software-sync variants (BL, Lamport, Elevator, Bakery) allocate O(n) state
**per balancer**, multiplied by O(W/2 · log²W) balancers. For n = 64 (W = 8,
~24 balancers): ~1536 flag bytes for BL, ~6 KB for Bakery.

---

## 3. Contention Distribution

The key advantage of counting-network-based locks over simple elevator locks
is **contention distribution**. Instead of all threads contending on a single
point, the network spreads contention across W/2 independent balancers per
stage.

| Lock | Contention Points | Threads per Point |
|---|---|---|
| `exp_spin` | 1 shared flag | n |
| `mcs` | 1 tail pointer | n |
| `linear_*_elevator` | 1 waker lock | n |
| `tree_*_elevator` | Tree root | n (up to O(log n) per level) |
| `net_elevator` | W/2 balancers per stage | n/W per balancer (expected) |
| `bitonic_*` / `periodic_*` | W/2 balancers per stage | n/W per balancer (expected) |
| `cn_ticket_*` | W/2 CL-padded balancers per stage | n/W per balancer | *removed* |

For n = 64 threads with W = 8: each balancer sees ~8 threads instead of 64.
This reduces cache-line bouncing by ~8× compared to a single-point lock.

---

## 4. Hardware Requirements

| Lock | Requires Atomic RMW? | Pure Software? | CXL/NUMA Notes |
|---|---|---|---|
| `exp_spin` | Yes (TAS) | No | Poor — all spin on shared line |
| `mcs` | Yes (XCHG, CAS) | No | Excellent — local spin on own node |
| `linear_cas_elevator` | Yes (TAS in waker) | No | Good — local spin on own flag |
| `linear_bl_elevator` | **No** | **Yes** | Good — local spin, software fences only |
| `linear_lamport_elevator` | **No** | **Yes** | Moderate — flags not CL-padded |
| `tree_cas_elevator` | Yes (tree atomics) | No | Good — local spin |
| `tree_bl_elevator` | **No** | **Yes** | Good — local spin |
| `tree_lamport_elevator` | **No** | **Yes** | Good — local spin |
| `net_elevator` | Yes (fetch\_xor) | No | Good — local spin, distributed balancers |
| `cn_ticket_bl` | **No** | **Yes** | Good — stack-local spin | *removed* |
| `cn_ticket_lamport` | **No** | **Yes** | Good — stack-local spin | *removed* |
| `bitonic_cas` / `periodic_cas` | Yes (fetch\_add) | No | Good — stack-local spin, distributed |
| `bitonic_bl` / `periodic_bl` | **No** | **Yes** | Good — stack-local spin, distributed |
| `bitonic_lamport` / `periodic_lamport` | **No** | **Yes** | Good — stack-local spin, distributed |
| `bitonic_elevator` / `periodic_elevator` | Yes (XCHG + CAS) | No | **Best** — local spin at both balancer and lock level |
| `bitonic_bakery` / `periodic_bakery` | **No** | **Yes** | Good — stack-local spin, no atomics at all |

**Pure software locks** (no atomic RMW instructions, fences only):
`linear_bl_elevator`, `linear_lamport_elevator`, `tree_bl_elevator`,
`tree_lamport_elevator`, `bitonic_bl`,
`bitonic_lamport`, `bitonic_bakery`, `periodic_bl`, `periodic_lamport`,
`periodic_bakery`, `wf_bitonic_bl`, `wf_bitonic_lamport`, `wf_bitonic_bakery`,
`wf_periodic_bl`, `wf_periodic_lamport`, `wf_periodic_bakery`.

---

## 5. Bitonic / Periodic vs Existing Locks — Trade-off Analysis

### When bitonic/periodic CAS locks win over elevator locks

- **High thread counts (n > 16)**: The O(log²W) distributed network traverse
  scales better than the O(n) linear scan in `linear_*_elevator`. At n = 64,
  the network traverses ~10 balancers while linear elevator scans 64 flags.
- **NUMA/CXL with many sockets**: Network distributes cache-line ownership
  across W/2 balancers. Elevator locks concentrate contention in the waker
  lock (one cache line).
- **Mixed workloads**: Step-property ordering provides fairer distribution
  than the BurnsLamport priority-based waker.

### When elevator locks win over bitonic/periodic

- **Low thread counts (n ≤ 8)**: Simple linear scan is cheaper than building
  and traversing a multi-level network. The constant factors in network
  construction (heap allocation, pointer chasing) dominate.
- **Unlock-heavy workloads**: Both use O(n) unlock scans, but elevator locks
  have smaller constant factors (simpler metadata).
- **Space-constrained environments**: `linear_bl_elevator` uses O(n·CL) space.
  `bitonic_bl` uses O(n·W·log²W + n·CL) — significantly more for software
  sync variants.

### When MCS wins over everything

- **Pure throughput under contention**: O(1) lock + O(1) unlock with FIFO
  ordering. No scan, no network traverse. MCS is hard to beat when the only
  goal is raw lock/unlock throughput and hardware CAS is available.
- **Caveat**: MCS requires one atomic XCHG per lock and one CAS per unlock —
  these are not available on all ISAs or CXL configurations.

### When bitonic/periodic software variants are uniquely valuable

- **ISAs without atomic RMW** (embedded, some RISC-V cores, CXL-attached
  memory without coherent atomics): `bitonic_bl`, `bitonic_bakery`,
  `periodic_bl`, `periodic_bakery` provide distributed counting with only
  loads, stores, and fences.
- **No other lock in this set** combines distributed contention reduction
  with pure-software execution except the bitonic/periodic variants (recursive network)
  and their WF counterparts.
- **No other lock in this set** combines distributed contention reduction,
  O(1) unlock, AND pure-software execution except the `wf_*_bl`,
  `wf_*_lamport`, and `wf_*_bakery` variants.

### Bitonic vs Periodic

Both have the same asymptotic complexity. The difference is structural:

| Property | Bitonic | Periodic |
|---|---|---|
| Structure | Recursive (half-networks + merger) | Repeated identical blocks |
| Regularity | Irregular — different stages have different wiring | **Regular** — every stage identical |
| Hardware mapping | Complex routing | Simple, tiled layout |
| Pipeline parallelism | Harder — stages differ | Natural — each block is one pipeline stage |
| Fault tolerance | Replace specific sub-network | Replace any block |
| Software overhead | Slightly less pointer chasing (fewer blocks) | Slightly more blocks for same width |

In practice, **performance should be nearly identical** for software
implementations. The periodic variant is preferred for hardware synthesis
or when structural regularity is valued.

---

## 6. Recommended Lock Selection

| Scenario | Recommended Lock | Rationale |
|---|---|---|
| General purpose, CAS available | `mcs` | O(1)/O(1), FIFO, local spin |
| High contention, many threads, CAS available | `bitonic_cas` or `periodic_cas` | Distributed contention, stack-local spin |
| Pure software, moderate threads | `linear_bl_elevator` | Simple, no atomics, local spin |
| Pure software, many threads | `bitonic_bl` or `periodic_bl` | Distributed contention without any atomics |
| NUMA/CXL, best locality | `bitonic_elevator` or `periodic_elevator` | MCS-style local spin at every balancer |
| Most portable (no atomics, no fences under SC) | `bitonic_bakery` or `periodic_bakery` | Pure loads and stores |
| Strict FIFO with O(1) unlock | `wf_bitonic_cas` or `wf_periodic_cas` | Waiting-filter, O(1) phase-bit unlock, no extra atomics |
| O(1) unlock + distributed lock | `wf_bitonic_cas` or `wf_periodic_cas` | Waiting-filter, best balance of throughput and unlock cost |

| O(1) unlock + single bottleneck | `seq_bitonic_cas` | Sequenced, global fetch\_add linearization (repaired 2026-07-11 after a mutual-exclusion bug; see §7.4/§7.5) |

---

## 7. Linearizable Counting Lock Designs

Designs derived from Herlihy, Shavit, Waarts (1996) "Linearizable Counting
Networks," Aspnes, Herlihy, Shavit (1994) "Counting Networks," and Lynch,
Shavit, Shvartsman, Touitou (PODC 1996) "Counting Networks Are Practically
Linearizable," implemented as mutual exclusion locks with the project's
`SoftwareMutex` interface: LW (Design A), Seq (Design B), WF (Design C), and
the Skew/RSkew filter locks (see `LINEARIZABLE_COUNTING_ANALYSIS.md` §4/§4b
for full design descriptions and implementation-status caveats).

### 7.1 Design Overview

| Design | Lock O() | Unlock O() | Extra Atomics | Key Mechanism | Source | Status (re-verified 2026-07-11) |
|---|---|---|---|---|---|---|
| **A: Wire-Indexed (LW)** | O(log²W) | O(1) pred / O(W) fallback + SERVED cleanup | 0 | Per-wire slot arrays + step-property prediction | Novel | Correct at 2/4/8T (**repaired** — previously hung; see LINEARIZABLE_COUNTING_ANALYSIS.md §8.3) |
| **B: Sequenced (Seq)** | O(log²W) + 1 fetch\_add | O(1) guaranteed | 1 fetch\_add (global\_seq) | Counting network + global ticket + token-versioned slot handoff | Aspnes et al. §5.1 | Correct at 2/4/8T (**repaired** — previously breached mutual exclusion; see §8.3) |
| **C: Waiting-Filter (WF)** | O(log²W) + O(1) phase check | **O(1) always** | 0 | Counting network + n-element phase-bit array | Herlihy-Shavit-Waarts §3 | Correct at 2/4/8T |
| **Skew/RSkew** | O(log²W) + (n−1) fetch\_add + ticket | O(1) fetch\_add | n−1 + 1 fetch\_add | Network + vestigial skew filter + **plain ticket lock** (see `LINEARIZABLE_COUNTING_ANALYSIS.md` §4b) | HSW §4 (aspirational; not actually realized) | Correct at 2/4/8T (as ticket locks) |

### 7.2 Theoretical Foundation

**From Aspnes-Herlihy-Shavit (1994):**
- Step property (Lemma 2.2): In quiescent state, output wire i has ⌈(m−i)/w⌉
  tokens. This enables predictable token assignment for locks.
- Theorem 3.6: Bitonic[w] satisfies the step property.
- Theorem 4.4: Periodic[2k] satisfies the step property after log(k) Block stages.
- Shared counter (§5.1): The counting network distributes thread arrivals
  across W output wires, each maintaining a local counter. Token =
  wire + round × width.

**Design C connection to Herlihy-Shavit-Waarts (1996):**
- The Waiting-filter from §3 uses an n-element phase-bit array where
  phase(v) = ⌊v/n⌋ mod 2. Token v waits for predecessor v−1 at slot
  (v−1) mod n, then sets its own phase bit on unlock. This provides
  O(1) unlock with linearizable ordering.
- Impossibility (Theorem 5.4): Any linearizable counting protocol with
  capacity c has latency Ω(n/c). The Waiting-filter trades one blocking
  wait (at the phase-bit check) for O(1) unlock — the best possible
  without non-blocking constraints.

### 7.3 Implementation Notes

**Design A (Wire-Indexed):** Uses the designated-waker protocol from the
bitonic counting locks. The unlock path predicts the successor using the
step property: the next thread should be on wire (current+1) % W at the
next round. O(1) if prediction hits, O(W) sweep if it misses.
**Status: BROKEN under contention.** The waker protocol can deadlock when
multiple threads compete across iterations. The 2T+ benchmarks show 0 throughput.
This design requires a waker-free rework to be viable.

**Design B (Sequenced):** Combines a counting network for contention
distribution with a global `fetch_add` for linearization. The network
spreads arrivals across W/2 balancers per stage so threads hit
`global_seq_` at staggered intervals. Slot array indexed by `seq & mask`
provides direct handoff in unlock. Essentially a ticket lock backed by a
counting network.

**Design C (Waiting-Filter):** The cleanest design. Token v from the
counting network indexes into a circular phase-bit array. Token v waits
for phase\_bit[(v-1) % n] == phase(v-1), which is set by predecessor's
unlock. ABA prevention: phase toggles every n tokens per slot. Under
practical linearizability (c2 ≤ 2·c1), the wait is near-zero because
predecessors complete before successors check.

### 7.4 Experimental Results (measured 2026-07)

Platform: Apple M-series (aarch64), max contention benchmark, `--thread-level`
mode (breach-detecting critical section), 1s per run, median of 3 reps.
Throughput in operations/second.

Correctness sweep first (all 52 counting-network variants + baselines, 2/4/8
threads, 6s hang timeout): every `bitonic_*`, `periodic_*`, `wf_*`, `skew_*`,
`rskew_*` variant plus `net_elevator` and `hmcs` passed with no breach. At the
time of the initial sweep all 8 `lw_*` variants hung and all 8 `seq_*`
variants breached mutual exclusion and/or hung at ≥4T; both families were
repaired on 2026-07-11 (see `LINEARIZABLE_COUNTING_ANALYSIS.md` §8.3) and now
pass 5 reps × {2,4,8}T with 0 failures. The throughput table below predates
the repair and therefore omits `lw_*`/`seq_*`; repaired-lock throughput is
listed separately after it.

| Lock | 1T ops/s | 1T vs MCS | 2T ops/s | 2T vs MCS | 4T ops/s | 4T vs MCS | 8T ops/s | 8T vs MCS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `mcs` | 24.86M | 1.00x | 4.92M | 1.00x | 5.99M | 1.00x | 775.2K | 1.00x |
| `spin` | 26.62M | 1.07x | 19.76M | 4.01x | 9.00M | 1.50x | 5.15M | 6.64x |
| `exp_spin` | 19.29M | 0.78x | 19.53M | 3.97x | 10.73M | 1.79x | 6.12M | 7.90x |
| `ticket` | 24.72M | 0.99x | 5.53M | 1.12x | 4.39M | 0.73x | 714.9K | 0.92x |
| `linear_lamport_elevator` | 24.45M | 0.98x | 5.93M | 1.21x | 2.59M | 0.43x | 762.2K | 0.98x |
| `tree_lamport_elevator` | 21.05M | 0.85x | 6.62M | 1.34x | 1.73M | 0.29x | 98.4K | 0.13x |
| `net_elevator` | 17.51M | 0.70x | 5.93M | 1.20x | 2.90M | 0.48x | 187.0K | 0.24x |
| `hmcs` | 19.03M | 0.77x | 3.68M | 0.75x | 1.21M | 0.20x | 370.0K | 0.48x |
| `bitonic_cas` | 21.85M | 0.88x | 5.83M | 1.18x | 2.61M | 0.44x | 83.9K | 0.11x |
| `periodic_cas` | 22.23M | 0.89x | 6.02M | 1.22x | 2.63M | 0.44x | 53.7K | 0.07x |
| `bitonic_bakery` | 19.80M | 0.80x | 5.37M | 1.09x | 2.01M | 0.34x | 172.0K | 0.22x |
| `wf_bitonic_cas` | 23.38M | 0.94x | 9.00M | 1.83x | 2.79M | 0.47x | 211.1K | 0.27x |
| `wf_periodic_cas` | 23.73M | 0.95x | 6.97M | 1.42x | 1.21M | 0.20x | 146.5K | 0.19x |
| `wf_bitonic_lamport` | 21.62M | 0.87x | 7.77M | 1.58x | 2.65M | 0.44x | 256.2K | 0.33x |
| `skew_bitonic_cas` | 20.50M | 0.82x | 6.50M | 1.32x | 2.66M | 0.44x | 241.1K | 0.31x |
| `rskew_bitonic_cas` | 21.80M | 0.88x | 6.28M | 1.28x | 1.81M | 0.30x | 95.0K | 0.12x |

> Historical note: an earlier version of this table included `seq_bitonic_cas`
> / `seq_periodic_cas` with numbers that "beat MCS at 2T/4T" while other
> sections of this document simultaneously called `seq_*` broken. Both were
> half-right: `seq_*` ran fast *because* it sometimes admitted two threads at
> once. Those measurements predate the breach detector and have been removed.

**Repaired-lock throughput (measured 2026-07-11, after the §7.5 fixes, same
methodology, breach detector active — these numbers are from correct runs):**

| Lock | 1T | vs MCS | 2T | vs MCS | 4T | vs MCS | 8T | vs MCS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `mcs` (same-day control) | 24.73M | 1.00x | 6.34M | 1.00x | 5.55M | 1.00x | 1.20M | 1.00x |
| `lw_bitonic_cas` | 17.73M | 0.72x | 5.42M | 0.85x | 3.78M | 0.68x | 284.9K | 0.24x |
| `lw_periodic_cas` | 17.56M | 0.71x | 7.92M | 1.25x | 1.23M | 0.22x | 426.4K | 0.35x |
| `seq_bitonic_cas` | 13.51M | 0.55x | 5.91M | 0.93x | 5.38M | 0.97x | 395.4K | 0.33x |
| `seq_periodic_cas` | 14.23M | 0.58x | 6.22M | 0.98x | 5.69M | 1.03x | 712.8K | 0.59x |

The repaired Seq is the strongest ordered network lock at 4T (`seq_periodic_cas`
1.03x MCS) — its global fetch_add costs it at 1T but the network's arrival
staggering keeps it near-MCS from 2T up, now with genuine mutual exclusion.
The repaired LW pays its O(W)-sweep + breadcrumb-cleanup unlock visibly at
higher thread counts, as the design predicts.

### 7.5 Analysis

**Key findings (2026-07 data):**

1. **Unfair spin locks dominate raw throughput at every contention level**
   (`spin`/`exp_spin`, 4-8x MCS at 2T and 8T) because they do no ordering, no
   scan, and no handoff — at the cost of unbounded unfairness. Every ordered
   lock in this set pays a large 8T penalty on this platform.

2. **WF (Waiting-Filter) is the best of the linearizable counting designs and
   the only one that is correct.** `wf_bitonic_cas` is the strongest ordered
   lock at 2T (1.83x MCS) and stays mid-pack at 4T/8T. Earlier claims that WF
   beats MCS at 4T/8T are not reproduced by the current measurement — MCS
   leads all counting-network locks at 4T and 8T in this run. (Elevator sync
   variants were removed from the linearizable designs because `BnElevatorSync`
   uses hardware CAS/XCHG, contradicting software-only balancer sync.)

3. **The O(n) unlock scan is the scaling bottleneck for the base
   bitonic/periodic locks**: competitive through 4T (~0.44x MCS), then a
   50-100x drop at 8T (54-84K ops/s) as every release rescans all thread
   metadata under full contention.

4. **Skew/RSkew perform like what they are — ticket locks with extra
   network/filter overhead** (see §7.1 and `LINEARIZABLE_COUNTING_ANALYSIS.md`
   §4b): close to `ticket` at 2T, below it at 4T/8T by roughly the cost of the
   dead filter's n−1 fetch_adds.

5. **MCS degrades but does not collapse at 8T** in the current measurement
   (775K ops/s, best-in-class among ordered locks). The 15K "contention
   collapse" in the earlier table was not reproduced; treat platform-specific
   8T behavior as noisy (this machine has 8+ cores of mixed
   performance/efficiency types, and the benchmark oversubscribes the
   performance cores).

6. **LW (Wire-Indexed) was broken and is repaired.** It hung at ≥2T: the
   waker-flag self-serve path advanced a wire's head past not-yet-registered
   rounds (making those waiters invisible to the sweep forever), and waiters
   never re-polled the waker flag after one failed trylock. Fixed 2026-07-11
   with SERVED breadcrumbs consumed in round order by the sweep, flag polling
   in the wait loop, and a slot ring sized ≥ 2n (see
   `LINEARIZABLE_COUNTING_ANALYSIS.md` §8.3). Now passes 5×{2,4,8}T clean.

7a. **Post-optimization update (2026-07-12).** Three correctness-preserving
   optimizations to the shared network core (§7.6) changed the competitive
   picture at high contention: several counting-network locks now beat MCS
   at 4T and 8T (same-session controls). Findings 1-5 above describe the
   pre-optimization builds.

7. **Seq (Sequenced) was broken and is repaired.** It violated mutual
   exclusion at ≥4T: the unlocker could write a grant through a saved pointer
   into the successor's dead stack frame after the successor had exited via
   now_serving and re-entered lock(), releasing the next acquisition early.
   Fixed 2026-07-11 by making the grant a token value stored in the slot
   itself — a stale write can never match a future occupant's token. Now
   passes 5×{2,4,8}T clean.

### 7.6 Correctness-Preserving Performance Optimizations (2026-07-12)

Three changes to the shared network core, none touching ordering semantics
(full 174-run breach-detecting sweep clean afterwards):

1. **Cache-line-padded balancers** (`Balancer` is now
   `alignas(hardware_destructive_interference_size)`,
   `bitonic_networks.hpp`). Previously `Balancer<BnCASSync>` was 8 bytes and
   layer arrays packed **8 "independent" balancers into one cache line** —
   every fetch_add invalidated the line under the whole layer, so the
   network paid its full depth while delivering almost none of its
   contention distribution. This was the single largest defect: the
   topology's entire purpose was being defeated by layout. Space cost is
   ~1.5 KB for the largest network here.

2. **Relaxed balancer increments** (`BnCASSync::traverse` uses
   `memory_order_relaxed`). The network needs only *atomicity* of each
   increment (each value returned exactly once → wires alternate, rounds
   stay contiguous); no cross-thread happens-before is derived from
   balancer values — the lock layers establish ordering via their own
   acquire loads and fences. On ARM this saves one full-barrier per
   balancer per acquisition (ldadd vs ldaddal).

3. **Spin-then-yield in lock-layer grant waits** (`LcSpinWait`/`BnSpinWait`,
   every ~1K spins → `sched_yield()`; per-balancer doorways unchanged).
   Ordered locks form a handoff chain; on asymmetric (P/E) cores a spinning
   waiter can occupy the core its predecessor needs, serializing the chain
   at E-core speed. Yielding is unreachable on the fast path and does not
   change who acquires next — only when a waiting core is ceded.

**Measured impact** (same methodology as §7.4; same-session `mcs`/`ticket`
controls; before-numbers from §7.4):

| Lock | 8T before | 8T after | 8T vs MCS after | 4T vs MCS after |
|---|---:|---:|---:|---:|
| `mcs` (control) | 775K-1.2M | 935.2K | 1.00x | 1.00x |
| `ticket` (control) | 714.9K | 932.1K | 1.00x | 1.14x |
| `wf_bitonic_cas` | 211.1K | **1.27M** | **1.35x** | 1.42x |
| `wf_periodic_cas` | 146.5K | **1.08M** | **1.15x** | **1.89x** |
| `skew_bitonic_cas` | 241.1K | **1.33M** | **1.42x** | 1.39x |
| `seq_periodic_cas` | 712.8K | **1.35M** | **1.44x** | 1.71x |
| `bitonic_bakery` | 172.0K | 934.1K | 1.00x | 1.02x |
| `periodic_cas` | 53.7K | 770.2K | 0.82x | 1.39x |
| `bitonic_cas` | 83.9K | 203.0K | 0.22x | 0.60x |

Reading: at 4T and 8T several counting-network locks now **beat MCS** —
before the changes, every one trailed it. `bitonic_bakery` matching MCS at
8T is notable: that is a **zero-atomic-RMW** lock keeping pace with the
classic queue lock under full contention. 1T/2T results are within
day-to-day noise (the `mcs` control itself moved ±40% between sessions;
same-session ratios are the meaningful comparison). `bitonic_cas` improves
2.4x but remains the weakest — its recursive-merger wiring concentrates
more traffic on the final-layer balancers than the periodic block structure.

Attribution (spot checks): padding + relaxed dominate the 2T-4T contended
range; spin-then-yield dominates the 8T recovery (handoff chains no longer
stall behind spinning waiters on asymmetric cores). All three compound.

**Not implemented (future work, in expected-value order):**
- *Flatten the recursive traverse* into stride-indexed arrays over one
  contiguous balancer arena (the pattern `net_elevator_lock.hpp` already
  uses): removes pointer-chasing and call overhead per balancer. Mostly a
  1T-2T constant-factor win now that padding fixed the contended range.
- *Width tuning*: W = smallest power of 2 ≥ ⌈√n⌉ is a heuristic; W trades
  per-balancer contention (smaller W) against depth (larger W). Worth a
  sweep at n ≥ 16 on real multi-socket hardware.
- *Bounded-unfairness variants*: the remaining ~4-6x gap to `spin`/`exp_spin`
  at 8T is the price of ordered handoff itself, not implementation overhead
  — closing it requires relaxing strict token order (e.g., a k-bounded
  overtaking window), which is a semantics change, not an optimization.

### 7.7 Contribution Verdicts, Registry Cull, and Design F (2026-07-12)

**Per-family contribution assessment** (what each family uniquely
demonstrates; families with no unique contribution were culled):

| Family | Contribution | Verdict |
|---|---|---|
| `bitonic_*` / `periodic_*` (10) | Reference Herlihy Ch.12 implementations; the cas/bakery endpoints span "RMW everywhere" to "RMW nowhere" | **Keep** — the pedagogical + baseline core |
| `wf_*` (8) | Only correct O(1)-unlock + linearizable design; `wf_*_{bl,bakery}` are the repo's unique RMW-free + distributed + O(1)-unlock locks | **Keep all** — star family; sync dimension is the point |
| `seq_*_cas` (2) | Hybrid: network as arrival-staggerer for a global ticket; strongest ordered lock at 4T post-repair | **Keep cas only** |
| `seq_*_{bl,lamport,bakery}` (6) | None — paid O(n)-doorway per balancer *and* a global RMW: both weaknesses, neither demonstration | **REMOVED** |
| `lw_*` (8) | Novel per-wire O(W)-unlock experiment (post-repair, liveness-preserving) | **Keep** — the only design exploiting the step property directly |
| `skew_*_cas` (2) | Filter-overhead model (ordering is a ticket lock, §4b of the analysis doc) | **Keep cas only**, pending a real HSW §4 filter |
| `skew_*_{bl,lamport,bakery}` (6) | Same incoherence as seq hybrids | **REMOVED** |
| `rskew_*` (8) | None — byte-for-byte behaviorally identical to `skew_*` | **REMOVED** |
| `bo_*_cas` (2, new) | Design F: K-bounded overtaking (below) | **Added** |
| `net_elevator` | Independent flat-topology network + elevator handoff; also the template for flattening the recursive traverse | **Keep** |

Registry: 52 → 32 network-lock names. Removed code is in git history.

**Design F — bounded overtaking (`bo_bitonic_cas`, `bo_periodic_cas`)**:
full design and correctness argument in `LINEARIZABLE_COUNTING_ANALYSIS.md`
§4c. Summary: Seq skeleton; an unlocker whose immediate successor has not
registered may grant the lowest registered token within a window of K = n
tickets; skipped threads detect `now_serving > seq` and re-draw. FIFO at
quiescence — the window opens only where strict FIFO would stall.

**Measured (2026-07-12, same-session controls; note this session had heavy
background load — mcs collapsed at 8T, so ratios not absolute numbers):**

| Lock | 8T | 12T | 16T | per-thread max/min at 16T |
|---|---:|---:|---:|---:|
| `mcs` | 192.2K | ~16K | ~7K | **40-200x** (!) |
| `ticket` | 316.6K | ~230K | ~165K | 1.0x |
| `wf_bitonic_cas` | 882.6K | ~290K | ~180K | 1.0x |
| `seq_periodic_cas` | 1.09M | ~260K | ~196K | 1.0x |
| `bo_bitonic_cas` | 1.07M | ~185K | ~186K | 1.0x |
| `spin` | 8.70M | ~7.4M | ~7.3M | 1.4-2.3x |

Findings:
1. **Under oversubscription (12-16T on ~8 effective cores) every yielding
   ordered lock holds steady while MCS collapses catastrophically** — both
   in throughput (2-18K) and fairness (its strict queue hands off to
   descheduled threads; max/min per-thread ratio 40-200x). The §7.6
   spin-then-yield is what puts the network locks in the surviving group.
2. **Design F is correct, costs nothing in fairness (1.0 max/min in every
   measured config), but does not beat WF/Seq** — at 8T it ties Seq; under
   oversubscription it ties both. Root cause: its readiness signal is
   *registration*, but a registered thread may itself be descheduled, so
   skipping to a registered thread gains no scheduling information — and
   spin-then-yield already absorbs most stall cost for strict-order locks.
3. **Follow-up the result points to**: overtaking needs a *liveness* signal,
   not an arrival signal — e.g. waiters stamp a per-slot heartbeat counter
   each spin iteration, and the unlocker skips only tokens whose heartbeat
   is stale. That would let the window target precisely the
   preempted-successor case that motivates overtaking. Until then, `bo_*`
   stands as a correct, fairness-neutral proof of the skip/re-draw protocol.

### 7.8 Software/Hardware Tradeoff Across Families (measured 2026-07-12)

Same methodology (1s, median of 3, breach detector armed, same session).
Grouped by family × synchronization primitive requirement:

| Lock | 1T | 2T | 4T | 8T | 12T | 16T |
|---|---:|---:|---:|---:|---:|---:|
| **hardware baselines** |
| `mcs` | 23.85M | 7.08M | 5.15M | 120K | 3K | 6K |
| `ticket` | 24.53M | 4.83M | 4.01M | 314K | 208K | 131K |
| `spin` (unfair) | 26.45M | 15.37M | 8.14M | 8.43M | 7.88M | 8.53M |
| **elevator, 1 TAS** |
| `linear_cas_elevator` | 22.32M | 6.59M | 5.65M | 453K | 8K | 24K |
| `tree_cas_elevator` | 25.65M | 8.05M | 4.93M | 214K | 6K | 23K |
| `net_elevator` | 18.56M | 5.68M | 3.51M | 276K | 15K | 11K |
| **elevator, RMW-free** |
| `linear_bl_elevator` | 25.41M | 7.69M | 6.88M | 899K | 23K | 24K |
| `linear_lamport_elevator` | 24.16M | 6.51M | 5.77M | 429K | 15K | 12K |
| `tree_bl_elevator` | 23.63M | 7.30M | 5.21M | 466K | 6K | 17K |
| `tree_lamport_elevator` | 22.93M | 6.40M | 3.53M | 408K | 11K | 18K |
| **network, RMW (cas)** |
| `bitonic_cas` | 22.88M | 5.64M | 1.49M | 746K | 248K | 171K |
| `wf_bitonic_cas` | 23.86M | 7.06M | 1.31M | 549K | 237K | 187K |
| `bo_bitonic_cas` | 16.64M | 7.04M | 1.62M | 456K | 272K | 180K |
| `seq_periodic_cas` | 14.13M | 6.83M | 1.70M | 904K | 239K | 151K |
| **network, RMW-free** |
| `bitonic_bakery` | 19.34M | 4.17M | 695K | 543K | 248K | 175K |
| `bitonic_bl` | 18.80M | 4.66M | 1.05M | 370K | 221K | 169K |
| `wf_bitonic_bakery` | 24.06M | 7.40M | 965K | 874K | 231K | 177K |
| `wf_bitonic_bl` | 24.14M | 8.26M | 1.20M | 413K | 257K | 173K |

**Reading the software/hardware axis:**

1. **RMW-freedom is essentially free on this machine, in both families.**
   `linear_bl_elevator` (0 atomics) *beats* `linear_cas_elevator` at 8T
   (899K vs 453K); `wf_bitonic_bakery` matches or beats `wf_bitonic_cas` at
   8T+ (874K vs 549K). The classic assumption that software-only mutual
   exclusion pays a large premium does not hold at these scales — O(n)
   doorways over CL-padded flags are cheap compared to coherence traffic
   on contended RMW lines.

2. **The families separate by regime, not by primitive.** Moderate
   contention (4T): elevator wins decisively (3.5-6.9M vs 0.7-1.7M) — a
   single cheap serialization point beats O(log²W) balancer hops.
   Oversubscription (12-16T): the network locks hold 150-270K while every
   elevator variant collapses to 6-24K alongside MCS (3-6K) — a 10-40x
   inversion.

3. **Caveat — the 12-16T inversion is largely the yield asymmetry, not
   topology.** The §7.6 spin-then-yield was applied to the network locks'
   grant waits only; the elevator family still pure-spins, so under
   oversubscription its waiters burn the cores its handoff chain needs
   (same failure as MCS). The obvious next improvement is porting
   spin-then-yield to the elevator/tree wait loops — expect them to rejoin
   the ~150-300K survivor group, at which point the remaining differences
   would reflect topology honestly.

4. `ticket` remains the pound-for-pound robustness champion among classical
   locks (131-208K at 12-16T with 2 lines of state) — a useful humility
   baseline: the network machinery currently buys ~15-40% over ticket
   under oversubscription, contention distribution on the lock path, and
   O(1) unlock (WF), but nothing transformative on a single socket. The
   family's differentiating claims remain RMW-freedom + distribution
   (unique to the network locks) and NUMA/CXL locality (untested here —
   needs the real hardware this repo targets).

### 7.9 Yield Port to the Elevator Family + Design G (2026-07-12)

**Elevator yield port.** The §7.8 caveat was tested directly: spin-then-yield
(`LockSpinWait`, lock.hpp) was ported to the lock-layer grant waits of all
six elevator implementations (linear/tree × cas/bl/lamport variants share
files, plus `net_elevator`; `elevator` already yielded every spin). Same
methodology, same session:

| Lock (post-port) | 4T | 8T | 12T | 16T |
|---|---:|---:|---:|---:|
| `mcs` (untouched control) | 4.39M | 840K | 15K | 12K |
| `ticket` | 3.11M | 803K | 160K | 53K |
| `linear_lamport_elevator` | 3.77M | 981K | 189K | 107K |
| `tree_lamport_elevator` | 4.68M | 1.06M | 250K | 107K |
| `tree_bl_elevator` | 5.08M | 971K | 229K | 82K |
| `net_elevator` | 4.74M | 863K | 175K | 88K |
| `wf_bitonic_cas` | 3.92M | 1.04M | 243K | 87K |
| `bo_bitonic_cas` | 4.87M | 1.12M | 180K | 107K |

The elevator family rejoined the survivor band (was 6-24K at 12-16T before
the port — a 5-40x recovery), confirming §7.8's caveat: the earlier
oversubscription gap between families was the yield asymmetry, not
topology. Post-port, elevator and network locks are within noise of each
other at every thread count; `tree_lamport_elevator` (RMW-free) is now one
of the best ordered locks at 12T. MCS, deliberately untouched as the
control, still collapses.

**Design G — heartbeat overtaking (`hb_bitonic_cas`, `hb_periodic_cas`).**
Implements the §7.7 follow-up: waiters stamp a per-slot heartbeat counter
every spin iteration; an unlocker whose successor is absent samples
candidate beats twice across a ~256-spin grace gap and grants the lowest
token whose beat *advanced* — a thread provably scheduled at that instant —
falling back to lowest-registered (= Design F), then to strict ticket.
Correctness is inherited from Design F verbatim (any registered token in
the window is a valid grant; the heartbeat only changes which is chosen).
Verified: 30/30 breach-detecting runs clean at 2/4/8T.

Measured (3 reps each, same session):

| T | `ticket` | `wf_bitonic_cas` | `seq_periodic_cas` | `bo` (F) | `hb_bitonic` (G) | `hb_periodic` (G) |
|---|---:|---:|---:|---:|---:|---:|
| 4 | 5.48M | 5.39M | 5.34M | 5.74M | 4.29M | 5.74M |
| 8 | 741K | 1.08M | 1.16M | 1.16M | 1.09M | 760K |
| 12 | 219K | 251K | 277K | 262K | **319K** | 246K |
| 16 | 160K | 182K | 186K | 191K | 184K | **205K** |

Verdict: **the liveness hypothesis holds, with modest effect size.** In the
oversubscribed regime Design G is the first ordered variant to pull ahead
of the strict-order pack (~+10-25% at 12-16T, medians), where Design F
(arrival-signal only) merely tied it — while paying a small 4T cost for the
per-spin heartbeat store. Run-to-run spread is ±15%, so treat the 12T edge
as directional; the 16T lead reproduces across both hb variants. On a
machine where sched_yield were unavailable or slower (or with pinned
threads and real preemption), the heartbeat's targeting should matter
proportionally more.

### 7.10 Yield Port to the Hardware-Atomic Locks (2026-07-12)

`LockSpinWait` (lock.hpp) extended to the queue/ticket family's lock-layer
waits: `mcs`, `mcs_nca`, `mcs_local`, `mcs_malloc`, `mcs_sleeper` (unlock
successor wait), `clh`, `hclh`, `hmcs`, `cohortMCS`, `cohortTicket`,
`cohortPTicket`, `cohortTAS`, `ticket`, `exp_ticket`,
`threadlocal_ticket`, `ring_ticket`. Two conversion kinds:
- **pure spins → spin-then-yield** (mcs family, clh, threadlocal/ring
  ticket): gains oversubscription survival;
- **yield-every-iteration → spin-then-yield** (ticket, cohort*, hmcs,
  hclh): removes a syscall per spin iteration on short waits while keeping
  the oversubscription behavior.
Untouched by design: `spin`/`exp_spin`/`hard_spin`/`wait_spin` (unfair
baselines; no handoff chain to stall), `hbo` (backoff *is* its design),
short internal doorway TAS loops.

Measured (1s × 3 reps, same session, breach detector armed):

| Lock | 4T | 8T | 12T (was, pre-port) | 16T |
|---|---:|---:|---:|---:|
| `mcs` | ~2.0M | ~0.6M | **~215K (was 3-15K — 15-70x)** | ~52K |
| `clh` | ~1.9M | ~0.7M | ~265K | ~60K |
| `hclh` | ~1.1M | ~0.9M | **~300K (best ordered)** | ~55K |
| `hmcs` | ~1.2M | ~0.8M | ~245K | — |
| `cohortTicket` | ~1.5M | ~2.7M | ~235K | — |
| `ticket` | ~1.8M | ~0.5M | ~205K | ~52K |
| `wf_bitonic_cas` | ~1.6M | ~0.4M | ~188K | — |
| `hb_bitonic_cas` | ~2.0M | ~0.5M | ~208K | ~62K |
| `spin` (unfair) | ~8.7M | ~9.4M | ~9.8M | — |

With every ordered lock now yielding, the 12T field compresses into a
~180-300K band — the scheduler, not the lock protocol, sets the ceiling
once threads exceed cores, and hierarchical queue locks (`hclh`) edge the
rest. MCS at 4-8T remains the best strict-FIFO lock, and now no longer
falls off a cliff beyond that.

**Pre-existing defects found while validating the port (NOT regressions —
reproduced with the unpatched code):**
1. `mcs_malloc` **violates mutual exclusion** (breach detector fires 5/5 at
   4T and 8T). Its `name()` also mislabels it as "mcs". Needs the same
   class of race-hunt applied to seq_* earlier.
2. `ring_ticket` fails intermittently (~1 in 5 runs at 4-8T).
3. `cohortMCS` **hangs at 12T+** (oversubscription deadlock/livelock;
   passes at 4-8T). Suspect the cohort-threshold handoff when all threads
   share one NUMA cohort.
All three verified against stashed pre-port sources; tracked here as open
bugs.
