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

## Windows commands

Run from the repository root using Visual Studio 2026 with MSVC.

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -DJISTATS_BUILD_TESTS=ON
cmake --build build --config Debug --target JiStatsTests
ctest --test-dir build -C Debug --output-on-failure
```

[tests/CMakeLists.txt](../../tests/CMakeLists.txt) registers `JiStatsTests` and sets its working directory so relative fixture paths resolve. For a focused run, use a Catch2 filter such as `[openai]`, `[schema][migration]`, or `[taskbar]` with `build\bin\Debug\JiStatsTests.exe`.

Run [the SQLite gate script](../../scripts/run_sqlite_quality_gate.ps1) locally. The [CI workflow](../../.github/workflows/ci.yml) runs the Windows SQLite gate and a Linux MinGW-w64 cross-build for build-affecting pull requests targeting `main` or `release/**` and for manual runs.

Pull requests classify paths against the test merge commit. Documentation, including Markdown notes in the fixture archive, and certain repository metadata can skip both builds. Edits to fixture payloads, the CI workflow, or the gate script still trigger both builds. Pushes to either protected branch compare the previous and new tips. A documentation-only push can reuse Windows validation after the previous tip's CI run succeeds on the same branch. A failed, pending, or missing run triggers a fresh Windows gate. If CI cannot determine the changed paths, it runs the applicable builds. The required `CI passed` check reports the combined result, including documentation-only runs.

## Pull request lint

The [Lint workflow](../../.github/workflows/lint.yml) checks changed first-party C and C++ lines under `src/`, `include/`, and `tests/` with LLVM 18 and `.clang-format`. It runs `actionlint` when workflows change.

To check committed changes locally, run this from the repository root in Bash with Git and the native LLVM 18 `clang-format-18` binary on your path. Substitute the pull request's target branch for `origin/main` if needed.

```bash
bash scripts/check_changed_format.sh "$(git merge-base origin/main HEAD)"
```

Set `JISTATS_CLANG_FORMAT` to the LLVM 18 executable if it has a different name or location. The check reports source locations that need formatting.

Use [the Windows clang gate script](../../scripts/run_clang_quality_gate.ps1) for full-tree formatting and `clang-tidy` analysis.

## Native UI coverage

Verify tray icon lifecycle, tooltip and menu interaction, taskbar placement, Explorer recovery, and sleep/resume in a live session when the change touches them. Win32 sends `WM_CREATE` during `CreateWindowExW`. Initialize the presenter window handle before calling helpers that require it during `WM_CREATE`.
