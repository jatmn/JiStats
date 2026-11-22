# JiStats

JiStats is a local-first Windows 11 desktop app built with native C++17 and CMake. It tracks AI provider activity, imports usage history, and mirrors quota where supported. The dashboard, tray, and taskbar show distinct views of that data.

## Working agreement

- Use [docs/plan.md](docs/plan.md) for scope, architecture, and roadmap, and [docs/progress.md](docs/progress.md) for current work and handoffs. Update the tracker when work changes state and update the relevant docs when behavior or decisions change.
- Check the implementation and its tests when documentation disagrees with the code. Keep changes focused and fit new work into the existing design.
- Use the [capability matrix](docs/capability-matrix.md) for provider evidence and the [release checklist](docs/release-checklist.md) for release signoff.

## Provider and privacy boundaries

- All four integrations are opt-in. OpenAI/Codex is subscription-first, with separate credentials and billing for the optional API-platform path. MiniMax focuses on subscription quota, with broader PAYG support tracked separately.
- Claude Code uses bounded local evidence. Kimi Code uses bounded local evidence and can show provider-exact quota through the validated OAuth-backed surface when available. Preserve the provenance and confidence of each metric and show uncertainty when source evidence is incomplete.
- Prefer official provider signals and non-model endpoints. If a model call is needed for monitoring, use the lowest-cost suitable path and make it explicit and configurable.
- Use the existing [logger and categories](include/core/Logger.hpp). Keep API keys, tokens, and raw request or response bodies out of logs. Never log `UsageResponse::rawResponse` or `QuotaResponse::rawResponse` directly, because they can hold raw provider payloads or local Codex session content. Use `sanitize::mask` when a sensitive value needs a log reference. The application log is at `%LOCALAPPDATA%/JiStats/logs/jistats.log`.
- Keep live captures outside the checkout. Commit only synthetic or appropriately redacted [fixtures](docs/fixtures/README.md), keeping account details, private paths, and conversation contents out of the repository.

## Implementation

- Use the `JiStats` namespace and keep the mirrored [include/](include/) and [src/](src/) layout aligned. Follow surrounding C++ conventions and established PIMPL usage in classes that use it. Prefer native APIs and keep dependencies small.
- Keep provider-specific behavior under [src/providers/](src/providers/) and shared models provider-agnostic. AppController coordinates lifecycle and runtime state. MirrorEngine reconciles quota, while analytics and reporting produce derived summaries.
- Route database writes through StorageService and place numbered SQL migrations in [db_migrations/](db_migrations/). Keep blocking network and database work off the UI thread. Dashboard, tray, and taskbar code should consume derived views.
- Preserve startup responsiveness, offline recovery, and quota provenance. Keep the taskbar overlay scoped to subscription quota. Use [the TrafficMonitor research note](docs/trafficmonitor-research.md) for taskbar reference.

## Build and verify

Use Visual Studio 2026 with MSVC and run commands from the repository root. See [README.md](README.md) for Release builds and packaging.

~~~powershell
cmake -S . -B build -G "Visual Studio 18 2026" -DJISTATS_BUILD_TESTS=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
~~~

- Use [docs/test-strategy.md](docs/test-strategy.md) to choose verification for the change and record what ran and any blockers in the handoff.
- For storage, schema, or SQLite persistence changes, run the [SQLite-enabled quality gate](docs/sqlite-storage-quality-gate.md). Run [scripts/run_sqlite_quality_gate.ps1](scripts/run_sqlite_quality_gate.ps1) locally or dispatch the [Windows CI workflow](.github/workflows/ci.yml).
- Use the [manual taskbar checks](docs/taskbar-manual-validation.md) when changing Windows shell behavior.
