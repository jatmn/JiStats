# Verify a change

Choose checks for the behavior affected and record the commands, results, and any untested paths in the [tracker handoff](../progress.md#handoffs).

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

## Windows commands

Run from the repository root using Visual Studio 2026 with MSVC.

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -DJISTATS_BUILD_TESTS=ON
cmake --build build --config Debug --target JiStatsTests
ctest --test-dir build -C Debug --output-on-failure
```

[tests/CMakeLists.txt](../../tests/CMakeLists.txt) registers `JiStatsTests` and sets its working directory so relative fixture paths resolve. For a focused run, use a Catch2 filter such as `[openai]`, `[schema][migration]`, or `[taskbar]` with `build\bin\Debug\JiStatsTests.exe`.

## Continuous integration

For build-affecting pull requests to `main` or `release/**`, Linux runs the portable tests and cross-compiles all three Windows executables. A hosted Windows runner downloads that run's `JiStatsTests.exe` and executes the full suite from the checkout root. The required `CI passed` check includes both jobs. CI compares the PR test merge commit with its base parent. Documentation and certain repository metadata can skip the builds. Test payloads still trigger them, including Markdown inside the Kimi synthetic runtime and ChatGPT export payload directories.

Protected-branch pushes compare the previous and new tips. Documentation-only pushes can reuse a successful CI run on the previous tip of the same branch. Missing or unsuccessful predecessor validation or an unreadable diff triggers a fresh Windows build and full suite. A manual dispatch runs both the Linux job and the native MSVC gate. To exercise MSVC before merging a toolchain-specific change, dispatch [CI](../../.github/workflows/ci.yml) on the PR branch or run the [SQLite gate script](../../scripts/run_sqlite_quality_gate.ps1) on Windows.

## Pull request lint

The [Lint workflow](../../.github/workflows/lint.yml) checks changed first-party C and C++ lines under `src/`, `include/`, and `tests/` with LLVM 18 and `.clang-format`. It runs `actionlint` when workflows change.

To check committed changes locally, run this from the repository root in Bash with Git and the native LLVM 18 `clang-format-18` binary on your path. Substitute the pull request's target branch for `origin/main` if needed.

```bash
bash scripts/check_changed_format.sh "$(git merge-base origin/main HEAD)"
```

Set `JISTATS_CLANG_FORMAT` to the LLVM 18 executable if it has a different name or location. The check reports source locations that need formatting.

Use [the Windows clang gate script](../../scripts/run_clang_quality_gate.ps1) for full-tree formatting and `clang-tidy` analysis.

## CodeQL scanning

The [CodeQL workflow](../../.github/workflows/codeql.yml) scans C/C++, JavaScript/TypeScript, and GitHub Actions on same-repository pull requests and pushes for `main` and `release/**`. It skips the analysis jobs when the tested diff contains only Markdown documentation. Markdown inside the synthetic Kimi runtime and redacted ChatGPT export payloads still triggers scans. CodeQL also scans weekly, on manual dispatch, and when a diff cannot be verified.

During the transition, the Actions repository variable `CODEQL_UPLOAD_DISABLED=true` lets the new workflow finish its analyses while GitHub-managed default setup publishes results. After the workflow lands on `main`:

1. Disable GitHub-managed default setup in **Settings → Advanced Security → CodeQL analysis**. See [GitHub's setup transition instructions](https://docs.github.com/en/code-security/reference/code-scanning/troubleshoot-analysis-errors/results-different-than-expected).
2. Delete `CODEQL_UPLOAD_DISABLED` under **Settings → Secrets and variables → Actions → Variables** so the repository workflow publishes results.
3. Enable the repository workflow if GitHub lists it as disabled, then dispatch **CodeQL analysis** on `main` and confirm that all three languages publish results.

## Native UI coverage

Verify tray icon lifecycle, tooltip and menu interaction, taskbar placement, Explorer recovery, and sleep/resume in a live session when the change touches them. Win32 sends `WM_CREATE` during `CreateWindowExW`. Initialize the presenter window handle before calling helpers that require it during `WM_CREATE`.
