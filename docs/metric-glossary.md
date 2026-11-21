# v1.0 Metric Glossary

> **Status**: Current recommended MVP metric list
> **Purpose**: Keep dashboard, taskbar, and reporting vocabulary aligned with the shipped runtime model instead of older prototype-only metric ideas

This glossary is intentionally smaller than the earlier planning taxonomy.

It focuses on the metrics and labels JiStats should treat as the current MVP baseline across:

- dashboard quota views
- dashboard activity summaries
- taskbar overlay rows
- HTML/reporting output

## Current Runtime Vocabulary

### Shared Runtime States

| Term | Meaning | Current Surface Use |
| --- | --- | --- |
| `Current` | Provider data is fresh enough for normal display | Dashboard status, taskbar runtime fallback, reports |
| `Stale` | Last known provider data is still usable, but freshness has aged out | Dashboard status, tray warning state, taskbar degraded rows, reports |
| `Catching Up` | JiStats is reconciling or backfilling after a gap | Dashboard recovery wording, reports, future taskbar/runtime fallback |
| `Offline` | Provider connectivity is unavailable | Dashboard status, tray offline state, taskbar degraded rows/runtime fallback, reports |
| `Unknown` | The app cannot produce a trustworthy current state | Planning/reporting vocabulary when no reliable state can be inferred |

### Shared Provider-State Vocabulary

| Term | Meaning | Notes |
| --- | --- | --- |
| `Live` | Provider snapshot is fresh and usable | Normal provider row state |
| `Stale` | Provider snapshot exists but is too old to count as fresh | May still show quota values with degraded emphasis |
| `Syncing` | Provider/account path is actively catching up | Real in the shared runtime model; still only partially provider-fed end to end |
| `Offline` | Provider fetch failed or connectivity is unavailable | Can surface alongside another provider that is still live |

### Confidence And Provenance Vocabulary

Use the current repo vocabulary consistently:

| Label | Meaning |
| --- | --- |
| `LocalExact` | Exact for activity JiStats directly observed on this machine or exact for approved local aggregate/runtime counters; not automatically exact provider quota truth |
| `Imported` | Loaded from a separately validated provider or user-export surface |
| `Exact` | Direct provider-returned counter or window field |
| `HighConfidence` | Strongly supported by validated behavior with minor inference or cadence assumptions |
| `Estimated` | Derived from partial provider signals plus local inference |
| `Unknown` | No trustworthy basis exists for the metric |

OpenAI source labels remain:

- `openai_subscription_local`
- `openai_subscription_codex_local`
- `openai_subscription_export_import`
- `openai_api_import`

Current local-first provider source labels should remain explicit about provenance. The current shipped examples include:

- `claude_code_local_stats`
- `claude_code_local_runtime`
- `kimi_local_runtime`

## Current v1.0 Provider Posture

The current shipped vocabulary should stay honest about the four-provider v1.0 boundary:

| Provider | Shipped v1.0 posture | Honest quota/display boundary |
| --- | --- | --- |
| OpenAI / Codex | Subscription-first plus bounded Codex-local and export/import support | Local activity can be `LocalExact`; provider-exact quota truth still depends on the validated source path in use |
| MiniMax | Subscription-first with validated remains endpoints as the main mirror anchors | Coding Plan / Token Plan remains are the strongest current provider-exact quota sources |
| Claude Code | Bounded local-first support through approved `~/.claude` artifacts | Local activity/runtime counters may be `LocalExact`, but quota truth remains `Estimated` or `Unknown` unless separate corroboration exists |
| Kimi Code | Bounded local-first support through approved `~/.kimi` runtime artifacts, plus validated `/usages` quota mirroring when usable OAuth exists | Local runtime metadata may be `LocalExact`; provider-exact quota truth is available only through the validated `/usages` path and should stay empty rather than fabricated when that path is unavailable |

## Recommended MVP Metric List

### 1. Shared Quota And Freshness Metrics

These are the core MVP metrics that should stay aligned across dashboard, taskbar, and reporting.

| Metric ID | User-Facing Meaning | Primary Surfaces | Current Source |
| --- | --- | --- | --- |
| `runtime_state` | Shared current/stale/catching-up/offline state | Dashboard, taskbar fallback, tray, reports | Derived shared runtime model |
| `provider_state` | Live/stale/syncing/offline state for one provider row | Dashboard, taskbar, reports | Provider-fed runtime model |
| `connection_label` | Short explanation of connection/runtime health | Dashboard help text, reports | Shared runtime model |
| `last_sync_utc` | Time of the last successful provider snapshot or import run | Dashboard table, reports | Provider snapshot/import metadata |
| `quota_used_percent` | Percentage used in the dashboard quota table `Used` column | Dashboard quota table | Normalized quota snapshot |
| `quota_remaining_percent` | Percentage remaining for summary cards and taskbar rows | Dashboard summary cards, taskbar, reports | Normalized quota snapshot |
| `quota_used_value` | Supporting used count/value for the current window | Dashboard details, reports | Provider/imported snapshot |
| `quota_limit_value` | Current validated limit for the same window | Dashboard table/details, reports | Provider/imported snapshot |
| `quota_remaining_value` | Remaining count/value in the same window | Dashboard table/details, reports | Provider/imported snapshot or derived from validated counters |
| `quota_window_label` | Current window label such as interval/family wording | Dashboard, reports | Provider snapshot metadata |
| `quota_confidence_tier` | Confidence badge for the quota snapshot | Dashboard, reports | Snapshot confidence metadata |
| `provider_surface_label` | Concrete provenance/source label for the row | Reports first; dashboard/taskbar only when helpful | Provider-surface/source-label field |

### 2. Local Activity Metrics

These remain the current MVP activity metrics for the subscription-first baseline.

| Metric ID | User-Facing Meaning | Primary Surfaces | Current Source |
| --- | --- | --- | --- |
| `activity_sessions_5m` | Count of recent local sessions/messages in the last 5 minutes | Dashboard summary, reports | `activity_events` |
| `activity_sessions_30m` | Count of recent local sessions/messages in the last 30 minutes | Dashboard summary, reports | `activity_events` |
| `activity_sessions_5h` | Count of recent local sessions/messages in the last 5 hours | Dashboard summary, reports | `activity_events` |
| `activity_sessions_today` | Count of local sessions/messages today | Dashboard summary, reports | `activity_events` |
| `activity_active_minutes_5m` | Locally observed active minutes in the last 5 minutes | Dashboard summary, reports | `activity_events.duration_seconds` |
| `activity_active_minutes_30m` | Locally observed active minutes in the last 30 minutes | Dashboard summary, reports | `activity_events.duration_seconds` |
| `activity_active_minutes_5h` | Locally observed active minutes in the last 5 hours | Dashboard summary, reports | `activity_events.duration_seconds` |
| `activity_active_minutes_today` | Locally observed active minutes today | Dashboard summary, reports | `activity_events.duration_seconds` |

### 3. Reporting And Recovery-Support Metrics

These are part of the current MVP reporting vocabulary even when they are not all first-class taskbar fields.

| Metric ID | User-Facing Meaning | Primary Surfaces | Current Source |
| --- | --- | --- | --- |
| `is_backfilled` | Indicates periods that were reconstructed after a gap | Reports, dashboard recovery wording | Recovery/backfill metadata |
| `has_unknown_gaps` | Indicates periods JiStats could not reconstruct confidently | Reports, future dashboard/support copy | Gap metadata |
| `scheduled_export_last_run_utc` | Last successful scheduled HTML export time | Dashboard, reports | Scheduled export state |
| `scheduled_export_status` | Whether scheduled export is enabled and healthy | Dashboard, reports | Scheduled export settings/runtime state |
| `codex_tokens_per_minute_5m` | Codex token throughput (tokens/min) over the last 5 minutes | Analysis workspace | `usage_events` with `source_type = codex_local_rollout` |
| `codex_prompts_per_minute_5m` | Codex prompt throughput (prompts/min) over the last 5 minutes | Analysis workspace | `usage_events` with `source_type = codex_local_rollout` |
| `codex_tokens_per_minute_30m` | Codex token throughput (tokens/min) over the last 30 minutes | Analysis workspace | `usage_events` with `source_type = codex_local_rollout` |
| `codex_prompts_per_minute_30m` | Codex prompt throughput (prompts/min) over the last 30 minutes | Analysis workspace | `usage_events` with `source_type = codex_local_rollout` |

## Surface Rules

### Dashboard

The dashboard MVP should keep these display rules stable:

- provider quota table uses `quota_used_percent` for `Used`
- summary cards and bars use `quota_remaining_percent`
- raw used/limit/remaining values remain supporting detail, not ambiguous headline numbers
- activity windows use the local `activity_*` family above
- runtime copy uses `Current`, `Stale`, `Catching Up`, and `Offline`

### Taskbar

The taskbar MVP should stay smaller than the dashboard:

- provider label
- `quota_remaining_percent`
- compact state icon/color treatment
- runtime-only fallback row when no provider rows are available and the runtime is degraded
- local-first Claude/Kimi rows or activity summaries, if present, must stay honest about provenance and must not be read as provider-exact quota parity

The taskbar should not be treated as the place to mirror the full dashboard metric taxonomy.

### Reporting

Reports should preserve:

- runtime state
- quota snapshot values and confidence
- provider-surface/source labeling when available
- explicit local-first wording for Claude/Kimi when the data came from approved local artifacts rather than a provider-exact quota surface
- gap/backfill context
- scheduled export status where relevant

## Explicitly Deferred Or Non-MVP Metrics

These ideas still exist in older planning notes, but they are not part of the current recommended MVP metric list:

- a permanently visible taskbar request-rate meter such as `↑42/m`
- standalone taskbar RPM/TPM rate-limit gauges
- synthetic dashboard/taskbar quota totals that collapse subscription and API-import truth together
- exact provider-side OpenAI subscription quota totals when no validated source exists
- broad per-model imported dashboards shown by default without explicit validated imported data

## References

- `docs/dashboard-quota-display-contract.md`
- `docs/taskbar-metrics.md`
- `docs/taskbar-overlay-display-contract.md`
- `docs/openai-runtime-source-label-contract.md`
- `docs/openai-mirror-spec.md`
- `docs/minimax-mirror-spec.md`
