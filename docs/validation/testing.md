# Verify a change

Choose checks for the behavior affected and record the commands, results, and any untested paths in the [progress log](../progress.md#handoffs).

| Change | Verification |
| --- | --- |
| Documentation | Check claims against source, tests, and configured workflows. Check links and anchors from the page's final location. |
| Deterministic runtime or analytics | Use focused Catch2 tests. Cover user-visible routes through controller or presenter tests where applicable. |
| Provider parsing or collection | Replay relevant [fixtures](../fixtures/README.md), including rejected inputs and source attribution. |
| Schema, storage, or SQLite persistence | Run the [SQLite-backed quality gate](sqlite-quality-gate.md). |
| Native dashboard, tray, or taskbar | Run presenter/controller tests and the relevant [desktop checks](desktop.md). Verify actual Win32 behavior in a Windows desktop session. |
| Release | Follow the [release checklist](../release-checklist.md). |

## Linux commands

Run the portable Catch2 tests from the repository root on Debian or Ubuntu.

```bash
sudo apt install build-essential cmake ninja-build
cmake -S . -B build-linux -G Ninja -DJISTATS_BUILD_TESTS=ON
cmake --build build-linux --target JiStatsTests
ctest --test-dir build-linux --output-on-failure
```

For a focused native run, execute `build-linux/bin/JiStatsTests "[schema][migration]"` from the repository root. The native target covers the portable components listed in [tests/CMakeLists.txt](../../tests/CMakeLists.txt).

For a Windows-facing change, install the [MinGW cross-build prerequisites](../../README.md#building-on-linux) and build the desktop app, CLI, and Windows test executable.

```bash
cmake -S . -B build-mingw-checks -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Debug -DJISTATS_BUILD_TESTS=ON -DJISTATS_BUILD_MINIMAX_CLI=ON
cmake --build build-mingw-checks --target JiStats JiStatsMiniMaxCli JiStatsTests
```

Both suites randomize case order. Full Windows runs compare the Catch2 execution report with its registration list and every tracked test translation unit. The `windows-test-file-times` artifact records the revision, seed and elapsed seconds per source file for 14 days.

## Windows commands

Install the [UCRT64 build prerequisites](../../README.md#building-on-windows) and run the full suite in a separate build directory from the repository root. Some Windows tests open desktop windows while exercising the UI.

```bash
cmake -S . -B build-ucrt-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DJISTATS_BUILD_TESTS=ON
cmake --build build-ucrt-tests --target JiStatsTests
ctest --test-dir build-ucrt-tests --output-on-failure
```

The Windows target adds provider, controller, reporting, and desktop integration coverage. [CTest](../../tests/CMakeLists.txt) sets the working directory for relative fixture paths. For a focused run, launch `build-ucrt-tests/bin/JiStatsTests.exe "[openai]"` from the repository root.

## Continuous integration

For build-affecting pull requests to `main` or `release/**`, independent Linux runners execute the portable tests and cross-compile all three Windows executables in parallel. A hosted Windows runner downloads that run's `JiStatsTests.exe` and executes the full suite from the checkout root. The required `CI passed` check includes all three jobs. CI compares the PR test merge commit with its base parent. On a queued PR, the native suites may be omitted only for prose under `docs/`, the listed root contributor documents, `LICENSE`, `.gitignore`, and the three Pullfrog JavaScript files covered by the authorization workflow. Every fixture, source, header, build definition, workflow, other script, and unknown path triggers the full native suite. Renames check both paths. A missing merge parent or unreadable diff also runs everything. The [classifier](../../scripts/native_ci_input.sh) and its [required-input check](../../scripts/check_native_ci_selection.sh) run in the detector for PRs and manual dispatches. Manual dispatch skips queue lookup and diff selection.

Merge groups and protected-branch pushes always build and run both native suites and the Pullfrog authorization tests. The required check accepts skipped builds only on a pull request with a successful change detector and an explicit no-build decision. GitHub requires organization ownership to enable a merge queue on a public repository. Dispatch [CI](../../.github/workflows/ci.yml) to run the full CI suite on a selected branch regardless of the diff. Enable **Run eight additional offline notice passes to diagnose restart stalls** when investigating offline restart timing. CI retains the cross-built test executable for 30 days so a Windows job retry can reuse it.

Both Linux jobs restore compiler results seeded by pushes to the default branch. Both native and MinGW compilers use ccache in preprocessor mode. Each run reports compiler hits and misses and still links the binaries and executes the suites.

### CI timing audit

The [CI timing budgets workflow](../../.github/workflows/ci-budgets.yml) reads completed jobs for a specific workflow attempt. For CI, Lint, and Pullfrog checks, the [jq filter](../../scripts/check_ci_budgets.jq) requires jobs to finish in under 35 seconds, setup in under 15 seconds, and the Windows suite step in under 30 seconds. CodeQL and Pullfrog timings are report-only.

To replay captured GitHub job pages locally, run this from the repository root. Use `true` for a report-only workflow.

```bash
jq --slurp --raw-output --argjson report_only false -f scripts/check_ci_budgets.jq jobs.jsonl
```

### Prebuilt Linux toolchain

Dispatch **Prepare Linux CI toolchain** to verify the MinGW and native toolchain on a standard runner. An organization with an image-generation runner can set `JISTATS_IMAGE_GENERATOR` to its label, add `snapshot: jistats-linux-build` beneath the job’s `runs-on` line, then dispatch the workflow to snapshot the verified tools. GitHub rejects workflow definitions containing `snapshot` on personal repositories before evaluating job conditions. Configure a Linux larger runner from the generated `jistats-linux-build` image, then set `JISTATS_LINUX_BUILD_RUNNER` to the consumer's label. CI uses that image for the cross-build and fingerprints the installed compiler packages on every run. With both variables unset, the current standard runners work without a subscription.

Schedule refreshes before the [configured maximum image age](https://docs.github.com/en/enterprise-cloud@latest/admin/enforcing-policies/enforcing-policies-for-your-enterprise/enforcing-policies-for-github-actions-in-your-enterprise#custom-images-retention-policies). Run **Prepare Linux CI toolchain** to publish a new verified version, then select it on the consumer runner if the runner is pinned. If the configured runner cannot take jobs, clear `JISTATS_LINUX_BUILD_RUNNER` and start a new CI run. CI will use `ubuntu-latest` and install the toolchain with APT.

## Pull request lint

The [Lint workflow](../../.github/workflows/lint.yml) checks changed first-party C and C++ lines under `src/`, `include/`, and `tests/` with LLVM 18 and `.clang-format`. It runs `actionlint` when workflows change.

To check committed changes locally, run this from the repository root in Bash with Git and the native LLVM 18 `clang-format-18` binary on your path. Substitute the pull request's target branch for `origin/main` if needed.

```bash
bash scripts/check_changed_format.sh "$(git merge-base origin/main HEAD)"
```

Set `JISTATS_CLANG_FORMAT` to the LLVM 18 executable if it has a different name or location. The check reports source locations that need formatting.

For full-tree formatting and static analysis on Windows, install `mingw-w64-ucrt-x86_64-clang-tools-extra` and `mingw-w64-ucrt-x86_64-clang` alongside the UCRT64 build tools. From the UCRT64 shell, run the [clang gate](../../scripts/run_clang_quality_gate.ps1) with `CC=gcc CXX=g++ powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_clang_quality_gate.ps1`. Use a fresh build directory when changing compilers.

## JavaScript checks

The [Pullfrog checks workflow](../../.github/workflows/pullfrog-checks.yml) runs pinned Bun lint, formatting, type checking and tests, then runs the authorization tests under Node. It reports failures in the advisory `Pullfrog authorization tests` check. The required merge check on `main` is `CI passed`.

## CodeQL scanning

The [repository CodeQL workflow](../../.github/workflows/codeql.yml) scans C/C++, JavaScript/TypeScript, and GitHub Actions on same-repository pull requests and pushes for `main` and `release/**`. On pull requests it skips the analysis jobs when the tested diff contains only Markdown documentation. Markdown inside the synthetic Kimi runtime and redacted ChatGPT export payloads still triggers scans. The workflow also scans merge groups, weekly, on manual dispatch, and when a diff cannot be verified. The C/C++ job runs without a build, so it first configures CMake to download the pinned dependency headers into `build-codeql/` and excludes that directory from the scan.

The repository workflow publishes results to **Security and quality → Code scanning**. To scan a branch on demand, select **Run workflow** on the **CodeQL analysis** page in GitHub Actions.

## Native UI coverage

Verify tray icon lifecycle, tooltip and menu interaction, taskbar placement, Explorer recovery, and sleep/resume in a live session when the change touches them. Win32 sends `WM_CREATE` during `CreateWindowExW`. Initialize the presenter window handle before calling helpers that require it during `WM_CREATE`.
