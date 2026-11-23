# shellcheck shell=bash
# Return success when a changed path can affect the native build or tests.
is_native_ci_input() {
    case "$1" in
        docs/fixtures/*) return 0 ;;
        docs/*.md | README.md | AGENTS.md | CONTRIBUTING.md | SECURITY.md | LICENSE | .gitignore)
            return 1 ;;
        scripts/pullfrog-command-check.mjs | scripts/pullfrog-review-check.mjs | scripts/pullfrog-review-check.test.mjs)
            # The independent Pullfrog checks workflow runs the authorization suite.
            return 1 ;;
        *) return 0 ;;
    esac
}
