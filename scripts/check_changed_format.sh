#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 1 ]]; then
    printf 'Usage: %s <base-commit>\n' "$0" >&2
    exit 2
fi

cd "$(git rev-parse --show-toplevel)"
base=$(git rev-parse --verify --end-of-options "$1^{commit}")
head=$(git rev-parse --verify 'HEAD^{commit}')
formatter=${JISTATS_CLANG_FORMAT:-clang-format-18}

"$formatter" --style=file:.clang-format --assume-filename=src/format-check.cpp --dump-config >/dev/null

changed_files=$(mktemp)
trap 'rm -f "$changed_files"' EXIT
git diff --raw -z --no-abbrev --no-renames --diff-filter=AMT "$base" "$head" -- src/ include/ tests/ >"$changed_files"

# Git batches missing parent blobs for a single multi-file patch. Hydrate them
# together before asking for each file's changed line ranges below.
if [[ -s $changed_files && $(git config --bool --get remote.origin.promisor) == true ]]; then
    git diff --no-ext-diff --no-textconv --no-renames "$base" "$head" -- src/ include/ tests/ >/dev/null
fi

hunk_pattern='^@@ -[0-9]+(,[0-9]+)? \+([0-9]+)(,([0-9]+))? @@'
failed=0
while IFS= read -r -d '' change && IFS= read -r -d '' file; do
    read -r _ mode _ blob _ <<<"$change"
    [[ $mode == 100644 || $mode == 100755 ]] || continue
    case "${file,,}" in
        *.c|*.cc|*.cp|*.cpp|*.cxx|*.c++|*.h|*.hh|*.hpp|*.hxx|*.h++|*.inc|*.cppm|*.ccm|*.cxxm|*.c++m) ;;
        *) continue ;;
    esac

    patch=$(git --literal-pathspecs diff --no-color --no-ext-diff --no-textconv --no-renames -U0 "$base" "$head" -- "$file")
    ranges=()
    while IFS= read -r line; do
        if [[ $line =~ $hunk_pattern ]]; then
            first=${BASH_REMATCH[2]}
            count=${BASH_REMATCH[4]:-1}
            # Preserve the formatter check at the preceding line for deletions.
            (( first > 0 )) || continue
            (( count > 0 )) || count=1
            ranges+=("--lines=$first:$((first + count - 1))")
        fi
    done <<<"$patch"

    if (( ${#ranges[@]} > 0 )) &&
        ! git cat-file blob "$blob" | "$formatter" --assume-filename="$file" --dry-run --Werror "${ranges[@]}"; then
        failed=1
    fi
done <"$changed_files"

exit "$failed"
