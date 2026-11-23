#!/usr/bin/env bash
set -euo pipefail

if [[ ${1:-} == --pre-push ]]; then
    start=$EPOCHREALTIME
    target=${JISTATS_PR_BASE:-origin/main}
    declare -A verified=()
    check_push() {
        local revision=$1 base
        if [[ $revision =~ ^0+$ ]]; then return 0; fi
        if [[ ${verified[$revision]+present} ]]; then return 0; fi
        verified[$revision]=1
        base=$(git merge-base "$target" "$revision") || return 1
        JISTATS_PUSH_CHECK=1 bash "$0" "$base" "$revision"
    }
    # hk selects files against one ref. Git supplies every update on stdin.
    updates=0
    status=0
    while read -r _local_ref revision _remote_ref _old_revision; do
        [[ -n $revision ]] || continue
        updates=$((updates + 1))
        check_push "$revision" || status=1
    done
    if (( updates == 0 )); then check_push "${2:?expected the pushed revision}" || status=1; fi
    awk -v start="$start" -v end="$EPOCHREALTIME" 'BEGIN {
        printf "Pre-push checks: %.3fs (limit < 9s)\n", end-start
        exit (end-start >= 9)
    }' || status=1
    exit "$status"
fi
base=$(git rev-parse --verify --end-of-options "${1:?expected a base revision}^{commit}")
head=$(git rev-parse --verify --end-of-options "${2:-HEAD}^{commit}")
cd "$(git rev-parse --show-toplevel)"

# actionlint reads local action definitions and workflow configuration from the
# checkout. Check the committed version even on a push with uncommitted edits.
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
git archive "$head" | tar -x -C "$tmp"
status=0
JISTATS_FORMAT_TREE="$tmp" bash scripts/check_changed_format.sh "$base" "$head" || status=1
(cd "$tmp" && git init --quiet && actionlint) || status=1
if [[ ${JISTATS_PUSH_CHECK:-0} == 1 && -f "$tmp/scripts/check_js_ci.sh" ]]; then
    # The archive contains the pushed lockfile, even on a non-HEAD ref.
    if (cd "$tmp" && bun install --frozen-lockfile); then
        (cd "$tmp" && bash scripts/check_js_ci.sh) || status=1
    else
        status=1
    fi
    (cd "$tmp" && node --test scripts/pullfrog-review-check.test.mjs) || status=1
fi
exit "$status"
