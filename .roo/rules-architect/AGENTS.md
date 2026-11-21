# AGENTS.md

This file provides guidance to architect agents working with this repository.

## Project State

- JiStats is in early implementation mode, not planning-only.
- Read the root `AGENTS.md` first for current product boundaries.
- Treat `docs/plan.md` as the architecture and roadmap source of truth.
- Treat `docs/progress.md` as the live execution-state and handoff source of truth.

## Product Boundary

- OpenAI / Codex is the primary proof-of-concept provider, and current v1.0 planning is subscription-first rather than assumed OpenAI API-platform billing access.
- MiniMax is also in v1.0 scope, with subscription-backed tracking first and PAYG API validation kept as follow-on work.
- Quota mirroring is the central product feature; uncertainty must stay explicit when provider signals are incomplete.

## Threading Model

The intended runtime split remains:

```
UI thread         - Win32 message loop, native dashboard shell, tray, taskbar
Network worker    - Provider API requests, retry, connectivity checks
Database worker   - SQLite writes, migrations, retention, import transactions
Analytics worker  - Mirror recomputation, rollups, cache refresh
```

Optional ImGui surfaces may exist, but the current dashboard shell uses native Win32 controls.

## Concurrency Rules

- No blocking network or DB work on the UI thread.
- All DB writes stay serialized through `StorageService`.
- Mirror recomputation should operate on immutable input snapshots.
- UI surfaces consume derived view models, never raw provider or DB objects.
- Background workers publish state through explicit events or message passing.

## Component Architecture

- `AppController` owns startup, shutdown, and shell/runtime coordination.
- `ProviderManager` owns provider adapters, accounts, and capability metadata.
- `SyncScheduler` owns polling cadence, retry, backoff, and catch-up timing.
- `MirrorEngine` computes mirrored quota state and confidence/provenance.
- `AnalyticsEngine` computes rolling summaries and trend views.
- `StorageService` owns SQLite access, migrations, and transactions.
- `ConnectivityService` models offline state and recovery timing.
- `NotificationService` owns dismissible notices and cooldown behavior.
- `DashboardPresenter`, `TaskbarPresenter`, and `TrayController` are shell-facing adapters over derived state.
- `ReportingService` exports persisted and mirrored data without becoming the live data source.

## Operating States

Use the documented runtime state model when evaluating architecture drift:

```
starting → steady_online ↔ degraded_offline
                ↓
             syncing → catching_up
                ↓
          error_blocked → shutting_down
```

## Provider Abstraction Requirements

- Provider adapters must handle credentials, capability discovery, usage/quota retrieval, and provider-specific interpretation.
- Core models must remain provider-agnostic enough to support future providers without baking in OpenAI- or MiniMax-only assumptions.
- Subscription-backed and API-backed surfaces must remain distinguishable in the data model and UX.

## Mirror Engine Requirements

- Treat quota mirroring as a first-class architectural concern, not a reporting add-on.
- Combine imported provider data with local observations when available.
- Record confidence tier and provenance for mirrored values.
- Support reconciliation after downtime, offline periods, and delayed imports.
- Keep mirror logic reusable across providers.

## Directory Structure

```
src/platform/      - Windows-specific shell hooks, startup, credentials
src/taskbar/       - Windows 11 taskbar integration
src/providers/     - Provider adapters and provider-specific import logic
src/core/          - Shared runtime infrastructure
src/db/            - SQLite schema, migrations, retention
src/analytics/     - Mirror and summary computation
src/gui/           - Dashboard and notification shell code
src/reporting/     - Export/report generation
```

## Toolchain

- MSVC on Windows is the primary build target today.
- CMake is the build system.
- The current build config targets C++17 (`CMakeLists.txt`), even where planning docs discuss a more modern preference.
- `clang-format` and `clang-tidy` remain the preferred formatting and static-analysis tools.

## Dependency Policy

- Justify each added dependency in terms of purpose, license, binary/runtime cost, and replaceability.
- Prefer native solutions for solvable native problems.
- Optional UI/logging libraries should remain optional where the repo already treats them that way.

## Handoff Requirements

Before marking architecture work done:

- Update `docs/progress.md` when task state or architecture decisions change.
- Record architecture-impacting decisions in the relevant docs during the same session.
- Call out any code/doc drift explicitly instead of assuming either side is automatically right.
