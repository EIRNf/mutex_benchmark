# Linearizable Counting Networks for Mutual Exclusion: Analysis & Design

## 1. Paper Summaries

### 1.1 Lynch, Shavit, Shvartsman, Touitou (PODC 1996) — "Counting Networks Are Practically Linearizable"

**Core result**: Standard counting networks (bitonic, periodic) are only *quiescently
consistent*, not linearizable. However, under bounded-time execution assumptions
(each balancer access completes within time *s*), the tokens produced are
**approximately linearizable** — the ordering deviates from a linearizable
counter by at most O(depth × concurrency).

**Key insight for locks**: If balancer access time is bounded (reasonable on
non-preemptive or real-time systems), then the successor of a thread holding
token *k* is likely to hold a token within *k ± window*, where
*window = O(depth) = O(log²W)*. This bounds the search for the successor
from O(n) to O(log²W) under favorable conditions.

**Timing assumption**: Each thread's step (balancer access) takes at most *s*
time units. Network processing of a single token takes at most *d* steps (depth).
Under these bounds, the "linearizability window" — the maximum reordering
distance — is O(d · s).

### 1.2 Herlihy, Shavit, Waarts (1996) — "Linearizable Counting Networks"

**Core result**: Constructs counting networks that ARE linearizable (not just
quiescently consistent). The construction modifies standard networks to add
a comparison-based routing mechanism.

**Key construction**: Each "comparable balancer" compares timestamps of arrivals
and routes the earlier arrival to the lower output wire. This serializes
traversals at each balancer, creating a linearization point.

**Lower bound**: Any **nonblocking** linearizable counting network requires
contention Ω(n). This means lock-free linearizable counting must have at least
linear contention — the distributed advantage of counting networks is lost.

**Blocking construction**: O(log²n) depth is achievable with **blocking**
balancers (threads may wait at balancers). This preserves the contention
distribution but sacrifices lock-freedom.

**Implication for locks**: Since we're building locks (inherently blocking),
the Ω(n) lower bound for nonblocking doesn't apply. We CAN have linearizable
counting with O(log²n) depth using blocking balancers. However, the blocking
at each balancer adds latency to the lock path.

---

## 2. The Successor-Lookup Problem

### 2.1 Current State: O(n) Scan

The existing `BitonicCountingLock` and `PeriodicCountingLock` unlock via:

```
grant_next():
  for i in [0, n):                      // O(n) scan
    if thread[i].waiting:
      compute (wire_dist, round, tid) priority
      track minimum
  wake minimum-priority thread
```

This scans ALL n threads to find the one with the best (wire_distance, round,
tid) triple. The priority ordering ensures the step-property-predicted successor
is preferred, but every thread must still be checked.

### 2.2 Goal: O(1) or O(W) Successor Lookup

If the network produces tokens with predictable structure, the unlocker can
compute the successor's (wire, round) pair directly:

```
Given current thread at (wire=w, round=r):
  successor_wire  = (w + 1) % W
  successor_round = r + (w == W-1 ? 1 : 0)
```

This prediction is exact under the step property (quiescence) and approximately
correct under practical linearizability (bounded timing).

### 2.3 Why Linearizability Matters

**Quiescent consistency** (standard counting networks):
- The step property y_i = ⌈(m-i)/W⌉ holds only at quiescent states
- Between quiescent states, tokens may be assigned "out of order"
- Per-pair rounds from different final-layer balancers are INDEPENDENT
- Tokens from `traverse_full()` are unique but NOT contiguous during concurrent access
- The successor at the predicted (wire, round) might not exist yet → scan needed

**Linearizability** (Lynch et al. practical, or Herlihy et al. construction):
- Token ordering is consistent with real-time (or approximately so)
- The successor's token value is close to predicted → bounded window search
- Under practical linearizability: window = O(depth) = O(log²W)
- Under true linearizability: successor is exactly at predicted position

**The critical difference**: Linearizability bounds the REAL-TIME gap between
consecutive tokens. Thread k's successor (token k+1) entered the network at
approximately the same time, so it will register soon. With mere quiescent
consistency, token k+1 could be a thread that entered much later, leaving the
unlocker spinning unpredictably.

---

## 3. Assumptions (Made Explicit)

### A1: Step Property Holds Approximately Under Concurrency
The step property y_i = ⌈(m-i)/W⌉ holds at quiescence. Under concurrent access,
the distribution across wires is approximately even but may deviate by up to
O(depth) tokens. This affects accuracy of the successor prediction.

**Where this breaks**: Under extreme contention, many tokens are "in flight"
simultaneously. The step property violation can be as large as the number of
concurrent traversals (bounded by n).

### A2: Balancer Access Time is Bounded (Practical Linearizability)
Each balancer access (fetch_add, TAS, or software lock) completes within bounded
time *s*. The network depth *d* gives a total traversal time bound of *d·s*.

**Where this breaks**: OS preemption. A thread can be descheduled mid-traversal
for arbitrarily long. This violates the timing bound and invalidates the
practical linearizability window. On preemptive systems, the "window" is
effectively unbounded.

**Mitigation**: The waker protocol handles this — if the predicted successor
hasn't registered, the unlocker sets a waker flag and the successor self-starts
on registration. The lock is still correct; only the O(1) prediction fails.

### A3: Thread Registration After Traversal is Bounded-Time
After completing network traversal, a thread writes its metadata to the per-wire
slot before spinning. The window between traversal completion and registration
is the "registration gap."

**Where this breaks**: Preemption between traversal and registration. Same
mitigation as A2: waker flag handles this.

### A4: Per-Wire Round Capacity is Sufficient
The per-wire slot array has finite capacity (rounds_cap). If more than
rounds_cap tokens land on a single wire before earlier tokens are served,
slots wrap around and corrupt.

**Condition**: rounds_cap ≥ max_concurrent_waiters / width. With n threads and
W = O(√n) width, this is O(√n) slots per wire.

### A5: Per-Pair Rounds Are Independent Across Final-Layer Balancers
The `traverse()` last-balancer value encodes a per-PAIR round, not a global round.
Two final-layer balancers may have arbitrarily different round counts during
concurrent access. This means `(last_val >> 1) * width + wire` produces unique
but NON-CONTIGUOUS tokens.

**Impact**: Direct successor prediction `next_token = my_token + 1` may reference
a token that doesn't exist (gap in the token space). The per-wire sweep fallback
handles this at O(W) cost.

### A6: Truly Linearizable Network Construction Requires Blocking
Herlihy-Shavit-Waarts proved that any nonblocking linearizable counting network
has contention Ω(n). Their blocking construction achieves O(log²n) but adds
waiting at each balancer. This increases lock-path latency.

**Trade-off**: True linearizability gives guaranteed O(1) successor lookup but
at the cost of higher lock-path latency. Practical linearizability gives O(1)
best-case with O(W) fallback, without modifying the network.

---

## 4. Proposed Designs

### Design A: Wire-Indexed Counting Lock (Predictive, O(W) Unlock)

**Architecture**:
- Standard bitonic/periodic network (unchanged)
- Per-wire slot arrays indexed by per-pair round
- Each wire tracks its `serve_round` (next round to serve)
- Non-atomic `next_to_serve` counter protected by lock ownership

**Lock path** (same complexity as current):
1. `(wire, round) = network.traverse(input, tid, &lv); round = lv >> 1`
2. Register at `wire_slots[wire][round % cap]`
3. Waker protocol (same as existing)

**Unlock path** (O(1) best, O(W) worst):
1. Increment `next_to_serve` (non-atomic, protected by lock)
2. Predict: `pred_wire = next_to_serve % width`, `pred_round computed from wire_heads`
3. Check `wire_slots[pred_wire][pred_round % cap]`
4. If hit: wake thread, advance `wire_heads[pred_wire]` → **O(1)**
5. Else: sweep all W wires for minimum `(serve_round, wire)` → **O(W)**
6. If none found: set waker flag

**Space**: O(W × rounds_cap) = O(W × n/W) = O(n) per-wire slots + O(W) wire heads

**Key advantage**: No extra atomic instruction on lock path. Unlock reduces from
O(n) to O(W). Under practical linearizability, prediction hits often → O(1) average.

**Key limitation**: Non-contiguous per-pair rounds break the `next_to_serve` prediction
under high concurrency. Fallback to O(W) sweep. Still much better than O(n).

> **Status: REPAIRED 2026-07-11** (all 8 `lw_*` variants now pass the
> breach-detecting benchmark, 5 reps × {2,4,8} threads, 0 failures). The
> implementation previously hung at every thread count ≥ 2. Root causes as
> found during repair (§8.3 has full detail): (1) the waker-flag self-serve
> path advanced its wire head to `round + 1`, skipping earlier rounds whose
> owners had not yet registered — since the sweep probes only each wire's
> exact head round, those waiters became permanently invisible; (2) waiters
> tried the waker lock once and then spun passively, so a free-floating lock
> token could never reach an invisible waiter. The slot-ring overflow risk
> (Assumption A4 below) was real too and is closed by rounds_cap ≥ 2n.

### Design B: Sequenced Counting Lock (Guaranteed O(1) Unlock)

**Architecture**:
- Standard bitonic/periodic network (for contention distribution)
- ONE global `atomic<size_t> global_seq` (the linearization point)
- Slot array indexed by sequence number
- `now_serving` counter (non-atomic, protected by lock)

**Lock path** (one extra atomic):
1. `network.traverse(input, tid)` — distributes contention across W/2 balancers
2. `seq = global_seq.fetch_add(1)` — THE linearization point. Contiguous.
3. Register at `slots[seq % cap]`
4. If `now_serving == seq`: self-grant
5. Else: spin on local flag

**Unlock path** (guaranteed O(1)):
1. `now_serving++`
2. Check `slots[now_serving % cap]`
3. If occupied: wake → **O(1) always**
4. Else: brief spin, then waker flag (successor hasn't registered yet)

**Space**: O(cap) slots where cap ≥ n (total threads)

**Key advantage**: Guaranteed O(1) unlock. The global_seq provides a true
linearization point — tokens are contiguous and globally ordered.

**Key limitation**: One additional shared cache line (global_seq). Under very high
contention, this becomes a serialization bottleneck. However, the counting network
distributes arrival times, so threads hit global_seq at staggered intervals,
reducing contention on it compared to a pure ticket lock.

> **Status: REPAIRED 2026-07-11** (all 8 `seq_*` variants now pass the
> breach-detecting benchmark, 5 reps × {2,4,8} threads, 0 failures; the
> repaired `seq_periodic_cas` matches MCS at 4T — see
> `ELEVATOR_SET_COMPARISON.md` §7.4). The implementation previously violated
> mutual exclusion at ≥ 4 threads: unlock() saved a pointer into the
> successor's stack (`spin_addr`) and could write the grant through it up to
> 64 spins after publishing `now_serving_` — by which time the successor may
> have exited via the now_serving path, unlocked, and re-entered lock() with
> a new wait flag at the same stack address, so the delayed write released
> the *next* acquisition early. Fixed by replacing the pointer with a
> token-versioned grant field in the slot itself (§8.3): a stale write can
> only equal the registration it observed, never a future occupant's token.

**Comparison to `cn_array_lock`** (removed): Architecturally similar — both use
a global counter for contiguous tokens + slot array for O(1) unlock. The
difference: cn_array used a FLAT (non-recursive) bitonic network where the
last-layer balancer's counter was the global sequence. Design B uses a RECURSIVE
network (deeper, better contention distribution at scale) with a SEPARATE global
counter. The cn_array implementation has been removed from the codebase.

### Design C: Waiting-Filter (WF) — `wf_*`

(Consistent with the "Design C" naming used in `ELEVATOR_SET_COMPARISON.md` §7.)

**Architecture**:
- Standard bitonic/periodic network (unchanged) produces token
  v = (round × width) + wire
- An n-element circular **phase-bit array**, where phase(v) = ⌊v/n⌋ mod 2

**Lock path**: traverse the network to obtain token v, then wait until
`phase_bits[(v−1) % n] == phase(v−1)` — i.e. wait for the immediate
predecessor token to have *unlocked*. Token 0 (and each thread's first
acquisition of a "fresh" slot generation) proceeds immediately.

**Unlock path** (guaranteed O(1)): a single store —
`phase_bits[v % n] = phase(v)` — plus a fence.

Because entry for token v strictly requires token v−1 to have completed its
unlock, critical-section occupancy is fully serialized by construction. The
phase toggle every n tokens prevents slot-generation aliasing; note that under
the forced serialization the aliasing scenario is structurally unreachable
anyway, so the toggle is belt-and-braces rather than load-bearing.

> **Status (verified 2026-07):** all 8 `wf_*` variants pass the
> breach-detecting max-contention benchmark at 2/4/8 threads. This is the only
> linearizable design in this file that is both implemented and correct.

### Design D: True Linearizable Network (Herlihy-Shavit-Waarts Construction)

**Not implemented.** The Ω(n) lower bound for nonblocking linearizable counting
makes this design impractical compared to Design B (which achieves linearization
with one atomic). A blocking construction is possible but adds complexity and
latency at each balancer without clear benefit over Design B.

### 4b. Skew / Reverse-Skew filter locks — `skew_*`, `rskew_*` (what the code actually does)

The doc-comments above `SkewFilterCountingLock` and
`ReverseSkewFilterCountingLock` (`lib/lock/linearizable_counting_lock.hpp`)
describe Herlihy-Shavit-Waarts's skew and reverse-skew filter networks —
comparison-free filter layers that are supposed to produce a linearizable
token ordering directly from topology, with the skew variant admitting
starvation and the reverse-skew variant wait-free.

**The current implementation does not realize that design.** What the code
actually does per `lock()`:

1. Traverse the underlying bitonic/periodic counting network (real contention
   distribution, output kept as a seed).
2. Run the seed through `num_threads − 1` "filter layers," each performing one
   `fetch_add` on a toggle array — real atomic work — mutating a local `row`
   variable.
3. **Discard `row`.** It is never read again.
4. Take a ticket from a global `fetch_add(global_seq_)` and spin until
   `now_serving_` matches. `unlock()` is `now_serving_.fetch_add(1)`.

Steps 3-4 mean ordering is enforced entirely by an ordinary **ticket lock**;
the filter contributes only overhead (n−1 extra atomic RMWs per acquisition).
Consequences:

- `skew_*` and `rskew_*` are **behaviorally identical** — their only code
  difference is whether the discarded `row` is incremented or decremented.
- They are **correct** mutexes (ticket locks trivially are; verified 2026-07
  at 2/4/8 threads), and linearizable via the global ticket, but the
  linearization point is the `fetch_add`, not the filter network — so they do
  not demonstrate the HSW filter constructions their comments cite.
- The toggle array is allocated with plain `new[]`/`delete[]` rather than the
  codebase's `ALLOCATE`/`FREE` (CXL-aware) macros, so under CXL/NUMA builds
  it lives outside the CXL region every other lock in the file uses.

**Backlog options**: either (a) complete the filter so its output actually
determines ordering (implementing HSW §4 for real), or (b) delete the dead
filter loop and rename these to what they are (network-fronted ticket locks),
or (c) keep them as-is for measuring "network + ticket" hybrid overhead —
but the doc-comments in the source should be softened in any case.

---

## 5. Performance Analysis

| Metric               | Current O(n)           | Design A (Wire-Indexed) | Design B (Sequenced)  |
|-----------------------|------------------------|-------------------------|-----------------------|
| Lock atomics          | O(log²W) (network)     | O(log²W) (network)      | O(log²W + 1)          |
| Unlock scan           | O(n)                   | O(1) to O(W)            | O(1) guaranteed       |
| Unlock worst-case     | Θ(n)                   | O(W) + waker            | O(1) + waker          |
| Extra shared state    | None                   | Per-wire slots + heads  | Global seq + slot arr |
| Space                 | O(n) thread meta       | O(n) wire slots         | O(n) token slots      |
| Contention bottleneck | None added             | None added              | global_seq (bounded)  |
| Prediction accuracy   | N/A                    | High under step prop    | Perfect (linearizable)|

### 5.1 When to Use Which

All four options below are implemented and verified correct as of 2026-07-11
(Designs A and B after the §8.3 repairs):

- **Design A**: when lock-path contention must be minimized (no extra
  atomic), moderate contention where the step property holds well. Note the
  repaired implementation is liveness-preserving but not starvation-free
  under adversarial scheduling (§8.3).

- **Design B**: when unlock latency must be guaranteed O(1). The extra
  fetch_add on the lock path is a small price for deterministic unlock —
  and measured post-repair it is the strongest ordered network lock at 4T.

- **Design C (WF)**: O(1) unlock + linearizable ordering with **zero** extra
  atomics — the choice when the RMW-free property matters.

- **Current O(n)**: when n is small (≤16) or when W ≈ n (so O(W) ≈ O(n) anyway).
  The simpler code may be faster for small thread counts due to lower constant factors.

### 5.2 The Step Property Under Concurrency

The step property guarantees even distribution at quiescence. Under k concurrent
traversals, the wire distribution can differ by at most k from the step property
prediction. Since the network has depth d = O(log²W), and at most d threads can
be simultaneously inside the network, the maximum deviation is O(d) = O(log²W).

This means Design A's prediction fails at most O(log²W) times per "epoch" of W
tokens, giving amortized O(1 + log²W/W) per unlock — essentially O(1) for large W.

### 5.3 Preemption Sensitivity

As designed, both A and B were meant to tolerate preemption:
- If the predicted successor hasn't registered (preempted mid-traversal):
  - Design A: falls back to wire sweep, then waker flag
  - Design B: bounded handoff spin (`HANDOFF_SPINS`), after which the
    successor's own spin loop self-resolves once `now_serving_` catches up
    (the implementation has no waker flag — only Design A has waker machinery)

Post-repair (2026-07-11), the implementations realize this intent: Design A's
SERVED-breadcrumb sweep plus waker-flag polling handles a preempted
registrant without losing anyone (§8.3), and Design B's now_serving escape
path is now race-free (token-versioned handoff). Preemption degrades
performance, not correctness, in all four implemented designs.

---

## 6. Composable Unlock Schedules

The user's hypothesis about "composable unlock schedules" from linearizability
windows has a precise technical interpretation:

**Schedule**: The sequence of (wire, round) pairs in which threads are granted
the lock. The step property defines a "natural" schedule: wire 0 round 0,
wire 1 round 0, ..., wire W-1 round 0, wire 0 round 1, ...

**Composability**: If two counting networks (e.g., bitonic and periodic) produce
tokens with the same step property, their unlock schedules are interchangeable.
The per-wire slot infrastructure works regardless of which network produced the
(wire, round) assignment. This enables:

1. **Network-agnostic unlock**: The unlock logic is independent of whether the
   network is bitonic, periodic, or any other step-property-satisfying network.

2. **Modular design**: Swap networks without changing the unlock mechanism.
   Different networks may perform better under different contention patterns,
   and the per-wire unlock handles all of them.

3. **Window-based scheduling**: Under practical linearizability, the unlock
   examines a window of O(depth) candidates. Different networks have different
   depths (bitonic: O(log²W), periodic: O(log²W)), giving different window
   sizes and thus different scheduling precision/cost trade-offs.

---

## 7. Summary of Critical Findings

1. **Standard counting networks are NOT linearizable** — even with linearizable
   (CAS/fetch_add) balancers. The network composition only guarantees quiescent
   consistency. Per-pair rounds from different final-layer balancers are
   independent, producing unique but non-contiguous tokens.

2. **Practical linearizability** (Lynch et al.) bounds the non-contiguity under
   timing assumptions. The window is O(depth × concurrency). This enables
   predicted successor lookup with bounded fallback.

3. **True linearizability** (Herlihy et al.) requires Ω(n) contention for
   nonblocking, or O(log²n) with blocking. A single global fetch_add achieves
   the same linearization more efficiently (Design B).

4. **Per-wire indexing** (Design A) reduces the unlock scan from O(n) threads to
   O(W) wires without any extra atomic. This is the most novel contribution —
   on paper. The current implementation is broken (hangs; see §4 status note),
   so the idea remains unvalidated empirically.

5. **The step property IS the key** — it enables wire-based organization and
   successor prediction. Linearizability refines the prediction accuracy but
   the step property alone provides the O(n) → O(W) reduction.

6. **OS preemption** is the practical enemy of all these optimizations. The waker
   protocol is essential for correctness under arbitrary scheduling.

7. **Implementation status (re-verified 2026-07-11 after repair, breach-detecting
   benchmark, 5 reps × {2,4,8} threads)**: Design C (WF) — correct, all 8
   variants. Design A (LW) — **repaired** (previously hung at ≥2T; see §8.3
   for the two root causes and the fix). Design B (Seq) — **repaired**
   (previously violated mutual exclusion at ≥4T; see §8.3). Skew/RSkew —
   correct, but behaviorally ticket locks (§4b); they do not exercise the HSW
   filter constructions they cite.

---

## 8. Formal Design Summary — Counting-Network Locks in Context

This section states precisely what each lock family guarantees, under what
model, and where it sits relative to the classical locks in this repo
(`spin`, `ticket`, `mcs`, the elevator family). "Verified" below means the
breach-detecting max-contention benchmark (2026-07, Apple M-series, 2/4/8
threads, 5 reps); "argued" means a code-level invariant argument, not a proof.

### 8.1 Model

- n asynchronous threads, ids 0..n−1, arbitrary preemption (no timing bounds).
- Shared memory with `volatile` accesses ordered by explicit `Fence()`
  (DMB ISH on ARM, locked add on x86). CAS-family locks additionally use
  `fetch_add`/`exchange`/`compare_exchange` RMWs.
- A *token* (ticket) is a value drawn from some counting mechanism; a lock is
  built by serving tokens in an agreed order.

The families differ only in **how the token is drawn** and **how the next
token holder is found at unlock** — this two-axis view is the formalization:

| Axis 1: token source | Contention profile | Token properties |
|---|---|---|
| Single RMW (`ticket`, `mcs` tail, Seq's `global_seq_`) | All n threads hit one cache line | Contiguous, linearizable |
| Counting network (`bitonic_*`, `periodic_*`, WF, LW) | Spread over Θ(w·log²w) balancers, ≤ ⌈n/W⌉ threads each (expected) | Unique; **quiescently consistent only** — non-contiguous under concurrency (AHS Lemma 2.2 holds only at quiescence) |
| None (`spin`) | One cache line, unordered | No tokens — no fairness |

| Axis 2: successor lookup at unlock | Cost | Used by |
|---|---|---|
| None (release a flag) | O(1) | `spin` |
| Increment a serving counter | O(1) | `ticket`, Seq, Skew/RSkew |
| Follow a queue link | O(1) | `mcs` |
| Scan all thread metadata | O(n) | base `bitonic_*`/`periodic_*`, elevator family |
| Per-wire heads + prediction | O(1) hit / O(W) miss | LW |
| Predecessor phase bit (no lookup at all) | O(1) | WF |

The central obstruction — and the reason Designs A/B/C exist — is that
counting-network tokens are **not contiguous**: a thread cannot spin on
"serving == my_token" because token values can have transient gaps
(LINEARIZABLE_COUNTING_ANALYSIS §2.3/A5). Each design closes the gap
differently: LW re-sorts tokens per wire; Seq abandons network ordering and
draws a second, contiguous token; WF exploits that network tokens on the
*same* wire are contiguous per-wire and chains predecessors across an
n-slot filter.

### 8.2 Properties per family

| Lock | Mutual exclusion | Deadlock-freedom | Starvation-freedom | Fairness | lock() cost | unlock() cost | RMW-free? |
|---|---|---|---|---|---|---|---|
| `spin` | verified | argued (TAS) | **no** | none | O(1) amortized | O(1) | no |
| `ticket` | verified | argued | argued | FIFO | 1 RMW + spin | O(1) | no |
| `mcs` | verified | argued | argued | FIFO | 2 RMW + local spin | O(1) | no |
| `linear/tree_*_elevator` | verified | argued | argued (cyclic sweep) | ≈FIFO / tree-order | O(n) / O(log n) | O(n) / O(log n) | **BL/Lamport variants yes** |
| `bitonic_*`, `periodic_*` | verified | argued (waker-flag token conservation) | argued (round-robin wire advance) | bounded skew (≤ W−1 grants) | O(log²W × T_sync) | O(n) | **bl/lamport/bakery yes** |
| WF (`wf_*`) | verified | argued (predecessor chain is total) | argued | linearizable token order | O(log²W × T_sync) + chain wait | **O(1)** | **bl/lamport/bakery yes** |
| Seq (`seq_*`, repaired) | verified | argued (now_serving escape) | argued | linearizable (global RMW) | O(log²W × T_sync) + 1 RMW | **O(1)** + bounded handoff | no (global_seq_ is RMW) |
| LW (`lw_*`, repaired) | verified | argued (§8.3 token conservation) | **argued only under fair scheduling** — flag polling is competitive, not queued | ≈ step-property order, unbounded skew possible | O(log²W × T_sync) | O(W) + SERVED cleanup | **bl/lamport/bakery yes** |
| Skew/RSkew | verified | argued (ticket) | argued (ticket) | FIFO (ticket order, *not* filter order) | O(log²W) + (n−1)+1 RMW | O(1) | no |

T_sync = per-balancer sync cost: O(1) for cas, O(n) doorway for bl/bakery,
O(1)..O(n) for lamport (see BITONIC_NETWORKS_COMPLEXITY.md).

### 8.3 The two repaired implementations (2026-07-11)

**Design B (Seq) — was: mutual-exclusion violation.** The unlocker saved a
pointer to the successor's *stack* (`spin_addr`) and could write through it
up to 64 spins after publishing `now_serving_`. The successor could exit via
the `now_serving_` path, unlock, and re-enter `lock()` reusing the same stack
address for its next wait flag; the delayed grant then released the *next*
acquisition early — two holders. **Fix**: the grant is now a token value
written into the slot itself (`granted_token = seq`); a stale write can only
ever equal the registration it observed and can never match a future
occupant of that slot (tokens sharing a slot differ by num_slots ≥ 4n while
unserved tokens span ≤ n). Verified: 0 breaches/hangs in 5×{2,4,8}T.

**Design A (LW) — was: hang at every thread count ≥ 2.** Two cooperating
defects: (1) a thread that claimed the free lock through the waker flag
advanced its wire's head to `round+1`, skipping earlier *not-yet-registered*
rounds on that wire; since the sweep probes only the exact head round of each
wire, those waiters became permanently invisible. (2) A waiter that failed
`trylock` once fell into a passive spin and never re-examined the waker flag
— so the "free-floating lock" token could never reach an invisible waiter.
**Fix**: (1) self-served threads now leave a SERVED breadcrumb in their slot
instead of advancing the head; the holder's sweep consumes breadcrumbs in
round order (`catch_up_head`), so heads advance past completed rounds without
ever skipping a pending one. (2) All waiters poll the waker flag in their
spin loop (re-attempting `trylock` whenever it is set). (3) `rounds_cap_`
raised from `4n/W + 4` to `≥ 2n` so a wire's slot ring can never overflow
(unserved tokens ≤ n). Invariant restored: **token conservation** — at every
instant exactly one of {a holder exists, a grant is in flight, `waker_flag_`
is set} holds, and every waiter either becomes visible to a sweep (its round
is reached by `catch_up_head`) or claims the flag itself. Verified: 0
breaches/hangs in 5×{2,4,8}T.

Cost of the repairs: Seq's unlock is unchanged (O(1) + bounded handoff
spins). LW's unlock gains amortized-O(1) breadcrumb cleanup on top of the
O(W) sweep; LW's starvation story is now *liveness-preserving but not
starvation-free* — an unlucky waiter can lose the flag race repeatedly under
adversarial scheduling (same class of guarantee as the original design's
waker protocol, made explicit).

### 8.4 Are the counting-network locks achieving their design goals?

**Goal 1 — distribute lock-path contention (AHS): partially achieved.**
The network genuinely spreads RMW traffic across balancers, and at 2T the
best network lock (`wf_bitonic_cas`, 1.83× MCS) shows the benefit. But at
4T/8T on this machine every network lock trails MCS: the network's extra
depth costs more than the contention it removes, and (for base locks) the
O(n) unlock scan dominates. The AHS use-case — shared *counters* with no
unlock/handoff phase — distributes better than mutual exclusion does,
because a mutex re-serializes at handoff no matter how distributed the
arrival is. This is inherent, not an implementation defect.

**Goal 2 — O(1) unlock with linearizable ordering (HSW): achieved by WF.**
`wf_*` is correct, O(1)-unlock by construction, and the best-performing
ordered network lock measured. Seq achieves the same guarantee after repair,
at the price of one global RMW (its `global_seq_` is the very serialization
point counting networks were invented to avoid — it is a *hybrid*, not a
pure counting-network lock).

**Goal 3 — RMW-free mutual exclusion with distributed contention: achieved,
uniquely.** `bitonic_bl/bakery`, `periodic_bl/bakery`, `wf_*_bl/bakery` are
the only locks in the repo that combine (a) no atomic RMW instructions at
all, (b) contention spread over many cache lines, and (c) verified
correctness. No classical lock here offers this combination (`ticket`/`mcs`
need RMW; the elevator BL variants are RMW-free but single-point). This is
the strongest claim the counting-network locks can make, and it survives
scrutiny — with the caveat that their absolute throughput at 8T is 1-2
orders of magnitude below MCS.

**Goal 4 — filter-network linearization (HSW §4, Skew/RSkew): not achieved.**
The filter is an overhead model; ordering is a ticket lock (§4b). Open work.
