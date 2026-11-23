#!/usr/bin/env bash
set -euo pipefail

if [[ ${1:-} == --pre-push ]]; then
    # Git represents a deletion as an all-zero destination object.
    if [[ ${2:?expected the pushed revision} =~ ^0+$ ]]; then exit 0; fi
    target=${JISTATS_PR_BASE:-origin/main}
    set -- "$(git merge-base "$target" "$2")" "$2"
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
