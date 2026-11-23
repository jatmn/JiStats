#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
bun="$PWD/node_modules/.bin/bun"
logs=$(mktemp -d)
trap 'rm -rf -- "$logs"' EXIT

check() {
    local name=$1 limit=$2 start=$EPOCHREALTIME status=0
    shift 2
    "$bun" "$@" || status=$?
    awk -v start="$start" -v end="$EPOCHREALTIME" -v limit="$limit" -v name="$name" 'BEGIN {
        elapsed = end - start
        printf "%s: %.3fs (target < %ss)%s\n", name, elapsed, limit, (elapsed >= limit ? " [over target]" : "")
    }' || status=1
    return "$status"
}

# Finish every command and print all diagnostics even when a sibling fails.
check lint 1 lint --type-aware --max-warnings=0 >"$logs/lint" 2>&1 & lint=$!
check format 1 fmt --check scripts/*.mjs >"$logs/format" 2>&1 & format=$!
check types 5 check >"$logs/types" 2>&1 & types=$!
check tests 30 test scripts/*.test.mjs >"$logs/tests" 2>&1 & tests=$!
status=0
for pid in "$lint" "$format" "$types" "$tests"; do
    wait "$pid" || status=1
done
cat "$logs/lint" "$logs/format" "$logs/types" "$logs/tests"
exit "$status"
