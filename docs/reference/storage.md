# Storage and schema

[StorageService](../../src/db/StorageService.cpp) owns SQLite persistence. The default database is `%LOCALAPPDATA%\JiStats\jistats.db` on Windows. Use [numbered migrations](../../db_migrations/) and the [SQLite quality gate](../validation/sqlite-quality-gate.md) when changing persistence.

| Data | Role |
| --- | --- |
| `schema_version`, `providers`, `accounts`, `plans`, `credential_profiles` | Schema version and provider, account, plan, and credential identity. MiniMax profiles persist a secret reference and validation metadata. |
| `usage_events` | Quantified usage such as requests, tokens, or cost, when the source supplies it. |
| `activity_events` | Bounded local observations or imported history. Keep source, confidence, and any available durable identifier. |
| `quota_snapshots`, `quota_snapshot_rollups_daily` | Persisted quota readings and daily history for longer analysis windows. |
| `sync_state`, `connectivity_state`, `runtime_state`, `provider_capabilities`, `gap_markers`, `recovery_checkpoints` | Refresh and backfill progress, offline state, and known coverage gaps. |
| `usage_rollups_hourly`, `usage_rollups_daily` | Aggregated quantified usage with provenance and backfill counts. |
| `codex_local_sources`, `codex_thread_import_state` | Local SQLite source and thread baselines used to compute subsequent usage deltas. |
| `settings`, `export_runs` | App preferences and report export history. |

When local observation lacks a verified provider account, it uses a synthetic `local-observation:<providerSurface>` binding. This can combine multiple real accounts using the same local surface. Keep that fallback stable when adding richer identity.

The runtime retention job prunes `usage_events` and `gap_markers` after 90 days, `usage_rollups_hourly` after 30 days, and `usage_rollups_daily` after 365 days. This job leaves `activity_events` intact, including local observations and imported ChatGPT activity. The Codex throughput and SQLite model-attribution sources have separate query scopes. Keep them out of general subscription usage rollups.

## Migration behavior

The portable executable embeds the [version 12 SQLite baseline](../../db_migrations/012_baseline_schema.sql). [SchemaManager](../../src/db/SchemaManager.cpp) initializes it in one transaction, including when the app starts outside the portable folder. The baseline seeds OpenAI, MiniMax, Claude Code, Kimi Code, and HICAP, the default settings, and the singleton runtime-state row. Settings control which integrations the app enables.

Existing version 12 databases reopen in place. The historical loader applied only one of the two migrations numbered `010`. On startup JiStats adds missing rollup provenance columns, recovery or export tables and indexes, or the Kimi provider row in a single transaction. It preserves existing data, settings, and schema history. If the schema version cannot be read, startup stops migration before applying schema changes.

JiStats rejects development databases at versions 1 through 11 without modifying them. The desktop warning gives the database path and explains that persistence is disabled. Follow the [backup and restart instructions](../quick-start.md#launch) to start fresh.

Use `013` for the next numbered SQL migration in [`db_migrations/`](../../db_migrations/). The loader discovers later files relative to the working directory. Advance `schema_version` in the same transaction as the schema change. When a release includes a later migration, verify its discovery during actual [packaged startup and sign-in](../validation/desktop.md#dashboard-and-background-checks).
