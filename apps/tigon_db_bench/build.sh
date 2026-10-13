#!/usr/bin/env bash
# build.sh — build Tigon benchmarks with pthread_mutex_t latch redirection enabled
# so that the repo's libmutex_preload shim can intercept and benchmark its latches.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build/apps/tigon_db_bench}"
[[ $# -gt 0 ]] && shift
TIGON_DIR="$SCRIPT_DIR/tigon"
PATCH="$SCRIPT_DIR/patches/0001-mutex-benchmark-latch-redirection.patch"
CMAKE_BIN="${CMAKE_BIN:-/usr/bin/cmake}"
if [[ ! -x "$CMAKE_BIN" ]]; then
    CMAKE_BIN="$(command -v cmake)"
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "error: Tigon only builds on Linux (x86_64): the bundled libcxlalloc_static.a is an ELF archive" >&2
    echo "       and Tigon relies on Linux-specific APIs. Build inside a Linux container/VM," >&2
    echo "       with libjemalloc-dev, libgoogle-glog-dev and libgflags-dev installed." >&2
    exit 1
fi

echo "==> Preparing Tigon submodule..."
git -C "$REPO_ROOT" submodule update --init apps/tigon_db_bench/tigon
if git -C "$TIGON_DIR" apply --check "$PATCH" 2>/dev/null; then
    git -C "$TIGON_DIR" apply "$PATCH"
    echo "    applied $(basename "$PATCH")"
elif git -C "$TIGON_DIR" apply --check -R "$PATCH" 2>/dev/null; then
    echo "    patch already applied"
else
    echo "error: $(basename "$PATCH") neither applies nor is already applied to $TIGON_DIR" >&2
    exit 1
fi

echo "==> Configuring Tigon in $BUILD_DIR..."
mkdir -p "$BUILD_DIR"

"$CMAKE_BIN" -S "$SCRIPT_DIR/tigon" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DTIGON_USE_PTHREAD_MUTEX=ON \
    "$@"

NCPU=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
echo "==> Building Tigon with $NCPU jobs..."
"$CMAKE_BIN" --build "$BUILD_DIR" -j "$NCPU"

echo "==> Tigon build complete: $BUILD_DIR/bench_ycsb, $BUILD_DIR/bench_tpcc"
