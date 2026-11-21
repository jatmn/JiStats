# OpenAI Subscription v1.0 Requirements

> **Status**: Planning
> **Purpose**: Turn the OpenAI subscription-first scope into buildable product requirements
> **Driven by**: `docs/openai-mirror-spec.md`, `docs/openai-scope-decision.md`, `docs/offline-retry-policy.md`, and `docs/gap-handling-policy.md`

## 1. Summary

JiStats must support OpenAI in v1.0 even when the user has:

- a ChatGPT / Codex subscription
- no OpenAI API billing
- no API key
- no trustworthy provider-side quota endpoint available

That means the base OpenAI product cannot be "API import with nicer labels."

The base OpenAI v1.0 product must instead provide:

- useful local subscription activity tracking
- clear confidence labeling
- truthful `Unknown` states for provider-side quota truth
- optional later separation for any independently configured API-platform import

## 2. Product Goal

For an OpenAI subscription user, JiStats should answer:

1. What OpenAI activity have I observed recently on this device?
2. What local OpenAI usage history do I have over time?
3. Which OpenAI values are known locally, estimated, imported, or unknown?

It must **not** claim exact provider-side subscription quota state unless that state is validated from a trustworthy official signal.

## 3. OpenAI Surface Model

OpenAI support must distinguish these surfaces:

### 3.1 Base Surface: `openai_subscription`

- ChatGPT / Codex / workspace subscription usage
- default OpenAI v1.0 surface
- local-first
- may have no provider-side quota truth available

Concrete v1 source buckets under that base surface are:

- `openai_subscription_local`
- `openai_subscription_codex_local`
- `openai_subscription_export_import`

### 3.2 Optional Surface: `openai_api_import`

- separate OpenAI API platform data
- requires independent API billing/credentials
- not part of the base subscription promise
- must remain visibly distinct in storage, analytics, and UI

## 4. Required v1.0 User Experience

### 4.1 Dashboard Requirements

The dashboard must show, for OpenAI subscription users:

- recent locally observed activity
- historical local usage trend summaries
- freshness of the local observation stream
- provider-truth confidence for quota-related values
- explicit `Unknown` state for provider-side quota values when not validated

The dashboard must not:

- imply that local activity equals provider-side total usage
- show a fake percent remaining
- invent reset windows
- collapse subscription-local data and optional API-import data into one unlabeled stream

### 4.2 Tray Requirements

The tray surface must show:

- overall OpenAI state
- whether local observation is active
- whether the app is offline / stale / catching up
- a short explanation if provider-side quota truth is unavailable

### 4.3 Taskbar Requirements

The taskbar surface must:

- prefer local activity intensity for OpenAI subscription users
- show offline/stale state cleanly
- avoid showing a quota percent unless that value is backed by a validated signal
- fall back to a neutral state instead of misleading quota pressure

## 5. Confidence And State Rules

### 5.1 Confidence Tiers

OpenAI subscription metrics must use:

| Tier | Meaning |
|------|---------|
| `LocalExact` | Exact for what JiStats directly observed on this device |
| `Imported` | From a separately validated imported surface |
| `Estimated` | Derived locally with clear inference |
| `Unknown` | No trustworthy basis exists |

### 5.2 OpenAI Runtime States

The OpenAI provider state shown in UI should map to:

| State | Meaning |
|------|---------|
| `Live` | JiStats is actively observing local OpenAI activity or has fresh OpenAI-derived data |
| `Offline` | JiStats cannot reach required surfaces or is otherwise disconnected |
| `Stale` | Local data exists, but freshness threshold has expired |
| `Syncing` | A local catch-up or optional import job is in progress |
| `Unknown Provider Truth` | Local activity may be known, but provider-side quota truth is unavailable |

`Unknown Provider Truth` may be rendered through badges/labels even if the underlying provider state enum stays `Live`/`Stale`/`Offline`.

## 6. Required Local Signals

JiStats must define and persist a minimal local observation model for OpenAI subscription activity.

### 6.1 Minimum Event Fields

Each locally observed OpenAI subscription event should be able to store:

- `provider_surface`: `openai_subscription_local`
- `event_time_utc`
- `activity_kind`
- `model_label` when known
- `usage_unit_value` when known
- `usage_unit_type` when known
- `source_confidence`
- `raw_reference`

### 6.2 Activity Kinds

v1.0 should support a flexible activity taxonomy rather than assuming API requests only:

| Activity Kind | Meaning |
|---------------|---------|
| `message_send` | User submitted a prompt/message |
| `response_complete` | A response finished |
| `session_active` | OpenAI surface was actively in use |
| `tool_run` | A Codex/tooling action occurred |
| `unknown_activity` | Observable activity existed but could not be classified precisely |

### 6.3 Optional Fields

When available, JiStats may additionally store:

- input/output token estimates
- request/response durations
- model family
- workspace/account label
- cost estimate

But v1.0 must not depend on these being available.

## 7. Gap And Offline Requirements

OpenAI subscription tracking must integrate with the existing gap/offline policies:

- if the app was not running, the missing period is **not zero by default**
- if the subscription surface provides no trustworthy backfill, missing periods become `Unknown`
- offline/reconnection UX must remain non-blocking
- local observation freshness must be shown separately from provider-truth confidence

## 8. Display Rules

### 8.1 Dashboard Labels

Examples of acceptable OpenAI subscription wording:

- `Recent activity: 12 locally observed events`
- `Provider quota state: Unknown`
- `Last local observation: 8 min ago`
- `History contains unknown periods`

Examples of unacceptable wording:

- `67% remaining` when no validated quota source exists
- `OpenAI total usage today` when only local activity is known
- `Resets in 3 hours` without validated provider evidence

### 8.2 Badge Rules

All OpenAI subscription-facing metrics must carry one of:

- `Local`
- `Imported`
- `Estimated`
- `Unknown`

### 8.3 Imported API Data Rule

If optional OpenAI API import exists later:

- imported metrics must be separately badged
- imported API history must not silently overwrite subscription-local history
- a user must be able to tell which OpenAI surface each metric came from

## 9. v1.0 Non-Goals For OpenAI Subscription

The following are explicitly out of scope unless validated:

- exact remaining message count
- exact provider-side spend
- exact quota reset countdown
- exact organization/project usage totals
- forced dependency on API keys or platform billing

## 10. Implementation Slices

### Slice A: Data Model

- add/confirm `provider_surface` distinction in runtime models
- define local OpenAI subscription event shape
- separate optional API import from subscription-local events

### Slice B: Observation Pipeline

- define how local OpenAI subscription activity is detected
- persist minimal events with confidence labels
- record freshness and gaps

### Slice C: UX

- dashboard wording for `Unknown Provider Truth`
- tray/taskbar fallback behavior when only local signals exist
- confidence badges and stale/offline handling

### Slice D: Analytics

- local rolling windows from subscription events
- unknown-period propagation into reports/charts
- no fake quota percentages without provider truth

## 11. Acceptance Criteria

OpenAI subscription support is v1.0-ready when:

1. A subscription-only user can use JiStats without API credentials
2. Local OpenAI activity appears in dashboard/tray/taskbar flows
3. Provider-side quota truth is labeled `Unknown` when not validated
4. Offline/gap periods are handled according to policy
5. Optional API import, if later added, remains visibly separate

## 12. Open Questions

1. What is the first practical local observation mechanism for OpenAI subscription activity?
2. What OpenAI/Codex surfaces are realistically observable from Windows in v1.0?
3. Should `Unknown Provider Truth` be a first-class enum value or a presentation-layer badge?
4. What minimum event granularity is useful enough for taskbar/dashboard without over-promising?

## References

- `docs/openai-mirror-spec.md`
- `docs/openai-scope-decision.md`
- `docs/gap-handling-policy.md`
- `docs/offline-retry-policy.md`
- `docs/taskbar-metrics.md`
