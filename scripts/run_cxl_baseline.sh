#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run_cxl_baseline.sh [options]

Run NUMA-bound CXL comparisons for Tigon YCSB and the contention applications.
This intentionally uses malloc/mmap plus numactl placement rather than the
unsupported native CXLALLOC_BACKEND=cxl backend.

Options:
  --cxl-node N        NUMA node backed by CXL memory (default: 1)
  --threads N         Maximum thread count (default: 8)
  --seconds N         Measured seconds per point (default: 30)
  --repetitions N     Repetitions per point (default: 3)
  --locks LIST        Comma-separated lock names (default: paper)
  --apps LIST         tigon_ycsb,max,min,grouped (default: all)
  --groups N          Group count for grouped (default: 2)
  --output DIR        Output root (default: data/cxl_baseline)
  --skip-build        Do not build before running
  -h, --help          Show this help

Example:
  scripts/run_cxl_baseline.sh --cxl-node 1 --threads 8 --seconds 30
  scripts/run_cxl_baseline.sh --apps tigon_ycsb,max --locks exp_spin,cxl_fast_exp
EOF
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

cxl_node=1
max_threads=64
seconds=10
repetitions=3
locks=paper
apps=tigon_ycsb,max,min,grouped
groups=2
output_root="$ROOT/data/cxl_baseline"
skip_build=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --cxl-node) cxl_node="$2"; shift ;;
        --threads) max_threads="$2"; shift ;;
        --seconds) seconds="$2"; shift ;;
        --repetitions) repetitions="$2"; shift ;;
        --locks) locks="$2"; shift ;;
        --apps) apps="$2"; shift ;;
        --groups) groups="$2"; shift ;;
        --output) output_root="$2"; shift ;;
        --skip-build) skip_build=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if ! [[ "$cxl_node" =~ ^[0-9]+$ && "$max_threads" =~ ^[0-9]+$ &&
        "$seconds" =~ ^[0-9]+$ && "$repetitions" =~ ^[0-9]+$ &&
        "$groups" =~ ^[0-9]+$ && "$max_threads" -gt 0 &&
        "$seconds" -gt 0 && "$repetitions" -gt 0 && "$groups" -gt 0 ]]; then
    echo "error: node, threads, seconds, repetitions, and groups must be positive integers" >&2
    exit 2
fi
if ! command -v numactl >/dev/null 2>&1; then
    echo "error: numactl is required for CXL NUMA placement" >&2
    exit 1
fi

declare -A valid_apps=(
    [tigon_ycsb]=1
    [max]=1
    [min]=1
    [grouped]=1
)
IFS=',' read -r -a app_list <<< "$apps"
for app in "${app_list[@]}"; do
    if [[ -z "${valid_apps[$app]:-}" ]]; then
        echo "error: unsupported application '$app'" >&2
        exit 2
    fi
done

run_timestamp="$(date -u +%Y%m%dT%H%M%S%NZ)"
run_root="$output_root/$run_timestamp"
if [[ "$locks" == paper ]]; then
    lock_args=(-s paper)
else
    IFS=',' read -r -a lock_list <<< "$locks"
    lock_args=(-i "${lock_list[@]}")
fi

if (( ! skip_build )); then
    echo "==> NUMA hardware"
    numactl --hardware
    echo "==> Building main project and Tigon"
    meson setup build >/dev/null
    meson compile -C build
    ./apps/tigon_db_bench/build.sh
fi

for app in "${app_list[@]}"; do
    app_output="$run_root/$app"
    mkdir -p "$app_output"
    echo "==> Running $app on CXL NUMA node $cxl_node"

    extra_args=()
    sweep_args=(--sweep threads 1 "$max_threads" 1)
    if [[ "$app" == grouped ]]; then
        grouped_stop=$((max_threads / groups * groups))
        if (( grouped_stop < groups )); then
            echo "error: --threads must be at least --groups for grouped" >&2
            exit 2
        fi
        extra_args+=(--groups "$groups")
        sweep_args=(--sweep threads "$groups" "$grouped_stop" "$groups")
    fi

    numactl --membind="$cxl_node" env \
        CXLALLOC_BACKEND=mmap MUTEX_BENCH_SKIP_BUILD="$skip_build" \
        python3 -m scripts.main "$max_threads" "$seconds" "$repetitions" \
        --bench "$app" \
        --alloc malloc \
        --capture throughput \
        "${sweep_args[@]}" \
        --plot faceted \
        --data-folder "$app_output" \
        "${extra_args[@]}" \
        "${lock_args[@]}"
done

cat <<EOF
CXL NUMA baseline complete.

Applications:
  $apps
Results and plots:
  $run_root/
Placement:
  numactl --membind=$cxl_node
Tigon allocator:
  CXLALLOC_BACKEND=mmap
EOF
