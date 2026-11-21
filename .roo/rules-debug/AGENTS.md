# AGENTS.md

This file provides guidance to debug agents working with this repository.

## Project Context

- JiStats is a Windows 11 native app for tracking OpenAI, Codex, and MiniMax usage.
- The repo is in early implementation mode, so debug work should check both live code and live docs.
- Read the root `AGENTS.md` first, then `docs/plan.md` and `docs/progress.md`.

## Provider Boundaries

- OpenAI / Codex debugging should assume the current subscription-first product boundary unless a task explicitly targets API-platform paths.
- MiniMax debugging should assume subscription-backed paths are the current v1.0 focus; PAYG validation is follow-on work.
- Quota mismatches should be treated as potentially signal-boundary issues, not automatically as computation bugs.

## Log Locations

- App log: `%LOCALAPPDATA%/JiStats/logs/jistats.log`
- The logger uses a 5 MB rotating file sink with `MAX_FILES = 3` in `src/core/Logger.cpp`.

## Log Categories

`Startup`, `Sync`, `Retry`, `Offline`, `Reconciliation`, `Mirror`, `Taskbar`, `Db`, `General`

## Credential Safety

Never log these directly:

- API keys or subscription secrets
- bearer tokens
- `UsageResponse::rawResponse`
- `QuotaResponse::rawResponse`
- raw request/response bodies

Use `sanitize::mask(value)` when a sensitive value must appear in logs.

## Common Execution Paths

Common quota path:

```
AppController → ProviderManager → SyncScheduler → MirrorEngine → presenters/taskbar
```

Common local-observation path:

```
LocalObservationService → ObservationPersistenceBridge → StorageService
```

Common historical import path:

```
OpenAIExportImport → StorageService → reporting/runtime summaries
```

Treat these as common paths, not the only ones.

## Connectivity State Model

```
starting → steady_online ↔ degraded_offline
                ↓
             syncing → catching_up
                ↓
          error_blocked → shutting_down
```

## Mirror Confidence Tiers

`Exact` → `HighConfidence` → `Estimated` → `Unknown`

## Debug Rules

- Check logs first, then the relevant provider/state transitions.
- Confirm whether a failure is in provider retrieval, persistence, mirror recomputation, or UI presentation before patching.
- For auth/import/quota issues, verify the product boundary first so unsupported surfaces are not mistaken for regressions.
- No blocking network or DB work should happen on the UI thread; UI stalls during sync are strong signals to inspect scheduling/thread ownership.
