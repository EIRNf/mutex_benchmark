#ifndef COUNTING_LOCK_COMMON_HPP
#define COUNTING_LOCK_COMMON_HPP

#pragma once

// Shared spin primitives for the linearizable-counting lock family
// (wire_indexed_/sequenced_/bounded_overtaking_/heartbeat_/waiting_filter_/
// skew_filter_counting_lock.hpp). See linearizable_counting_lock.hpp for the
// family index and LINEARIZABLE_COUNTING_ANALYSIS.md for theory; measured
// comparisons live in COUNTING_LOCKS.md.

#include <sched.h>

// ─── Spin-wait hint ──────────────────────────────────────────────────────────
#if defined(__aarch64__) || defined(_M_ARM64)
  #define LcSpinHint() __asm__ volatile("yield")
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
  #define LcSpinHint() __asm__ volatile("pause")
#else
  #define LcSpinHint() ((void)0)
#endif

// Long-wait spin: pure spinning collapses once runnable threads outnumber
// the (performance) cores — a spinner burns the cycles its predecessor in
// the handoff chain needs to reach its own unlock. Yielding every ~1K
// iterations is unreachable on the fast path (uncontended waits finish in
// far fewer spins) and restores progress under saturation. Ordering and
// correctness are untouched: who gets the lock next is already fixed by
// the token protocol; this only affects when the waiter's core is ceded.
#define LcSpinWait(spins) \
    do { if (((++(spins)) & 1023) == 0) sched_yield(); else LcSpinHint(); } while (0)

#endif // COUNTING_LOCK_COMMON_HPP
