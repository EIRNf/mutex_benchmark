#!/bin/sh
# with_lock.sh — run any dynamically linked program with one of this repo's
# SoftwareMutex implementations transparently injected in place of its
# pthread mutexes (LD_PRELOAD on Linux, DYLD_INSERT_LIBRARIES on macOS).
#
# usage: scripts/with_lock.sh [options] <lock_name> <command> [args...]
#
#   -b DIR    build directory containing preload/libmutex_preload.* (default: build)
#   -n N      MUTEX_SHIM_MAX_THREADS — max concurrent threads (default: 128)
#   -s SCOPE  all|app|region — which mutexes to replace (default: all)
#   -S        dump interception stats at exit (stderr)
#   -f FILE   write stats to FILE (implies -S)
#   -v        log each claimed/ignored mutex
#
# examples:
#   scripts/with_lock.sh mcs ./my_server --port 8080
#   scripts/with_lock.sh -S -n 64 -s app wf_bitonic_cas ./sqlite_bench
#   scripts/with_lock.sh '' ./my_server        # shim loaded but inert (baseline)
set -eu

builddir=build
maxt=""
scope=""
stats=""
statsfile=""
log=""
while getopts "b:n:s:Sf:v" opt; do
    case $opt in
        b) builddir=$OPTARG ;;
        n) maxt=$OPTARG ;;
        s) scope=$OPTARG ;;
        S) stats=1 ;;
        f) statsfile=$OPTARG; stats=1 ;;
        v) log=1 ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))

if [ $# -lt 2 ]; then
    echo "usage: $0 [-b builddir] [-n max_threads] [-s all|app|region] [-S] [-f stats_file] [-v] <lock_name> <command> [args...]" >&2
    exit 2
fi
lock=$1
shift

case "$(uname)" in
    Darwin) shim="$builddir/preload/libmutex_preload.dylib"; var=DYLD_INSERT_LIBRARIES ;;
    *)      shim="$builddir/preload/libmutex_preload.so";    var=LD_PRELOAD ;;
esac
if [ ! -f "$shim" ]; then
    echo "error: $shim not found — build it first: meson setup build && meson compile -C build" >&2
    exit 1
fi
# Resolve to an absolute path so the target's cwd doesn't matter.
shim=$(cd "$(dirname "$shim")" && pwd)/$(basename "$shim")

MUTEX_SHIM_LOCK=$lock;                                 export MUTEX_SHIM_LOCK
[ -n "$maxt" ]      && { MUTEX_SHIM_MAX_THREADS=$maxt; export MUTEX_SHIM_MAX_THREADS; }
[ -n "$scope" ]     && { MUTEX_SHIM_SCOPE=$scope;      export MUTEX_SHIM_SCOPE; }
[ -n "$stats" ]     && { MUTEX_SHIM_STATS=1;           export MUTEX_SHIM_STATS; }
[ -n "$statsfile" ] && { MUTEX_SHIM_STATS_FILE=$statsfile; export MUTEX_SHIM_STATS_FILE; }
[ -n "$log" ]       && { MUTEX_SHIM_LOG=1;             export MUTEX_SHIM_LOG; }

# Setting the DYLD_/LD_ variable here (rather than inheriting it through a
# protected binary) is what makes this work under macOS SIP: the target we
# exec must simply be a non-platform, non-hardened binary.
export "$var=$shim"
exec "$@"
