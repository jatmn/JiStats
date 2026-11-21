# Windows 11 Taskbar Metric Set

> **Status**: Current shipped baseline plus explicitly separated future refinements
> **Purpose**: Describe the minimum taskbar metric set JiStats actually ships today while keeping the TrafficMonitor-backed direction explicit
> **Driven by**: `docs/plan.md` section 10.3, the shared runtime model, and the current overlay support docs

## Current Decision

The taskbar is a required v1.0 surface, but the shipped baseline is intentionally compact.

The current minimum metric decision is:

1. show provider identity
2. show remaining-percentage quota semantics when a trustworthy provider row exists
3. show degraded state through the row icon/color treatment instead of a separate bulky status block
4. fall back to one compact runtime row only when provider rows are unavailable and the shared runtime state is not `Current`

This is narrower than the earlier prototype idea of a multi-field strip like `[OpenAI] [↑42/m] [67%] [●]`.

That older four-slot layout should now be treated as planning history, not the shipped baseline.

Live validation note:

- The 2026-04-20 Windows 11 desktop pass confirmed placement, transparent/blended presentation, and the intended current row model.
- In the current baseline, `OpenAI: 99% | 50%` is an acceptable dual-remaining row when both subscription windows are intentionally shown.
- The current baseline is text-first and does not require a standalone glyph column.
- That live pass closed the current OpenAI/MiniMax quota-row baseline only; Claude Code and Kimi Code remain in the shipped taskbar scope as bounded local-first inputs rather than provider-exact quota-parity claims.

## Current Shipped Behavior

### Presentation Model

The taskbar overlay now aims for a TrafficMonitor-style embedded surface:

- transparent or lightly blended background
- compact width driven by the real row content
- one or two concise provider rows when provider/taskbar metrics exist
- shared runtime-state fallback when no provider rows can be shown

Related support docs:

- `docs/taskbar-overlay-display-contract.md`
- `docs/taskbar-overlay-validation-scenarios.md`
- `docs/taskbar-manual-validation.md`

### Current Row Types

#### 1. Provider Quota Row

This is the main shipped row type when JiStats has provider-backed taskbar metrics.

| Field | Meaning | Notes |
| --- | --- | --- |
| Provider label | Provider identity such as `OpenAI`, `MiniMax`, `Claude Code`, or `Kimi Code` | Preserves incoming provider order |
| Remaining percent | Current remaining percentage | Taskbar stays remaining-first even though the dashboard quota table uses used-percent wording |
| Optional secondary remaining percent | Additional remaining value when intentionally surfaced | Used today for OpenAI dual-window remaining display |
| Status color/emphasis | Compact health/degraded meaning | Keeps degraded state glanceable without a separate wide status column |

Expected user reading:

- `OpenAI 75% | 60%`
- `MiniMax 24%`

with color/emphasis carrying the pressure or degraded-state emphasis.

For OpenAI/Codex and MiniMax, the current row model can reflect the strongest validated quota surfaces JiStats ships today. For Claude/Kimi, the same compact row model may still be used for local-first runtime snapshots or activity summaries, but the taskbar must not imply provider-exact quota parity unless a separately validated corroborating signal exists.

#### 2. Persisted-Activity Summary Row

When the taskbar is fed from the persisted-activity summary path, the presenter may show summarized activity-derived rows first and then overlay or append degraded provider states.

Current contract:

- persisted-activity ordering stays intact
- matching providers can receive degraded-state overlay treatment in place
- missing non-live providers may be appended afterward

This is a real shipped path, but it is still subordinate to the same compact overlay rules.

#### 3. Runtime Fallback Row

If there are no provider rows and the shared runtime state is degraded, JiStats emits one runtime-only fallback row.

Canonical meaning:

| Field | Meaning |
| --- | --- |
| Label | `Runtime` |
| Primary text | Compact runtime-state label |
| Secondary text | Full runtime-state label |
| Icon | Shared runtime-state indicator |

Important boundary:

- if runtime state is `Current` and there are no provider rows, production should prefer an empty taskbar metric list rather than a synthetic healthy placeholder row

## Current Metric Vocabulary

The taskbar currently depends on the same small shared metric vocabulary as the rest of the runtime:

| Metric | Current Meaning |
| --- | --- |
| Provider label | Provider identity for the visible row |
| `quota_remaining_percent` | Remaining-first percentage shown on provider rows |
| `quota_secondary_remaining_percent` | Optional second remaining-first percentage for providers that intentionally expose more than one relevant window |
| `provider_state` | Live / Stale / Syncing / Offline |
| `runtime_state` | Current / Stale / Catching Up / Offline |
| Status color/emphasis | Compact visual treatment for pressure or degraded state |

The taskbar does not currently need its own separate metric taxonomy beyond that.

## What Is Explicitly Shipped Today

- four-provider participation in the shared taskbar pipeline, with OpenAI/Codex and MiniMax as the current provider-exact quota anchors and Claude/Kimi as bounded local-first inputs
- provider label plus remaining-percentage semantics
- compact degraded-state overlay/fallback behavior
- support for mixed provider states
- runtime-only fallback when provider rows are unavailable and runtime is degraded
- TrafficMonitor-style transparent/lightly blended presentation direction
- Explorer/taskbar recreation hardening in the shell path

## What Is Not Part Of The Current Minimum Set

- a permanently visible request-rate or usage-intensity slot
- a separate standalone offline-dot column
- full dashboard-style multi-column quota detail on the taskbar
- detailed timestamps as always-visible taskbar text
- a synthetic healthy runtime row when the app has no provider rows

## Future Refinements Worth Considering

These remain legitimate follow-on ideas, but they should stay separate from the current shipped baseline:

1. richer hover/tooltip detail for last sync, freshness, and source labels
2. broader live validation across more Windows taskbar layouts, tray crowding, and utility combinations
3. a future compact activity-intensity hint only if it remains glanceable and does not widen the overlay back into prototype territory
4. better provider-fed `Catching Up` coverage once the syncing path is produced end to end more often

## TrafficMonitor Direction

TrafficMonitor remains the implementation-direction reference for the taskbar surface:

- dedicated taskbar-anchored window strategy
- transparent/blended native-feeling presentation
- shell lifecycle handling such as Explorer/taskbar recreation
- compact text-first layout instead of dashboard reuse

JiStats should keep using that direction as the reference point for future taskbar refinements rather than reintroducing wider dashboard-like overlays.

## References

- `docs/metric-glossary.md`
- `docs/taskbar-overlay-display-contract.md`
- `docs/taskbar-overlay-validation-scenarios.md`
- `docs/taskbar-manual-validation.md`
- `docs/trafficmonitor-research.md`
- `docs/plan.md`
