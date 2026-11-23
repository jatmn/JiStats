#!/usr/bin/env bash
set -euo pipefail

if [[ ${1:-} == --pre-push ]]; then
    target=${JISTATS_PR_BASE:-origin/main}
    check_push() {
        local revision=$1 base
        if [[ $revision =~ ^0+$ ]]; then return 0; fi
        base=$(git merge-base "$target" "$revision") || return 1
        bash "$0" "$base" "$revision"
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
exit "$status"
