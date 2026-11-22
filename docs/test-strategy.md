# Test Strategy and Fixture Plan

> **Status**: Implemented baseline with ongoing expansion
> **Purpose**: Describe the current repo test strategy, fixture layout, and verification lanes without reverting to planning-era placeholders
> **Driven by**: `docs/plan.md` section 14.3 plus the shipped test/configuration files in `tests/`, `scripts/`, and the [GitHub Actions workflow](../.github/workflows/ci.yml)

## Current Test Philosophy

JiStats now uses a layered test strategy:

- Catch2 unit tests for isolated logic
- fixture replay tests for OpenAI and MiniMax parser/runtime seams
- integration tests for app-controller, storage, reporting, and shell routing
- SQLite-enabled migration/storage gates using bundled SQLite
- manual Windows validation for tray/taskbar/shell behavior that cannot be trusted from unit tests alone

## Current Test Harness

- Framework: Catch2 via `third_party/catch2/catch_amalgamated.cpp`
- Primary target: `JiStatsTests`
- Test registration: `tests/CMakeLists.txt`
- Test discovery path: `add_test(NAME JiStatsTests ...)`
- Windows-specific compile stability: `/FS` is now enabled for the test target

## Current Test Categories

### Unit and Component Tests

Current coverage includes:

- `MirrorEngine`
- `AnalyticsEngine`
- `SyncScheduler`
- `SchemaManager`
- `StorageService`
- `NotificationService`
- `StartupManager`
- `TrayController`
- `DashboardPresenter`
- `TaskbarPresenter`
- `ReportingService`
- `ProviderManager`
- `OpenAIAdapter`
- `MiniMaxAdapter`
- `MiniMaxLiveCapture`
- `LocalObservationService`
- `ObservationPersistenceBridge`

### Fixture Replay and Provider Shape Tests

The repo already includes fixture-backed coverage for:

- OpenAI sample usage fixtures
- OpenAI Codex public rate-limit shape parsing
- OpenAI local rollout parsing under `%USERPROFILE%\\.codex\\sessions\\**\\rollout-*.jsonl`
- OpenAI export-import acceptance, rejection, and persistence
- MiniMax remains payload parsing and live-capture helper behavior

### Integration Tests

Current cross-component coverage includes:

- dashboard shell routing
- app-controller startup/tray/runtime behavior
- app-facing OpenAI historical import flow
- persisted local-observation wiring
- reporting metadata/export behavior
- storage migration and mirror-state persistence

### SQLite Quality Gate

The repo now has a dedicated SQLite-enabled verification lane:

- script: `scripts/run_sqlite_quality_gate.ps1`
- GitHub Actions: [CI](../.github/workflows/ci.yml) runs this gate on Windows and a MinGW-w64 cross-build on Linux for pull requests targeting `main` and `release/**`, and for manual runs. On pull requests, it uses the test merge commit to skip both builds for documentation, repository metadata, or scripts that CI does not use. Changes under `docs/fixtures/` run both builds because the tests read those fixtures. Pushes to `main` and `release/**` run the Windows gate on the integrated tree. The required `CI passed` check reports the combined result.

This is the current highest-signal automated storage/migration gate because it configures a clean bundled-SQLite build, builds `JiStatsTests`, and runs the full suite through `ctest`.

## Fixture Archive Reality

Fixture material now lives primarily under `docs/fixtures/`, not `test/fixtures/`.

Key fixture areas:

- `docs/fixtures/openai/`
- `docs/fixtures/minimax/`

Those archives are used by both docs and tests, so support docs and parser tests should continue to point at the same captured evidence whenever possible.

## Manual Validation Still Required

Some Windows-native behavior still needs live manual checks even when unit coverage exists:

- tray icon lifecycle
- taskbar overlay placement/transparency/alignment
- sleep/wake recovery
- Explorer restart behavior
- settings/shell affordances that depend on native windows behavior

Manual support artifacts already exist for that work:

- `docs/taskbar-manual-validation.md`
- `docs/taskbar-overlay-validation-scenarios.md`
- dashboard/taskbar display contracts under `docs/`

## Win32 Presenter Gotchas

One recurring class of failure in JiStats is Win32 lifecycle ordering inside presenter windows.

- If a helper such as `ensureConnectionTooltip()` or `ensureUsageTooltip()` guards on `window != nullptr`, assign `window = hwnd` at the top of the main `WM_CREATE` handler before calling any control-creation or helper-initialization path.
- Do not assume the later `CreateWindowExW(...)` return path in `runUiThread()` is early enough for presenter helpers that run during `WM_CREATE`; by then those helpers have already had a chance to return early and silently skip initialization.
- When a native shell affordance appears "wired correctly" but never shows up at runtime, check whether the dependent `HWND` or presenter state is being populated before the helper is first invoked.
- Keep this class of bug covered by both code review and manual smoke testing because unit tests may still pass while initialization order is wrong for live Win32 behavior.

Reference fix:

- commit `086a429` corrected the dashboard tooltip initialization path by assigning `window = hwnd` at the start of `DashboardPresenter::Impl::handleMainMessage(..., WM_CREATE, ...)`, before `createMainControls()` called `ensureConnectionTooltip()` / `ensureUsageTooltip()`.

## Recommended Local Commands

### Fast local test pass

```powershell
cmake --build build --config Debug --target JiStatsTests
ctest --test-dir build -C Debug --output-on-failure
```

### SQLite-enabled clean gate

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run_sqlite_quality_gate.ps1
```

### Focused targeted runs

```powershell
.\build_ci\bin\Debug\JiStatsTests.exe "[storage][rollups]"
.\build_ci\bin\Debug\JiStatsTests.exe "[schema][migration]"
.\build_ci\bin\Debug\JiStatsTests.exe "[openai]"
```

## Coverage Guidance

When adding a new slice:

- add unit coverage for deterministic logic
- add fixture coverage when parser or provider payload shape is involved
- add app/presenter integration coverage when a runtime affordance is introduced
- use the SQLite quality gate for migration/storage changes
- add or update manual validation docs when native shell behavior changes

## Workstream Responsibility

The ownership split still broadly maps as:

- Platform: startup/tray/lifecycle tests
- Storage: migrations, persistence, rollups, retention
- Provider: OpenAI and MiniMax adapters plus fixture replay
- Analytics: mirror/rollup behavior
- UX: presenter and dashboard shell behavior
- Taskbar: overlay semantics and manual desktop checks
- Reporting: export/report rendering and metadata
- Quality: CI, gating scripts, and test-lane reliability

## References

- `docs/plan.md`
- `docs/capability-matrix.md`
- `docs/fixtures/README.md`
- `docs/release-checklist.md`
