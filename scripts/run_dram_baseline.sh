#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run_dram_baseline.sh [options]

Run comparable DRAM-baseline sweeps for Tigon YCSB, max_contention_bench,
and min_contention_bench, using the paper lock list and generating plots.

Options:
  --dram-node N       NUMA node to bind the experiment to (default: 0)
  --threads N         Maximum thread count (default: 8)
  --seconds N         Measured seconds per point (default: 30)
  --repetitions N     Repetitions per point (default: 3)
  --skip N            Thread sweep step (default: 1)
  --thread-step N     Alias for --skip
  --locks LIST        Comma-separated lock names (default: paper)
  --output DIR        Output root (default: data/dram_baseline)
  --skip-build        Do not build before running
  --tigon-only        Run only the Tigon sweep
  --contention-only   Run only max and min sweeps
  -h, --help          Show this help

Examples:
  scripts/run_dram_baseline.sh
  scripts/run_dram_baseline.sh --dram-node 0 --threads 4 --seconds 10
  scripts/run_dram_baseline.sh --tigon-only --locks exp_spin,mcs,hclh

The Tigon counterpart for a native CXL allocator is:
  CXLALLOC_BACKEND=cxl scripts/tigon_cxl_experiment.sh \
    --skip-build --compare-only --compare-backends cxl
EOF
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

dram_node=0
max_threads=64
seconds=10
repetitions=3
thread_step=1
locks="paper"
output_root="$ROOT/data/dram_baseline"
skip_build=0
tigon_only=0
contention_only=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dram-node) dram_node="$2"; shift ;;
        --threads) max_threads="$2"; shift ;;
        --seconds) seconds="$2"; shift ;;
        --repetitions) repetitions="$2"; shift ;;
        --skip|--thread-step) thread_step="$2"; shift ;;
        --locks) locks="$2"; shift ;;
        --output) output_root="$2"; shift ;;
        --skip-build) skip_build=1 ;;
        --tigon-only) tigon_only=1 ;;
        --contention-only) contention_only=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if (( tigon_only && contention_only )); then
    echo "error: --tigon-only and --contention-only are mutually exclusive" >&2
    exit 2
fi
if ! [[ "$dram_node" =~ ^[0-9]+$ && "$max_threads" =~ ^[0-9]+$ &&
        "$seconds" =~ ^[0-9]+$ && "$repetitions" =~ ^[0-9]+$ &&
"$max_threads" -gt 0 && "$seconds" -gt 0 && "$repetitions" -gt 0 &&
"$thread_step" =~ ^[0-9]+$ && "$thread_step" -gt 0 ]]; then
    echo "error: node, threads, seconds, and skip must be positive integers" >&2
    exit 2
fi
if ! command -v numactl >/dev/null 2>&1; then
    echo "error: numactl is required to make the DRAM placement explicit" >&2
    exit 1
fi

run_timestamp="$(date -u +%Y%m%dT%H%M%S%NZ)"
run_root="$output_root/$run_timestamp"
mkdir -p "$run_root/tigon_ycsb" "$run_root/max" "$run_root/min"
if [[ "$locks" == paper ]]; then
    lock_args=(-s paper)
else
    IFS=',' read -r -a lock_list <<< "$locks"
    lock_args=(-i "${lock_list[@]}")
fi

if (( ! skip_build )); then
    echo "==> Verifying NUMA node $dram_node"
    numactl --hardware
    echo "==> Building main project and Tigon"
    meson setup build >/dev/null
    meson compile -C build
    ./apps/tigon_db_bench/build.sh
fi

if (( ! contention_only )); then
    echo "==> Running Tigon YCSB DRAM baseline on NUMA node $dram_node"
    numactl --membind="$dram_node" env \
        CXLALLOC_BACKEND=mmap MUTEX_BENCH_SKIP_BUILD="$skip_build" \
        python3 -m scripts.main "$max_threads" "$seconds" "$repetitions" \
        --bench tigon_ycsb \
        --capture throughput \
        --sweep threads "$thread_step" "$max_threads" "$thread_step" \
        --plot faceted \
        --data-folder "$run_root/tigon_ycsb" \
        "${lock_args[@]}"
fi

if (( ! tigon_only )); then
    echo "==> Running max contention DRAM baseline on NUMA node $dram_node"
    numactl --membind="$dram_node" env MUTEX_BENCH_SKIP_BUILD="$skip_build" \
        python3 -m scripts.main "$max_threads" "$seconds" "$repetitions" \
        --bench max \
        --alloc malloc \
        --capture throughput \
        --sweep threads "$thread_step" "$max_threads" "$thread_step" \
        --plot faceted \
        --data-folder "$run_root/max" \
        "${lock_args[@]}"

    echo "==> Running min contention DRAM baseline on NUMA node $dram_node"
    numactl --membind="$dram_node" env MUTEX_BENCH_SKIP_BUILD="$skip_build" \
        python3 -m scripts.main "$max_threads" "$seconds" "$repetitions" \
        --bench min \
        --alloc malloc \
        --capture throughput \
        --sweep threads "$thread_step" "$max_threads" "$thread_step" \
        --plot faceted \
        --data-folder "$run_root/min" \
        "${lock_args[@]}"
fi

cat <<EOF
DRAM baseline complete.

Data and plots:
  $run_root/tigon_ycsb/
  $run_root/max/
  $run_root/min/

All runs were bound with:
  numactl --membind=$dram_node
EOF
