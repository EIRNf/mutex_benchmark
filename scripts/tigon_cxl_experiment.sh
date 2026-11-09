#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
    cat <<'EOF'
Usage: scripts/tigon_cxl_experiment.sh [options]

Verify the Linux/CXL environment, build the project and Tigon, run allocator
smoke tests, and compare selected SoftwareMutex implementations.

Options:
  --skip-build              Skip Meson and Tigon builds
  --smoke-only              Run environment/build/smoke checks only
  --compare-only            Run the lock comparison only
  --smoke-backends LIST     Backends to probe (default: mmap,shm,cxl)
  --compare-backends LIST   Backends for comparison (default: mmap)
  --locks LIST              Locks (default: exp_spin,mcs,hclh,cohortTicket)
  --threads LIST            Thread counts (default: 1,2,4)
  --seconds N               Measured seconds per run (default: 10)
  --warmup N                Warmup seconds (default: 2)
  --cxl-node N              NUMA node for an additional membind probe
  --output DIR              Output root (default: data/tigon_cxl)
  -h, --help                Show this help

Examples:
  scripts/tigon_cxl_experiment.sh
  scripts/tigon_cxl_experiment.sh --cxl-node 2 --seconds 30
  scripts/tigon_cxl_experiment.sh --smoke-only --smoke-backends mmap,cxl
EOF
}

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

skip_build=0
smoke_only=0
compare_only=0
smoke_backends="mmap,shm,cxl"
compare_backends="mmap"
locks="exp_spin,mcs,hclh,cohortTicket"
threads="1,2,4"
seconds=10
warmup=2
cxl_node=""
output_root="$ROOT/data/tigon_cxl"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) skip_build=1 ;;
        --smoke-only) smoke_only=1 ;;
        --compare-only) compare_only=1 ;;
        --smoke-backends) smoke_backends="$2"; shift ;;
        --compare-backends) compare_backends="$2"; shift ;;
        --locks) locks="$2"; shift ;;
        --threads) threads="$2"; shift ;;
        --seconds) seconds="$2"; shift ;;
        --warmup) warmup="$2"; shift ;;
        --cxl-node) cxl_node="$2"; shift ;;
        --output) output_root="$2"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if (( smoke_only && compare_only )); then
    echo "error: --smoke-only and --compare-only are mutually exclusive" >&2
    exit 2
fi
if ! [[ "$seconds" =~ ^[0-9]+$ && "$warmup" =~ ^[0-9]+$ && "$seconds" -gt 0 ]]; then
    echo "error: --seconds and --warmup must be non-negative integers; seconds > 0" >&2
    exit 2
fi

run_timestamp="$(date -u +%Y%m%dT%H%M%S%NZ)"
output_dir="$output_root/$run_timestamp"
mkdir -p "$output_dir"
report="$output_dir/environment.txt"
results="$output_dir/results.csv"
smoke_results="$output_dir/smoke.csv"
log_dir="$output_dir/logs"
mkdir -p "$log_dir"
: > "$report"
: > "$smoke_results"

log() {
    printf '[%s] %s\n' "$(date -Is)" "$*" | tee -a "$report"
}
have() { command -v "$1" >/dev/null 2>&1; }
csv_items() { tr ',' ' ' <<< "$1"; }

log "repository: $ROOT"
log "kernel: $(uname -sr)"
log "architecture: $(uname -m)"
if [[ "$(uname -s)" != Linux || "$(uname -m)" != x86_64 ]]; then
    log "WARNING: Tigon's bundled cxlalloc archive is supported here only on Linux x86_64"
fi

for tool in meson ninja cmake numactl; do
    if have "$tool"; then
        log "$tool: $(command -v "$tool")"
    else
        log "WARNING: missing optional/required tool: $tool"
    fi
done

{
    echo
    echo "=== NUMA hardware ==="
    if have numactl; then numactl --hardware 2>&1 || true; else echo "numactl unavailable"; fi
    echo
    echo "=== DAX devices ==="
    if [[ -d /sys/class/dax ]]; then
        find /sys/class/dax -maxdepth 1 -mindepth 1 -printf '%f\n' | sort
    else
        echo "/sys/class/dax unavailable"
    fi
    echo
    echo "=== CXL devices ==="
    if have cxl; then cxl list 2>&1 || true; else echo "cxl utility unavailable"; fi
    echo
    echo "=== DAX control ==="
    if have daxctl; then daxctl list 2>&1 || true; else echo "daxctl utility unavailable"; fi
} | tee -a "$report"

if [[ -f "$ROOT/apps/tigon_db_bench/tigon/dependencies/cxlalloc/libcxlalloc_static.a" ]]; then
    log "cxlalloc archive: present"
    file "$ROOT/apps/tigon_db_bench/tigon/dependencies/cxlalloc/libcxlalloc_static.a" | tee -a "$report"
else
    log "ERROR: Tigon cxlalloc archive is missing"
fi

if (( ! compare_only && ! skip_build )); then
    log "initializing Tigon submodule"
    git submodule update --init apps/tigon_db_bench/tigon
    log "building the main project"
    meson setup build >/dev/null
    meson compile -C build
    log "running targeted preload smoke tests"
    meson test -C build --print-errorlogs --num-processes 1 \
        region_layout_test preload_victim_baseline preload_shim_exp_spin \
        preload_shim_system preload_shim_ticket preload_shim_scope_app \
        preload_shim_mcs
    log "building Tigon"
    ./apps/tigon_db_bench/build.sh
fi

tigon="$ROOT/build/apps/tigon_db_bench/bench_ycsb"
if [[ ! -x "$tigon" ]]; then
    log "ERROR: Tigon binary not found: $tigon"
    exit 1
fi

had_tigon_log=0
[[ -e "$ROOT/_non_group_commit.txt" ]] && had_tigon_log=1
cleanup() {
    if (( ! had_tigon_log )); then
        rm -f "$ROOT/_non_group_commit.txt"
    fi
}
trap cleanup EXIT

run_tigon() {
    local backend="$1" lock="$2" thread_count="$3" log_file="$4"
    shift 4
    local -a prefix=()
    if [[ -n "$cxl_node" ]]; then
        if ! have numactl; then
            echo "numactl unavailable" > "$log_file"
            return 125
        fi
        prefix=(numactl --membind="$cxl_node")
    fi
    CXLALLOC_BACKEND="$backend" \
        timeout --signal=TERM --kill-after=5s "$((seconds + warmup + 30))" \
        "${prefix[@]}" \
        "$ROOT/scripts/with_lock.sh" -S -n "$((thread_count * 4 < 128 ? 128 : thread_count * 4))" \
        "$lock" "$tigon" \
        --logtostderr=1 --id=0 --servers=127.0.0.1:1234 \
        --threads="$thread_count" --partition_num="$thread_count" \
        --time_to_run="$seconds" --time_to_warmup="$warmup" \
        --protocol=TwoPL --query=rmw --read_write_ratio=80 \
        --cross_ratio=0 --keys=200000 "$@" >"$log_file" 2>&1
}

parse_metric() {
    local log_file="$1"
    awk -F': ' '/average commit:/ { split($2, a, " "); value=a[1] } END { if (value == "") exit 1; print value }' "$log_file"
}

parse_stat() {
    local key="$1" log_file="$2"
    sed -n "s/^${key}=//p" "$log_file" | tail -1
}

if (( ! compare_only )); then
    echo "backend,status,average_commit,claimed_mutexes,shim_acquisitions,log" > "$smoke_results"
    for backend in $(csv_items "$smoke_backends"); do
        smoke_log="$log_dir/smoke-${backend}.log"
        log "smoke: backend=$backend"
        if run_tigon "$backend" exp_spin 1 "$smoke_log"; then
            average="$(parse_metric "$smoke_log" || echo NA)"
            claimed="$(parse_stat claimed_mutexes "$smoke_log" || echo NA)"
            acquired="$(parse_stat shim_acquisitions "$smoke_log" || echo NA)"
            printf '%s,pass,%s,%s,%s,%s\n' "$backend" "$average" "$claimed" "$acquired" "$smoke_log" | tee -a "$smoke_results"
        else
            rc=$?
            printf '%s,fail,NA,NA,NA,%s\n' "$backend" "$smoke_log" | tee -a "$smoke_results"
            log "WARNING: backend=$backend failed (exit=$rc); see $smoke_log"
        fi
    done
fi

if (( ! smoke_only )); then
    echo "backend,lock,threads,average_commit,logical_ops_per_sec,claimed_mutexes,shim_acquisitions,log" > "$results"
    for backend in $(csv_items "$compare_backends"); do
        for thread_count in $(csv_items "$threads"); do
            for lock in $(csv_items "$locks"); do
                result_log="$log_dir/${backend}-${lock}-t${thread_count}.log"
                log "comparison: backend=$backend lock=$lock threads=$thread_count"
                if run_tigon "$backend" "$lock" "$thread_count" "$result_log"; then
                    average="$(parse_metric "$result_log" || echo 0)"
                    logical="$(awk -v v="$average" 'BEGIN { printf "%.3f", v * 10 }')"
                    claimed="$(parse_stat claimed_mutexes "$result_log" || echo NA)"
                    acquired="$(parse_stat shim_acquisitions "$result_log" || echo NA)"
                    printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
                        "$backend" "$lock" "$thread_count" "$average" "$logical" \
                        "$claimed" "$acquired" "$result_log" | tee -a "$results"
                else
                    rc=$?
                    log "WARNING: comparison failed: backend=$backend lock=$lock threads=$thread_count exit=$rc"
                fi
            done
        done
    done

    if have python3; then
        python3 - "$results" "$output_dir/comparison.png" <<'PY'
import csv
import sys
from collections import defaultdict

try:
    import matplotlib.pyplot as plt
except ImportError:
    print("matplotlib unavailable; CSV was still written", file=sys.stderr)
    raise SystemExit(0)

rows = list(csv.DictReader(open(sys.argv[1], newline="")))
groups = defaultdict(list)
for row in rows:
    groups[(row["backend"], row["lock"])].append(row)

fig, ax = plt.subplots(figsize=(10, 6))
for (backend, lock), values in sorted(groups.items()):
    values.sort(key=lambda r: int(r["threads"]))
    ax.plot([int(r["threads"]) for r in values],
            [float(r["average_commit"]) for r in values],
            marker="o", label=f"{backend}/{lock}")
ax.set_xlabel("Tigon worker threads")
ax.set_ylabel("Committed transactions / second")
ax.set_title("Tigon YCSB RMW throughput by lock and allocator")
ax.grid(True, alpha=.3)
ax.legend(fontsize="small")
fig.tight_layout()
fig.savefig(sys.argv[2], dpi=160)
print(f"wrote {sys.argv[2]}")
PY
    fi
fi

log "smoke results: $smoke_results"
log "comparison results: $results"
log "environment report: $report"
