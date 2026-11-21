# Provider, Account, Plan, and Mirror Entity Model

> **Status**: Partially implemented
> **Purpose**: Define the core storage entities for provider-agnostic usage tracking and quota mirroring, while distinguishing what already exists from what is still planned
> **Driven by**: `docs/plan.md`, `docs/openai-mirror-spec.md`, and `docs/minimax-mirror-spec.md`

## Model Overview

The v1.0 storage model should be read in two layers:

- implemented baseline: entities that already exist in migrations `001-008`
- planned extension layer: entities still reserved for later performance or registry refinement work

The central relationship stays:

`provider -> account -> plan -> usage/activity/quota history`

Mirror-specific state now layers on top of that registry through shipped baseline tables:

`account + plan -> sync_state + quota_snapshots + mirror_rules + connectivity_state + gap_markers`

## Implementation Status by Entity

| Entity | Status | Notes |
|--------|--------|-------|
| `providers` | Implemented | Created in migration `001`; seeded with OpenAI and MiniMax |
| `accounts` | Implemented | Created in migration `001`; represents provider scope anchors |
| `credential_profiles` | Implemented | Created in migration `001`; stores secret references, not secrets |
| `plans` | Implemented | Created in migration `001`; currently sufficient for initial plan/account linkage |
| `usage_events` | Implemented | Created in migration `001`; holds quantified usage history |
| `quota_snapshots` | Implemented | Created in `001` and expanded in `004` with baseline mirror metadata |
| `activity_events` | Implemented | Created in migration `003`; stores local subscription/session activity |
| `sync_state` | Implemented | Added in migration `004`; tracks import cursors, retries, and reconciliation flow |
| `mirror_rules` | Implemented | Added in migration `004`; stores provider-specific quota mirroring rules |
| `connectivity_state` | Implemented | Added in migration `004`; tracks offline/retry visibility per account |
| `runtime_state` | Implemented | Added in migration `005`; stores launch recovery and long-gap catch-up state |
| `provider_capabilities` | Implemented | Added in migration `006`; records which provider signals are actually available |
| `usage_rollups_hourly` | Implemented | Added in migration `007`; caches hourly quantified usage aggregates |
| `usage_rollups_daily` | Implemented | Added in migration `007`; caches daily quantified usage aggregates |
| `gap_markers` | Implemented | Added in migration `008`; stores known-zero / reconstructed / unknown recovery windows |

## Core Registry Entities

### `providers`

Represents a provider registry entry such as OpenAI or MiniMax.

Current minimum fields:

- `id`
- `provider_key`
- `display_name`
- `enabled`
- `created_at_utc`
- `updated_at_utc`

Storage role:

- parent of every provider-scoped account
- home for provider-wide capabilities and mirror rules

### `accounts`

Represents a user-visible provider scope such as an org, workspace, project, or local observation bucket.

Current minimum fields:

- `id`
- `provider_id`
- `account_key`
- `display_name`
- `scope_type`
- `scope_identifier`
- `enabled`
- `created_at_utc`
- `updated_at_utc`

Storage role:

- anchor for sync and connectivity state
- parent for plans and usage history
- stable binding for local observation that should not invent fake provider usage rows

### `credential_profiles`

Stores credential references and metadata for an account without storing the raw secrets in SQLite.

Current minimum fields:

- `id`
- `provider_id`
- `account_id`
- `profile_name`
- `secret_ref`
- `credential_type`
- `created_at_utc`
- `updated_at_utc`
- `last_validated_at_utc`

Storage role:

- allows one account to hold multiple auth paths when providers expose separate API and subscription credentials
- the current storage seam is now concrete enough for MiniMax subscription polling metadata:
  - `profile_name`, `secret_ref`, and `credential_type` persist the credential reference
  - `last_validated_at_utc` persists the latest successful validation timestamp when known
  - runtime-only validation state/error details should remain outside SQLite until a real need justifies a schema expansion

### `plans`

Represents the quota or billing context currently attached to an account.

Current minimum fields:

- `id`
- `account_id`
- `plan_key`
- `plan_type`
- `display_name`
- `quota_window`
- `limit_value`
- `is_active`
- `created_at_utc`
- `updated_at_utc`

Storage role:

- attaches quota and usage history to a concrete plan context
- allows one account to carry both API and subscription-backed plan rows over time

Provider staging guidance:

- OpenAI API pay-as-you-go can map to `plan_type="payg"`
- OpenAI subscription/local observation contexts can map to `plan_type="subscription"` when a user-visible plan exists
- MiniMax Token Plan maps cleanly to `plan_type="subscription"` and a monthly window
- MiniMax Coding Plan maps to `plan_type="subscription"` with a daily or other provider-confirmed reset window

## History and Mirror Entities

### `usage_events`

Stores one quantified usage record per observed or imported usage unit.

Current minimum fields:

- `id`
- `provider_id`
- `account_id`
- `plan_id`
- `event_time_utc`
- `source_type`
- `source_run_id`
- `model_key`
- `request_type`
- `input_tokens`
- `output_tokens`
- `total_tokens`
- `cost_minor_units`
- `is_backfilled`
- `raw_reference`
- `created_at_utc`

Semantic rule:

- use `usage_events` only for quantified usage units such as request, token, or cost rows
- do not use it for subscription presence/session activity

### `activity_events`

Stores locally observed session/activity records that do not naturally fit quantified usage semantics.

Current minimum fields:

- `id`
- `provider_id`
- `account_id`
- `plan_id`
- `provider_surface`
- `activity_kind`
- `started_at_utc`
- `ended_at_utc`
- `duration_seconds`
- `source_type`
- `source_run_id`
- `surface_key`
- `window_title_hint`
- `confidence_tier`
- `raw_reference`
- `created_at_utc`

Semantic rule:

- `activity_events` is the durable home for local subscription observation
- it should remain separate from `usage_events`

### `quota_snapshots`

Stores mirrored quota state over time.

Implemented today:

- `id`
- `provider_id`
- `account_id`
- `plan_id`
- `snapshot_time_utc`
- `window_start_utc`
- `window_end_utc`
- `used_value`
- `limit_value`
- `confidence_tier`
- `basis_summary`
- `created_at_utc`

Expanded fields now present:

- `quota_dimension`
- `remaining_value`
- `is_reconciled`

Storage role:

- records the mirror output over time rather than only the latest quota state
- supports “what did we think the quota state was at this time?” debugging

### `sync_state`

Implemented entity for per-account sync progress and failure metadata.

Current fields:

- `id`
- `provider_id`
- `account_id`
- `sync_kind`
- `cursor_value`
- `last_attempt_at_utc`
- `last_success_at_utc`
- `last_failure_at_utc`
- `last_failure_category`
- `created_at_utc`
- `updated_at_utc`

Storage role:

- now available as the baseline storage home for reconciliation/import progress

Timestamp note:

- `last_attempt_at_utc`, `last_success_at_utc`, and `last_failure_at_utc` are the semantic sync timestamps
- `created_at_utc` and `updated_at_utc` are row-audit timestamps that are also present in the shipped migration `004` schema and `PersistedSyncState`

### `mirror_rules`

Implemented entity for provider-specific mirroring configuration and rule versioning.

Current fields:

- `id`
- `provider_id`
- `rule_key`
- `rule_version`
- `config_json`
- `created_at_utc`

Storage role:

- captures the logic hints and versioning needed when OpenAI and MiniMax quota behavior differ

### `connectivity_state`

Implemented entity for last-known connectivity and retry timing.

Current fields:

- `id`
- `provider_id`
- `account_id`
- `status`
- `failure_category`
- `last_checked_at_utc`
- `next_retry_at_utc`
- `notice_dismissed_at_utc`
- `created_at_utc`
- `updated_at_utc`

Storage role:

- keeps offline handling explicit rather than implicit in transient runtime state

Timestamp note:

- `last_checked_at_utc`, `next_retry_at_utc`, and `notice_dismissed_at_utc` are the operational connectivity timestamps
- `created_at_utc` and `updated_at_utc` are row-audit timestamps that are also present in the shipped migration `004` schema and `PersistedConnectivityState`

## Support and Recovery Entities

### `runtime_state`

Implemented singleton state for launch recovery and catch-up behavior.

Current fields:

- `id`
- `last_launch_at_utc`
- `last_successful_sync_utc`
- `last_local_observation_utc`
- `recovery_marker`
- `created_at_utc`
- `updated_at_utc`

### `gap_markers`

Implemented table for persisted recovery and missing-period classification.

Current fields:

- `id`
- `provider_id`
- `account_id`
- `period_start_utc`
- `period_end_utc`
- `classification`
- `basis_summary`
- `source_kind`
- `created_at_utc`
- `updated_at_utc`

Storage role:

- records whether a recovered period is currently understood as `known_zero`, `reconstructed`, or `unknown`
- gives runtime and reporting surfaces a durable source for `hasUnknownGaps` and `isBackfilled` instead of inferring those flags ad hoc

### `provider_capabilities`

Implemented table for discovered provider support rather than assumptions.

Current fields:

- `id`
- `provider_id`
- `capability_key`
- `supported`
- `config_json`
- `discovered_at_utc`

## Implemented Performance Caches

### `usage_rollups_hourly` and `usage_rollups_daily`

Implemented aggregate caches for performance, not source-of-truth history.

Current fields:

- `id`
- `provider_id`
- `account_id`
- `rollup_period_start_utc`
- `model_key`
- `request_count`
- `input_tokens`
- `output_tokens`
- `total_tokens`
- `total_cost_minor_units`
- `created_at_utc`
- `updated_at_utc`

## Recommended Migration Staging

The entity model should land in this order:

1. stabilize `providers`, `accounts`, `credential_profiles`, and `plans` as the shared registry
2. keep `usage_events` and `activity_events` as the two distinct history streams
3. add `sync_state`, expand `quota_snapshots`, and add `mirror_rules` plus `connectivity_state`
4. persist launch/runtime recovery in `runtime_state` and discovered provider support in `provider_capabilities`
5. rebuild and consume rollups only where quantified `usage_events` queries justify the cache layer
6. persist gap classification in `gap_markers` once launch/offline recovery is real

That order keeps the provider/account/plan graph in place before mirror logic and avoids coupling storage work to dashboard or tray behavior. The new rollup tables cache quantified `usage_events`; they do not replace raw event history or the separate local-first `activity_events` stream.

## Consistency Decisions

### `settings` naming

The live schema uses a physical table named `settings`. Planning docs may still refer to the conceptual area as `app settings`, but implementation should not assume an `app_settings` table exists yet.

### `quota_snapshots` maturity

Treat the current `quota_snapshots` table as a usable baseline mirror schema. Future expansion should now be driven by real runtime needs rather than by the earlier planning gap.

### `activity_events` maturity

`activity_events` is no longer hypothetical. It is part of the implemented data model and should stay in any storage planning documents.
