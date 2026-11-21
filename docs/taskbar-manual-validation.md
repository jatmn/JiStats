# Taskbar Manual Validation

> **Status**: Release-signoff taskbar validation is now recorded via the 2026-04-20 live display pass plus the 2026-04-21 scripted shell pass
> **Purpose**: Provide the repeatable manual checklist and evidence matrix for taskbar quota presentation, while preserving the earlier live smoke-check result as historical context.
> **Related Specs**: `docs/taskbar-overlay-display-contract.md`, `docs/taskbar-metrics.md`, `docs/progress.md`

This note now covers the quota-presentation validation needed after the earlier placement/toggle smoke pass.

It should stay explicit about the shipped four-provider v1.0 story: OpenAI/Codex and MiniMax are the current quota-row validation anchors, while Claude Code and Kimi Code remain in scope as bounded local-first surfaces that must not be over-claimed as provider-exact quota parity from this pass alone.

It does not claim that every optional deeper scenario in the reusable checklist below was exercised in the latest live pass.

Between the 2026-04-20 display pass and the current release-signoff path, the taskbar implementation also absorbed a focused TrafficMonitor-alignment and stability pass: the overlay is back on a single interactive taskbar-owned surface, hover/menu behavior is owned directly by `TaskbarPresenter`, and focused automated `taskbar` coverage now locks down placement math, taskbar-handle recreation decisions, hover/menu exclusivity, click-away recovery, and presenter-owned context-menu dispatch. The remaining shell interaction lane is now recorded too through `scripts/run_taskbar_shell_signoff.ps1` plus the focused `[taskbar]` test rerun, so this doc is no longer waiting on an unstructured human-only follow-up.

## Latest Live Pass Result

### 2026-04-20 Desktop Setup

- Machine: Windows 11 desktop session
- Primary display: `3440x1440`
- Taskbar theme: dark
- Windows transparency: enabled
- JiStats build observed: `build/bin/Release/JiStats.exe`
- Nearby taskbar utility: TrafficMonitor visible to the right of JiStats

### Observed Provider Rows

The live taskbar screenshot from this pass showed:

- `MiniMax: 100%`
- `OpenAI: 99% | 50%`

### Summary

- The overlay is real, visible, and positioned cleanly to the left of TrafficMonitor.
- The background reads as transparent or lightly blended rather than like an opaque dashboard panel.
- The current runtime matches the intended baseline for visible row content:
  - OpenAI intentionally renders a dual-remaining row (`99% | 50%`) when both relevant remaining windows are available.
  - MiniMax renders one compact remaining-percentage value.
  - Standalone glyphs are intentionally absent from the current text-first row design.
- This desktop pass closes the current OpenAI/MiniMax quota-row presentation baseline only; Claude/Kimi still belong to the shipped taskbar story, but as local-first rows or activity summaries rather than proof of provider-exact quota truth.
- Toggle hide/show behavior was **not** rerun in this 2026-04-20 display pass; the earlier smoke result remains the only direct live evidence for repeated enable/disable behavior.

### 2026-04-21 Shell Signoff Pass

- Validation harness: `scripts/run_taskbar_shell_signoff.ps1`
- JiStats build observed: `build/bin/Release/JiStats.exe`
- Focused automation rerun: `.\build\bin\Debug\JiStatsTests.exe "[taskbar]" --reporter compact` (`230` assertions across `27` test cases)
- Recorded shell results:
  - three repeated overlay right-click / click-away dismiss cycles passed
  - overlay remained visible after each click-away recovery cycle
  - Explorer/taskbar restart recovery passed and the overlay returned cleanly
  - hover/menu suppression and recovery behavior remain covered by the focused `[taskbar]` seam tests rather than a separate visual-only script oracle

## Scope

This pass should verify:

- the overlay still appears on a real Windows taskbar
- the overlay background reads as transparent or lightly blended instead of as an opaque panel
- remaining values are shown as remaining percentages for OpenAI and MiniMax
- the doc and pass stay honest about the full four-provider v1.0 scope, including Claude/Kimi's local-first boundary
- the taskbar keeps its own remaining-percentage contract even while the dashboard quota overview uses percentage-first `Used` wording
- redundant `enabled` text is gone
- text treatment is understandable without knowing any older punctuation shorthand
- color tiers, low-quota bolding, and compact width behave consistently
- placement and alignment still stay clear of the notification area, clock, and TrafficMonitor

This pass should not turn into:

- synthetic provider-state reverse engineering
- a full dashboard validation pass
- hidden-state or restart engineering that requires special setup unless it is easy to exercise
- broader taskbar redesign beyond the current quota-display contract

## Preflight

Before running the live pass:

- use a build that includes the current taskbar quota-display baseline
- confirm `ui.taskbar_overlay_enabled` is enabled
- keep the dashboard available so taskbar values can be compared against the dashboard quota view
- note the Windows 11 taskbar theme/background in use if transparency readability looks marginal
- if possible, run with nearby tray/taskbar elements present, such as a crowded notification area or TrafficMonitor
- note the provider values being tested so the expected remaining percentages are known ahead of time
- if Claude/Kimi are enabled, note whether the observed rows are quota-style rows, activity-derived rows, or absent in the specific setup so the pass does not overstate what was actually exercised

## Validation Contract

Use these expectations when interpreting the pass:

- When `ui.taskbar_overlay_enabled` is `true`, the overlay should render provider lines derived from the same normalized provider data pipeline as the dashboard, while keeping the overlay-specific remaining-percentage presentation contract.
- The visible taskbar percentages should be remaining percentages, not raw used values.
- OpenAI may intentionally show a second remaining percentage when the row includes a secondary subscription window.
- The background should feel transparent or lightly blended into the taskbar rather than rendering as a dashboard-style opaque panel.
- The overlay should not display redundant state text such as `enabled`.
- The current baseline does not require a standalone glyph column; comprehension should come from compact text, remaining values, and color/emphasis.
- A Claude or Kimi row, if present, should use the same compact overlay treatment while remaining honest about its local-first provenance; this pass must not reinterpret a local-first row as provider-exact quota proof.
- It is acceptable for the baseline live pass to exercise only OpenAI/MiniMax quota rows, as long as that scope boundary is recorded explicitly.
- When the overlay is disabled, the taskbar surface should disappear cleanly and dashboard/tray state should continue updating normally.
- If taskbar presenter initialization fails while the preference remains enabled, record that as a taskbar-specific availability failure rather than a dashboard/tray/runtime failure.

## Manual Checklist

The checklist below is a reusable operator worksheet.

The current 2026-04-20 pass result is recorded in the scenario matrix and evidence log rather than by checking every box here.

### Value Semantics

- [ ] OpenAI shows remaining `%`, not the raw used value.
- [ ] MiniMax shows remaining `%`, not the raw used value.
- [ ] The taskbar value for each provider follows the overlay's remaining-percentage meaning even if the dashboard quota overview uses percentage-first `Used` wording.
- [ ] If OpenAI shows two percentages, both are documented remaining values.
- [ ] No provider row shows redundant `enabled` text.

### Four-Provider Honesty

- [ ] The pass record makes clear whether it exercised only OpenAI/MiniMax quota rows or also observed Claude/Kimi local-first rows.
- [ ] Any visible Claude/Kimi row is described as local-first evidence or activity summary unless a separately validated provider signal exists.
- [ ] The pass does not claim provider-exact Claude/Kimi quota parity based only on local taskbar output.

### Background And Transparency

- [ ] The overlay background reads as transparent or lightly blended into the Windows 11 taskbar.
- [ ] The overlay does not look like a reused opaque dashboard panel.
- [ ] Text remains readable against the observed taskbar background.

### Color And Emphasis

- [ ] The row treatment is understandable without relying on retired punctuation shorthand.
- [ ] Blue / green / yellow / orange / red tiers match the intended pressure band thresholds.
- [ ] Any row at `10%` remaining or lower renders the numeric value in bold.
- [ ] Any visible degraded-state treatment stays consistent with the row's color/emphasis behavior.

### Width, Placement, And Alignment

- [ ] The overlay width looks sized to the longest real provider line plus light padding, not to stale placeholder text.
- [ ] Both provider lines share the same left alignment.
- [ ] Text is vertically centered within the overlay bounds.
- [ ] The overlay appears on the live taskbar without covering the clock, tray chevron, or nearby taskbar utility windows.
- [ ] If TrafficMonitor is present, JiStats remains positioned cleanly to its left.
- [ ] There is no obvious clipping or truncation in the observed overlay state.

### Toggle And Visibility

- [ ] The overlay is visible when `ui.taskbar_overlay_enabled` is on.
- [ ] The overlay disappears when the preference is turned off.
- [ ] The overlay returns after the preference is turned back on.
- [ ] Dashboard and tray remain usable while toggling the overlay.

### Optional Extra Checks

- [ ] If the taskbar is crowded, the overlay still avoids obvious overlap during live metric changes.
- [ ] If auto-hide is easy to exercise, record whether the overlay returns cleanly.
- [ ] If Explorer restart is easy to exercise, record recovery behavior honestly, but do not block this display pass on it.

### Release-Signoff Shell Interaction Checks

Use this short list during the final taskbar signoff pass now that the engineering lane is closed:

- [x] Open the overlay context menu repeatedly and confirm click-away dismiss works on the first open and on later opens.
- [x] After dismissing the context menu, confirm hover tooltip behavior returns normally instead of remaining suppressed.
- [x] Click elsewhere on the taskbar and confirm the overlay remains visible or recovers immediately instead of disappearing permanently.
- [x] If Explorer restart is practical to exercise, confirm the overlay recreates cleanly after the shell returns.
- [ ] If the taskbar theme/background changes during the pass, confirm transparency and readability still match the current baseline.

Current signoff note:

- The shell-interaction checks above are now covered by the scripted `2026-04-21` pass plus the focused `[taskbar]` automation rerun. The theme/background-change line remains optional because no theme transition was exercised during that scripted run.

## Scenario Matrix

| Scenario | Provider Values | Expected Row Text | Expected Color/Icon | Expected Width/Alignment | Pass/Fail | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Healthy state | `99%` / `100%` remaining live rows | `Provider 99%`, `Provider 100%`, or intentional dual-remaining OpenAI row | Healthy tier treatment | Compact, no clipping | Pass | Healthy percentages rendered as intended for the current baseline. |
| Warning state | Not exercised in this pass | `Provider 24%` with warning-tier treatment | `orange` tier treatment | Compact, no clipping | Not exercised | Future validation depth only; not a blocker for the completed baseline pass. |
| Critical state | Not exercised in this pass | `Provider 9%` with critical emphasis | `red`, bold treatment | Compact, no clipping | Not exercised | Future validation depth only; not a blocker for the completed baseline pass. |
| Multi-provider view | OpenAI and MiniMax both visible | One line per provider, stable labels | Correct tier per line | Width fits longest line only | Pass | Two rows rendered cleanly; OpenAI dual-remaining display is intentional. |
| Claude/Kimi local-first row | Not exercised in this pass | Compact provider/activity row if those providers are enabled and have visible local-first output | Same compact treatment without implied provider-exact parity | Same compact layout rules | Not exercised | This pass used OpenAI/MiniMax as the quota-row anchors and did not attempt to close Claude/Kimi parity claims. |
| Transparent background check | Windows 11 dark taskbar visible behind overlay | Same provider rows as above | Same tier behavior as above | Background reads transparent/blended, not opaque | Pass | No opaque panel was visible in the live screenshot. |
| TrafficMonitor present | TrafficMonitor window visible | Same provider rows as above | Same tier behavior as above | JiStats aligned cleanly to the left of TrafficMonitor | Pass | Overlay stayed clear of TrafficMonitor, tray, and clock. |

## Evidence Log

| Check | Setup | Expected | Observed | Pass/Fail | Follow-up |
| --- | --- | --- | --- | --- | --- |
| OpenAI remaining value | 2026-04-20 live Windows 11 pass with TrafficMonitor present | Remaining-percentage semantics matching the overlay contract | Row read as `OpenAI: 99% | 50%`; both values are intentional remaining values for the current baseline | Pass | None |
| MiniMax remaining value | Same pass | One compact remaining `%` value matching the overlay contract | Row read as `MiniMax: 100%` | Pass | None |
| Four-provider scope boundary | Same pass plus current Phase 7 docs | Pass record stays honest about four-provider scope without overstating local-first providers | OpenAI/MiniMax quota rows were exercised; Claude/Kimi were not exercised in this desktop pass and remain local-first surfaces | Pass | Scope boundary is now recorded in the Phase 7 closeout notes; additional Claude/Kimi taskbar observation is optional future validation rather than a Phase 7 blocker. |
| Background transparency | Same pass | Transparent/blended background, readable text | Background blended into the dark Windows 11 taskbar without an opaque panel | Pass | None |
| Row treatment clarity | Same pass | Understandable text-first treatment without punctuation shorthand | Healthy rows were readable without needing standalone glyphs | Pass | None |
| Low-quota emphasis | Same pass | Bold red value at `<= 10%` | Not exercised; live rows were `99%` and `100%` | Not exercised | Optional deeper validation later |
| Width and alignment | Same pass | Width fits longest line plus light padding | Two rows were compact, left-aligned, and free of visible clipping | Pass | None |
| Toggle / visibility | Historical smoke pass plus 2026-04-20 display pass | Surface hides and returns cleanly | Not rerun on 2026-04-20; earlier smoke already confirmed repeated disable/enable without freeze or crash | Historical pass only | Optional confirmation rerun if desired |
| Placement / overlap | 2026-04-20 live desktop layout | No overlap with clock, tray chevron, or TrafficMonitor | Overlay remained to the left of TrafficMonitor and clear of the tray/clock region | Pass | None |
| Repeated context-menu dismiss / hover return | `2026-04-21` scripted shell pass plus focused `[taskbar]` tests | Repeated right-click open/dismiss cycles behave the same as the first one, and hover tooltips return afterward | `scripts/run_taskbar_shell_signoff.ps1` passed three repeated right-click / click-away cycles; focused `[taskbar]` tests still cover hover suppression/recovery at `230` assertions across `27` cases | Pass | None |
| Explorer/taskbar restart recovery | `2026-04-21` scripted shell pass | Overlay recreates after `TaskbarCreated` / shell recovery without losing transparency or interaction behavior | `scripts/run_taskbar_shell_signoff.ps1` passed Explorer restart recovery and the overlay returned cleanly afterward | Pass | None |

## Handoff Note

Record these before closing the pass:

- machine/taskbar setup used for the pass
- whether the pass covered just value semantics or also width/alignment and optional recovery checks
- whether nearby taskbar tools, a crowded tray, or TrafficMonitor were present
- whether Explorer restart or auto-hide was exercised directly or deferred
- exact value mismatches, clipping, overlap, color-tier, icon, or toggle recovery failures observed
- the next recommended pickup if the pass found follow-up work

## Recommended Follow-Up From The 2026-04-20 Pass

1. Keep future taskbar docs aligned with the intentional OpenAI dual-remaining row and the glyph-free text-first presentation.
2. If deeper confidence is wanted later, rerun a warning-band or critical-band scenario so low-quota color and bolding can be checked directly.
3. If future taskbar shell behavior changes, rerun `scripts/run_taskbar_shell_signoff.ps1` and record the new outcome here rather than reopening the signoff item from memory.
4. If a future shell/workflow change touches taskbar visibility, optionally rerun the explicit toggle off/on check even though the historical smoke pass already covered it.
5. If a future live pass captures visible Claude/Kimi taskbar rows, record them as local-first rows or activity summaries without implying provider-exact quota parity.

## Historical Smoke Result

The earlier live smoke pass is still useful historical evidence for placement and toggle basics, but it does not close the current quota-presentation contract:

- User manually verified that enabling and disabling the overlay works as expected multiple times.
- Overlay did not overlap the notification area, task icons, or clock on the tested machine.
- Latest manual validation confirmed the overlay now respects TrafficMonitor positioning as well.
- The overlay now disables and re-enables correctly without freezing or crashing the app.
- The 2026-04-20 pass closed the earlier transparency uncertainty and confirmed transparent/blended presentation on the tested dark Windows 11 taskbar.
- The 2026-04-20 pass confirmed that the current row content matches the intended baseline after the docs were reconciled.
