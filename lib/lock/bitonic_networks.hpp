#ifndef BITONIC_NETWORKS_HPP
#define BITONIC_NETWORKS_HPP

#pragma once

#include "lock.hpp"
#include "burns_lamport_lock.hpp"
#include "lamport_lock.hpp"
#include "../utils/cxl_utils.hpp"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <new>
#include <atomic>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <mutex>

// =============================================================================
// Bitonic Counting Networks — Herlihy Ch. 12
//
// This file implements the full hierarchy of counting networks from
// "The Art of Multiprocessor Programming" (Herlihy & Shavit), Chapter 12:
//
//   1. Balancer       — The fundamental toggle switch (Fig 12.10, 12.14)
//   2. Merger[2k]     — Merges two step-property sequences (Fig 12.12, 12.15)
//   3. Bitonic[2k]    — Full counting network via recursive merger (Fig 12.13, 12.16)
//   4. Block[2k]      — Building block for periodic networks (Fig 12.19)
//   5. Periodic[2k]   — Counting network from repeated identical blocks (Fig 12.18)
//
// Hardware Abstraction / ISA Memory Model Support:
//   Each balancer's traverse() must be atomic (mutual exclusion over the
//   toggle).  Three synchronisation strategies are provided:
//
//   A. CAS (lock-free):  Uses atomic fetch_add — requires hardware RMW
//      (x86 LOCK prefix, ARM LDXR/STXR).  Best throughput on modern
//      out-of-order cores.
//
//   B. Software Mutex:   A per-balancer software lock guards the toggle.
//      No RMW atomics needed — only loads, stores, and memory fences.
//      Works on any ISA with sequential consistency or with explicit
//      fence instructions.  Two sub-variants:
//        - Burns-Lamport (2-thread fast path, N-thread safe)
//        - Lamport Fast Lock (bounded, fence-based)
//
//   C. Elevator Lock (Buhr et al.):  A per-balancer MCS-style queue lock.
//      Uses only loads/stores and a single CAS for enqueue (or a software
//      alternative).  The "elevator" property provides local spinning —
//      each thread spins on its own cache-line node, reducing coherence
//      traffic.  Suitable for NUMA/CXL where remote CAS is expensive.
//
// The key invariant from Herlihy: the traverse() operation on each
// balancer must be SERIALISED (either by a mutex, by CAS, or by any
// mechanism providing mutual exclusion over the toggle flip).
//
// Complexity Table (see BITONIC_NETWORKS_COMPLEXITY.md):
//   Network      | Depth (balancers) | Width | Balancer count
//   Bitonic[w]   | O(log²w)          | w     | O(w/2 · log²w)
//   Periodic[w]  | O(log²w)          | w     | O(w/2 · log²w)
//   Merger[2k]   | O(log(2k))        | 2k    | O(k · log(2k))
//   Block[w]     | O(log w)          | w     | O(w/2 · log w)
// =============================================================================


// ─── Spin-wait hints ─────────────────────────────────────────────────────────
#if defined(__aarch64__) || defined(_M_ARM64)
  #define BnSpinHint() __asm__ volatile("yield")
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #define BnSpinHint() __asm__ volatile("pause")
#else
  #define BnSpinHint() ((void)0)
#endif

// Long-wait spin with periodic sched_yield — see LcSpinWait in
// linearizable_counting_lock.hpp for rationale. Used only for the
// lock-layer grant waits, not the short per-balancer doorways.
#define BnSpinWait(spins) \
    do { if (((++(spins)) & 1023) == 0) sched_yield(); else BnSpinHint(); } while (0)


// =============================================================================
//  SECTION 1: Balancer Synchronisation Policies
//
//  Each policy wraps a monotonic counter.  traverse(tid) increments and
//  returns the pre-increment value.  The low bit (val & 1) is the toggle
//  direction (0 = top/north, 1 = bottom/south).
// =============================================================================

// ── Policy A: CAS (lock-free) ────────────────────────────────────────────────
//    Context: best for x86/ARM with native RMW atomics.  Each balancer
//    is a single cache-line atomic counter.  fetch_add serialises the
//    toggle with minimal cache-line bouncing.
struct BnCASSync {
    std::atomic<size_t> counter{0};

    void init(size_t /*num_threads*/) {
        counter.store(0, std::memory_order_relaxed);
    }

    size_t traverse(size_t /*tid*/) {
        // Relaxed is sufficient: the network needs only atomicity of each
        // increment (every value returned exactly once, so wires alternate
        // and per-balancer rounds stay contiguous). No cross-thread
        // happens-before is derived from balancer values — the lock layers
        // establish ordering separately (now_serving/phase-bit acquire
        // loads and explicit Fence()s). On ARM this is ldadd vs ldaddal —
        // one barrier per balancer per acquisition saved.
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    void destroy() {}
    static const char* sync_name() { return "cas"; }
};

// ── Policy B1: Burns-Lamport software mutex ──────────────────────────────────
//    Context: ISAs without hardware CAS (e.g., early RISC, some embedded).
//    Uses only loads, stores, and fences.  The Burns-Lamport algorithm
//    provides mutual exclusion for N threads with O(N) space.
struct BnBLSync {
    BurnsLamportMutex mtx;
    volatile size_t counter;

    void init(size_t num_threads) {
        mtx.init(num_threads);
        counter = 0;
    }

    size_t traverse(size_t tid) {
        mtx.lock(tid);
        Fence();  // acquire: ensure visibility of previous holder's counter write
        size_t val = counter;
        counter = val + 1;
        Fence();  // release: ensure counter write visible before unlock
        mtx.unlock(tid);
        return val;
    }

    void destroy() { mtx.destroy(); }
    static const char* sync_name() { return "bl"; }
};

// ── Policy B2: Lamport Fast Lock ─────────────────────────────────────────────
//    Context: similar to Burns-Lamport but with a fast path when
//    uncontended.  Good for low-contention scenarios on fence-capable ISAs.
struct BnLamportSync {
    LamportLock mtx;
    volatile size_t counter;

    void init(size_t num_threads) {
        mtx.init(num_threads);
        counter = 0;
    }

    size_t traverse(size_t tid) {
        mtx.lock(tid);
        Fence();  // acquire: ensure visibility of previous holder's counter write
        size_t val = counter;
        counter = val + 1;
        Fence();  // release: ensure counter write visible before unlock
        mtx.unlock(tid);
        return val;
    }

    void destroy() { mtx.destroy(); }
    static const char* sync_name() { return "lamport"; }
};

// ── Policy C: Elevator Lock (Buhr et al.) ────────────────────────────────────
//    Context: NUMA and CXL systems where remote CAS is 3-10x slower than
//    local.  The elevator lock uses an MCS-style queue: each thread spins
//    on its own node (local spinning), and only the unlock write crosses
//    NUMA domains.  The single CAS is at enqueue; if even that is
//    unavailable, it can be replaced with a LL/SC pair or a software
//    alternative (e.g., fetch-and-phi from Herlihy Ch. 5).
//
//    The "elevator" name (Buhr, Dice, Scherer 2005) comes from the
//    sequential handoff property: like an elevator, threads are served
//    in arrival order with each handoff being a single cache-line write.
struct BnElevatorSync {
    struct alignas(64) Node {
        std::atomic<Node*> next{nullptr};
        std::atomic<bool>  granted{false};
    };

    std::atomic<Node*> tail{nullptr};
    Node* nodes = nullptr;
    size_t num_threads_ = 0;
    volatile size_t counter = 0;

    void init(size_t num_threads) {
        num_threads_ = num_threads;
        nodes = new Node[num_threads]();
        tail.store(nullptr, std::memory_order_relaxed);
        counter = 0;
    }

    size_t traverse(size_t tid) {
        Node* me = &nodes[tid];
        me->next.store(nullptr, std::memory_order_relaxed);
        me->granted.store(false, std::memory_order_relaxed);

        Node* prev = tail.exchange(me, std::memory_order_acq_rel);
        if (prev != nullptr) {
            prev->next.store(me, std::memory_order_release);
            while (!me->granted.load(std::memory_order_acquire)) {
                BnSpinHint();
            }
        }

        // Critical section: increment counter
        size_t val = counter;
        counter = val + 1;

        // Unlock: hand off to next or release tail
        Node* succ = me->next.load(std::memory_order_acquire);
        if (succ == nullptr) {
            // Try to set tail to nullptr (no successor yet)
            Node* expected = me;
            if (tail.compare_exchange_strong(expected, nullptr,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return val;  // No successor, done
            }
            // Successor is in the process of linking, wait for it
            while ((succ = me->next.load(std::memory_order_acquire)) == nullptr) {
                BnSpinHint();
            }
        }
        succ->granted.store(true, std::memory_order_release);
        return val;
    }

    void destroy() {
        delete[] nodes;
        nodes = nullptr;
    }

    static const char* sync_name() { return "elevator"; }
};

// ── Policy D: Pure Software (no atomics at all) ─────────────────────────────
//    Context: hypothetical ISAs with NO atomic RMW instructions and only
//    sequential consistency (or ISAs where all memory operations are
//    sequentially consistent by default, e.g., some research architectures).
//    Uses Lamport's Bakery algorithm (loads and stores only, no fences
//    needed under SC) to protect the toggle counter.
//
//    This is the most portable but slowest option — O(N) doorway per
//    balancer access.
struct BnBakerySync {
    // Inline minimal bakery for 2 "threads" (the two wires entering
    // this balancer aren't thread IDs — any thread can arrive on
    // either wire).  We need full N-thread bakery since the balancer
    // can be accessed by any of N threads.
    volatile bool* choosing = nullptr;
    volatile size_t* number = nullptr;
    volatile size_t counter = 0;
    size_t n = 0;

    void init(size_t num_threads) {
        n = num_threads;
        choosing = new volatile bool[n]();
        number = new volatile size_t[n]();
        counter = 0;
    }

    size_t traverse(size_t tid) {
        // Doorway
        choosing[tid] = true;
        Fence();
        size_t max_num = 0;
        for (size_t i = 0; i < n; i++) {
            size_t v = number[i];
            if (v > max_num) max_num = v;
        }
        number[tid] = max_num + 1;
        Fence();
        choosing[tid] = false;
        Fence();

        // Wait
        for (size_t j = 0; j < n; j++) {
            if (j == tid) continue;
            while (choosing[j]) { BnSpinHint(); }
            Fence();
            while (number[j] != 0 &&
                   (number[j] < number[tid] ||
                    (number[j] == number[tid] && j < tid))) {
                BnSpinHint();
            }
        }
        Fence();  // acquire: ensure visibility of previous holder's counter write

        size_t val = counter;
        counter = val + 1;
        Fence();

        // Release
        number[tid] = 0;
        Fence();
        return val;
    }

    void destroy() {
        delete[] choosing;
        delete[] number;
        choosing = nullptr;
        number = nullptr;
    }

    static const char* sync_name() { return "bakery"; }
};


// =============================================================================
//  SECTION 2: Balancer (Herlihy Fig 12.10, 12.14)
//
//  A balancer is a simple toggle switch.  Tokens arrive on two input wires
//  and are sent alternately to top (0) and bottom (1) output wires.
//
//  Application context:
//    The balancer is the atomic building block of all counting networks.
//    It provides the guarantee that |y0 - y1| <= 1 in any quiescent state.
//    Used as a load-balancing primitive in distributed counters, pool
//    allocators, and work-stealing schedulers.
// =============================================================================

// alignas: balancers are allocated in arrays (one per merger/block layer).
// Without padding, Balancer<BnCASSync> is 8 bytes and a whole layer of
// "independent" balancers shares one cache line, so every fetch_add
// invalidates the line under every other balancer in the layer — the
// network pays its full depth while getting none of its contention
// distribution. Padding each balancer to its own destructive-interference
// span restores the property the topology is supposed to provide.
template <typename Sync>
class alignas(std::hardware_destructive_interference_size) Balancer {
public:
    Sync sync;

    void init(size_t num_threads) { sync.init(num_threads); }
    void destroy() { sync.destroy(); }

    // Returns output wire: 0 (top/north) or 1 (bottom/south).
    // If last_val is non-null, stores the raw counter value there
    // (needed by the lock layer to extract round numbers).
    int traverse(size_t tid, size_t* last_val = nullptr) {
        size_t val = sync.traverse(tid);
        if (last_val) *last_val = val;
        return (int)(val & 1);
    }
};


// =============================================================================
//  SECTION 3: Merger[2k] (Herlihy Fig 12.12, 12.15)
//
//  Merges two width-k step-property sequences into one width-2k
//  step-property sequence.  Constructed recursively:
//    - Base case (k=1): single balancer
//    - Recursive case: two Merger[k] sub-networks + k final balancers
//      The even subsequence of x merges with odd subsequence of x',
//      and vice versa, then pairs are balanced in a final layer.
//
//  Application context:
//    The merger is the key correctness component.  It preserves the step
//    property across merges, enabling the inductive proof of Theorem 12.5.1.
//    In practice, merger networks appear in sorting networks (bitonic sort),
//    load balancing for shared data structures, and distributed barrier
//    implementations.
//
//  Depth: log(2k) layers of k balancers each.
// =============================================================================

template <typename Sync>
class Merger {
public:
    Merger<Sync>* half[2] = {nullptr, nullptr};
    Balancer<Sync>* layer = nullptr;  // width/2 balancers (final layer)
    size_t width = 0;

    Merger() = default;

    void build(size_t myWidth, size_t num_threads) {
        width = myWidth;
        layer = new Balancer<Sync>[width / 2];
        for (size_t i = 0; i < width / 2; i++) {
            layer[i].init(num_threads);
        }
        if (width > 2) {
            half[0] = new Merger<Sync>();
            half[1] = new Merger<Sync>();
            half[0]->build(width / 2, num_threads);
            half[1]->build(width / 2, num_threads);
        }
    }

    // Herlihy Fig 12.15: traverse(input)
    // Input wire determines path through sub-mergers.
    // Returns output wire index [0, width).
    int traverse(int input, size_t tid, size_t* last_val = nullptr) {
        int output;
        if (width == 2) {
            return layer[0].traverse(tid, last_val);
        }
        if (input < (int)(width / 2)) {
            output = half[input % 2]->traverse(input / 2, tid);
        } else {
            output = half[1 - (input % 2)]->traverse(input / 2, tid);
        }
        int wire = layer[output].traverse(tid, last_val);
        return 2 * output + wire;
    }

    // Returns globally unique ticket: (round * width) + output_wire.
    size_t traverse_full(int input, size_t tid) {
        size_t lv = 0;
        int wire = traverse(input, tid, &lv);
        return (lv >> 1) * width + (size_t)wire;
    }

    void destroy() {
        if (layer) {
            for (size_t i = 0; i < width / 2; i++) {
                layer[i].destroy();
            }
            delete[] layer;
            layer = nullptr;
        }
        if (half[0]) { half[0]->destroy(); delete half[0]; half[0] = nullptr; }
        if (half[1]) { half[1]->destroy(); delete half[1]; half[1] = nullptr; }
    }
};


// =============================================================================
//  SECTION 4: Bitonic[2k] (Herlihy Fig 12.13, 12.16)
//
//  The full bitonic counting network.  Constructed recursively:
//    Bitonic[2k] = two Bitonic[k] + one Merger[2k]
//    Base case: Bitonic[2] = single balancer (= Merger[2])
//
//  The bitonic counting network satisfies the step property
//  (Theorem 12.5.1): in any quiescent state, if n tokens have passed
//  through, then output wire i has ceil((n-i)/w) tokens.
//
//  Application context:
//    - Distributed shared counters: W output wires each maintain a local
//      counter.  Thread gets unique index = wire + round * W.  This
//      eliminates the single-point contention of a shared fetch_add counter.
//    - Memory allocators: each wire distributes a disjoint set of pool
//      slots, reducing false sharing.
//    - Work-stealing deques: counting network routes new tasks to W queues,
//      maintaining approximate balance without centralised coordination.
//    - Barrier synchronisation: threads traverse the network and spin on
//      per-wire flags, distributing wakeup traffic.
//
//  Depth: (log₂w + 1) / 2 choose 2 = O(log²w) layers
//  Balancers: O(w/2 · log²w)
//  Width: w (must be power of 2)
// =============================================================================

template <typename Sync>
class BitonicNetwork {
public:
    BitonicNetwork<Sync>* half[2] = {nullptr, nullptr};
    Merger<Sync> merger;
    size_t width = 0;

    BitonicNetwork() = default;

    void build(size_t myWidth, size_t num_threads) {
        width = myWidth;
        merger.build(width, num_threads);
        if (width > 2) {
            half[0] = new BitonicNetwork<Sync>();
            half[1] = new BitonicNetwork<Sync>();
            half[0]->build(width / 2, num_threads);
            half[1]->build(width / 2, num_threads);
        }
    }

    // Herlihy Fig 12.16: traverse(input)
    // Returns output wire index [0, width).
    int traverse(int input, size_t tid, size_t* last_val = nullptr) {
        int output = 0;
        if (width > 2) {
            int half_idx = input / (int)(width / 2);
            output = half[half_idx]->traverse(input % (int)(width / 2), tid);
        }
        int merger_input;
        if (width <= 2) {
            merger_input = input;
        } else {
            int half_idx = input / (int)(width / 2);
            merger_input = half_idx * (int)(width / 2) + output;
        }
        return merger.traverse(merger_input, tid, last_val);
    }

    // Returns globally unique ticket: (round * width) + output_wire.
    size_t traverse_full(int input, size_t tid) {
        size_t lv = 0;
        int wire = traverse(input, tid, &lv);
        return (lv >> 1) * width + (size_t)wire;
    }

    void destroy() {
        merger.destroy();
        if (half[0]) { half[0]->destroy(); delete half[0]; half[0] = nullptr; }
        if (half[1]) { half[1]->destroy(); delete half[1]; half[1] = nullptr; }
    }
};


// =============================================================================
//  SECTION 5: Block[2k] (Herlihy Fig 12.19)
//
//  Building block for the periodic counting network.
//    Block[2] = single balancer
//    Block[2k] = two Block[k] (top/bottom halves) + k final balancers
//
//  Unlike the Merger, Block does NOT assume its inputs have the step
//  property.  Instead, log(k) repetitions of Block[2k] compose to form
//  the Periodic[2k] network, which achieves the step property through
//  iterative refinement.
//
//  Application context:
//    The block is the repeating unit in periodic networks.  Its value
//    is in simplicity and regularity: every stage is structurally
//    identical, making it well-suited for:
//    - Hardware implementations (FPGA/ASIC) where routing regularity
//      reduces wiring complexity
//    - Pipelined network counters where each block can be a pipeline stage
//    - Fault-tolerant designs where identical blocks can be hot-swapped
//
//  Depth: log(w) layers (one layer per recursive level)
//  Balancers per block: w/2 · log(w)
// =============================================================================

template <typename Sync>
class Block {
public:
    Block<Sync>* sub[2] = {nullptr, nullptr};  // Two half-width sub-blocks
    Balancer<Sync>* final_layer = nullptr;       // w/2 final balancers
    size_t width = 0;

    Block() = default;

    void build(size_t myWidth, size_t num_threads) {
        width = myWidth;
        // Final layer: w/2 balancers combining pairs
        final_layer = new Balancer<Sync>[width / 2];
        for (size_t i = 0; i < width / 2; i++) {
            final_layer[i].init(num_threads);
        }
        if (width > 2) {
            sub[0] = new Block<Sync>();
            sub[1] = new Block<Sync>();
            sub[0]->build(width / 2, num_threads);
            sub[1]->build(width / 2, num_threads);
        }
    }

    // Traverse: input wire -> output wire, per AoMP (Herlihy-Shavit)
    // Fig 12.19 Block[2k]:
    //   sub-block A takes the EVEN-indexed inputs x0, x2, ..., x_{2k-2}
    //   sub-block B takes the ODD-indexed  inputs x1, x3, ..., x_{2k-1}
    //   final_layer[i] combines yA_i and yB_i -> z_{2i} (top), z_{2i+1}
    //
    // The previous wiring split inputs into CONTIGUOUS halves and emitted
    // z_i / z_{i+k}. Cascading that variant (Periodic = Block ∘ Block ∘ ...)
    // inserts stray wire permutations between stages: the result smooths
    // uniform traffic (so tight-loop density checks pass) but is NOT a
    // counting network — under lock-shaped schedules it occasionally
    // violated the step property, emitting e.g. wire-2 round R while
    // wire-1 round R never appeared. Any design that chain-waits on the
    // missing token then deadlocks (observed: seq_periodic_cas 3/60,
    // wf_periodic_cas 2/60 hangs pre-fix, with a captured draw-history
    // proving value 192469 was never emitted while 192470 was).
    int traverse(int input, size_t tid, size_t* last_val = nullptr) {
        if (width == 2) {
            return final_layer[0].traverse(tid, last_val);
        }
        int output;
        if ((input & 1) == 0) {
            output = sub[0]->traverse(input >> 1, tid);
        } else {
            output = sub[1]->traverse(input >> 1, tid);
        }
        int bit = final_layer[output].traverse(tid, last_val);
        return 2 * output + bit;
    }

    // Returns globally unique ticket: (round * width) + output_wire.
    size_t traverse_full(int input, size_t tid) {
        size_t lv = 0;
        int wire = traverse(input, tid, &lv);
        return (lv >> 1) * width + (size_t)wire;
    }

    void destroy() {
        if (final_layer) {
            for (size_t i = 0; i < width / 2; i++) {
                final_layer[i].destroy();
            }
            delete[] final_layer;
            final_layer = nullptr;
        }
        if (sub[0]) { sub[0]->destroy(); delete sub[0]; sub[0] = nullptr; }
        if (sub[1]) { sub[1]->destroy(); delete sub[1]; sub[1] = nullptr; }
    }
};


// =============================================================================
//  SECTION 6: Periodic[2k] (Herlihy Fig 12.18)
//
//  A counting network consisting of log(k) identical Block[2k] stages
//  connected in sequence.  The remarkable property is that every stage
//  is structurally identical — the network is PERIODIC.
//
//  Periodic[2k] = Block[2k] ∘ Block[2k] ∘ ... ∘ Block[2k]  (log(k) times)
//
//  Application context:
//    - Hardware counting networks: periodic structure maps naturally to
//      VLSI layouts with minimal routing overhead
//    - Pipeline-parallel counters: each block is a pipeline stage,
//      allowing overlapped execution of multiple tokens
//    - Self-similar fault tolerance: any block can be replaced without
//      changing the overall structure
//    - Streaming token distribution: tokens flow through identical stages,
//      making timing analysis straightforward
//
//  Depth: log(k) · log(w) = O(log²w) layers
//  Balancers: log(k) · w/2 · log(w) = O(w/2 · log²w)
//  Width: w = 2k (must be power of 2)
// =============================================================================

template <typename Sync>
class PeriodicNetwork {
public:
    Block<Sync>** blocks = nullptr;
    size_t width = 0;
    size_t num_blocks = 0;

    PeriodicNetwork() = default;

    void build(size_t myWidth, size_t num_threads) {
        width = myWidth;
        // Number of Block repetitions for Periodic[2k]:
        //   log2(k) where w = 2k  =>  log2(w) - 1.
        // The previous log2(w) staging over-applied one extra Block and
        // occasionally violated the step property under lock-like schedules.
        size_t log_w = 0;
        { size_t tmp = width; while (tmp > 1) { log_w++; tmp >>= 1; } }
        num_blocks = (log_w > 1) ? (log_w - 1) : 1;

        blocks = new Block<Sync>*[num_blocks];
        for (size_t i = 0; i < num_blocks; i++) {
            blocks[i] = new Block<Sync>();
            blocks[i]->build(width, num_threads);
        }
    }

    // Traverse: token passes through each block in sequence.
    int traverse(int input, size_t tid, size_t* last_val = nullptr) {
        int wire = input;
        for (size_t i = 0; i < num_blocks; i++) {
            // Only capture last_val from the final block
            wire = blocks[i]->traverse(wire, tid,
                (i == num_blocks - 1) ? last_val : nullptr);
        }
        return wire;
    }

    // Returns globally unique ticket: (round * width) + output_wire.
    size_t traverse_full(int input, size_t tid) {
        size_t lv = 0;
        int wire = traverse(input, tid, &lv);
        return (lv >> 1) * width + (size_t)wire;
    }

    void destroy() {
        if (blocks) {
            for (size_t i = 0; i < num_blocks; i++) {
                if (blocks[i]) {
                    blocks[i]->destroy();
                    delete blocks[i];
                }
            }
            delete[] blocks;
            blocks = nullptr;
        }
    }
};


// =============================================================================
//  SECTION 7: Counting Network Locks (SoftwareMutex adapters)
//
//  Each lock wraps a counting network (Bitonic or Periodic) into the
//  project's SoftwareMutex interface.  The network distributes threads
//  across W output wires; each wire has a local counter producing
//  globally unique tickets.  A "now_serving" counter determines which
//  ticket holds the lock.
//
//  The architecture matches Herlihy's Fig 12.9:
//    n threads → counting network (width W) → W counters → unique indexes
// =============================================================================

// ── Designated-waker lock (reused across network lock variants) ──────────────
struct BnWakerLock {
    volatile bool* in_contention = nullptr;
    volatile bool fast = false;
    size_t num_threads_ = 0;

    void init(volatile bool* mem, size_t n) {
        in_contention = mem;
        for (size_t i = 0; i < n; i++) in_contention[i] = false;
        fast = false;
        num_threads_ = n;
    }

    bool trylock(size_t tid) {
        in_contention[tid] = true;
        Fence();
        for (size_t h = 0; h < tid; h++) {
            if (in_contention[h]) {
                in_contention[tid] = false;
                return false;
            }
        }
        for (size_t l = tid + 1; l < num_threads_; l++) {
            while (in_contention[l]) { BnSpinHint(); }
        }
        Fence();
        bool leader;
        if (!fast) {
            fast = true;
            leader = true;
        } else {
            leader = false;
        }
        Fence();
        in_contention[tid] = false;
        return leader;
    }

    void unlock() {
        fast = false;
    }
};


// ─────────────────────────────────────────────────────────────────────────────
// BitonicCountingLock<Sync>
//
// Mutual exclusion using a Bitonic counting network for ticket distribution.
// Threads traverse the bitonic network to get a globally unique ticket,
// then spin until now_serving matches their ticket.
// ─────────────────────────────────────────────────────────────────────────────
template <typename Sync>
class BitonicCountingLock : public virtual SoftwareMutex {
public:
    ~BitonicCountingLock() override { destroy(); }

    void init(size_t num_threads) override {
        num_threads_ = num_threads;

        // Network width: smallest power of 2 >= ceil(sqrt(num_threads))
        size_t target = (size_t)std::ceil(std::sqrt((double)num_threads));
        if (target < 2) target = 2;
        width_ = 1;
        while (width_ < target) width_ *= 2;

        network_.build(width_, num_threads);

        // Per-thread metadata (cache-line padded)
        size_t meta_bytes   = CL * num_threads;
        size_t waker_bytes  = sizeof(volatile bool) * num_threads;
        size_t flag_bytes   = CL;
        region_size_ = flag_bytes + meta_bytes + waker_bytes;
        region_ = (volatile char*)ALLOCATE(region_size_);
        memset((void*)region_, 0, region_size_);

        size_t off = 0;
        waker_flag_ = (volatile bool*)&region_[off]; off += flag_bytes;
        meta_base_  = &region_[off];                 off += meta_bytes;
        volatile bool* waker_mem = (volatile bool*)&region_[off];

        for (size_t i = 0; i < num_threads; i++) {
            auto* m = get_meta(i);
            m->waiting = false;
            m->my_wire = 0;
            m->my_round = 0;
            m->spin_addr = nullptr;
        }

        *waker_flag_ = true;
        next_wire_ = 0;
        waker_lock_.init(waker_mem, num_threads);
        initialized_ = true;
    }

    void lock(size_t thread_id) override {
        volatile bool local_grant = false;

        // Traverse bitonic counting network
        size_t round;
        size_t wire = traverse_network(thread_id % width_, thread_id, round);

        auto* meta = get_meta(thread_id);
        meta->my_wire = wire;
        meta->my_round = round;
        meta->spin_addr = &local_grant;
        Fence();
        meta->waiting = true;
        Fence();

        // Designated-waker protocol
        if (waker_lock_.trylock(thread_id)) {
            Fence();
            unsigned spins = 0;
            while (!local_grant && !*waker_flag_) { BnSpinWait(spins); }
            *waker_flag_ = false;
            Fence();
            waker_lock_.unlock();
        } else {
            unsigned spins = 0;
            while (!local_grant) { BnSpinWait(spins); }
        }
    }

    void unlock(size_t thread_id) override {
        get_meta(thread_id)->waiting = false;
        Fence();
        grant_next();
    }

    void destroy() override {
        if (!initialized_) return;
        initialized_ = false;
        network_.destroy();
        if (region_) {
            FREE((void*)region_, region_size_);
            region_ = nullptr;
        }
    }

    std::string name() override {
        return std::string("bitonic_") + Sync::sync_name();
    }

private:
    static constexpr size_t CL = std::hardware_destructive_interference_size;

    struct ThreadMeta {
        volatile bool   waiting;
        volatile size_t my_wire;
        volatile size_t my_round;
        volatile bool*  spin_addr;
    };

    BitonicNetwork<Sync> network_;
    size_t num_threads_ = 0;
    size_t width_ = 0;
    volatile char* region_ = nullptr;
    size_t region_size_ = 0;
    volatile bool* waker_flag_ = nullptr;
    volatile char* meta_base_ = nullptr;
    BnWakerLock waker_lock_;
    size_t next_wire_ = 0;
    bool initialized_ = false;

    ThreadMeta* get_meta(size_t tid) const {
        return (ThreadMeta*)&meta_base_[tid * CL];
    }

    size_t traverse_network(size_t input_wire, size_t tid, size_t& out_round) {
        size_t lv = 0;
        int wire = network_.traverse((int)input_wire, tid, &lv);
        out_round = lv >> 1;
        return (size_t)wire;
    }

    void grant_next() {
        size_t best_tid = num_threads_;
        size_t best_wd = width_;
        size_t best_round = ~(size_t)0;

        for (size_t i = 0; i < num_threads_; i++) {
            if (i + 2 < num_threads_)
                __builtin_prefetch(get_meta(i + 2), 0, 1);

            auto* m = get_meta(i);
            if (!m->waiting) continue;

            size_t wd = (m->my_wire + width_ - next_wire_) % width_;
            size_t r = m->my_round;

            if (wd < best_wd ||
                (wd == best_wd && r < best_round) ||
                (wd == best_wd && r == best_round && i < best_tid)) {
                best_wd = wd;
                best_round = r;
                best_tid = i;
            }
        }

        if (best_tid < num_threads_) {
            auto* winner = get_meta(best_tid);
            next_wire_ = (winner->my_wire + 1) % width_;
            *winner->spin_addr = true;
            Fence();
        } else {
            *waker_flag_ = true;
            Fence();
        }
    }
};


// ─────────────────────────────────────────────────────────────────────────────
// PeriodicCountingLock<Sync>
//
// Same ticket-lock architecture but using a Periodic counting network
// instead of Bitonic.  The periodic network consists of log(k) identical
// Block[2k] stages, giving it a regular, repeating structure.
// ─────────────────────────────────────────────────────────────────────────────
template <typename Sync>
class PeriodicCountingLock : public virtual SoftwareMutex {
public:
    ~PeriodicCountingLock() override { destroy(); }

    void init(size_t num_threads) override {
        num_threads_ = num_threads;

        size_t target = (size_t)std::ceil(std::sqrt((double)num_threads));
        if (target < 2) target = 2;
        width_ = 1;
        while (width_ < target) width_ *= 2;

        network_.build(width_, num_threads);

        size_t meta_bytes   = CL * num_threads;
        size_t waker_bytes  = sizeof(volatile bool) * num_threads;
        size_t flag_bytes   = CL;
        region_size_ = flag_bytes + meta_bytes + waker_bytes;
        region_ = (volatile char*)ALLOCATE(region_size_);
        memset((void*)region_, 0, region_size_);

        size_t off = 0;
        waker_flag_ = (volatile bool*)&region_[off]; off += flag_bytes;
        meta_base_  = &region_[off];                 off += meta_bytes;
        volatile bool* waker_mem = (volatile bool*)&region_[off];

        for (size_t i = 0; i < num_threads; i++) {
            auto* m = get_meta(i);
            m->waiting = false;
            m->my_wire = 0;
            m->my_round = 0;
            m->spin_addr = nullptr;
        }

        *waker_flag_ = true;
        next_wire_ = 0;
        waker_lock_.init(waker_mem, num_threads);
        initialized_ = true;
    }

    void lock(size_t thread_id) override {
        volatile bool local_grant = false;

        size_t lv = 0;
        int wire_i = network_.traverse((int)(thread_id % width_), thread_id, &lv);
        size_t wire = (size_t)wire_i;
        size_t round = lv >> 1;

        auto* meta = get_meta(thread_id);
        meta->my_wire = wire;
        meta->my_round = round;
        meta->spin_addr = &local_grant;
        Fence();
        meta->waiting = true;
        Fence();

        if (waker_lock_.trylock(thread_id)) {
            Fence();
            unsigned spins = 0;
            while (!local_grant && !*waker_flag_) { BnSpinWait(spins); }
            *waker_flag_ = false;
            Fence();
            waker_lock_.unlock();
        } else {
            unsigned spins = 0;
            while (!local_grant) { BnSpinWait(spins); }
        }
    }

    void unlock(size_t thread_id) override {
        get_meta(thread_id)->waiting = false;
        Fence();
        grant_next();
    }

    void destroy() override {
        if (!initialized_) return;
        initialized_ = false;
        network_.destroy();
        if (region_) {
            FREE((void*)region_, region_size_);
            region_ = nullptr;
        }
    }

    std::string name() override {
        return std::string("periodic_") + Sync::sync_name();
    }

private:
    static constexpr size_t CL = std::hardware_destructive_interference_size;

    struct ThreadMeta {
        volatile bool   waiting;
        volatile size_t my_wire;
        volatile size_t my_round;
        volatile bool*  spin_addr;
    };

    PeriodicNetwork<Sync> network_;
    size_t num_threads_ = 0;
    size_t width_ = 0;
    volatile char* region_ = nullptr;
    size_t region_size_ = 0;
    volatile bool* waker_flag_ = nullptr;
    volatile char* meta_base_ = nullptr;
    BnWakerLock waker_lock_;
    size_t next_wire_ = 0;
    bool initialized_ = false;

    ThreadMeta* get_meta(size_t tid) const {
        return (ThreadMeta*)&meta_base_[tid * CL];
    }

    void grant_next() {
        size_t best_tid = num_threads_;
        size_t best_wd = width_;
        size_t best_round = ~(size_t)0;

        for (size_t i = 0; i < num_threads_; i++) {
            if (i + 2 < num_threads_)
                __builtin_prefetch(get_meta(i + 2), 0, 1);

            auto* m = get_meta(i);
            if (!m->waiting) continue;

            size_t wd = (m->my_wire + width_ - next_wire_) % width_;
            size_t r = m->my_round;

            if (wd < best_wd ||
                (wd == best_wd && r < best_round) ||
                (wd == best_wd && r == best_round && i < best_tid)) {
                best_wd = wd;
                best_round = r;
                best_tid = i;
            }
        }

        if (best_tid < num_threads_) {
            auto* winner = get_meta(best_tid);
            next_wire_ = (winner->my_wire + 1) % width_;
            *winner->spin_addr = true;
            Fence();
        } else {
            *waker_flag_ = true;
            Fence();
        }
    }
};


// =============================================================================
// Concrete type aliases for all sync × network combinations
// =============================================================================

// ── Bitonic network locks ────────────────────────────────────────────────────
using BitonicCASLock      = BitonicCountingLock<BnCASSync>;
using BitonicBLLock       = BitonicCountingLock<BnBLSync>;
using BitonicLamportLock  = BitonicCountingLock<BnLamportSync>;
using BitonicElevatorLock = BitonicCountingLock<BnElevatorSync>;
using BitonicBakeryLock   = BitonicCountingLock<BnBakerySync>;

// ── Periodic network locks ───────────────────────────────────────────────────
using PeriodicCASLock      = PeriodicCountingLock<BnCASSync>;
using PeriodicBLLock       = PeriodicCountingLock<BnBLSync>;
using PeriodicLamportLock  = PeriodicCountingLock<BnLamportSync>;
using PeriodicElevatorLock = PeriodicCountingLock<BnElevatorSync>;
using PeriodicBakeryLock   = PeriodicCountingLock<BnBakerySync>;

#endif // BITONIC_NETWORKS_HPP
