# MiniMax Reset-Window Capture Inputs

> **Status**: Ready for repeated live validation
> **Purpose**: Make MiniMax Coding Plan and Token Plan reset-window evidence capture deterministic and repeatable

## Goal

These inputs exist to settle three remaining MiniMax questions without inventing quota behavior:

- what user-visible period label JiStats should show for each remains endpoint
- whether the returned `start_time` / `end_time` windows move on a fixed reset boundary or a rolling interval
- how fresh the remains payload appears around a reset boundary

## Current Archived Observation Status

The committed fixture archive currently contains one remains snapshot per validated subscription surface from `2026-04-17`.

That is enough to confirm the field inventory, but it is **not** enough to settle reset semantics.

Important nuance from the single archived payloads:

- the same remains response already shows more than one interval family across models
- some rows span roughly `5h`
- other rows span roughly `24h`
- weekly counters are also present alongside the interval counters

Implication:

- do not collapse Coding Plan or Token Plan into one plan-wide user-facing period label yet
- repeated live capture must stay model-aware until the evidence proves which interval families can safely share one label

## Required Credential Surfaces

Use one credential profile per validated subscription surface:

- Coding Plan
  - `planKey = coding_plan`
  - `endpointFamily = coding_plan_remains`
  - endpoint: `GET /v1/api/openplatform/coding_plan/remains`
- Token Plan
  - `planKey = token_plan`
  - `endpointFamily = token_plan_remains`
  - endpoint: `GET /v1/token_plan/remains`

## Capture Cadence

For each plan, capture the same endpoint on this cadence:

1. One baseline capture at least 2 hours before the expected reset boundary.
2. Three pre-reset captures:
   - `T-30m`
   - `T-10m`
   - `T-2m`
3. Four near-reset captures:
   - `T+1m`
   - `T+5m`
   - `T+15m`
   - `T+30m`
4. One follow-up capture `T+2h`.

If the expected reset boundary is not yet known, run the same cadence around the best current hypothesis and repeat on the next candidate boundary until the window behavior is obvious.

## Required Payload Fields To Preserve

Every capture should preserve these raw response fields exactly when present:

- top-level:
  - `base_resp.status_code`
  - `base_resp.status_msg`
  - `model_remains`
- per `model_remains[]` row:
  - `model_name`
  - `start_time`
  - `end_time`
  - `remains_time`
  - `current_interval_total_count`
  - `current_interval_usage_count`
  - `current_weekly_total_count`
  - `current_weekly_usage_count`

Also record:

- local capture timestamp in UTC
- HTTP status code
- which credential profile was used
- which endpoint family was used

## Provider-to-Model Mapping To Record

For each capture, record which returned `model_name` values belong to which JiStats plan context:

- Coding Plan:
  - mark rows as `planKey = coding_plan`
  - keep the raw `model_name`
- Token Plan:
  - mark rows as `planKey = token_plan`
  - keep the raw `model_name`

Do not collapse rows across models until repeated captures prove the plan/window semantics are identical.

## Comparison Checks

For each adjacent capture pair, compare:

- did `start_time` stay fixed or jump
- did `end_time` stay fixed or jump
- did `current_interval_usage_count` reset to `0` or continue
- did `current_interval_total_count` stay stable
- did `remains_time` count down smoothly or jump to a new window
- did weekly counters change independently of interval counters

## Minimal Capture Log Template

Use this record shape for every live capture:

```text
capture_utc:
plan_key:
endpoint_family:
http_status:
base_status_code:
base_status_msg:
model_name:
start_time_ms:
end_time_ms:
remains_time_ms:
current_interval_total_count:
current_interval_usage_count:
current_weekly_total_count:
current_weekly_usage_count:
notes:
```

## What Counts As Decision-Ready Evidence

The reset-window question is considered settled for v1.0 runtime behavior when either of these is true:

- primary path:
  - at least one full pre/post reset sequence exists for Coding Plan
  - at least one full pre/post reset sequence exists for Token Plan
  - the sequences show whether the interval is fixed-boundary or rolling
  - the recommended JiStats user-facing label can be written without guessing
- comparison path:
  - one subscription surface has a full pre/post reset sequence
  - the sibling subscription surface has at least one archived pre-boundary remains snapshot plus one current live post-boundary capture
  - both surfaces show the same model-family interval shapes and the same active post-boundary window family
  - no contradictory interval/week fields appear between the sibling surfaces

The [reset-window findings](reset-window-evidence-log.md) summarize the accepted Coding Plan rollover and Token Plan comparison. A future direct Token Plan pre/post capture can refresh that evidence.

## Output Expectation

A local evidence drop should include:

- one short summary note
- one or more redacted JSON payload captures
- one compact comparison table showing window movement across captures
