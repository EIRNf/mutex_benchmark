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
§ Seq (`seq_*`) designs are broken: they probabilistically **violate mutual
exclusion** at ≥4 threads (verified 2026-07 with a breach-detecting critical
section). LW (`lw_*`) designs hang at ≥2 threads. Neither should be benchmarked.

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

(`seq_*` was previously recommended for "O(1) unlock + single bottleneck" —
removed: the implementation violates mutual exclusion at ≥4 threads, see §7.4.
A plain ticket lock — or `skew_*`, which is behaviorally a network-fronted
ticket lock — covers that niche until `seq_*` is fixed.)

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

| Design | Lock O() | Unlock O() | Extra Atomics | Key Mechanism | Source | Status (verified 2026-07) |
|---|---|---|---|---|---|---|
| **A: Wire-Indexed (LW)** | O(log²W) | O(1) pred / O(W) fallback | 0 | Per-wire slot arrays + step-property prediction | Novel | **BROKEN — hangs at ≥2T** |
| **B: Sequenced (Seq)** | O(log²W) + 1 fetch\_add | O(1) guaranteed | 1 fetch\_add (global\_seq) | Counting network + global ticket + slot array | Aspnes et al. §5.1 | **BROKEN — mutual-exclusion breach at ≥4T** |
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
`rskew_*` variant plus `net_elevator` and `hmcs` passed with no breach; all 8
`lw_*` variants hung; all 8 `seq_*` variants breached mutual exclusion and/or
hung at ≥4T. `seq_*` and `lw_*` are therefore excluded from the throughput
table — a lock that admits two threads at once produces meaningless numbers.

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
> half-right: `seq_*` runs fast *because* it sometimes admits two threads at
> once. Those measurements predate the breach detector and have been removed.

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

6. **LW (Wire-Indexed) is broken** — hangs at ≥2T, confirmed empirically. The
   most defensible root cause from code audit is the per-wire slot ring
   overwrite losing a delayed waiter's wakeup pointer (Assumption A4 in
   `LINEARIZABLE_COUNTING_ANALYSIS.md`), not the waker protocol per se. Needs
   an occupancy guard or redesign before it can be benchmarked.

7. **Seq (Sequenced) is broken — it violates mutual exclusion** at ≥4T
   (probabilistically; reproduced 2-3 times out of 5 runs at 4T). The design
   is sound on paper; the implementation's handoff/slot-reuse path has a race.
   Until fixed, any Seq throughput numbers are meaningless.
