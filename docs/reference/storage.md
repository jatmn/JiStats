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

The runtime retention job prunes `usage_events` and `gap_markers` after 90 days, `usage_rollups_hourly` after 30 days, and `usage_rollups_daily` after 365 days. This job leaves `activity_events` intact, including local observations and imported ChatGPT activity. The Codex throughput and SQLite model-attribution sources have separate query scopes. General subscription usage excludes both sources from raw queries and rollups, including when raw events fill a missing rollup.

## Migration behavior

The portable executable embeds the [version 12 SQLite baseline](../../db_migrations/012_baseline_schema.sql), the [version 13 privacy upgrade](../../db_migrations/013_redact_claude_source_references.sql), the [version 14 ChatGPT activity repair](../../db_migrations/014_deduplicate_chatgpt_activity.sql), the [version 15 quota precision upgrade](../../db_migrations/015_quota_snapshot_precision.sql), the [version 16 Claude activity repair](../../db_migrations/016_deduplicate_claude_daily_activity.sql), and the [version 17 Kimi task activity repair](../../db_migrations/017_kimi_task_activity_identity.sql). [SchemaManager](../../src/db/SchemaManager.cpp) applies them even when the app starts outside the portable folder. The baseline seeds OpenAI, MiniMax, Claude Code, Kimi Code, and HICAP, the default settings, and the singleton runtime-state row. Settings control which integrations the app enables.

The version 13 migration upgrades existing version 12 databases in place before applying version 14. One transaction replaces raw Claude stats-cache references in activity and usage rows with hashed references and preserves the original events. SQLite then rebuilds the database to remove plaintext left in unused page space. The rebuild can require free disk space up to twice the size of the database. WAL checkpoint attempts share a five-second lock-wait budget. JiStats records version 13 after the rebuild and any WAL checkpoint succeed. If either fails, the next open retries the upgrade.

The historical loader applied only one of the two migrations numbered `010`. On startup JiStats adds missing rollup provenance columns, recovery or export tables and indexes, or the Kimi provider row in a single transaction. It preserves existing data, settings, and schema history. If the schema version cannot be read, startup stops migration before applying schema changes.

JiStats rejects development databases at versions 1 through 11 without modifying them. The desktop warning gives the database path and explains that persistence is disabled. Follow the [backup and restart instructions](../quick-start.md#launch) to start fresh.

Version 14 keeps the first stored imported ChatGPT activity row for each conversation/message reference. The delete and schema version update commit together.

Version 15 adds nullable source timestamp precision to raw quota snapshots. Existing rows keep their original whole-second timestamps and values. A matching Codex replay can fill in precision for the stored observation.

Version 16 keeps the first saved daily activity row per Claude stats cache and account. An opaque reference distinguishes caches, and an index prevents later cache revisions from recreating a day already saved.

Version 17 keeps one Kimi local runtime activity per provider account and task reference. It retains the newest saved terminal state when one exists and the newest unfinished state otherwise. The repair works with the stored rows alone.

Use `018` for the next numbered SQL migration in [`db_migrations/`](../../db_migrations/). The loader discovers later files relative to the working directory. Advance `schema_version` in the same transaction as the schema change. When a release includes a later migration, verify its discovery during actual [packaged startup and sign-in](../validation/desktop.md#dashboard-and-background-checks).
