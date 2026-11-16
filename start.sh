#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

# Run configuration; each value can be overridden in the environment.
RUN_TIME="${RUN_TIME:-10}"
RUNS="${RUNS:-1}"
THREAD_START="${THREAD_START:-1}"
MAX_THREADS="${MAX_THREADS:-42}"
THREAD_INTERVAL="${THREAD_INTERVAL:-4}"
DRAM_NODE="${DRAM_NODE:-0}"
CXL_NODE="${CXL_NODE:-1}"
OUTPUT_ROOT="${OUTPUT_ROOT:-$ROOT/data/master_runs}"

LOCK_SET="${LOCK_SET:-paper}"
TIGON_LOCKS="${TIGON_LOCKS:-exp_spin,ticket,mcs,cxl_fast_exp}"
LOCK_SET_OVERRIDE=0
TIGON_YCSB_QUERY="${TIGON_YCSB_QUERY:-rmw}"
TIGON_TPCC_QUERY="${TIGON_TPCC_QUERY:-mixed}"
TIGON_PROTOCOL="${TIGON_PROTOCOL:-TwoPL}"
TIGON_WARMUP="${TIGON_WARMUP:-}"
TIGON_READ_WRITE_RATIO="${TIGON_READ_WRITE_RATIO:-80}"
TIGON_READ_ONLY_RATIO="${TIGON_READ_ONLY_RATIO:-0}"
TIGON_CROSS_RATIO="${TIGON_CROSS_RATIO:-0}"
TIGON_CROSS_PARTITIONS="${TIGON_CROSS_PARTITIONS:-2}"
TIGON_KEYS="${TIGON_KEYS:-200000}"
TIGON_ZIPF="${TIGON_ZIPF:-0}"
TIGON_NEWORDER_DIST="${TIGON_NEWORDER_DIST:-10}"
TIGON_PAYMENT_DIST="${TIGON_PAYMENT_DIST:-15}"
TIGON_PARTITION_MODES="${TIGON_PARTITION_MODES:-threads,half,quarter}"

# Without a PCI BDF, default to running CXL at the host's current latency.
# Supplying a BDF preserves the prior default of testing both adjusted values.
CXL_LATENCY_PCI="${CXL_LATENCY_PCI:-}"
if [[ -n "$CXL_LATENCY_PCI" ]]; then
    DEFAULT_RUN_CXL_UNMODIFIED=0
    DEFAULT_RUN_CXL_LOW=1
    DEFAULT_RUN_CXL_HIGH=1
else
    DEFAULT_RUN_CXL_UNMODIFIED=1
    DEFAULT_RUN_CXL_LOW=0
    DEFAULT_RUN_CXL_HIGH=0
fi

RUN_NATIVE="${RUN_NATIVE:-1}"
RUN_TIGON="${RUN_TIGON:-1}"
RUN_DRAM="${RUN_DRAM:-1}"
RUN_CXL_UNMODIFIED="${RUN_CXL_UNMODIFIED:-$DEFAULT_RUN_CXL_UNMODIFIED}"
RUN_CXL_LOW="${RUN_CXL_LOW:-$DEFAULT_RUN_CXL_LOW}"
RUN_CXL_HIGH="${RUN_CXL_HIGH:-$DEFAULT_RUN_CXL_HIGH}"
SKIP_BUILD="${SKIP_BUILD:-0}"
DRY_RUN="${DRY_RUN:-0}"
LIST_RUNS=0

# These are platform-specific PCI config writes. Set the verified BDF for the
# CXL latency control on this host; never assume a copied BDF is correct.
CXL_LATENCY_REGISTER="${CXL_LATENCY_REGISTER:-0xe08.l}"
CXL_LATENCY_LOW="${CXL_LATENCY_LOW:-0020}"
CXL_LATENCY_HIGH="${CXL_LATENCY_HIGH:-ffff}"
CGROUP_PROCS="${CGROUP_PROCS-/sys/fs/cgroup/a/cgroup.procs}"

usage() {
    cat <<'EOF'
Usage: start.sh [--list-runs] [--lock-set NAME]

Configure runs through environment variables, for example:
  THREAD_START=4 MAX_THREADS=64 THREAD_INTERVAL=4 RUN_TIME=20 RUNS=3 ./start.sh
  DRY_RUN=1 ./start.sh
  THREAD_START=1 MAX_THREADS=64 THREAD_INTERVAL=4 RUN_TIME=10 RUNS=2 ./start.sh --list-runs
  ./start.sh --lock-set champions

If CXL_LATENCY_PCI is unset, the script runs CXL at the host's current
latency setting and makes no PCI configuration changes. To test adjusted
latencies, export CXL_LATENCY_PCI with the verified PCI BDF for this host.
Core settings: THREAD_START, MAX_THREADS, THREAD_INTERVAL, RUN_TIME, RUNS,
DRAM_NODE, CXL_NODE, OUTPUT_ROOT, LOCK_SET, TIGON_LOCKS, TIGON_PARTITION_MODES.
Toggle suites with RUN_NATIVE, RUN_TIGON, RUN_DRAM, RUN_CXL_UNMODIFIED,
RUN_CXL_LOW, RUN_CXL_HIGH.
--list-runs lists the configured benchmark matrix and estimates timed workload
duration without starting benchmarks or creating output directories.
--lock-set NAME selects a named mutex set for both native and Tigon runs.
EOF
}

resolve_lock_set_csv() {
    python3 - "$1" <<'PY'
import sys
from scripts.constants import Constants

names = Constants.Defaults.MUTEX_SETS.get(sys.argv[1])
if names is None:
    available = ", ".join(sorted(Constants.Defaults.MUTEX_SETS))
    print(f"error: unknown lock set '{sys.argv[1]}'; available sets: {available}", file=sys.stderr)
    raise SystemExit(2)
print(",".join(dict.fromkeys(names)))
PY
}

while (( $# > 0 )); do
    case "$1" in
    -h|--help)
        usage
        exit 0
        ;;
    --list-runs)
        LIST_RUNS=1
        shift
        ;;
    --lock-set)
        if (( $# < 2 )) || [[ -z "$2" || "$2" == -* ]]; then
            echo "error: --lock-set requires a set name" >&2
            usage >&2
            exit 2
        fi
        if (( LOCK_SET_OVERRIDE )); then
            echo "error: --lock-set may only be specified once" >&2
            exit 2
        fi
        LOCK_SET="$2"
        LOCK_SET_OVERRIDE=1
        shift 2
        ;;
    *)
        echo "error: unexpected argument: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

if (( LOCK_SET_OVERRIDE )); then
    if ! resolved_lock_set="$(resolve_lock_set_csv "$LOCK_SET")"; then
        exit 2
    fi
    TIGON_LOCKS="$resolved_lock_set"
fi

for value_name in RUN_TIME RUNS THREAD_START MAX_THREADS THREAD_INTERVAL DRAM_NODE CXL_NODE; do
    value="${!value_name}"
    if ! [[ "$value" =~ ^(0|[1-9][0-9]*)$ ]]; then
        echo "error: $value_name must be a non-negative integer" >&2
        exit 2
    fi
done
if (( RUN_TIME < 1 || RUNS < 1 || THREAD_START < 1 || MAX_THREADS < THREAD_START ||
      THREAD_INTERVAL < 1 )); then
    echo "error: require RUN_TIME/RUNS/THREAD_START/THREAD_INTERVAL > 0 and MAX_THREADS >= THREAD_START" >&2
    exit 2
fi
if [[ -z "$TIGON_WARMUP" ]]; then
    TIGON_WARMUP=1
    if (( RUN_TIME == 1 )); then
        TIGON_WARMUP=0
    fi
fi
for flag_name in RUN_NATIVE RUN_TIGON RUN_DRAM RUN_CXL_UNMODIFIED \
    RUN_CXL_LOW RUN_CXL_HIGH SKIP_BUILD DRY_RUN; do
    if [[ "${!flag_name}" != 0 && "${!flag_name}" != 1 ]]; then
        echo "error: $flag_name must be 0 or 1" >&2
        exit 2
    fi
done
if (( RUN_TIGON )); then
    for value_name in TIGON_WARMUP TIGON_READ_WRITE_RATIO TIGON_READ_ONLY_RATIO \
        TIGON_CROSS_RATIO TIGON_CROSS_PARTITIONS TIGON_KEYS TIGON_NEWORDER_DIST \
        TIGON_PAYMENT_DIST; do
        value="${!value_name}"
        if ! [[ "$value" =~ ^(0|[1-9][0-9]*)$ ]]; then
            echo "error: $value_name must be a non-negative integer" >&2
            exit 2
        fi
    done
    if (( TIGON_READ_WRITE_RATIO > 100 || TIGON_READ_ONLY_RATIO > 100 ||
          TIGON_CROSS_RATIO > 100 || TIGON_CROSS_PARTITIONS < 1 || TIGON_KEYS < 1 ||
          TIGON_NEWORDER_DIST > 100 || TIGON_PAYMENT_DIST > 100 )); then
        echo "error: invalid Tigon ratio, partition, or key setting" >&2
        exit 2
    fi
    if ! python3 - "$TIGON_ZIPF" <<'PY'
import math
import sys

try:
    value = float(sys.argv[1])
except ValueError:
    raise SystemExit(1)
raise SystemExit(0 if math.isfinite(value) and value >= 0 else 1)
PY
    then
        echo "error: TIGON_ZIPF must be a finite number >= 0" >&2
        exit 2
    fi
    case "$TIGON_YCSB_QUERY" in
        mixed|rmw|scan|insert|delete) ;;
        *) echo "error: unsupported TIGON_YCSB_QUERY '$TIGON_YCSB_QUERY'" >&2; exit 2 ;;
    esac
    case "$TIGON_TPCC_QUERY" in
        mixed|first_two|neworder|payment|test) ;;
        *) echo "error: unsupported TIGON_TPCC_QUERY '$TIGON_TPCC_QUERY'" >&2; exit 2 ;;
    esac
    case "$TIGON_PROTOCOL" in
        Silo|SiloGC|Star|Sundial|TwoPL|TwoPLGC|Calvin|HStore|Aria|TwoPLPasha|SundialPasha) ;;
        *) echo "error: unsupported TIGON_PROTOCOL '$TIGON_PROTOCOL'" >&2; exit 2 ;;
    esac
    IFS=',' read -r -a validated_partition_modes <<< "$TIGON_PARTITION_MODES"
    if (( ${#validated_partition_modes[@]} == 0 )); then
        echo "error: TIGON_PARTITION_MODES must not be empty" >&2
        exit 2
    fi
    if [[ -z "$TIGON_LOCKS" ]]; then
        echo "error: TIGON_LOCKS must not be empty when RUN_TIGON=1" >&2
        exit 2
    fi
    for ratio in "${validated_partition_modes[@]}"; do
        case "$ratio" in
            threads|half|quarter) ;;
            *) echo "error: unsupported Tigon partition mode '$ratio'" >&2; exit 2 ;;
        esac
    done
    if (( ! RUN_NATIVE && ! RUN_TIGON )); then
        echo "error: enable at least one benchmark suite (RUN_NATIVE or RUN_TIGON)" >&2
        exit 2
    fi
fi

PLAN_GROUPS=0
PLAN_EXECUTIONS=0
PLAN_ESTIMATED_SECONDS=0
PLAN_THREAD_COUNT=0
PLAN_THREAD_VALUES=""
PLAN_NATIVE_LOCK_COUNT=0
PLAN_TIGON_LOCK_COUNT=0

add_planned_group() {
    local category="$1" app="$2" node="$3" latency="$4"
    local locks="$5" lock_count="$6" workload="$7"
    local executions seconds_per_execution

    executions=$((lock_count * PLAN_THREAD_COUNT * RUNS))
    seconds_per_execution="$RUN_TIME"
    if [[ "$app" == tigon_* ]]; then
        seconds_per_execution=$((seconds_per_execution + TIGON_WARMUP))
    fi

    PLAN_GROUPS=$((PLAN_GROUPS + 1))
    PLAN_EXECUTIONS=$((PLAN_EXECUTIONS + executions))
    PLAN_ESTIMATED_SECONDS=$((PLAN_ESTIMATED_SECONDS + executions * seconds_per_execution))
    printf '  %2d. category=%s | app=%s | node=%s | latency=%s | locks={%s} | %s | %d executions (%d locks x %d threads x %d repetitions)\n' \
        "$PLAN_GROUPS" "$category" "$app" "$node" "$latency" "$locks" "$workload" \
        "$executions" "$lock_count" "$PLAN_THREAD_COUNT" "$RUNS"
}

list_planned_runs() {
    local thread app query ratio mode category node latency lock_names mode_index
    local native_lock_csv sweep_values partition_counts partition_count workload
    local -a modes=() nodes=() latencies=()
    local -a native_lock_names=() tigon_lock_names=() partition_modes=()

    if (( RUN_NATIVE )); then
        if ! native_lock_csv="$(resolve_lock_set_csv "$LOCK_SET")"; then
            exit 2
        fi
        IFS=',' read -r -a native_lock_names <<< "$native_lock_csv"
        PLAN_NATIVE_LOCK_COUNT="${#native_lock_names[@]}"
    fi
    if (( RUN_TIGON )); then
        IFS=',' read -r -a tigon_lock_names <<< "$TIGON_LOCKS"
        PLAN_TIGON_LOCK_COUNT="${#tigon_lock_names[@]}"
        IFS=',' read -r -a partition_modes <<< "$TIGON_PARTITION_MODES"
    fi

    sweep_values=""
    for ((thread = THREAD_START; thread <= MAX_THREADS; thread += THREAD_INTERVAL)); do
        (( PLAN_THREAD_COUNT += 1 ))
        if [[ -n "$sweep_values" ]]; then
            sweep_values+=","
        fi
        sweep_values+="$thread"
    done
    PLAN_THREAD_VALUES="$sweep_values"

    if (( RUN_DRAM )); then
        modes+=(dram)
        nodes+=("$DRAM_NODE")
        latencies+=(none)
    fi
    if (( RUN_CXL_UNMODIFIED )); then
        modes+=(cxl-unmodified)
        nodes+=("$CXL_NODE")
        latencies+=(unmodified)
    fi
    if (( RUN_CXL_LOW )); then
        modes+=(cxl-low)
        nodes+=("$CXL_NODE")
        latencies+=("$CXL_LATENCY_LOW")
    fi
    if (( RUN_CXL_HIGH )); then
        modes+=(cxl-high)
        nodes+=("$CXL_NODE")
        latencies+=("$CXL_LATENCY_HIGH")
    fi

    printf 'Planned benchmark matrix (listing only; no benchmarks or output directories will be created)\n'
    printf 'Output root: %s\n' "$OUTPUT_ROOT"
    printf 'Thread sweep (%d values): %s\n' "$PLAN_THREAD_COUNT" "$PLAN_THREAD_VALUES"
    printf 'Repetitions per lock/thread combination: %s\n\n' "$RUNS"

    for ((mode_index = 0; mode_index < ${#modes[@]}; mode_index++)); do
        mode="${modes[$mode_index]}"
        node="${nodes[$mode_index]}"
        latency="${latencies[$mode_index]}"

        if (( RUN_TIGON )); then
            for app in tigon_ycsb tigon_tpcc; do
                if [[ "$app" == tigon_ycsb ]]; then
                    query="$TIGON_YCSB_QUERY"
                    workload="query=$query protocol=$TIGON_PROTOCOL warmup=${TIGON_WARMUP}s"
                    workload+=" rw=${TIGON_READ_WRITE_RATIO}% ro=${TIGON_READ_ONLY_RATIO}%"
                    workload+=" cross=${TIGON_CROSS_RATIO}% cross_partitions=$TIGON_CROSS_PARTITIONS"
                    workload+=" keys=$TIGON_KEYS zipf=$TIGON_ZIPF"
                else
                    query="$TIGON_TPCC_QUERY"
                    workload="query=$query protocol=$TIGON_PROTOCOL warmup=${TIGON_WARMUP}s"
                    workload+=" neworder=${TIGON_NEWORDER_DIST}% payment=${TIGON_PAYMENT_DIST}%"
                fi
                for ratio in "${partition_modes[@]}"; do
                    case "$ratio" in
                        threads|half|quarter) ;;
                        *) echo "error: unsupported Tigon partition mode '$ratio'" >&2; exit 2 ;;
                    esac
                    partition_counts=""
                    for ((thread = THREAD_START; thread <= MAX_THREADS; thread += THREAD_INTERVAL)); do
                        case "$ratio" in
                            threads) partition_count="$thread" ;;
                            half) partition_count=$((thread / 2)); (( partition_count > 0 )) || partition_count=1 ;;
                            quarter) partition_count=$((thread / 4)); (( partition_count > 0 )) || partition_count=1 ;;
                        esac
                        if [[ -n "$partition_counts" ]]; then
                            partition_counts+=","
                        fi
                        partition_counts+="$thread->$partition_count"
                    done
                    category="$app/$mode/partitions-$ratio"
                    add_planned_group "$category" "$app" "$node" "$latency" \
                        "$TIGON_LOCKS" "$PLAN_TIGON_LOCK_COUNT" \
                        "$workload partition_mode=$ratio effective_partitions={$partition_counts}"
                done
            done
        fi

        if (( RUN_NATIVE )); then
            lock_names="$native_lock_csv"
            for app in max min; do
                add_planned_group "$app/$mode" "$app" "$node" "$latency" \
                    "$lock_names" "$PLAN_NATIVE_LOCK_COUNT" "lock_set=$LOCK_SET"
            done
        fi
    done

    printf '\nSummary:\n'
    printf '  Timestamped output groups: %d\n' "$PLAN_GROUPS"
    printf '  Benchmark executions: %d\n' "$PLAN_EXECUTIONS"
    printf '  Estimated timed workload: %d seconds (%dh %02dm %02ds)\n' \
        "$PLAN_ESTIMATED_SECONDS" \
        "$((PLAN_ESTIMATED_SECONDS / 3600))" \
        "$(((PLAN_ESTIMATED_SECONDS % 3600) / 60))" \
        "$((PLAN_ESTIMATED_SECONDS % 60))"
    printf '  Estimate includes RUN_TIME and Tigon warmup; excludes build, startup, plotting, and other overhead.\n'

    if (( (RUN_CXL_LOW || RUN_CXL_HIGH) )) && [[ -z "$CXL_LATENCY_PCI" ]]; then
        printf '  Note: adjusted-latency categories require CXL_LATENCY_PCI before execution.\n'
    fi
}

if (( LIST_RUNS )); then
    list_planned_runs
    exit 0
fi

validate_node() {
    local node="$1" meminfo="/sys/devices/system/node/node$1/meminfo"
    if [[ ! -r "$meminfo" ]] || ! grep -Eq "^Node $node MemTotal:[[:space:]]+[1-9][0-9]* kB" "$meminfo"; then
        echo "error: NUMA node $node is unavailable or has no online memory" >&2
        exit 1
    fi
}

if ! command -v numactl >/dev/null 2>&1; then
    echo "error: numactl is required for NUMA-bound benchmark runs" >&2
    exit 1
fi
if (( RUN_DRAM )); then
    validate_node "$DRAM_NODE"
fi
if (( RUN_CXL_UNMODIFIED || RUN_CXL_LOW || RUN_CXL_HIGH )); then
    validate_node "$CXL_NODE"
fi
if (( RUN_CXL_LOW || RUN_CXL_HIGH )); then
    if (( ! DRY_RUN )); then
        if [[ -z "$CXL_LATENCY_PCI" ]]; then
            echo "error: set CXL_LATENCY_PCI to the verified PCI BDF for this host" >&2
            exit 2
        fi
        if ! [[ "$CXL_LATENCY_PCI" =~ ^([[:xdigit:]]{4}:)?[[:xdigit:]]{2}:[[:xdigit:]]{2}\.[0-7]$ ]]; then
            echo "error: invalid CXL_LATENCY_PCI BDF: $CXL_LATENCY_PCI" >&2
            exit 2
        fi
        if ! [[ "$CXL_LATENCY_REGISTER" =~ ^(0x)?[[:xdigit:]]+\.[bwl]$ ]]; then
            echo "error: invalid CXL_LATENCY_REGISTER: $CXL_LATENCY_REGISTER" >&2
            exit 2
        fi
        if ! [[ "$CXL_LATENCY_LOW" =~ ^[[:xdigit:]]{1,8}$ &&
                "$CXL_LATENCY_HIGH" =~ ^[[:xdigit:]]{1,8}$ ]]; then
            echo "error: CXL latency values must be 1-8 hexadecimal digits" >&2
            exit 2
        fi
        if ! command -v setpci >/dev/null 2>&1; then
            echo "error: setpci is required to adjust CXL latency" >&2
            exit 1
        fi
    fi
fi

if [[ -n "$CGROUP_PROCS" && -e "$CGROUP_PROCS" && "$DRY_RUN" == 0 ]]; then
    printf '%s\n' "$$" | sudo tee "$CGROUP_PROCS" >/dev/null
elif [[ -n "$CGROUP_PROCS" && ! -e "$CGROUP_PROCS" ]]; then
    echo "warning: cgroup file not found, skipping process placement: $CGROUP_PROCS" >&2
fi

timestamp() {
    date -u +%Y%m%dT%H%M%S%N
}

new_run_dir() {
    local category="$1"
    printf '%s/%s/%s\n' "$OUTPUT_ROOT" "$category" "$(timestamp)"
}

write_metadata() {
    local run_dir="$1" category="$2" app="$3" node="$4" latency="$5"
    {
        printf 'category=%s\n' "$category"
        printf 'application=%s\n' "$app"
        printf 'threads=%s..%s step %s\n' "$THREAD_START" "$MAX_THREADS" "$THREAD_INTERVAL"
        printf 'run_seconds=%s\nrepetitions=%s\n' "$RUN_TIME" "$RUNS"
        printf 'numa_node=%s\nallocation=malloc\n' "$node"
        printf 'CXLALLOC_BACKEND=mmap\ncxl_latency=%s\n' "$latency"
        if [[ "$latency" == unmodified ]]; then
            printf 'cxl_latency_setting=host default; PCI register left unchanged\n'
        else
            printf 'cxl_latency_pci=%s\ncxl_latency_register=%s\n' "$CXL_LATENCY_PCI" "$CXL_LATENCY_REGISTER"
        fi
        printf 'created_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$run_dir/run_config.txt"
}

format_command() {
    printf '%q ' "$@"
}

run_benchmark() {
    local category="$1" node="$2" app="$3" latency="$4"
    shift 4
    local run_dir skip_build=1 command_text
    local -a lock_args command

    run_dir="$(new_run_dir "$category")"
    mkdir -p "$run_dir/raw" "$run_dir/logs"
    write_metadata "$run_dir" "$category" "$app" "$node" "$latency"

    if [[ "$app" == tigon_* ]]; then
        local -a tigon_lock_list
        IFS=',' read -r -a tigon_lock_list <<< "$TIGON_LOCKS"
        lock_args=(-i "${tigon_lock_list[@]}")
        if (( ! SKIP_BUILD )) && (( ! TIGON_BUILD_DONE )); then
            skip_build=0
        fi
    else
        lock_args=(-s "$LOCK_SET")
        if (( ! SKIP_BUILD )) && (( ! NATIVE_BUILD_DONE )); then
            skip_build=0
        fi
    fi

    command=(
        python3 -m scripts.main "$MAX_THREADS" "$RUN_TIME" "$RUNS"
        --bench "$app"
        --alloc malloc
        --capture throughput
        --sweep threads "$THREAD_START" "$MAX_THREADS" "$THREAD_INTERVAL"
        --plot faceted
        --data-folder "$run_dir/raw"
        --log-folder "$run_dir/logs"
        "${lock_args[@]}"
        "$@"
    )
    command=(
        numactl --membind="$node" env
        CXLALLOC_BACKEND=mmap
        MUTEX_BENCH_SKIP_BUILD="$skip_build"
        "${command[@]}"
    )

    command_text="$(format_command "${command[@]}")"
    printf 'command=%s\n' "$command_text" >> "$run_dir/run_config.txt"
    echo "==> $category (NUMA node $node, CXL latency $latency)"
    echo "    output: $run_dir"
    echo "    $command_text"

    if (( DRY_RUN )); then
        if [[ "$app" == tigon_* ]]; then
            TIGON_BUILD_DONE=1
        else
            NATIVE_BUILD_DONE=1
        fi
        return 0
    fi

    "${command[@]}" 2>&1 | tee "$run_dir/run.log"
    if [[ "$app" == tigon_* ]]; then
        TIGON_BUILD_DONE=1
    else
        NATIVE_BUILD_DONE=1
    fi
}

run_tigon_suite() {
    local mode="$1" node="$2" latency="$3"
    local app query ratio
    local -a workload_args
    local -a partition_modes
    IFS=',' read -r -a partition_modes <<< "$TIGON_PARTITION_MODES"

    for app in tigon_ycsb tigon_tpcc; do
        if [[ "$app" == tigon_ycsb ]]; then
            query="$TIGON_YCSB_QUERY"
            workload_args=(
                --tigon-read-write-ratio "$TIGON_READ_WRITE_RATIO"
                --tigon-read-only-ratio "$TIGON_READ_ONLY_RATIO"
                --tigon-cross-ratio "$TIGON_CROSS_RATIO"
                --tigon-cross-partitions "$TIGON_CROSS_PARTITIONS"
                --tigon-keys "$TIGON_KEYS"
                --tigon-zipf "$TIGON_ZIPF"
            )
        else
            query="$TIGON_TPCC_QUERY"
            workload_args=(
                --tigon-neworder-dist "$TIGON_NEWORDER_DIST"
                --tigon-payment-dist "$TIGON_PAYMENT_DIST"
            )
        fi
        for ratio in "${partition_modes[@]}"; do
            case "$ratio" in
                threads|half|quarter) ;;
                *) echo "error: unsupported Tigon partition mode '$ratio'" >&2; exit 2 ;;
            esac
            local category="$app/$mode/partitions-$ratio"
            run_benchmark "$category" "$node" "$app" "$latency" \
                --tigon-query "$query" \
                --tigon-protocol "$TIGON_PROTOCOL" \
                --tigon-partition-ratio "$ratio" \
                --tigon-warmup-seconds "$TIGON_WARMUP" \
                "${workload_args[@]}"
        done
    done
}

run_native_suite() {
    local mode="$1" node="$2" latency="$3" app
    for app in max min; do
        run_benchmark "$app/$mode" "$node" "$app" "$latency"
    done
}

hex_to_int() {
    local value="${1#0x}"
    printf '%d' "0x$value"
}

LATENCY_ORIGINAL=""
LATENCY_SAVED=0
restore_latency() {
    if (( LATENCY_SAVED )); then
        if sudo setpci -s "$CXL_LATENCY_PCI" \
            "$CXL_LATENCY_REGISTER=$LATENCY_ORIGINAL"; then
            echo "==> Restored $CXL_LATENCY_PCI $CXL_LATENCY_REGISTER=$LATENCY_ORIGINAL"
        else
            echo "ERROR: failed to restore original CXL latency register value" >&2
        fi
        LATENCY_SAVED=0
    fi
}
trap restore_latency EXIT

prepare_latency_control() {
    if (( DRY_RUN )); then
        echo "==> Dry run: PCI latency register will not be read or written"
        return
    fi
    sudo -v
    LATENCY_ORIGINAL="$(sudo setpci -s "$CXL_LATENCY_PCI" "$CXL_LATENCY_REGISTER")"
    if ! [[ "$LATENCY_ORIGINAL" =~ ^[[:xdigit:]]{1,8}$ ]]; then
        echo "error: could not read a valid original value from CXL latency register" >&2
        exit 1
    fi
    LATENCY_SAVED=1
    echo "==> Saved original latency control: $CXL_LATENCY_PCI $CXL_LATENCY_REGISTER=$LATENCY_ORIGINAL"
}

apply_latency() {
    local value="$1" actual
    if (( DRY_RUN )); then
        echo "==> Dry run: would set $CXL_LATENCY_PCI $CXL_LATENCY_REGISTER=$value"
        return
    fi
    sudo setpci -s "$CXL_LATENCY_PCI" "$CXL_LATENCY_REGISTER=$value"
    actual="$(sudo setpci -s "$CXL_LATENCY_PCI" "$CXL_LATENCY_REGISTER")"
    if ! [[ "$actual" =~ ^[[:xdigit:]]{1,8}$ ]] ||
        (( $(hex_to_int "$actual") != $(hex_to_int "$value") )); then
        echo "error: latency register readback mismatch (wanted $value, got $actual)" >&2
        exit 1
    fi
    echo "==> Applied CXL latency value $actual to $CXL_LATENCY_PCI"
}

TIGON_BUILD_DONE=0
NATIVE_BUILD_DONE=0
if (( RUN_CXL_LOW || RUN_CXL_HIGH )); then
    prepare_latency_control
fi

if (( RUN_DRAM )); then
    (( RUN_TIGON )) && run_tigon_suite dram "$DRAM_NODE" none
    (( RUN_NATIVE )) && run_native_suite dram "$DRAM_NODE" none
fi

if (( RUN_CXL_UNMODIFIED )); then
    (( RUN_TIGON )) && run_tigon_suite cxl-unmodified "$CXL_NODE" unmodified
    (( RUN_NATIVE )) && run_native_suite cxl-unmodified "$CXL_NODE" unmodified
fi

if (( RUN_CXL_LOW )); then
    apply_latency "$CXL_LATENCY_LOW"
    (( RUN_TIGON )) && run_tigon_suite cxl-low "$CXL_NODE" "$CXL_LATENCY_LOW"
    (( RUN_NATIVE )) && run_native_suite cxl-low "$CXL_NODE" "$CXL_LATENCY_LOW"
fi

if (( RUN_CXL_HIGH )); then
    apply_latency "$CXL_LATENCY_HIGH"
    (( RUN_TIGON )) && run_tigon_suite cxl-high "$CXL_NODE" "$CXL_LATENCY_HIGH"
    (( RUN_NATIVE )) && run_native_suite cxl-high "$CXL_NODE" "$CXL_LATENCY_HIGH"
fi

echo "All requested benchmark categories complete."
echo "Timestamped output root: $OUTPUT_ROOT"