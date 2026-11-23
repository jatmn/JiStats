#!/usr/bin/env bash
set -euo pipefail

git_common_directory() (
    common_dir=$(git rev-parse --git-common-dir) && cd "$common_dir" && pwd -P
)

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
        elapsed = end-start
        printf "Pre-push checks: %.3fs (target < 9s)%s\n", elapsed, (elapsed >= 9 ? " [over target]" : "")
    }'
    exit "$status"
fi
base=$(git rev-parse --verify --end-of-options "${1:?expected a base revision}^{commit}")
head=$(git rev-parse --verify --end-of-options "${2:-HEAD}^{commit}")
cd "$(git rev-parse --show-toplevel)"

# actionlint reads local action definitions and workflow configuration from the
# checkout. Check the committed version even on a push with uncommitted edits.
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
archive="$tmp/archive"
mkdir "$archive"
git archive "$head" | tar -x -C "$archive"
status=0
JISTATS_FORMAT_TREE="$archive" bash scripts/check_changed_format.sh "$base" "$head" || status=1
(cd "$archive" && git init --quiet && actionlint) || status=1
if [[ ${JISTATS_PUSH_CHECK:-0} == 1 && -f "$archive/scripts/check_js_ci.sh" ]]; then
    worktree_list="$tmp/worktree-list"
    if git worktree list --porcelain -z > "$worktree_list" 2>/dev/null; then
        delimiter=
    elif git -c core.quotePath=false worktree list --porcelain > "$worktree_list"; then
        delimiter=$'\n'
        repository=$(git_common_directory)
    else
        echo "Could not verify a checked-out worktree for $head: git worktree list failed." >&2
        exit 1
    fi
    # Run ref-owned scripts only for an exact revision checked out locally.
    trusted=false
    worktree=
    while IFS= read -r -d "$delimiter" entry; do
        case "$entry" in
            "worktree "*)
                worktree=${entry#worktree }
                # Legacy porcelain quotes paths with special characters.
                if [[ -n $delimiter && $worktree == '"'* ]]; then
                    worktree=
                fi
                ;;
            "HEAD $head")
                [[ -n $worktree && -e $worktree/.git ]] || continue
                # Verify legacy paths directly because newline-delimited output can be ambiguous.
                if [[ -n $delimiter ]] && ! (
                    cd -- "$worktree" &&
                        [[ $(git_common_directory) == "$repository" &&
                           $(git rev-parse --verify HEAD) == "$head" ]]
                ); then
                    continue
                fi
                trusted=true
                break
                ;;
        esac
    done < "$worktree_list"
    if [[ $trusted != true ]]; then
        echo "Skipping local JavaScript checks for $head because it is not checked out in a worktree." >&2
        exit "$status"
    fi
    # The archive contains the pushed lockfile, even on a non-HEAD ref.
    if (cd "$archive" && bun install --frozen-lockfile); then
        (cd "$archive" && bash scripts/check_js_ci.sh) || status=1
    else
        status=1
    fi
    (cd "$archive" && node --test scripts/pullfrog-review-check.test.mjs) || status=1
fi
exit "$status"
