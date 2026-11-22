# In-App Reporting And Analysis Expansion Spike

> **Status**: Historical planning spike; the dedicated Analysis workspace, shared export scope, and the first in-app history/trend/provenance slices are now shipped, and this document remains as design input for future refinements
> **Purpose**: Capture the product stance behind JiStats' in-app analysis growth so later refinement work keeps HTML export important without making it the only detailed view
> **Scope**: Historical product and UX design context. Follow-ups are tracked in the [work tracker](progress.md).

## Why This Spike Exists

The current dashboard is intentionally compact and useful for glanceable status:

- provider quota table
- remaining-first summary bars
- runtime, recovery, and scheduled-export status
- settings/import shell actions

That baseline is good enough for a first shell, but it is not yet a strong in-app surface for exploring:

- how usage changed across time
- which provider/account/model contributed most of the activity
- when data is local, imported, backfilled, stale, or unknown
- what happened during outages, recovery, or reconciliation
- how scheduled export and manual export relate to the current visible scope

At the same time, `docs/plan.md` already says the dashboard should be the primary live-data surface and that reports are export artifacts, not the primary interface for understanding current or recent data.

This spike exists to make that product stance concrete.

## Current Baseline

Today the app has both a compact dashboard shell and a dedicated Analysis workspace window:

- one quota list with provider/state/source/window/used/limit/remaining/confidence/last-sync columns
- compact usage-summary bars derived from selected provider/account scope
- status rows for connection, recovery, scheduled export, and last updated
- settings and historical import routed through the dashboard shell
- scheduled HTML export settings in preferences
- a lightweight dashboard `Analysis` launcher tab that opens a dedicated Analysis window
- explicit provider/account/date-range scope controls in that Analysis window
- daily history rows, provider/account breakdowns, compact trend visuals, provenance/data-quality notes, and gap visibility
- shared analysis-scope behavior for both manual and scheduled HTML export

That means the original "middle layer" problem is no longer about the absence of in-app analysis at all. The remaining product gap is now between:

- a compact operational dashboard
- a first shipped in-app analysis workspace
- a more polished long-term analysis/reporting experience

What is still missing is the next refinement layer: making the shipped Analysis workspace more cohesive, more explanatory, and more release-ready without reopening already-landed export and reporting foundations.

## Product Stance

JiStats should treat these surfaces as distinct:

### 1. Dashboard / In-App Analysis

Primary purpose:

- understand current status
- inspect recent and historical behavior
- compare providers/accounts/plans
- review freshness, gaps, backfill, and confidence
- decide whether action is needed

### 2. HTML Export

Primary purpose:

- create a portable artifact
- save or share a snapshot
- archive reporting output
- review data offline outside the running app

### 3. Taskbar / Tray

Primary purpose:

- provide glanceable status
- offer quick actions
- avoid becoming a miniature report viewer

The key rule for future work is:

- HTML export should remain important, but it must not become the only path to detailed understanding of the user’s data.

## Spike Goals

This spike should answer the following planning questions:

1. What in-app views does JiStats need so the user can understand live, recent, and historical data without exporting HTML first?
2. How should those views relate to the existing compact dashboard, provider quota table, and scheduled/manual export flows?
3. Which pieces belong in a first shippable in-app reporting slice versus later refinement?
4. What new view models, queries, or shared metric/reporting seams are required to support those views?
5. How can the repo reuse the existing reporting pipeline and metric vocabulary without forcing the UI to mirror the HTML document structure one-to-one?

## Explicit Non-Goals

This spike does not assume JiStats should:

- ship a full BI-style analytics suite
- add arbitrary custom report builders in v1.0
- replace the dashboard with a browser-style document viewer
- make taskbar or tray surfaces dense enough to duplicate the main window
- collapse imported, local, and provider-returned truth into unlabeled blended numbers
- redesign the whole shell before addressing smaller required gaps such as release-signoff, usability polish, and documentation accuracy

## User Problems To Solve

The richer in-app surface should help a user answer questions like:

- What is happening right now across my enabled providers?
- Which provider/account is under the most pressure?
- How much recent activity did JiStats observe in the last 5 minutes, 30 minutes, 5 hours, and today?
- How has quota pressure changed across the current day or week?
- Which values are local, imported, exact, estimated, or unknown?
- Did the app miss time while offline, and if so, what was recovered versus still unknown?
- What data range would be included if I exported a report right now?
- When was the last successful export or sync for the scope I am looking at?

The current dashboard answers only a subset of these questions.

## Design Principles

Any in-app reporting expansion should follow these rules:

### Live First

The app should be useful for active monitoring without forcing the user to generate a document.

### Clear Scope

Every detailed view should make provider/account/date-range scope obvious.

### Source Honesty

Imported, local, backfilled, estimated, and unknown values must remain clearly labeled.

### Progressive Disclosure

The first screen can stay compact, but the user must be able to drill into more detail without opening logs or reading raw database content.

### Shared Vocabulary

Use the current runtime/freshness/confidence labels already defined in:

- `docs/metric-glossary.md`
- `docs/mirror-accuracy-policy.md`
- `docs/offline-retry-policy.md`

### Performance Safety

Detailed views should prefer rollups and cached summaries where possible instead of turning the dashboard into an unbounded raw-event browser.

### Directional UI Research, Not UI Copying

TrafficMonitor should be treated as a useful reference for how a Windows-native utility can present dense operational information clearly, but not as a direct UI template.

JiStats should borrow lessons such as:

- compact information layouts that still remain readable
- strong glanceability at the top level with deeper detail available elsewhere
- desktop-utility discipline instead of web-dashboard sprawl
- careful use of small labels, status cues, and bounded information groups

JiStats should not copy TrafficMonitor mechanically because:

- network throughput is simpler and more continuously numeric than JiStats provider/quota/recovery data
- JiStats must explain provenance, freshness, confidence, imported history, and unknown gaps
- JiStats needs richer historical and diagnostic inspection than a realtime meter normally does
- JiStats must keep export/reporting behavior aligned with the same data model rather than just painting live counters

The goal is to use TrafficMonitor as a directional example of native information density and disciplined utility UX, not as a one-to-one feature or layout target.

## Research Input: TrafficMonitor

The repo already treats TrafficMonitor as an explicit Windows-native reference for taskbar work. This spike should also use it as a lighter-weight reference for in-app information presentation.

Relevant existing source-backed notes live in:

- `docs/trafficmonitor-research.md`
- `docs/taskbar-metrics.md`

Useful takeaways for this spike:

### 1. Dense Utility UI Can Still Feel Native

TrafficMonitor shows that a Windows utility can surface a lot of information without needing a web-style analytics shell.

For JiStats, this supports:

- compact summary zones
- short labels
- consistent status placement
- fast top-level scanning before drill-down

### 2. Top-Level Surfaces Should Stay Focused

TrafficMonitor keeps its taskbar surface narrow and purpose-built instead of trying to reproduce every metric everywhere.

For JiStats, the analogous rule is:

- overview/dashboard stays compact
- deeper analysis/history views carry the heavier detail
- taskbar and tray remain glanceable
- HTML export remains portable

### 3. Repeated Layout Patterns Matter

TrafficMonitor benefits from stable repeated patterns rather than a different visual treatment for every metric.

For JiStats, this suggests:

- repeated card/table/detail structures
- consistent provenance badge placement
- consistent freshness/confidence labeling
- stable provider/account/date-range scope treatment across views

### 4. Native Utility UX Rewards Progressive Disclosure

TrafficMonitor does not try to make every surface do every job.

For JiStats, this reinforces:

- summary first
- detail on selection
- deeper history/recovery views only when the user asks for them
- no need to overload the first screen with all tables and explanations at once

### 5. Information Expansion Should Stay Bounded

TrafficMonitor’s strength is not just “more data”; it is that the visible information remains bounded and intentional.

For JiStats, this means any richer reporting UI should prefer:

- a few strong high-signal panels
- explicit range/scope controls
- one good detail pane over many competing mini-widgets
- charts/tables that answer specific user questions instead of ornamental density

## Proposed Information Architecture

The current dashboard does not need to be thrown away. A better direction is to keep the compact overview and add adjacent detail surfaces.

Recommended top-level structure:

### Overview

Purpose:

- preserve the current glanceable dashboard role
- show live provider status, quota pressure, last sync, recovery summary, and scheduled export status

Keep:

- compact quota table
- summary cards/bars
- runtime/recovery/export status

Improve:

- better selection context
- clearer source badges
- quick jump actions into deeper views

### Activity

Purpose:

- explain local activity and imported historical activity in a way the user can inspect without exporting

Recommended content:

- recent-window summary cards
- activity trend chart for selected range
- breakdown by provider/account
- optional breakdown by model or activity kind when data supports it
- table view of aggregated periods, not just one-line totals

### Quota

Purpose:

- explain how provider quota changed over time rather than only showing the latest row

Recommended content:

- current quota snapshots
- trend view for remaining percent / used percent
- reset-window awareness
- provider/account comparison rows
- confidence/provenance badges on each series or summary

### History

Purpose:

- provide report-style range inspection inside the app

Recommended content:

- date-range picker with practical presets
- daily and weekly summaries
- provider/account/model breakdown tables
- visible source labels for imported versus local evidence
- ability to pivot between “summary first” and “table first” views

### Recovery And Data Quality

Purpose:

- make stale/backfilled/unknown periods understandable without forcing the user to infer state from one badge

Recommended content:

- last successful sync/import/export times
- recent recovery events
- known gap windows
- backfilled coverage summary
- unknown-gap explanation
- confidence policy version and source-summary text when useful

### Export Center

Purpose:

- keep manual and scheduled export visible as app-owned workflows

Recommended content:

- manual HTML export action
- scheduled export state
- last export result
- clear statement of what current scope/date range the export will use
- output-folder visibility and failure/success messaging

This surface should be adjacent to in-app analysis, not a substitute for it.

## Recommended UX Building Blocks

The first expansion does not need all of these at once, but the spike should treat them as the core candidate set.

### 1. Scope Bar

At minimum:

- provider selector
- account selector
- date-range selector
- refresh/export actions

Reason:

- the current dashboard can scope by selected provider/account, but the scope is too implicit for a richer analysis experience

### 2. Range Presets

Recommended presets:

- `Today`
- `Last 24 Hours`
- `Last 7 Days`
- `Current Billing/Quota Window`
- `This Month`
- `Custom`

Reason:

- the HTML export and in-app analysis should both depend on a clear time scope, even if the first shipped export keeps a simpler default

### 3. Detail Tables

Recommended table families:

- provider/account quota history table
- daily usage summary table
- hourly usage table for short ranges
- recovery/gap events table
- source/provenance summary table

Reason:

- the current single quota table is too narrow to carry historical and diagnostic understanding by itself

### 4. Trend Visuals

Recommended first-pass visuals:

- compact daily bars
- quota remaining trend line or step chart
- short-range activity bars

Avoid:

- highly decorative charting that adds complexity before the model/query layer is ready

### 5. Drill-Down Detail Pane

When the user selects a provider row, time bucket, or report card, the shell should be able to show:

- exact scope
- last sync
- freshness state
- source label
- confidence tier
- backfill/unknown-gap notes
- supporting used/limit/remaining values

This is likely more useful for v1.0 than jumping immediately to a multi-document tab explosion.

## Data Model And Query Implications

The spike should assume the UI expansion must be supported by deliberate query/view-model work, not just extra controls bolted onto the existing presenter.

Likely needs:

### 1. View-Model Separation Between Overview And Analysis

The current `DashboardViewModel` is good for compact status, but a richer analysis surface will probably need:

- explicit scope object
- explicit range presets/custom range state
- richer rollup collections
- provenance summaries
- recovery/gap timeline summaries
- export preview/status block

That does not require throwing away the existing dashboard model, but it likely does require adding new analysis-oriented view models.

### 2. Reusable Aggregation Queries

The richer UI should prefer query seams that can serve both:

- in-app detail views
- HTML export generation

without forcing either surface to own the other.

Useful query families:

- daily/hourly activity summaries by provider/account
- quota snapshot history by provider/account/window
- recovery/backfill/gap summaries by period
- export metadata summaries

### 3. Rollup-First Fetch Paths

The spike should strongly prefer:

- daily rollups for wider history
- hourly rollups for short-range charts/tables
- raw events only when the user is intentionally drilling into a narrow recent window

This keeps the in-app analysis surface performant and aligned with the retention strategy.

### 4. Explicit Provenance Bundles

The UI will be clearer if the backend can hand it grouped explanations such as:

- local-only
- imported historical
- provider-returned exact
- backfilled with caveats
- unknown coverage

instead of making the shell reconstruct those meanings ad hoc from many small flags.

## Candidate Delivery Slices

The spike should not end with “build everything.” It should end with a small, sequenced queue.

Recommended implementation slicing:

### Slice A: Manual Export Completion

Current status:

- landed

Outcome:

- the dashboard now has a real `File -> Export HTML Report...` action
- manual and scheduled export both use the shared analysis scope/date window
- export runs persist to `export_runs`

### Slice B: In-App Reporting Shell Foundation

Current status:

- landed

Outcome:

- the dashboard now exposes a lightweight Analysis launcher/status tab
- the dedicated Analysis workspace owns explicit provider/account/date-range scope controls
- export behavior references the same scope explicitly

### Slice C: History And Breakdown Views

Current status:

- largely landed as the current baseline

Current shipped baseline:

- daily summary/history rows
- provider/account breakdowns
- compact trend visuals backed by persisted rollups
- provenance/data-quality messaging inside the Analysis workspace

### Slice D: Recovery / Data-Quality Visibility

Current status:

- largely landed as the current baseline

Current shipped baseline:

- gap summaries and unknown-period messaging surface in Analysis
- last sync/import/export metadata is part of the scoped analysis/data-quality path
- typed recovery/export persistence now backs those summaries

## First Shipped Target

The first meaningful post-export target has already been chosen and shipped:

- an in-app `Analysis` surface with explicit provider/account/date-range scope, daily summary rows, trend visuals, and provenance badges

Potential refinement slices are recorded in the [work tracker](progress.md).

## Open Design Questions

The spike should resolve or narrow these questions before implementation:

1. Resolved: deeper analysis now lives in a dedicated secondary window launched from the dashboard.
2. Resolved: manual HTML export now uses the currently visible analysis scope/date range.
3. Which historical tables must be interactive in v1.0 versus read-only?
4. How much model-level breakdown is actually trustworthy for each provider path today?
5. Which recovery and provenance fields should always be visible versus tooltip-only?
6. Do we need lightweight charting helpers, or can a first pass use native custom drawing for compact visuals?
7. Which TrafficMonitor-inspired patterns are genuinely useful for JiStats desktop utility UX, and which ones should stay limited to taskbar/reference-only influence?

## Recommended Spike Deliverables

To treat this spike as complete, the repo should end up with:

1. A written product stance that clearly separates in-app analysis from HTML export.
2. A proposed information architecture for overview, history, quota, recovery, and export surfaces.
3. A first-pass list of required view-model/query seams.
4. A small implementation queue that identifies the next refinement slice after the shipped Analysis/export baseline.
5. A short research readout on what TrafficMonitor usefully suggests about dense native utility presentation versus what JiStats should not copy.
6. Acceptance questions that future dashboard/reporting changes can be reviewed against.

## Acceptance Questions

Use these questions whenever the dashboard/reporting surface grows:

- Can the user inspect meaningful historical data in-app without generating HTML first?
- Is the currently viewed provider/account/date-range scope obvious?
- Are local, imported, backfilled, estimated, and unknown values still clearly labeled?
- Does the HTML export remain a portable artifact rather than the only detailed viewer?
- Are rollups and retention behavior respected so the UI does not depend on indefinite raw-event storage?
- Does the overview remain compact enough to serve as the primary live dashboard?

## Related Files

- `docs/plan.md`
- `docs/progress.md`
- `docs/metric-glossary.md`
- `docs/dashboard-quota-display-contract.md`
- `docs/dashboard-menu-contract.md`
- `docs/openai-export-import-contract.md`
- `docs/scheduled-export-decision.md`
- `docs/trafficmonitor-research.md`
- `docs/taskbar-metrics.md`
