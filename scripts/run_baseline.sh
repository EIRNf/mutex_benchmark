#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/run_baseline.sh [options]

Run NUMA-bound DRAM or CXL comparisons for the benchmark applications.

Options:
  --memory KIND       dram or cxl (default: cxl)
  --node N            NUMA node to bind to (default: 1 for cxl, 0 for dram)
  --threads N         Maximum thread count (default: 64)
  --skip N            Thread sweep step (default: 1)
  --thread-step N     Alias for --skip
  --seconds N         Measured seconds per point (default: 10)
  --repetitions N     Repetitions per point (default: 3)
  --locks LIST        Comma-separated lock names (default: paper)
  --apps LIST         tigon_ycsb,max,min,grouped
  --groups N          Group count for grouped (default: 2)
  --output DIR        Output root (default depends on --memory)
  --skip-build        Do not build before running
  -h, --help          Show this help

Examples:
  scripts/run_baseline.sh --memory cxl --node 1 --skip 4
  scripts/run_baseline.sh --memory dram --node 0 --skip 4
  scripts/run_baseline.sh --memory cxl --apps tigon_ycsb,max
EOF
}

gcd() {
    local a="$1" b="$2" remainder
    while (( b != 0 )); do
        remainder=$((a % b))
        a="$b"
        b="$remainder"
    done
    printf '%s\n' "$a"
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

memory=cxl
node=1
max_threads=64
thread_step=1
seconds=10
repetitions=3
locks=paper
apps=""
groups=2
output_root=""
skip_build=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --memory) memory="$2"; shift ;;
        --node) node="$2"; shift ;;
        --threads) max_threads="$2"; shift ;;
        --skip|--thread-step) thread_step="$2"; shift ;;
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

if [[ "$memory" != dram && "$memory" != cxl ]]; then
    echo "error: --memory must be dram or cxl" >&2
    exit 2
fi
if ! [[ "$node" =~ ^[0-9]+$ && "$max_threads" =~ ^[0-9]+$ &&
        "$thread_step" =~ ^[0-9]+$ && "$seconds" =~ ^[0-9]+$ &&
        "$repetitions" =~ ^[0-9]+$ && "$groups" =~ ^[0-9]+$ &&
        "$node" -ge 0 && "$max_threads" -gt 0 &&
        "$thread_step" -gt 0 && "$seconds" -gt 0 &&
        "$repetitions" -gt 0 && "$groups" -gt 0 &&
        "$thread_step" -le "$max_threads" ]]; then
    echo "error: node, threads, skip, seconds, repetitions, and groups are invalid" >&2
    exit 2
fi
if ! command -v numactl >/dev/null 2>&1; then
    echo "error: numactl is required for explicit NUMA placement" >&2
    exit 1
fi

if [[ -z "$apps" ]]; then
    if [[ "$memory" == cxl ]]; then
        apps=tigon_ycsb,max,min,grouped
    else
        apps=tigon_ycsb,max,min
    fi
fi
if [[ -z "$output_root" ]]; then
    output_root="$ROOT/data/${memory}_baseline"
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
    if [[ "$app" == grouped && "$memory" == dram ]]; then
        echo "error: grouped is only included in the CXL baseline by default; use --memory cxl" >&2
        exit 2
    fi
done

run_timestamp="$(date -u +%Y%m%dT%H%M%S%NZ)"
run_root="$output_root/$run_timestamp"
mkdir -p "$run_root"

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
    echo "==> Running $app on $memory NUMA node $node"

    extra_args=()
    sweep_args=(--sweep threads "$thread_step" "$max_threads" "$thread_step")
    if [[ "$app" == grouped ]]; then
        grouped_step=$((thread_step * groups / $(gcd "$thread_step" "$groups")))
        grouped_start="$grouped_step"
        grouped_stop=$((max_threads / grouped_step * grouped_step))
        if (( grouped_stop < grouped_start )); then
            echo "error: --threads must be at least $grouped_start for grouped" >&2
            exit 2
        fi
        extra_args+=(--groups "$groups")
        sweep_args=(--sweep threads "$grouped_start" "$grouped_stop" "$grouped_step")
    fi

    numactl --membind="$node" env \
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
$memory NUMA baseline complete.

Applications:
  $apps
Results and plots:
  $run_root/
Placement:
  numactl --membind=$node
Tigon allocator:
  CXLALLOC_BACKEND=mmap
EOF
