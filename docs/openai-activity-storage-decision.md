# OpenAI Subscription Activity Storage Decision

> **Status**: Decision recorded; baseline implemented
> **Purpose**: Decide how OpenAI subscription session activity should be stored
> **Driven by**: `docs/openai-observation-mechanism-decision.md` and `docs/openai-subscription-requirements.md`

## Decision

**Do not force OpenAI subscription session events into `usage_events`.**

**Add a separate generalized `activity_events` model/table for subscription-local activity observation.**

This is the recommended v1.0 direction.

## Why `usage_events` Is The Wrong Fit

The current `usage_events` model is centered on quantitative usage units:

- `request_type`
- `input_tokens`
- `output_tokens`
- `total_tokens`
- `cost_minor_units`
- imported/local request-style semantics

That shape works well for:

- API request history
- imported provider usage units
- token/cost analytics

It is a poor fit for the first OpenAI subscription mechanism, which is:

- foreground-window session observation
- local-only
- duration-based
- often missing tokens/cost/request counts

If we shove session data into `usage_events`, we create several problems:

1. **Semantic mismatch**
   - a `session_active` observation is not the same as a request or usage unit

2. **Field abuse**
   - duration would need to be hidden in unrelated fields or dropped entirely

3. **Analytics confusion**
   - existing rollups assume token/request/cost semantics

4. **Provider model drift**
   - `usage_events` would stop meaning "usage unit" and become a mixed bag of requests, imports, and sessions

## Recommended Model

Introduce a separate generalized local-observation stream:

### `activity_events`

This table should represent:

- observed local subscription activity
- session-based foreground activity
- future non-API signals such as tool runs or UI-observed actions

### High-Level Separation

| Table | Meaning |
|------|---------|
| `usage_events` | Quantified usage units such as API requests, imported provider usage, token/cost events |
| `activity_events` | Locally observed activity/session events that may not map to tokens, cost, or requests |

## Proposed `activity_events` Shape

Minimum recommended columns:

| Field | Purpose |
|------|---------|
| `id` | PK |
| `provider_id` | Provider link |
| `account_id` | Account link |
| `plan_id` | Optional plan link |
| `provider_surface` | Concrete source bucket such as `openai_subscription_local`, `openai_subscription_codex_local`, `openai_subscription_export_import`, or `openai_api_import` |
| `activity_kind` | `session_active`, `message_send`, `tool_run`, `unknown_activity` |
| `started_at_utc` | Activity/session start |
| `ended_at_utc` | Activity/session end |
| `duration_seconds` | Session duration when applicable |
| `source_type` | `local`, `imported`, `derived` |
| `source_run_id` | Observation run identifier |
| `surface_key` | Recognized surface identifier / matcher name |
| `window_title_hint` | Optional normalized title or label |
| `confidence_tier` | `LocalExact`, `Estimated`, `Imported`, `Unknown` |
| `raw_reference` | Raw artifact or detector reference |
| `created_at_utc` | Created timestamp |

## Why This Should Be Generalized Instead Of OpenAI-Only

Although OpenAI triggered the issue, the table should be generalized because the same shape may later be needed for:

- Codex subscription activity
- future browser-based provider subscriptions
- workspace-local AI app usage
- UI-observed, non-tokenized activity from other providers

That keeps the architecture provider-agnostic instead of creating an OpenAI-only special case.

For OpenAI specifically:

- `openai_subscription` is still a useful high-level product category
- concrete stored provenance should use the normalized source buckets instead of the broad category label when the exact basis is known

## How `activity_events` And `usage_events` Work Together

### v1.0 Relationship

- `activity_events` powers local session/activity tracking
- `usage_events` powers token/request/cost-oriented analytics
- dashboards may show both, but with clear provenance

### Example

For a subscription-only OpenAI user:

- dashboard tiles may show session counts and active minutes from `activity_events`
- quota truth may remain `Unknown`
- no fake token/cost totals are required

For an API-import user later:

- imported API data lands in `usage_events`
- local session observation still lands in `activity_events`
- analytics/UI can compare the two without collapsing them into one model

## Rollup Implications

Do not immediately force `activity_events` into existing `usage_rollups_hourly` / `usage_rollups_daily`.

Instead:

1. keep existing usage rollups focused on quantified usage
2. add separate activity rollups later if needed, such as:
   - session count
   - active minutes
   - last active timestamp

This avoids corrupting existing usage analytics semantics.

## Schema Recommendation

Recommended migration path:

- add a new migration after the current schema baseline
- create `activity_events`
- add indexes on:
  - `(provider_id, account_id, started_at_utc)`
  - `(provider_surface, activity_kind, started_at_utc)`
  - `(source_type, started_at_utc)`

Optional later additions:

- `activity_rollups_hourly`
- `activity_rollups_daily`

## Interface Implications

The current `IProviderAdapter::fetchUsage()` return type should remain tied to quantified `UsageEvent` data.

Do **not** overload `UsageEvent` to carry session observations.

Instead, if needed later:

- introduce a separate `ActivityEvent` runtime model
- add a local observation service outside the provider-adapter request/response contract

This keeps provider adapters focused on provider data and keeps local observation as its own subsystem.

## Acceptance Criteria

This decision is correctly adopted when:

1. OpenAI subscription session data has a home that matches its semantics
2. `usage_events` remains cleanly focused on quantified usage units
3. future subscription providers can reuse `activity_events`
4. dashboard/taskbar OpenAI local activity can be built without inventing request/token data

## Follow-Up Tasks

1. Update `docs/entity-model.md` to add `activity_event`
2. Update `docs/sqlite-schema.md` with a new migration for `activity_events`
3. Define the runtime model and service boundary for local observation
4. Decide whether the first activity rollups are needed for v1.0 or can be deferred

## References

- `docs/openai-observation-mechanism-decision.md`
- `docs/openai-subscription-requirements.md`
- `docs/sqlite-schema.md`
- `docs/entity-model.md`
