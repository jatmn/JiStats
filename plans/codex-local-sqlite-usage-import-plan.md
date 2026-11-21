# Codex local SQLite usage import plan

## Context

JiStats currently reads Codex local `rollout-*.jsonl` files for quota/rate-limit mirroring, but does not import Codex token usage into `usage_events`. Codex also keeps a local `%USERPROFILE%\.codex\state_5.sqlite` database whose `threads` table contains per-thread cumulative token totals plus provider/model attribution.

Inspection found provider/model rows for `openai`, `hicap-ai`, and `minimax`. This makes `state_5.sqlite.threads` a useful local usage attribution source, while rollout JSONL remains the quota/rate-limit source.

User decision: **new-only import**. First scan establishes baseline state only. JiStats must not backfill all historic thread totals unless a future explicit backfill mode is requested.

## Source contract

### Existing quota source

- Path: `%USERPROFILE%\.codex\sessions\**\rollout-*.jsonl`
- Current parser: `payload.type == "token_count"` with `payload.rate_limits`
- Current persistence: `quota_snapshots`
- Current limitation: no `usage_events` rows are created from this source

### New usage attribution source

- Path: `%USERPROFILE%\.codex\state_5.sqlite`
- Table: `threads`
- Safe columns to read:
  - `id`
  - `model_provider`
  - `model`
  - `reasoning_effort`
  - `tokens_used`
  - `created_at_ms`
  - `updated_at_ms`
- Fields intentionally excluded from JiStats normalized storage:
  - `title`
  - `cwd`
  - prompts/messages/conversation bodies
  - auth/config/API-key material

## Storage design

Add migration after current highest migration, likely:

- `db_migrations/012_add_codex_local_sqlite_usage.sql`

Add `codex_local_sources`:

- `id INTEGER PRIMARY KEY`
- `source_path TEXT NOT NULL`
- `source_kind TEXT NOT NULL DEFAULT 'codex_state_sqlite'`
- `source_identity TEXT NOT NULL`
- `first_seen_utc INTEGER NOT NULL`
- `last_seen_utc INTEGER NOT NULL`
- `last_read_utc INTEGER`
- `UNIQUE(source_path, source_identity)`

Add `codex_thread_import_state`:

- `id INTEGER PRIMARY KEY`
- `source_id INTEGER NOT NULL REFERENCES codex_local_sources(id)`
- `thread_id TEXT NOT NULL`
- `model_provider TEXT NOT NULL`
- `model_key TEXT NOT NULL`
- `reasoning_effort TEXT`
- `first_seen_created_at_ms INTEGER`
- `last_seen_updated_at_ms INTEGER`
- `baseline_tokens_used INTEGER NOT NULL DEFAULT 0`
- `last_seen_tokens_used INTEGER NOT NULL DEFAULT 0`
- `last_imported_tokens_used INTEGER NOT NULL DEFAULT 0`
- `baseline_established_utc INTEGER NOT NULL`
- `last_event_time_utc INTEGER`
- `counter_reset_count INTEGER NOT NULL DEFAULT 0`
- `last_status TEXT NOT NULL DEFAULT 'active'`
- `updated_utc INTEGER NOT NULL`
- `UNIQUE(source_id, thread_id)`

Use existing `usage_events` for normalized deltas:

- `source_type = 'codex_local_sqlite'`
- deterministic `source_run_id`
- deterministic `raw_reference` based on source/thread/counter range
- `model_key = threads.model`
- `total_tokens = delta`
- no prompt/title/cwd content in `raw_reference`

If existing duplicate checks are not enough for crash-safe retries, add a unique source-event key/index.

## Reader design

Add:

- `include/providers/openai/CodexLocalSqliteReader.hpp`
- `src/providers/openai/CodexLocalSqliteReader.cpp`

Behavior:

1. Resolve source path:
   - `%CODEX_HOME%\state_5.sqlite`
   - `%USERPROFILE%\.codex\state_5.sqlite`
   - `$HOME/.codex/state_5.sqlite`
2. Open SQLite read-only.
3. Verify `threads` and required columns exist.
4. Query only safe columns.
5. Return sanitized rows.
6. Never mutate, vacuum, or delete Codex source DB.

## Importer design

Add:

- `include/providers/openai/CodexLocalUsageImporter.hpp`
- `src/providers/openai/CodexLocalUsageImporter.cpp`

Algorithm:

1. Read sanitized rows from Codex `state_5.sqlite`.
2. Upsert `codex_local_sources`.
3. For each row with supported provider and `tokens_used > 0`:
   - no existing state: insert baseline state, create no `usage_events`.
   - existing state with higher counter: insert one delta `usage_events` row, update state in same transaction.
   - existing state with same counter: update last-seen metadata only.
   - existing state with lower counter: treat as reset, update baseline/current state, create no negative event.
4. Rebuild affected usage rollups.
5. Update `sync_state` for `codex_local_sqlite_refresh`.

Provider mapping:

- `openai` -> OpenAI provider
- `hicap-ai` -> HICAP provider
- `minimax` -> defer unless existing provider registry path is ready; not required for requested charts

## App integration

Update:

- `src/app/AppController.cpp`
- `include/app/AppController.hpp` if needed

Run importer:

- on app launch/startup refresh
- during local refresh cadence when source DB file metadata changes
- without network/model calls
- gracefully when source DB is missing, locked, or has incompatible schema

## Overview chart design

Add Dashboard overview panels:

1. OpenAI local Codex usage
   - one line per model
2. HICAP local Codex usage
   - one line per model

Chart data:

- source filter: `codex_local_sqlite`
- provider filter: `openai` or `hicap-ai`
- series key: `model_key`
- point value: token delta per time bucket

Labels:

- `Observed local Codex usage since JiStats baseline`
- empty state: `No new Codex SQLite usage observed yet. First scan establishes baseline.`

UI files likely touched:

- `include/gui/AnalysisViewModel.hpp` or `include/gui/DashboardViewModel.hpp`
- `src/gui/DashboardPresenter.cpp`
- `src/reporting/AnalysisQueryEngine.cpp` if existing dashboard data path flows through analysis model

Render rules:

- reuse existing Codex throughput chart drawing patterns where practical
- include y-axis scale and model legend
- expand/stagger Overview layout as needed for readability

## Tests

Add or extend tests for:

- migration creates new tables/indexes
- reader extracts safe fixture rows
- missing/incompatible source DB handled gracefully
- first scan baselines only and creates zero usage events
- second scan with increased `tokens_used` creates one delta event
- repeated scan creates no duplicate
- counter decrease creates reset status and no negative event
- OpenAI/HICAP provider-model totals remain separate
- rollups include `codex_local_sqlite` as local source
- Dashboard view model exposes OpenAI/HICAP per-model series
- Dashboard empty state appears after baseline with no deltas

## Verification

Run:

```sh
cmake --build build --target JiStats
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Release
```

Manual smoke:

1. Start app with existing `%USERPROFILE%\.codex\state_5.sqlite`.
2. Confirm first run establishes baseline and does not show historic totals as new chart usage.
3. Use Codex after baseline.
4. Refresh/start app again.
5. Confirm OpenAI/HICAP charts show only new observed deltas, split by model.
6. Confirm original Codex DB is unchanged.

## Risks

- `threads.tokens_used` is cumulative; importing raw totals directly would overcount and violate new-only decision.
- Raw Codex DB can contain sensitive local history; JiStats should store sanitized usage metadata only.
- Existing migrations include two `010_...` files; verify migration ordering before adding next file.
