# OpenAI Subscription Observation Mechanism Decision

> **Status**: Decision recorded; baseline implemented
> **Purpose**: Select the first practical Windows-local observation mechanism for OpenAI subscription activity
> **Driven by**: `docs/openai-subscription-requirements.md`

## Decision

**Choose foreground-window session observation as the first OpenAI subscription observation mechanism for v1.0.**

This means JiStats will initially detect and record:

- when a known OpenAI/Codex surface is the active foreground surface
- how long that surface remains actively used
- session boundaries, freshness, and gaps

It will **not** initially require:

- API access
- browser extensions
- packet interception
- OCR
- invasive UI automation

## Why This Is The Right First Mechanism

### 1. It matches the subscription-first product story

The user may only have ChatGPT/Codex subscription access. Foreground-window observation works even when:

- there is no API key
- there is no API billing
- there is no provider-exported usage feed

### 2. It fits the native Windows v1.0 stack

This mechanism can be implemented with low-overhead Win32 APIs and does not force:

- a browser plugin
- a managed runtime
- traffic interception
- app-specific reverse engineering

### 3. It gives honest data

What it can claim exactly:

- a known OpenAI/Codex surface was active on this device
- for a measured span of time
- during a known session window

What it does not claim:

- exact provider-side quota consumption
- exact message count
- exact tokens or cost

That honesty is a feature, not a limitation.

## What v1.0 Will Observe

### Base Event Type

The first v1.0 OpenAI subscription event type should be:

- `session_active`

Each event should represent:

- provider surface identity
- session start time
- session end time
- active duration
- confidence = `LocalExact`

### Source Detection

JiStats should detect known OpenAI subscription surfaces by:

- foreground process identity
- window title / class heuristics
- configurable allowlist for recognized OpenAI/Codex surfaces

Examples:

- browser tab/window showing ChatGPT or Codex
- desktop app window if an official OpenAI/Codex app is used

## What v1.0 Will Not Infer Yet

The first mechanism should **not** attempt to infer:

- `message_send`
- `response_complete`
- `tool_run`
- exact model selection
- token counts
- cost

Those may be added later as higher-confidence secondary instrumentation, but they should not block the initial subscription implementation.

## Event Shape

The first observation mechanism should emit local events like:

```json
{
  "provider_surface": "openai_subscription_local",
  "activity_kind": "session_active",
  "event_time_utc": "<session_start>",
  "window_end_utc": "<session_end>",
  "duration_seconds": 420,
  "source_confidence": "LocalExact",
  "raw_reference": "foreground_window_observer"
}
```

`openai_subscription` remains the high-level product surface. For concrete persisted local observation events, the normalized provenance label is `openai_subscription_local`.

If the current `usage_events` schema is too API-oriented for this event shape, the next design task should explicitly decide whether to:

- extend `usage_events`, or
- add a parallel `activity_events` table for subscription-local observation

## Detection Rules

### Session Start

Start a session when:

- a recognized OpenAI/Codex surface becomes foreground
- and stays foreground for a minimum debounce threshold

Recommended debounce:

- `2-5 seconds`

### Session Continue

Continue the same session while:

- the same recognized surface remains foreground, or
- short focus changes remain under a grace threshold

Recommended grace threshold:

- `15-30 seconds`

### Session End

End a session when:

- focus moves away from the recognized surface beyond the grace threshold
- the machine sleeps
- the app shuts down
- the recognized window closes

## Confidence Model

For this mechanism:

- session presence and duration = `LocalExact`
- provider-side quota implication = `Unknown`
- any inferred usage intensity derived from session duration = `Estimated`

## UX Implications

### Dashboard

Show:

- `OpenAI active for 42 min today`
- `3 local sessions observed`
- `Last local observation: 6 min ago`
- `Provider quota state: Unknown`

Do not show:

- fake quota percentages
- fake request counts
- fake cost totals

### Taskbar

Prefer:

- active/inactive state
- recent-session intensity
- freshness/offline state

Avoid:

- quota pressure percentages for OpenAI subscription users unless a validated provider-side signal exists

## Alternatives Considered

### 1. API interception / proxy

Rejected as the default because:

- subscription users may not have API traffic at all
- it does not solve the base subscription use case

### 2. Browser extension

Deferred because:

- adds another runtime/distribution surface
- breaks the native-first implementation preference
- may still be useful later as an enhancement

### 3. OCR / UI scraping

Deferred because:

- too brittle for v1.0
- higher privacy risk
- more implementation complexity

### 4. Deep UI automation

Deferred because:

- invasive
- fragile across UI changes
- too expensive for the current project maturity

## Recommended Implementation Slices

### Slice 1

- foreground-window watcher
- recognized-surface matcher
- session start/end persistence
- freshness and gap integration

### Slice 2

- dashboard cards for local session activity
- taskbar/tray fallback behavior for subscription users
- confidence labels

### Slice 3

- optional heuristics for stronger activity classification
- optional manual/user-confirmed mapping of windows/surfaces
- optional separate browser/plugin enhancement

## Acceptance Criteria

This decision is successfully implemented when:

1. JiStats can detect at least one real OpenAI/Codex subscription surface on Windows
2. It stores local `session_active` events reliably
3. Dashboard/tray/taskbar surfaces can render useful OpenAI subscription activity without API access
4. The product remains honest about unknown provider-side quota truth

## Follow-Up Task

Decide whether the current `usage_events` schema should be extended for subscription session events or whether a new `activity_events` table is cleaner.

## References

- `docs/openai-subscription-requirements.md`
- `docs/openai-scope-decision.md`
- `docs/taskbar-metrics.md`
- `docs/gap-handling-policy.md`
