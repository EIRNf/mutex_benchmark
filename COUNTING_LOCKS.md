# Counting-Lock Family — implementation guide, measured comparison, redundancy register

Status date: 2026-07-14. Companion to `LINEARIZABLE_COUNTING_ANALYSIS.md`
(theory) — this document describes **what is actually implemented**, how it
performs against the rest of the suite on real hardware, and where the family
carries redundancy. Source of truth for each design's protocol is its header
comment; this file stays at the survey level.

## 1. File map (one design per file)

| Design | File | Benchmark names |
|---|---|---|
| A — Wire-Indexed | `lib/lock/wire_indexed_counting_lock.hpp` | `lw_*` (2 aliases still on fallbacks) |
| B — Sequenced | `lib/lock/sequenced_counting_lock.hpp` | `seq_bitonic_cas`, `seq_periodic_cas` |
| C — Waiting-Filter | `lib/lock/waiting_filter_counting_lock.hpp` | `wf_*` (8 variants) |
| F — Bounded-Overtaking | `lib/lock/bounded_overtaking_counting_lock.hpp` | `bo_bitonic_cas`, `bo_periodic_cas` |
| G — Heartbeat-Overtaking | `lib/lock/heartbeat_counting_lock.hpp` | `hb_bitonic_cas`, `hb_periodic_cas` |
| D — Skew-Filter (overhead model) | `lib/lock/skew_filter_counting_lock.hpp` | `skew_bitonic_cas`, `skew_periodic_cas` |
| E — Reverse-Skew | removed 2026-07-12 (behaviorally identical to D) | — |
| shared spin macros | `lib/lock/counting_lock_common.hpp` | — |
| umbrella + aliases | `lib/lock/linearizable_counting_lock.hpp` | — |

Predecessors (in `bitonic_networks.hpp`): `BitonicCountingLock` /
`PeriodicCountingLock` (`bitonic_*`, `periodic_*`) — network-ordered with an
O(n) waiting-thread scan at unlock.

## 2. The honest taxonomy

The family's load-bearing fact: **only Design A (and the plain
`bitonic_*`/`periodic_*` predecessors) take their service order from the
counting network.** Designs B, C, F, G and D all order through one global
`fetch_add` ticket; their network traversal is deliberate *arrival shaping* —
spreading threads across W/2 balancers so they reach the ticket cache line at
staggered intervals — and its output is discarded. They are therefore ticket
locks differing only in the successor-signal structure:

| Design | Successor signal | Fairness |
|---|---|---|
| B | token-versioned slot handoff + shared `now_serving_` | strict FIFO |
| C | phase-bit ring (n slots, mod-2 phase) | strict FIFO |
| F | `now_serving_` jump over unregistered tickets (window K=n) | FIFO at quiescence, K-bounded overtaking under load |
| G | F + grant only provably-scheduled waiters (heartbeat freshness) | as F, scheduling-aware |
| D | plain `now_serving_` fetch_add (filter is a pure cost model) | strict FIFO |

Design A's protocol (per-wire slot registration, step-property successor
prediction, waker-flag rescue for invisible waiters) is the genuinely
network-native design — and correspondingly the most intricate; it carries
the family's one remaining known bug (§5).

## 3. Measured comparison (same session, Apple M-series arm64, 8 cores)

`max_contention_bench <lock> T 1.0 --csv --thread-level`, median of 3.
Absolute numbers drift ±30-40% between sessions — compare ratios within this
table only. (`*` = a rep hit the hang residual, §5.)

| lock | T1 | T2 | T4 | T8 | T12 |
|---|---:|---:|---:|---:|---:|
| ticket (baseline FIFO) | 25.93M | 8.66M | 4.34M | 656K | 153K |
| mcs (baseline queue) | 21.11M | 5.22M | 1.04M | 689K | 175K |
| spin / exp_spin (unfair) | ~26M | ~20M | ~10M | 7-8.5M | 8-9M |
| bitonic_cas (predecessor) | 22.64M | 5.35M | 3.36M | 522K | 257K |
| periodic_cas (predecessor) | 23.19M | 5.55M | 1.93M | 358K | 230K |
| seq_bitonic_cas (B) | 15.32M | 4.97M | 1.77M | 473K | 245K |
| seq_periodic_cas (B) | 16.00M | 6.49M | 4.17M | 807K | 256K |
| bo_bitonic_cas (F) | 16.40M | 6.75M | 3.27M | 539K | 178K |
| bo_periodic_cas (F) | 14.79M | 5.38M | 4.20M | 633K | 266K |
| hb_bitonic_cas (G) | 16.26M | 6.45M | 2.61M | 379K | 219K |
| hb_periodic_cas (G) | 7.99M | 2.84M | 1.89M | 476K | 149K |
| wf_bitonic_cas (C) | 23.50M | 5.92M | 1.54M | 82K | 161K |
| wf_periodic_cas (C) | 14.95M | 6.08M | 1.68M | 426K | 159K |
| lw_periodic_cas (A) | 16.18M | 1.76M | 673K | 98K | 73K |
| lw_bitonic_bl (A) | 6.09M | 4.22M* | 2.52M | 59K | 254K |
| skew_bitonic_cas (D) | 22.21M | 5.77M | 4.61M | 641K | 284K |
| net_elevator | 18.76M | 5.33M | 3.77M | 601K | 237K |

Reading (within-session ratios; single machine, so treat as directional):

1. **Nothing here beats plain `ticket`/`spin` in their own regimes.**
   Uncontended, the ticket-ordered designs pay their network + `fetch_add`
   (~15-16M vs ticket's 26M — roughly the cost of one balancer chain).
   Under oversubscription, unfair spinning is in a different league (8M vs
   sub-300K) — fairness is what's being paid for.
2. **The interesting regime is saturated FIFO.** At T12, network-shaped fair
   locks hold 230-284K ops/s vs plain ticket's 153K and MCS's 175K — the
   arrival staggering is worth ~1.5-1.9× *among equally-fair locks* exactly
   where a bare ticket counter collapses into a coherence storm. This is the
   family's measurable value proposition as implemented.
3. **Design A is the slowest under contention** (673K @4T vs B's 4.17M) —
   the price of network-derived ordering with its grant/sweep machinery.
   Its scientific value is as the faithful network-ordered reference, not as
   a performance contender.
4. **F and G don't separate from B on this box** (bo/hb ≈ seq within noise),
   consistent with §7.7 of ELEVATOR_SET_COMPARISON.md: this benchmark rarely
   deschedules registered waiters at T≤8, so the overtaking window rarely
   opens. Their differentiation needs an oversubscribed/preemption-heavy
   workload (or the LD_PRELOAD harness against a real app).
5. `skew_*` (D) ≈ the best of the ticket cluster despite carrying n-1 extra
   RMWs — evidence that at these thread counts the ticket pair, not the
   filter arithmetic, is the binding cost.

## 4. State-of-the-art context — what this family contributes

Counting networks (Aspnes/Herlihy/Shavit) and their linearizable variants
(HSW '96, LSS '96) were proposed for scalable shared counting; using them as
the arbitration core of a *mutex* is unusual because a mutex serializes
anyway — the network can only help with the *coordination traffic*, not the
serial section. Against that background, what is defensible here:

- **Empirical**: a same-suite, same-harness comparison of network-ordered
  vs ticket-ordered vs queue locks, showing network arrival-shaping buys
  ~1.5-1.9× saturated FIFO throughput over a bare ticket (§3.2) while never
  winning below saturation. This bounded, honest claim is the family's main
  measured contribution.
- **Design G's heartbeat handoff** is the most novel mechanism: unlock-side
  grant selection gated on a waiter *liveness* signal (free-running beat
  counters, two-phase freshness scan). Closest published relative:
  time-published queue locks (He, Scherer, Scott) — G applies the idea to a
  ticket-window scan instead of a queue, with single-writer beats and a
  holder-only scratch. A publishable comparison would need preemption-heavy
  workloads where F vs G actually separate.
- **Design F's quiescence argument** (overtaking window opens only when
  strict ordering would stall) is a clean framing of bounded unfairness.
- Designs C and D as implemented are *not* the HSW constructions (C's token
  is a global ticket, not network-derived; D's filter output is discarded) —
  they contribute a successor-signal variant (C) and a calibrated overhead
  model (D), and their headers now say exactly that.

### 4b. The ticket is NOT fundamental (validated 2026-07-14)

The question "does correctness require the global ticket?" has a measured
answer: **no.** Substituting the counting-network output value
(`v = (lv>>1)·W + wire`, the pre-1d497da derivation) back into Design C on
top of the fixed release/acquire handoff passed every oracle (45/45
lock-level, breach-armed T∈{2..12} including odd counts, preload
fenced+fence-free, 2s soak) and ran 1.27-1.59× FASTER than the ticket
version on `wf_periodic_cas` at T2-T12 (parity on `wf_bitonic_cas`) in an
interleaved same-session A/B — removing the fetch_add removes the one
serialization hotspot the network was supposed to eliminate.

Why it is sound (the argument the ticket switch missed): the waiting chain
itself bounds the live-value window. Completed values form a prefix (v
cannot enter before v-1 unlocks); every value between the done-prefix and
the maximum emitted value is held by a distinct live thread (emitted →
its owner waits; un-emitted hole → its future owner is mid-traversal), so
the window never exceeds n — the n-cell mod-2 phase ring cannot ABA, and
v-1's owner always exists. The 2026-07 stalls that motivated the ticket are
attributable to the then-present wrong-side handoff fence, not token gaps.

Consequences: C can be restored to the faithful HSW §3 waiting network (a
5-line change recorded in its header), which also dissolves redundancy R6's
"C ≈ B" collapse — network-C and ticket-B become genuinely different
designs again. The same argument plausibly applies to Design B's 4n-slot
bound (unverified — needs its own run). F and G should keep the ticket:
their overtaking/re-draw semantics are built on dense, immediately-known
tokens. Fairness note: network tokens trade strict arrival-FIFO for
n-bounded overtaking (quiescently consistent order).

## 5. Correctness status (2026-07-14)

All 24 family names pass the current sweeps (lock-level ×10, breach-armed
thread-level, preload victim) except one residual: **Design A hangs in
~1-3% of short 4T lock-level runs** — sync-agnostic, not balancer token
duplication (instrumented and disproved), symptom is one lost lock token
with all waiters polling. History for context: A's dominant
token-destruction race (grant vs waker-flag self-serve, unarbitrated) and
C's wrong-side release fence were found and fixed 2026-07-13/14; B carried
the same fence pattern latently (fixed); details live in each file header
and the git log.

## 6. Redundancy register

| # | Redundancy | Status / recommendation |
|---|---|---|
| R1 | `lw_bitonic_cas` alias → `SeqBitonicLock<CAS>` — *is* `seq_bitonic_cas` under another name | Intentional fallback while A's residual stands. Exclude one of the two from sweeps; flip back when A closes. |
| R2 | `lw_bitonic_bakery` alias → `WFBitonicLock<Bakery>` — *is* `wf_bitonic_bakery` | Same as R1. |
| R3 | `WireIndexedCountingLock::global_seq_` member was dead (never read) | Removed 2026-07-14. |
| R4 | ReverseSkew ≡ Skew (both discard the filter row) | Already removed 2026-07-12; note kept in D's header. |
| R5 | Design F ⊂ Design G (G's ladder step 3 *is* F), and F measurably ties B | F is an ablation baseline for G. Keep only if the F-vs-G ablation is part of the story; otherwise retire `bo_*` and cite G's step-3 fallback. |
| R6 | Designs B, C, F, G, D share one ordering mechanism (global ticket + decorative network); B vs C differ only in signal medium, and D is B minus handoff plus deliberate overhead | The deepest redundancy. A minimal publishable set is: A (network-ordered), B (ticket + handoff), G (scheduling-aware), D (overhead model). C earns its place only if the phase-ring-vs-slot-handoff comparison is a claim you want to defend — currently it underperforms B (§3). |
| R7 | `wf_{bl,lamport,bakery}` variants persist although the same incoherent-hybrid argument used to delete non-CAS `seq_*`/`skew_*` applies to C too (its ordering is also the CAS ticket) | Either document them as "balancer-sync cost probes" (current umbrella note) or delete for consistency. |
| R8 | Plain `bitonic_*`/`periodic_*` vs Design A: two generations of network-ordered mutex (O(n) meta scan vs per-wire slots) | Keep both while A's residual stands (plain versions are the stable network-ordered reference); revisit after. |

## 7. Reproduction

```bash
# Correctness (per lock): 10 reps lock-level + breach-armed sweep
for i in $(seq 10); do gtimeout -s KILL 8 \
  ./build/apps/max_contention_bench/max_contention_bench wf_bitonic_cas 4 0.3 --csv --no-output; done

# Design A residual repro
for i in $(seq 30); do gtimeout -s KILL 6 \
  ./build/apps/max_contention_bench/max_contention_bench lw_periodic_bakery 4 0.25 --csv --no-output >/dev/null; echo $?; done

# Perf table: scripts in session scratchpad (perfrun.py) or README §Performance
```
