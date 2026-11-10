#!/bin/sh
# Smoke test for --bench damer_moe: export the Damer plan, then run each mode.
# usage: tests/damer_moe_smoke.sh [lock_name]   (set MUTEX_BENCH_NUMA_NODE for hardware_cxl builds)
set -eu
cd "$(dirname "$0")/.."
lock=${1:-ticket}
plan=$(mktemp)
trap 'rm -f "$plan"' EXIT
python3 scripts/damer_moe_plan.py --out "$plan" >/dev/null
for movement in fused staged; do
    for granularity in expert global; do
        out=$(./build/apps/damer_moe_bench/damer_moe_bench "$lock" 4 1 --plan "$plan" --csv \
            --movement "$movement" --granularity "$granularity" 2>&1)
        echo "$out" | grep -q "was not correct" && { echo "FAIL $movement/$granularity"; exit 1; }
        echo "$out" | grep -Eq '^3,[0-9.]+,[1-9][0-9]*$' || { echo "FAIL $movement/$granularity: no output"; exit 1; }
        echo "ok $movement/$granularity"
    done
done
