# Storage and schema

[StorageService](../../src/db/StorageService.cpp) owns SQLite persistence. The default database is `%LOCALAPPDATA%\JiStats\jistats.db` on Windows. Use [numbered migrations](../../db_migrations/) and the [SQLite quality gate](../validation/sqlite-quality-gate.md) when changing persistence.

| Data | Role |
| --- | --- |
| `providers`, `accounts`, `plans`, `credential_profiles` | Provider, account, plan, and credential identity. MiniMax profiles persist a secret reference and validation metadata. |
| `usage_events` | Quantified usage such as requests, tokens, or cost, when the source supplies it. |
| `activity_events` | Bounded local observations or imported history. Keep source, confidence, and any available durable identifier. |
| `quota_snapshots`, `quota_snapshot_rollups_daily` | Persisted quota readings and daily history for longer analysis windows. |
| `sync_state`, `connectivity_state`, `runtime_state`, `gap_markers`, `recovery_checkpoints` | Refresh and backfill progress, offline state, and known coverage gaps. |
| `usage_rollups_hourly`, `usage_rollups_daily` | Aggregated quantified usage with provenance and backfill counts. |
| `codex_local_sources`, `codex_thread_import_state` | Local SQLite source and thread baselines used to compute subsequent usage deltas. |
| `settings`, `export_runs` | App preferences and report export history. |

When local observation lacks a verified provider account, it uses a synthetic `local-observation:<providerSurface>` binding. This can combine multiple real accounts using the same local surface. Keep that fallback stable when adding richer identity.

The runtime retention job applies a 90-day cutoff for raw events and gap markers, 30 days for hourly usage rollups, and 365 days for daily usage rollups. The Codex throughput and SQLite model-attribution sources have separate query scopes. Keep them out of general subscription usage rollups.

## Migration behavior

[SchemaManager](../../src/db/SchemaManager.cpp) reads migrations from `db_migrations/` relative to the working directory. It advances from the recorded schema version through the largest numbered file, applying a transaction per version. If the directory is missing, discovery returns version zero and storage initialization skips migration. A running tray icon can therefore coexist with an uninitialized or older schema. Use the [portable launch instructions](../quick-start.md#launch) for first launch and upgrades, and verify the actual [sign-in route](../validation/desktop.md#dashboard-and-background-checks) on the release candidate.

The current highest numbered migration is [012](../../db_migrations/012_add_codex_local_sqlite_usage.sql). The two `010` scripts add analysis persistence metadata and seed the Kimi provider, respectively. The loader runs only the first match in directory order. Once version 10 is recorded, it does not revisit the other script. The repair path applies only to migration 004. The [schema tests](../../tests/test_schema_manager.cpp) check the analysis metadata upgrade. For [release verification](../release-checklist.md#evidence-backed-release-queue), inspect the actual SQLite schema and seed data so the recorded version does not stand in for both changes.
