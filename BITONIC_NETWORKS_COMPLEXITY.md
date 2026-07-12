# Bitonic Counting Networks — Complexity & Reference

Implementation of Herlihy's bitonic counting networks (Ch. 12, "The Art of
Multiprocessor Programming") with support for differing hardware ISA memory
models and synchronisation primitives.

---

## Algorithmic Complexity Table

All networks have width **w** (must be a power of 2).  
**n** = number of threads, **w** = network width.

| Component | Depth (balancers traversed) | Total Balancers | Width | Traverse Cost per Token |
|---|---|---|---|---|
| **Balancer** | 1 | 1 | 2 | O(1) |
| **Merger[2k]** | O(log 2k) = O(log w) | O(k · log w) | 2k | O(log w) |
| **Bitonic[w]** | O(log²w) | O(w/2 · log²w) | w | O(log²w) |
| **Block[w]** | O(log w) | O(w/2 · log w) | w | O(log w) |
| **Periodic[w]** | O(log²w) | O(w/2 · log²w) | w | O(log²w) |

### Per-Balancer Synchronisation Overhead

| Sync Policy | Per-traverse Cost | RMW Atomics Required? | Space per Balancer |
|---|---|---|---|
| **CAS** (fetch_add) | O(1) amortised | Yes (LOCK prefix / LDXR-STXR) | O(1) — single atomic counter |
| **Burns-Lamport** | O(n) doorway + O(n) wait | No — loads, stores, fences only | O(n) — per-thread flags |
| **Lamport Fast** | O(1) fast path, O(n) slow | No — loads, stores, fences only | O(n) — per-thread arrays |
| **Elevator (Buhr)** | O(1) enqueue + local spin | 1 XCHG (enqueue) + 1 CAS (dequeue) | O(n) — per-thread queue nodes |
| **Bakery** | O(n) doorway + O(n) wait | No — pure loads & stores (SC) | O(n) — choosing[] + number[] |

### End-to-End Lock Complexity

| Lock Variant | Network Traverse | Lock (total) | Unlock | Space |
|---|---|---|---|---|
| **bitonic_cas / periodic_cas** | O(log²w) × O(1) | O(log²w + n) | O(n) scan | O(w · log²w + n) |
| **bitonic_{bl,lamport,elevator,bakery}** | O(log²w) × T_sync | O(log²w × T_sync + n) | O(n) scan | O(w · log²w · n) |
| **periodic_{bl,lamport,elevator,bakery}** | O(log²w) × T_sync | O(log²w × T_sync + n) | O(n) scan | O(w · log²w · n) |
| **wf_bitonic_{sync}** | O(log²w) × T_sync | O(log²w × T_sync) + O(1) phase check | **O(1)** phase-bit set | O(n·CL) + network |
| **wf_periodic_{sync}** | O(log²w) × T_sync | O(log²w × T_sync) + O(1) phase check | **O(1)** phase-bit set | O(n·CL) + network |
| **seq_bitonic_cas** | O(log²w) × O(1) | O(log²w) + 1 fetch_add | **O(1)** slot lookup | O(n·CL) + network |
| **seq_periodic_cas** | O(log²w) × O(1) | O(log²w) + 1 fetch_add | **O(1)** slot lookup | O(n·CL) + network |
| **skew_* / rskew_*** | O(log²w) + (n−1) fetch_add | O(log²w + n) + ticket spin | **O(1)** fetch_add | O(n·w) toggles + network |

Where **T_sync** is the per-balancer synchronisation cost from the table above.
The space term for the non-CAS software-sync variants is O(w·log²w·n), not
O(w·log²w + n): every one of the Θ(w·log²w) balancers carries its own O(n)
per-thread state (flags/arrays/queue nodes), so the per-balancer O(n) factor
multiplies the balancer count.

**Verification status (measured 2026-07, Apple M-series, breach-detecting
critical section at 2/4/8 threads — see `ELEVATOR_SET_COMPARISON.md` §7.4):**
- All 10 base `bitonic_*`/`periodic_*` variants: **correct** at all thread counts.
- All 8 `wf_*` (Design C, Waiting-Filter): **correct** at all thread counts.
- All 16 `skew_*`/`rskew_*`: **correct** (but see the note in
  `LINEARIZABLE_COUNTING_ANALYSIS.md` §4b — the skew filter is vestigial and
  ordering is enforced by an embedded ticket lock).
- All 8 `lw_*` (Design A, Wire-Indexed): **repaired 2026-07-11** (previously
  hung at every thread count ≥ 2); now correct, 5 reps × {2,4,8}T clean. See
  `LINEARIZABLE_COUNTING_ANALYSIS.md` §8.3 for the root causes and fix.
- All 8 `seq_*` (Design B, Sequenced): **repaired 2026-07-11** (previously
  violated mutual exclusion at ≥ 4 threads via a stale stack-pointer grant);
  now correct, 5 reps × {2,4,8}T clean, and competitive — `seq_periodic_cas`
  matches MCS at 4T. See `LINEARIZABLE_COUNTING_ANALYSIS.md` §8.3.

---

## Network Implementations

### 1. Balancer (Herlihy Fig 12.10, 12.14)

**What**: A toggle switch with two input and two output wires.  Tokens
alternate between top (wire 0) and bottom (wire 1) outputs.

**Correctness**: In any quiescent state, |y₀ - y₁| ≤ 1.

**Application**: The atomic building block.  Any counting, sorting, or
load-balancing network is composed of balancers.

**Implementation**: `Balancer<Sync>` in `bitonic_networks.hpp`.  The
`traverse(tid)` method is the critical section — it must be serialised.

---

### 2. Merger[2k] (Herlihy Fig 12.12, 12.15)

**What**: Merges two width-k step-property sequences into one width-2k
step-property sequence.

**Construction** (recursive):
- Base (k=1): single balancer.
- Recursive: two Merger[k] sub-networks merge interleaved subsequences,
  then k balancers combine output pairs.

**Correctness**: Lemma 12.5.6 — if inputs have the step property, outputs
do too.

**Application**: Core correctness component of the bitonic network.  Also
used in bitonic sorting networks and distributed merge operations.

---

### 3. Bitonic[w] (Herlihy Fig 12.13, 12.16, Theorem 12.5.1)

**What**: Full counting network.  Two half-width Bitonic networks feed
into a full-width Merger.

**Construction**: Bitonic[2k] = Bitonic[k] ∘ Bitonic[k] → Merger[2k]
Base: Bitonic[2] = single balancer.

**Step Property**: In any quiescent state with m total tokens,
yᵢ = ⌈(m-i)/w⌉.  Tokens distribute cyclically: 0, 1, …, w-1, 0, 1, …

**Application**:
- **Distributed counters** (Fig 12.9): w output wires each maintain a
  local counter.  Thread gets unique index = wire + round × w.
- **Memory pool allocators**: disjoint slot sets per wire.
- **Work-stealing schedulers**: balanced task distribution.
- **Barrier synchronisation**: distributed wakeup traffic.

---

### 4. Block[w] (Herlihy Fig 12.19)

**What**: Building block for periodic networks.  Splits input into top/bottom
halves, routes through sub-blocks, combines with final balancers.

**Construction**: Block[2k] = Block[k] (top) + Block[k] (bottom) + k
final balancers.  Base: Block[2] = single balancer.

**Application**:
- **Hardware (FPGA/ASIC)**: Regular structure minimises routing complexity.
- **Pipeline stages**: Each block is one pipeline stage for overlapped
  token processing.
- **Fault tolerance**: Identical blocks can be hot-swapped.

---

### 5. Periodic[w] (Herlihy Fig 12.18)

**What**: A counting network made of log₂(w) identical Block[w] stages
connected in sequence.  Every stage is structurally identical — the
network is *periodic*.

**Construction**: Periodic[w] = Block[w]^(log₂ w) — log₂(w) blocks, each of
depth log₂(w), giving the classic log²(w) total depth (Dowd-Perl-Rudolph-Saks,
as presented in Aspnes-Herlihy-Shavit §4). This matches the implementation
(`PeriodicNetwork::build()` computes `num_blocks = log2(width)`).

**Step Property**: Same as Bitonic[w] — achieved through iterative
refinement across repeated blocks.

**Application**:
- **VLSI layouts**: Periodicity maps to regular silicon floor plans.
- **Pipeline-parallel counters**: Each block is a pipeline stage.
- **Streaming systems**: Uniform timing analysis across stages.
- **Self-similar fault tolerance**: Any block is replaceable.

---

## Hardware Abstraction & ISA Support

The key requirement from Herlihy: **the traverse operation on each balancer
must be serialised**.  This can be achieved through:

### A. CAS / RMW Atomics (lock-free)
- **ISAs**: x86 (LOCK prefix), ARM (LDXR/STXR), RISC-V (LR/SC)
- **Mechanism**: `atomic::fetch_add` on the balancer counter
- **Trade-off**: Best throughput, requires hardware RMW support
- **Lock names**: `bitonic_cas`, `periodic_cas`

### B. Software Mutex — Burns-Lamport
- **ISAs**: Any with loads, stores, and memory fences (x86, ARM, RISC-V, SPARC, MIPS)
- **Mechanism**: Per-balancer Burns-Lamport mutual exclusion (N-thread safe)
- **Trade-off**: No RMW needed, but O(n) doorway per balancer access
- **Lock names**: `bitonic_bl`, `periodic_bl`
- **Note (fixed 2026-07-11)**: `BurnsLamportMutex::trylock()`
  (`burns_lamport_lock.hpp`) was missing two `Fence()` calls that its
  hardened sibling `BnWakerLock::trylock()` (`bitonic_networks.hpp`) carries
  — between the doorway scan and the fast-flag RMW, and before publishing
  the doorway exit. On a weakly-ordered CPU this permitted two leaders
  (a plausible, never-observed race). Both fences are now present in both
  copies.

### C. Software Mutex — Lamport Fast Lock
- **ISAs**: Same as Burns-Lamport
- **Mechanism**: Per-balancer Lamport fast lock (O(1) uncontended fast path)
- **Trade-off**: Good for low contention; degrades to O(n) under contention
- **Lock names**: `bitonic_lamport`, `periodic_lamport`

### D. Elevator Lock (Buhr, Dice, Scherer 2005)
- **ISAs**: Requires single CAS for enqueue (can be replaced with LL/SC)
- **Mechanism**: MCS-style queue lock per balancer, local spinning
- **Trade-off**: Ideal for NUMA/CXL — each thread spins on its own node,
  only the handoff write crosses NUMA domains.  Sequential handoff
  ("elevator" property) provides FIFO ordering.
- **Lock names**: `bitonic_elevator`, `periodic_elevator`

### E. Bakery Algorithm (Lamport, no atomics)
- **ISAs**: Any with sequential consistency (or fence-augmented TSO/ARM)
- **Mechanism**: Lamport's bakery algorithm per balancer — pure loads & stores
- **Trade-off**: Most portable, but O(n) space and time per balancer access.
  Suitable for research/embedded ISAs without any atomic RMW.
- **Lock names**: `bitonic_bakery`, `periodic_bakery`

---

## Memory Model Considerations

| ISA | Memory Model | Recommended Sync | Notes |
|---|---|---|---|
| x86-64 | TSO (strong) | CAS or Burns-Lamport | `LOCK` prefix for CAS; stores are ordered |
| ARM (v8+) | Weakly ordered | CAS or Lamport | Requires `DMB ISH` fences; `LDXR/STXR` for CAS |
| RISC-V | RVWMO (weak) | CAS or Bakery | `LR/SC` for CAS; `fence` for software locks |
| SPARC (TSO mode) | TSO | Burns-Lamport | Similar to x86; `MEMBAR` for fences |
| MIPS | Weakly ordered | Bakery or Lamport | `LL/SC` available on most; `SYNC` for fences |
| Embedded (no RMW) | Varies | Bakery | Pure load/store algorithms only |

---

## File Map

| File | Contents |
|---|---|
| `lib/lock/bitonic_networks.hpp` | All network + lock implementations (base counting locks) |
| `lib/lock/linearizable_counting_lock.hpp` | Linearizable counting lock designs (WF, Seq, LW, Skew, RSkew) |
| `lib/lock/net_elevator_lock.hpp` | `net_elevator` — elevator lock with a hand-rolled bitonic-topology network assigning "floors" (CAS-only balancers) |
| `lib/lock/HMCS_lock.cpp` | `hmcs` — hierarchical MCS lock (unrelated to counting networks, listed for registry completeness) |
| `lib/utils/bench_utils.cpp` | Factory registration (both CXL and standard paths) |
| `BITONIC_NETWORKS_COMPLEXITY.md` | This document |
| `LINEARIZABLE_COUNTING_ANALYSIS.md` | Theoretical analysis of linearizable counting lock designs |
| `ELEVATOR_SET_COMPARISON.md` | Cross-lock overhead comparison + measured results |

## Lock Name Registry

All names below are registered in `lib/utils/bench_utils.cpp`. Every family is
the full cross product {bitonic, periodic} × {cas, bl, lamport, bakery} (the
base family additionally has an `elevator` sync variant).

Base counting locks (bitonic_networks.hpp) — all correct:
```
bitonic_cas        bitonic_bl         bitonic_lamport
bitonic_elevator   bitonic_bakery
periodic_cas       periodic_bl        periodic_lamport
periodic_elevator  periodic_bakery
```

Linearizable counting locks (linearizable_counting_lock.hpp):
```
wf_*   (correct):  wf_bitonic_{cas,bl,lamport,bakery}    wf_periodic_{cas,bl,lamport,bakery}
seq_*  (correct — repaired 2026-07-11):
                   seq_bitonic_{cas,bl,lamport,bakery}   seq_periodic_{cas,bl,lamport,bakery}
lw_*   (correct — repaired 2026-07-11):
                   lw_bitonic_{cas,bl,lamport,bakery}    lw_periodic_{cas,bl,lamport,bakery}
skew_* / rskew_* (correct; behaviorally ticket locks — see LINEARIZABLE_COUNTING_ANALYSIS.md §4b):
                   skew_bitonic_{cas,bl,lamport,bakery}  skew_periodic_{cas,bl,lamport,bakery}
                   rskew_bitonic_{cas,bl,lamport,bakery} rskew_periodic_{cas,bl,lamport,bakery}
```

Related (own files, see File Map): `net_elevator`, `hmcs`.
