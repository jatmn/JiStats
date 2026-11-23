# Contributing to JiStats

The maintainer sets JiStats' [roadmap](docs/plan.md) and decides what to merge. Bug reports and contributions are welcome. Discuss larger proposals early so you can check how they fit the project's direction.

## Project priorities

- JiStats is a native C++17 desktop app for Windows 11. Do not add Python code, scripts, tooling, or dependencies to the repository.
- The Windows XP-era look is intentional. Preserve that visual style and keep the app responsive and easy to use on Windows 11.
- Low memory use and speed come first. Keep JiStats lightweight while idle, quick to start, and responsive while collecting data or refreshing the UI. Share relevant before-and-after memory or timing measurements in the pull request when available.

## Reporting bugs and submitting changes

Report bugs in [GitHub Issues](https://github.com/jatmn/JiStats/issues). Include your JiStats version or commit, Windows 11 version, steps to reproduce, and what you expected to happen versus what happened.

Open a pull request against the default branch. Describe the behavior you changed, how you validated it, and any checks that still need to run. Follow [AGENTS.md](AGENTS.md) for project conventions, the [source build instructions](README.md#build-and-packaging) to build JiStats, and the [verification guide](docs/validation/testing.md) to choose checks.

Keep provider captures, credentials, and personal data out of public issues and pull requests. Use synthetic or redacted [fixtures](docs/fixtures/README.md) in tests. Send vulnerability reports through the [security policy](SECURITY.md).

Dependabot checks GitHub Actions for version updates. It does not track the C++ libraries pinned in [ThirdPartySources.cmake](cmake/ThirdPartySources.cmake). To upgrade one, update its `URL` and `URL_HASH` together and the corresponding version in the [notices](THIRD_PARTY_NOTICES.md). When updating Pullfrog, review the action pin and command payload version together.

### Local hooks

With hk 2.13.0, clang-format-18, actionlint 1.7.12 and Bash on your path, run `hk install` in the clone. Pre-commit formats staged C++ files and lints staged workflow changes. Pre-push runs the same changed-line format and workflow lint command as CI. For a release PR, set `JISTATS_PR_BASE` to its remote-tracking target branch. Keep that ref current before pushing. To run it directly, use `bash scripts/check_pr_lint.sh "$(git merge-base "${JISTATS_PR_BASE:-origin/main}" HEAD)" HEAD`. The native build and platform test commands are in the verification guide.

### Pullfrog

In the Pullfrog console, keep automatic PR reviews, draft reviews, and re-reviews on new commits enabled, with reviews for external contributors disabled. The [workflow](.github/workflows/pullfrog.yml) checks the PR author against GitHub before starting a review. Automatic reviews cover public JiStats PRs opened by `jatmn`, even when someone else pushes to them. The owner can still use `@pullfrog` commands in issue and PR comments.

Run `node --test scripts/pullfrog-*.test.mjs` to check the authorization logic locally. GitHub Actions runs the same tests on Pullfrog workflow and script changes.
