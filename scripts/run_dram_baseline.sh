#!/usr/bin/env bash
set -Eeuo pipefail

args=(--memory dram)
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dram-node) args+=(--node "$2"); shift ;;
        --tigon-only) args+=(--apps tigon_ycsb) ;;
        --contention-only) args+=(--apps max,min) ;;
        *) args+=("$1") ;;
    esac
    shift
done
exec "$(dirname "$0")/run_baseline.sh" "${args[@]}"
