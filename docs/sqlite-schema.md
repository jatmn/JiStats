# SQLite Schema and Migration Order

> **Status**: Partially implemented
> **Purpose**: Define the storage baseline, document what exists today, and recommend the next migration order for v1.0 storage work
> **Driven by**: [docs/entity-model.md](entity-model.md) and `docs/plan.md` section 11

## Current State

The repository already ships SQLite migrations through version `010`.

| Version | State | Summary |
|---------|-------|---------|
| `001_initial_schema.sql` | Implemented | Creates `schema_version`, `providers`, `accounts`, `credential_profiles`, `plans`, `usage_events`, `quota_snapshots`, and `settings`; seeds OpenAI and MiniMax provider rows |
| `002_add_taskbar_preference.sql` | Implemented | Seeds `ui.taskbar_overlay_enabled` in `settings` |
| `003_add_activity_events.sql` | Implemented | Adds `activity_events` plus supporting indexes |
| `004_add_mirror_state_tables.sql` | Implemented | Expands `quota_snapshots` with mirror metadata and adds `sync_state`, `mirror_rules`, and `connectivity_state` |
| `005_add_runtime_state.sql` | Implemented | Adds the singleton `runtime_state` table for launch recovery and catch-up checkpoints |
| `006_add_provider_capabilities.sql` | Implemented | Adds `provider_capabilities` for persisted capability discovery metadata |
| `007_add_usage_rollups.sql` | Implemented | Adds hourly and daily quantified-usage rollup caches |
| `008_add_gap_markers.sql` | Implemented | Adds persisted `gap_markers` for known-zero / reconstructed / unknown recovery windows |
| `009_add_claude_provider.sql` | Implemented | Seeds the Claude Code provider row |
| `010_add_kimi_provider.sql` | Implemented | Seeds the Kimi Code provider row |

The docs previously described a larger end-state schema as if it were already in migration `001`. That is no longer accurate. The recommended approach is to treat `001-010` as the current baseline and stage any further expansion through additive migrations.

## Migration Principles

- Keep migrations additive and forward-only.
- Treat `001-010` as the current landed history.
- Prefer one focused storage concern per migration or per tightly related migration pair.
- Expand mirror-related tables only after the provider/account/plan registry remains stable.
- Avoid physical renames that create churn unless the code is ready to migrate in the same change.

## Recommended Migration Order

### Phase 0: Existing baseline

These migrations already exist and should remain the starting point for any new database.

- `001` foundation registry and first-pass usage storage
- `002` taskbar preference seed
- `003` local `activity_events` persistence
- `004` mirror-state tables plus `quota_snapshots` metadata
- `005` runtime-state singleton persistence
- `006` provider capability metadata persistence
- `007` quantified `usage_events` rollup caches
- `008` gap-marker persistence for recovery/missing-period classification
- `009` Claude Code provider seeding
- `010` Kimi Code provider seeding

### Phase 1: Provider/account/plan hardening

No additional migration is queued here yet. The remaining provider/account/plan expansion should only land when a concrete multi-account or provider-runtime need demands it.

Recommended future schema additions in this phase:

- `accounts`
  - keep current shape
  - optionally add descriptive metadata only if backed by a real provider signal later
- `plans`
  - add `provider_plan_key` only if `plan_key` becomes user-facing rather than provider-facing
  - add `billing_currency` if spend quota mirroring needs it
  - keep one active row per current provider plan context
- `usage_events`
  - remain the durable history table for quantified provider usage
  - do not absorb session-style subscription activity

Why this phase comes first:

- mirror tables depend on stable `provider -> account -> plan` relationships
- provider/account/plan semantics are shared by OpenAI and MiniMax
- this work can land independently of any UI or polling implementation

### Phase 2: Query and performance support

Landed Phase 2 migration:

| Version | Purpose |
|---------|---------|
| `007_add_usage_rollups.sql` | Add hourly and daily usage rollup caches plus retention-ready query keys |

These tables now follow the baseline storage and runtime state instead of preceding it.

## Entity Staging by Migration

| Entity | Current status | Recommended migration stage |
|--------|----------------|-----------------------------|
| `providers` | Implemented in `001`, expanded in `009` and `010` | Seed rows now cover the four shipped providers; user-facing activation is controlled by persisted dashboard preferences rather than by disabling the registry rows themselves |
| `accounts` | Implemented in `001` | Keep as the anchor for provider scopes; harden semantics in Phase 1 only if required |
| `credential_profiles` | Implemented in `001` | Leave stable for now; extend only when auth flows need more metadata |
| `plans` | Implemented in `001` | Phase 1 hardening before additional mirror tables |
| `usage_events` | Implemented in `001` | Keep stable; add indexes later only when query pressure shows a need |
| `quota_snapshots` | Expanded in `004` | Baseline mirror metadata now exists; later migrations can extend only if real query needs appear |
| `activity_events` | Implemented in `003` | Keep separate from quantified usage history |
| `sync_state` | Implemented in `004` | Supports per-account sync cursors and failure tracking |
| `mirror_rules` | Implemented in `004` | Supports provider-specific rule versioning/configuration |
| `connectivity_state` | Implemented in `004` | Supports offline/retry visibility per provider account |
| `runtime_state` | Implemented in `005` | Supports launch recovery and catch-up checkpoints |
| `provider_capabilities` | Implemented in `006` | Supports persisted provider capability discovery metadata |
| `usage_rollups_hourly` | Implemented in `007` | Cached hourly token/cost/request aggregates over `usage_events` |
| `usage_rollups_daily` | Implemented in `007` | Cached daily token/cost/request aggregates over `usage_events` |
| `gap_markers` | Implemented in `008` | Persists known-zero / reconstructed / unknown runtime recovery windows |

## Implemented vs Planned Schema

### Implemented today

The current migrations create these tables:

- `schema_version`
- `providers`
- `accounts`
- `credential_profiles`
- `plans`
- `usage_events`
- `quota_snapshots`
- `sync_state`
- `mirror_rules`
- `connectivity_state`
- `runtime_state`
- `provider_capabilities`
- `settings`
- `activity_events`
- `usage_rollups_hourly`
- `usage_rollups_daily`
- `gap_markers`

### Planned but not yet migrated

These remain documentation targets rather than implemented schema:
- no additional post-`010` schema tables are currently queued

## Consistency Notes

### `settings` versus `app_settings`

`docs/plan.md` uses the logical name `app_settings`, but the actual database table is currently `settings`, and app code already reads and writes that physical table.

Recommendation:

- keep the physical table name `settings` until a dedicated rename migration is worth the churn
- use `app settings` as the conceptual label in docs
- if a rename is ever needed, do it in its own migration with matching code updates

### `schema_version`

The live migration includes:

- `version`
- `applied_at_utc`
- `description`

Any schema documentation should reflect the `description` column because it is already part of the stored migration history.

### `quota_snapshots`

The current table now includes the first mirror-oriented metadata pass:

- `quota_dimension`
- `remaining_value`
- `is_reconciled`

That gets the baseline mirror state into the shipped schema without rewriting the initial migration.

### `activity_events`

`activity_events` is already real schema, not just a future idea. Docs should continue to treat it as the durable home for locally observed subscription/session activity.

### `runtime_state`

`runtime_state` is now a shipped singleton table, not just a planning target.

Current stored fields:

- `last_launch_at_utc`
- `last_successful_sync_utc`
- `last_local_observation_utc`
- `recovery_marker`

### `gap_markers`

`gap_markers` is now a shipped recovery-classification table, not just a planning target.

Current stored fields:

- `provider_id`
- `account_id`
- `period_start_utc`
- `period_end_utc`
- `classification`
- `basis_summary`
- `source_kind`

### `credential_profiles`

`credential_profiles` is already sufficient for the first MiniMax subscription credential slice.

Current stored fields are enough to persist:

- `profile_name`
- `secret_ref`
- `credential_type`
- `last_validated_at_utc`

That means JiStats can keep the raw MiniMax subscription secret in Windows Credential Manager while SQLite stores only the credential reference and validation timestamp metadata.

### `provider_capabilities`

`provider_capabilities` is now a shipped table for persisted capability discovery.

Current stored fields:

- `provider_id`
- `capability_key`
- `supported`
- `config_json`
- `discovered_at_utc`

## Recommended Next DDL Scope

If implementation continues from this storage baseline, the cleanest next independent DDL or storage targets are:

1. expand plan/account registry only when a concrete provider/runtime need demands it
2. add retention policy for `gap_markers` once real long-gap volume exists
3. deepen provider-specific reconciliation/import state only after the first real PAYG/API-platform validation paths are proven

That keeps the remaining storage work aligned with actual runtime needs instead of expanding the schema speculatively.
