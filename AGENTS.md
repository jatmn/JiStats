# JiStats

JiStats is a local-first Windows 11 desktop app built with native C++17 and CMake. It tracks AI provider activity, imports usage history, and mirrors quota where supported. The dashboard, tray, and taskbar show distinct views of that data.

## Working agreement

- Start at the [documentation index](docs/README.md) for current guides and technical references.
- The maintainer sets the roadmap and decides what to merge. Contributions and bug reports are welcome. Follow [CONTRIBUTING.md](CONTRIBUTING.md) and use the [roadmap](docs/plan.md) to check how proposed changes fit the project.
- Record evidence and handoffs in the [progress log](docs/progress.md), and update the relevant technical docs when behavior or decisions change.
- Check the implementation and its tests when documentation disagrees with the code. Keep changes focused and fit new work into the existing design.
- Use the [capability matrix](docs/capability-matrix.md) for provider evidence and the [release checklist](docs/release-checklist.md) for release signoff.

## Platform, UI, and performance

- Write application code in native C++17 and use the existing CMake build. Do not add Python code, scripts, tooling, or dependencies to the repository.
- Target Windows 11. Preserve the intentional Windows XP-era look of the desktop UI while keeping it easy to use on Windows 11. Preserve keyboard navigation, readable contrast, and DPI scaling. Use the [desktop validation guide](docs/validation/desktop.md) for affected UI behavior.
- Prioritize low memory use and speed. Keep the idle and steady-state memory footprint small, startup quick, and the UI responsive during background work. Avoid unnecessary allocations, retained data, dependencies, and background work. When measuring performance, compare the relevant memory use or timings before and after on Windows 11 under the same workload.

## Provider and privacy boundaries

- Keep provider tracking opt-in. OpenAI/Codex is subscription-first, with separate credentials and billing for the optional API-platform path. See the [Codex SQLite reference](docs/providers/openai.md#codex-sqlite-privacy-boundary) for the separate model attribution opt-in. MiniMax focuses on subscription quota, with broader PAYG support tracked separately.
- Claude Code uses bounded local evidence. Kimi Code uses bounded local evidence and can show provider-exact quota through the validated OAuth-backed surface when available. Preserve the provenance and confidence of each metric and show uncertainty when source evidence is incomplete.
- Prefer official provider signals and non-model endpoints. If a model call is needed for monitoring, use the lowest-cost suitable path and make it explicit and configurable.
- Use the existing [logger and categories](include/core/Logger.hpp). Keep API keys, tokens, and raw request or response bodies out of logs. Never log `UsageResponse::rawResponse` or `QuotaResponse::rawResponse` directly, because they can hold raw provider payloads or local Codex session content. Use `sanitize::mask` when a sensitive value needs a log reference. The application log is at `%LOCALAPPDATA%/JiStats/logs/jistats.log`.
- Keep live captures outside the checkout. Commit only synthetic or appropriately redacted [fixtures](docs/fixtures/README.md), keeping account details, private paths, and conversation contents out of the repository.

## Implementation

- Use the `JiStats` namespace and keep the mirrored [include/](include/) and [src/](src/) layout aligned. Follow surrounding C++ conventions and established PIMPL usage in classes that use it. Prefer native APIs and keep dependencies small.
- Keep provider-specific behavior under [src/providers/](src/providers/) and shared models provider-agnostic. AppController coordinates lifecycle and runtime state. MirrorEngine reconciles quota, while analytics and reporting produce derived summaries.
- Route database writes through StorageService and place numbered SQL migrations in [db_migrations/](db_migrations/). Keep blocking network and database work off the UI thread. Dashboard, tray, and taskbar code should consume derived views.
- Preserve startup responsiveness, offline recovery, and quota provenance. Keep the taskbar overlay scoped to subscription quota. Use [the TrafficMonitor research note](docs/reference/trafficmonitor-research.md) for taskbar reference.

## Test failure evidence

During the [two-week observation window](docs/progress.md#test-failure-observation), record each test failure when found with the failing run, case and file, the fix, and what behavior the test protected. Keep unresolved entries open and exclude their files from removal. On resolution, classify a caught regression or an assertion that only detected an intended implementation change. Record fixture and infrastructure defects explicitly when neither description fits. A whole file is eligible for removal only after the window, when its evidence shows exclusively change detection and its remaining assertions protect no behavior. Record the demonstrated pattern here when removing it. Keep authorization, privacy, migration and concurrency tests.

## Build and verify

From Linux, run the portable tests before pushing code changes. For Windows-facing changes, also cross-build the app, CLI and full test executable with MinGW-w64. Both command sequences and dependencies are in the [Linux verification commands](docs/validation/testing.md#linux-commands).

CI runs the portable tests on Linux and shards the cross-built suite across hosted Windows runners when native validation is required. See the [CI rules](docs/validation/testing.md#continuous-integration) for when these jobs run. Use the [build and packaging instructions](README.md#build-and-packaging) for MinGW releases from Linux or Windows.

- Use the [verification guide](docs/validation/testing.md) to choose verification for the change and record what ran and any blockers in the handoff.
- For storage, schema, or SQLite persistence changes, run the [SQLite-enabled quality gate](docs/validation/sqlite-quality-gate.md). Run the bundled-SQLite portable tests on Linux and check the full Windows suite in CI. Use [manual CI dispatch](.github/workflows/ci.yml) to test any branch.
- Use the [manual taskbar checks](docs/validation/desktop.md#taskbar-checks) when changing Windows shell behavior.
