# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

JiStats is a native Windows 11 C++17 desktop app that mirrors AI provider quota state and usage. Initial providers are OpenAI/Codex and MiniMax. The project is in early implementation phase with core infrastructure built.

## Build Commands

```bash
# Configure (from repo root)
cmake -B build -G "Visual Studio 18 2026"

# Build
cmake --build build --config Release

# Output binary: build/bin/JiStats.exe
```

Visual Studio baseline: use Visual Studio 2026. Microsoft installs it under the major-version `18` path/root, so filesystem references should prefer `...\Microsoft Visual Studio\18\...` even when the product name is "Visual Studio 2026".

CMake options: `JISTATS_BUILD_TESTS`, `JISTATS_BUILD_EXAMPLES`, `JISTATS_USE_SYSTEM_SQLITE`, `JISTATS_USE_SYSTEM_JSON` (all OFF by default).

## Architecture

**Layered design with PIMPL pattern throughout:**

- **App layer** (`src/app/`) - `main.cpp` has dual WinMain/main entry points. `AppController` owns lifecycle: initializes DB, loads settings, creates presenters, enters GUI event loop.
- **Provider layer** (`src/providers/`) - `IProviderAdapter` interface with `fetchUsage()`, `fetchQuota()`, `validateCredentials()`. `ProviderManager` orchestrates multiple adapters. MiniMax adapter exists; OpenAI adapter not yet implemented.
- **Analytics layer** (`src/analytics/`) - `MirrorEngine` reconciles local events with provider-reported quota into confidence-tiered snapshots (High/Medium/Low). `AnalyticsEngine` computes trends, daily summaries, cost breakdowns.
- **Database layer** (`src/db/`) - `StorageService` wraps SQLite3 (bundled in `third_party/sqlite/`). `SchemaManager` runs SQL migrations from `db_migrations/`. `SettingsService` provides typed access (bool/int/string) to a key-value settings table.
- **UI layer** (`src/gui/`, `src/taskbar/`) - `DashboardPresenter` renders a native Win32 dashboard window with a `DashboardViewModel`. `TaskbarPresenter` shows quota metrics in the Windows 11 taskbar.

**Data flow:** Provider APIs -> IProviderAdapter -> MirrorEngine (confidence-tiered reconciliation) -> AnalyticsEngine (trends) -> DashboardPresenter/TaskbarPresenter -> SQLite persistence.

**Headers mirror source structure:** `include/` parallels `src/` directory layout.

## Key Conventions

- All public classes use PIMPL (`std::unique_ptr<Impl>`)
- Namespace: `JiStats::`
- C++17 required, no managed runtimes
- Windows SDK deps: comctl32, d3d11, dxgi, user32, gdi32
- `WIN32_LEAN_AND_MEAN` and `NOMINMAX` are defined project-wide
- Database migrations are numbered SQL files in `db_migrations/`
- Confidence tiers (High/Medium/Low) are central to quota data quality

## Logging

Structured logging via `Logger` static class (`include/core/Logger.hpp`), backed by spdlog (bundled in `third_party/spdlog/`).

- **Usage:** `JISTATS_LOG_INFO(LogCategory::Startup, "message")` - macros available at all log levels (trace/debug/info/warn/error/critical)
- **Categories:** `Startup`, `Sync`, `Retry`, `Offline`, `Reconciliation`, `Mirror`, `Taskbar`, `Db`, `General` - matching plan.md section 14.5 requirements
- **Log file:** `%LOCALAPPDATA%/JiStats/logs/jistats.log` with 5 MB rotation, 3 rotated files (20 MB max)
- **Credential safety:** Never log API keys, tokens, or raw request/response bodies. Use `sanitize::mask(value)` when logging values that might contain sensitive data. `UsageResponse::rawResponse` and `QuotaResponse::rawResponse` must never be logged directly.
- **Init/shutdown:** `Logger::init()` is called first in `AppController::init()`, `Logger::shutdown()` last in `AppController::shutdown()`. Safe to call log methods before init (silently returns).

## Working Agreement

- `docs/plan.md` is source of truth for roadmap and scope
- `docs/progress.md` is source of truth for execution state - **update it when task state changes**
- Keep tasks small, vertically sliced, and handoff-friendly
- Prefer non-model API endpoints for telemetry/usage; if a model call is needed, use lowest-cost path and make it configurable
- Cost-aware background behavior: monitoring must not meaningfully increase spend

## Key Documentation

- `AGENTS.md` - Product direction, constraints, working agreement
- `docs/plan.md` - Comprehensive implementation plan (~51KB)
- `docs/progress.md` - Live execution tracker (check first for current state)
- `docs/entity-model.md` - 14-entity data model
- `docs/sqlite-schema.md` - Database schema details
- `docs/capability-matrix.md` - Provider API capabilities
- `docs/openai-mirror-spec.md` / `docs/minimax-mirror-spec.md` - Quota mirroring specs
- `docs/test-strategy.md` - Testing approach
