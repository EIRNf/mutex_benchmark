#!/bin/sh
# run_preload_test.sh <shim_path> <victim_path> <lock_name> [scope]
#
# Runs the preload victim app under the shim with the given lock and verifies
# both the victim's own correctness check AND that the shim actually
# intercepted work (claimed mutexes + acquisition counts from the stats dump)
# — otherwise a silently inert shim would pass vacuously.
#
# Note (macOS/SIP): DYLD_* variables are stripped when a *protected* binary
# is exec'd, but setting the variable inside this script and exec'ing our own
# (unprotected) victim binary works fine.
set -eu

shim="$1"
victim="$2"
lock="$3"
scope="${4:-all}"

stats="${TMPDIR:-/tmp}/mutex_shim_stats.$$"
trap 'rm -f "$stats"' EXIT

MUTEX_SHIM_LOCK="$lock" ; export MUTEX_SHIM_LOCK
MUTEX_SHIM_SCOPE="$scope" ; export MUTEX_SHIM_SCOPE
MUTEX_SHIM_STATS=1 ; export MUTEX_SHIM_STATS
MUTEX_SHIM_STATS_FILE="$stats" ; export MUTEX_SHIM_STATS_FILE
# Small on purpose: the victim's churn phase spawns 96 sequential threads, so
# this passes only if the shim recycles thread ids at thread exit.
MUTEX_SHIM_MAX_THREADS=32 ; export MUTEX_SHIM_MAX_THREADS

case "$(uname)" in
  Darwin) DYLD_INSERT_LIBRARIES="$shim" ; export DYLD_INSERT_LIBRARIES ;;
  *)      LD_PRELOAD="$shim" ; export LD_PRELOAD ;;
esac

nthreads=4
iters=20000

if ! "$victim" "$nthreads" "$iters"; then
    echo "FAIL: victim reported incorrect counters under lock=$lock scope=$scope" >&2
    [ -f "$stats" ] && cat "$stats" >&2
    exit 1
fi

if [ ! -s "$stats" ]; then
    echo "FAIL: shim produced no stats dump (not loaded at all?)" >&2
    exit 1
fi

claimed=$(sed -n 's/^claimed_mutexes=//p' "$stats")
acq=$(sed -n 's/^shim_acquisitions=//p' "$stats")
cond_waits=$(sed -n 's/^cond_waits=//p' "$stats")

# The victim uses 5 distinct app mutexes; expect at least those in scope=all.
min_claimed=4
# static + dyn + rec(x3 outer) + try + queue ops all go through the shim:
# >= 5 mutex families * nthreads * iters is a safe floor.
min_acq=$((5 * nthreads * iters))

[ "${claimed:-0}" -ge "$min_claimed" ] || {
    echo "FAIL: expected >= $min_claimed claimed mutexes, got '${claimed:-}'" >&2
    cat "$stats" >&2
    exit 1
}
[ "${acq:-0}" -ge "$min_acq" ] || {
    echo "FAIL: expected >= $min_acq shim acquisitions, got '${acq:-}'" >&2
    cat "$stats" >&2
    exit 1
}
[ "${cond_waits:-0}" -ge 1 ] || {
    echo "FAIL: expected condvar waits to route through the shim, got '${cond_waits:-}'" >&2
    cat "$stats" >&2
    exit 1
}

echo "PRELOAD TEST OK lock=$lock scope=$scope claimed=$claimed acquisitions=$acq cond_waits=$cond_waits"
