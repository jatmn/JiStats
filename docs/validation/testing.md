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

Follow the [Linux build instructions](../../README.md#building-on-linux) for the native test build and the MinGW cross-build. For a focused native run, execute `build-linux/bin/JiStatsTests "[schema][migration]"` from the repository root. The native target covers the portable components listed in [tests/CMakeLists.txt](../../tests/CMakeLists.txt). The Windows target includes the remaining provider, controller, reporting and desktop integration tests.

Both suites randomize case order. Full Windows runs compare the Catch2 execution report with its registration list and every tracked test translation unit. The `windows-test-file-times` artifact records the revision, seed and elapsed seconds per source file for 14 days.

## Windows commands

Use the [UCRT64 build instructions](../../README.md#building-on-windows) to run the full suite locally. [CTest](../../tests/CMakeLists.txt) sets the working directory for relative fixture paths. For a focused run, launch `build-ucrt/bin/JiStatsTests.exe "[openai]"` from the repository root.

## Continuous integration

For build-affecting pull requests to `main` or `release/**`, independent Linux runners execute the portable tests and cross-compile all three Windows executables in parallel. A hosted Windows runner downloads that run's `JiStatsTests.exe` and executes the full suite from the checkout root. The required `CI passed` check includes all three jobs. CI compares the PR test merge commit with its base parent. Documentation and certain repository metadata can skip the builds. Test payloads still trigger them, including Markdown inside the Kimi synthetic runtime and ChatGPT export payload directories.

Protected-branch pushes compare the previous and new tips. Documentation-only pushes can reuse a successful CI run on the previous tip of the same branch. A failed, pending or missing predecessor run, or an unreadable diff, triggers the Linux build and full Windows suite. Dispatch [CI](../../.github/workflows/ci.yml) to run both jobs on a selected branch regardless of the diff. Enable **Run eight additional offline notice passes to diagnose restart stalls** when investigating offline restart timing. CI retains the cross-built test executable for 30 days so a Windows job retry can reuse it.

The Linux build restores compiler results seeded by pushes to the default branch. Both native and MinGW compilers use ccache in preprocessor mode. Each run reports compiler hits and misses and still links the binaries and executes the suites.

## Pull request lint

The [Lint workflow](../../.github/workflows/lint.yml) checks changed first-party C and C++ lines under `src/`, `include/`, and `tests/` with LLVM 18 and `.clang-format`. It runs `actionlint` when workflows change.

To check committed changes locally, run this from the repository root in Bash with Git and the native LLVM 18 `clang-format-18` binary on your path. Substitute the pull request's target branch for `origin/main` if needed.

```bash
bash scripts/check_changed_format.sh "$(git merge-base origin/main HEAD)"
```

Set `JISTATS_CLANG_FORMAT` to the LLVM 18 executable if it has a different name or location. The check reports source locations that need formatting.

For full-tree formatting and static analysis on Windows, install `mingw-w64-ucrt-x86_64-clang-tools-extra` and `mingw-w64-ucrt-x86_64-clang` alongside the UCRT64 build tools. From the UCRT64 shell, run the [clang gate](../../scripts/run_clang_quality_gate.ps1) with `CC=gcc CXX=g++ powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_clang_quality_gate.ps1`. Use a fresh build directory when changing compilers.

## CodeQL scanning

The [repository CodeQL workflow](../../.github/workflows/codeql.yml) scans C/C++, JavaScript/TypeScript, and GitHub Actions on same-repository pull requests and pushes for `main` and `release/**`. It skips the analysis jobs when the tested diff contains only Markdown documentation. Markdown inside the synthetic Kimi runtime and redacted ChatGPT export payloads still triggers scans. The workflow also scans weekly, on manual dispatch, and when a diff cannot be verified. The C/C++ job runs without a build, so it first configures CMake to download the pinned dependency headers into `build-codeql/` and excludes that directory from the scan.

The repository workflow publishes results to **Security and quality → Code scanning**. To scan a branch on demand, select **Run workflow** on the **CodeQL analysis** page in GitHub Actions.

## Native UI coverage

Verify tray icon lifecycle, tooltip and menu interaction, taskbar placement, Explorer recovery, and sleep/resume in a live session when the change touches them. Win32 sends `WM_CREATE` during `CreateWindowExW`. Initialize the presenter window handle before calling helpers that require it during `WM_CREATE`.
