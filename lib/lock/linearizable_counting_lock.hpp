#ifndef LINEARIZABLE_COUNTING_LOCK_HPP
#define LINEARIZABLE_COUNTING_LOCK_HPP

#pragma once

// =============================================================================
// Linearizable-Window Counting Locks — family umbrella header
//
// Lock designs that combine a counting network (bitonic_networks.hpp) with a
// mutual-exclusion protocol. One design per file; this header includes them
// all and defines the concrete benchmark aliases (Section 5) so existing
// includers keep working unchanged.
//
// Based on:
//   [1] Lynch, Shavit, Shvartsman (1996) — "Counting Networks Are Practically
//       Linearizable"  (PODC 96)
//   [2] Herlihy, Shavit, Waarts (1996) — "Linearizable Counting Networks"
//       (Distributed Computing)
//
// Theory: LINEARIZABLE_COUNTING_ANALYSIS.md.  Measured comparisons against
// the rest of the suite + redundancy register: COUNTING_LOCKS.md.
//
// ── Family index (honest taxonomy) ──────────────────────────────────────────
//
//  Design  File                                  Ordering comes from
//  ──────  ────────────────────────────────────  ─────────────────────────────
//  A       wire_indexed_counting_lock.hpp        NETWORK (wire, round) with
//          (lw_*)                                per-wire slot service;
//                                                residual instability
//                                                under short timeout
//                                                stress is documented
//                                                in-file
//  B       sequenced_counting_lock.hpp           bitonic: NETWORK output value
//          (seq_*)                               periodic: dense ticket fallback;
//                                                slot handoff + now_serving_
//  C       waiting_filter_counting_lock.hpp      bitonic: NETWORK output value
//          (wf_*)                                periodic: dense ticket fallback;
//                                                HSW §3 phase-bit waiting ring
//  F       bounded_overtaking_counting_lock.hpp  global fetch_add ticket;
//          (bo_*)                                K-bounded overtaking of
//                                                unregistered successors
//  G       heartbeat_counting_lock.hpp           global fetch_add ticket;
//          (hb_*)                                overtaking gated on waiter
//                                                heartbeats (liveness signal)
//  D       skew_filter_counting_lock.hpp         global fetch_add ticket;
//          (skew_*)                              HSW filter as OVERHEAD MODEL
//  E       (removed 2026-07-12)                  reverse-skew was behaviorally
//                                                identical to D — see D's file
//
// A is fully network-ordered. B/C are mixed: bitonic is network-ordered,
// periodic uses dense ticket fallback (periodic predecessor holes were
// observed under lock-shaped schedules). F/G keep global tickets ON PURPOSE —
// their overtaking/re-draw semantics require dense, immediately-known tokens.
// D keeps tickets because it is an overhead model with deliberately trivial
// ordering.
//
// The plain BitonicCountingLock/PeriodicCountingLock (bitonic_* /
// periodic_*, in bitonic_networks.hpp) are the family's predecessors:
// network-ordered like Design A, but with an O(n) waiting-thread scan at
// unlock; Design A replaced that scan with per-wire slot indexing.
// =============================================================================

#include "counting_lock_common.hpp"
#include "wire_indexed_counting_lock.hpp"
#include "sequenced_counting_lock.hpp"
#include "bounded_overtaking_counting_lock.hpp"
#include "heartbeat_counting_lock.hpp"
#include "waiting_filter_counting_lock.hpp"
#include "skew_filter_counting_lock.hpp"

// =============================================================================
// SECTION 5: Concrete Type Aliases (benchmark names)
// =============================================================================

// ── Design A: Wire-Indexed (network-ordered; O(1)/O(W) unlock) ──────────────
// Two aliases are still routed to fallback designs (marked FALLBACK): they
// were parked there while Design A hung deterministically. The dominant
// token-destruction race is fixed (2026-07-14), but residual timeout
// instability remains under stress (see wire_indexed_counting_lock.hpp), so
// the fallbacks stay until it is closed. NOTE the redundancy while they last:
// lw_bitonic_cas
// duplicates seq_bitonic_cas, lw_bitonic_bakery duplicates
// wf_bitonic_bakery — benchmarking them adds no information.
using LWBitonicCASLock      = SeqBitonicLock<BnCASSync>;      // FALLBACK (== seq_bitonic_cas)
using LWBitonicBLLock       = WireIndexedBitonicLock<BnBLSync>;
using LWBitonicLamportLock  = WireIndexedBitonicLock<BnLamportSync>;
using LWBitonicBakeryLock   = WFBitonicLock<BnBakerySync>;    // FALLBACK (== wf_bitonic_bakery)

using LWPeriodicCASLock      = WireIndexedPeriodicLock<BnCASSync>;
using LWPeriodicBLLock       = WireIndexedPeriodicLock<BnBLSync>;
using LWPeriodicLamportLock  = WireIndexedPeriodicLock<BnLamportSync>;
using LWPeriodicBakeryLock   = WireIndexedPeriodicLock<BnBakerySync>;

// ── Design B: Sequenced (network-ordered + slot handoff; O(1) unlock) ───────
// CAS-sync variants only, as a historical selection; with network-derived
// ordering (2026-07-14) software-sync variants would be coherent again —
// adding them is an open option.
using SeqBitonicCASLock      = SeqBitonicLock<BnCASSync>;
using SeqPeriodicCASLock     = SeqPeriodicLock<BnCASSync>;

// ── Design F: Bounded-overtaking — CAS only ─────────────────────────────────
using BoBitonicCASLock  = BoBitonicLock<BnCASSync>;
using BoPeriodicCASLock = BoPeriodicLock<BnCASSync>;

// ── Design G: Heartbeat-overtaking — CAS only ───────────────────────────────
using HbBitonicCASLock  = HbBitonicLock<BnCASSync>;
using HbPeriodicCASLock = HbPeriodicLock<BnCASSync>;

// ── Design C: Waiting-Filter (network-ordered + phase-bit ring; O(1) unlock) ─
// All four sync flavors. With network-derived ordering (2026-07-14) the
// non-CAS variants are coherent designs again — the balancers ARE the
// ordering mechanism, so the balancer sync layer is a legitimate axis
// (former redundancy R7 is dissolved).
using WFBitonicCASLock      = WFBitonicLock<BnCASSync>;
using WFBitonicBLLock       = WFBitonicLock<BnBLSync>;
using WFBitonicLamportLock  = WFBitonicLock<BnLamportSync>;
using WFBitonicBakeryLock   = WFBitonicLock<BnBakerySync>;

using WFPeriodicCASLock      = WFPeriodicLock<BnCASSync>;
using WFPeriodicBLLock       = WFPeriodicLock<BnBLSync>;
using WFPeriodicLamportLock  = WFPeriodicLock<BnLamportSync>;
using WFPeriodicBakeryLock   = WFPeriodicLock<BnBakerySync>;

// ── Design D: Skew-Filter (ticket + HSW-filter overhead model) — CAS only ───
using SkewBitonicCASLock      = SkewBitonicLock<BnCASSync>;
using SkewPeriodicCASLock     = SkewPeriodicLock<BnCASSync>;

#endif // LINEARIZABLE_COUNTING_LOCK_HPP
