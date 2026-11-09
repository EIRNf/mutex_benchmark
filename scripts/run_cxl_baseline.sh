#!/usr/bin/env bash
set -Eeuo pipefail

args=(--memory cxl)
while [[ $# -gt 0 ]]; do
    case "$1" in
        --cxl-node) args+=(--node "$2"); shift ;;
        *) args+=("$1") ;;
    esac
    shift
done
exec "$(dirname "$0")/run_baseline.sh" "${args[@]}"
