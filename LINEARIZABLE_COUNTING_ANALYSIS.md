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

> **Status (verified 2026-07):** Design A is **broken** under multi-thread
> contention — all 8 `lw_*` variants hang (no progress within timeout) at
> every thread count ≥ 2 on the max-contention benchmark. Static analysis of
> the waker protocol itself did not turn up a deadlock; the most defensible
> root-cause candidate is the **per-wire slot ring overflow** (Assumption A4
> below made real): registration overwrites `wire_slots[wire][round % cap]`
> unconditionally with no occupancy check, so when a wire accumulates more
> than `rounds_cap_` outstanding rounds (a delayed thread repeatedly losing
> the doorway while others keep landing on its wire), a newer registration
> silently clobbers the earlier waiter's `spin_addr`, permanently losing its
> wakeup. The `lw_*` type aliases exist in code but must not be benchmarked.

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

> **Status (verified 2026-07):** Design B is **broken**, but not the way
> previously described (it has no designated-waker protocol to deadlock — that
> machinery only exists in Design A). Under a breach-detecting critical
> section, all 8 `seq_*` variants probabilistically **violate mutual
> exclusion** at ≥ 4 threads (reproduced: 2-3 breaches out of 5 runs at 4T for
> `seq_bitonic_cas`/`seq_periodic_cas`, plus occasional hangs). Any historical
> throughput numbers for `seq_*` predate the breach detector and are invalid —
> a lock that admits two threads at once posts excellent but meaningless
> throughput. The design itself (network + global fetch_add + slot handoff)
> remains sound on paper; the defect is in the implementation's
> handoff/slot-reuse path and needs a race-hunt before `seq_*` can be
> benchmarked.

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

The table above describes the designs *as designed*. As implemented (see the
status notes in §4), only Design C (WF) and the base O(n) locks are usable:

- **Design A** (*theory*): when lock-path contention must be minimized (no
  extra atomic), moderate contention where the step property holds well.
  *Implementation currently hangs — do not use.*

- **Design B** (*theory*): when unlock latency must be guaranteed O(1). The
  extra fetch_add on the lock path is a small price for deterministic unlock.
  *Implementation currently violates mutual exclusion — do not use.*

- **Design C (WF)**: the working choice for O(1) unlock + linearizable
  ordering today.

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

In practice this section describes intent, not verified behavior: Design A's
implementation hangs under contention (the slot-ring overwrite defeats the
fallback — see §4), and Design B's implementation has a mutual-exclusion race.
Only the base O(n) locks and Design C currently degrade gracefully as described.

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

7. **Implementation status (verified 2026-07, breach-detecting benchmark at
   2/4/8 threads)**: Design C (WF) — correct, all 8 variants. Design A (LW) —
   hangs at ≥2T. Design B (Seq) — violates mutual exclusion at ≥4T.
   Skew/RSkew — correct, but behaviorally ticket locks (§4b); they do not
   exercise the HSW filter constructions they cite.
