# Taskbar Overlay Validation Scenarios

> **Status**: Reusable runtime-validation support note
> **Purpose**: Prepare representative provider/runtime scenarios for future live Windows taskbar overlay validation passes
> **Driven by**: the current provider-fed runtime path plus the taskbar support docs

## Scope

This note is the reusable support artifact for live taskbar overlay passes. It does not replace the manual desktop validation itself.

Use it to:
- pick a small set of representative runtime states before the live desktop pass
- keep those scenarios aligned with the current provider-fed app behavior
- distinguish what is already reproducible end to end from what still needs a helper seam

Related references:
- `docs/progress.md`
- `docs/taskbar-metrics.md`
- `docs/metric-glossary.md`
- `tests/runtime_test_support.hpp`
- `tests/test_app_controller.cpp`
- `tests/test_provider_data_path.cpp`
- `tests/test_runtime_status.cpp`

## Current Reproducible Seams

The narrowest code-backed path today is:

1. Inject providers into `AppController` with `setProviderManagerForTesting(...)`.
2. Use `setTaskbarTestModeForTesting(true)` to populate taskbar metrics without depending on a live Windows taskbar embed.
3. Feed providers `QuotaResponse` shapes that represent the state being validated.
4. Read back:
   - dashboard/runtime state via `getDashboardViewModelForTesting()`
   - taskbar output via `getTaskbarMetricsForTesting()`
   - tray state via `getTrayStateForTesting()`
   - offline notices via `getActiveNoticeForTesting(...)`

This is the best current prep seam because `AppController` already maps provider quota responses into `ProviderState`, `RuntimeState`, tray state, taskbar rows, and offline notices from one path.

## Canonical Fixture Inputs

Use the same narrow input shapes every time a live-pass setup or upcoming taskbar test needs a representative provider/runtime state.

Unless a test is explicitly checking provider order, use these canonical providers:

| Slot | `providerKey` | `displayName` | Intent |
| --- | --- | --- | --- |
| 1 | `openai` | `OpenAI` | Primary fresh/live provider |
| 2 | `minimax` | `MiniMax` | Secondary provider used for mixed-state coverage |

Use these exact quota/runtime field conventions:

| Shape | Required fields | Canonical value |
| --- | --- | --- |
| Fresh live quota | `success`, `snapshotTime`, `windowStart`, `windowEnd`, `used`, `limit`, `remaining`, `windowType` | `success = true`, `snapshotTime = now`, `windowStart = now - 3600`, `windowEnd = now`, `used = 35`, `limit = 100`, `remaining = 65`, `windowType = "hourly"` |
| Stale quota | Same as live quota | Same values as live quota except `snapshotTime = now - 7200`, `windowStart = snapshotTime - 3600`, `windowEnd = snapshotTime` |
| Offline quota | `success`, optional `error` | `success = false`, `error = "network unreachable"` |
| Runtime-only fallback | No provider metrics; runtime state only | `viewModel.providers` yields no enabled quota/provider rows and `viewModel.runtimeState != Current` |

For persisted-activity overlay scenarios, seed activity so the row order is deterministic:

| Provider | Activity window | Canonical activity shape |
| --- | --- | --- |
| `openai` | Most recent | One or more `activity_events` within the last 5 minutes so `MAX(started_at_utc)` is newer than `minimax` |
| `minimax` | Older but still visible | One or more `activity_events` within the last 30 minutes but older than the `openai` row |

## Scenario Matrix

| Scenario | Provider/runtime input | Expected runtime result | Why it matters |
| --- | --- | --- | --- |
| `Current` | `openai` with the canonical fresh live quota shape | Runtime `Current`, tray `Normal`, quota-style taskbar row for `openai` | Baseline healthy overlay rendering |
| `Offline` | `openai` with the canonical offline quota shape | Runtime `Offline`, tray `Offline`, offline notice shown, failing provider row uses the current compact degraded-state treatment | Confirms the live pass still shows degraded state clearly |
| `Stale` | `openai` with the canonical stale quota shape | Runtime `Stale`, tray `Warning`, taskbar keeps quota row with the current stale/degraded treatment | Exercises aging data without full connectivity loss |
| `Mixed Live + Offline` | `openai` with the canonical fresh live quota shape plus `minimax` with the canonical offline quota shape | Runtime `Offline`, live row still present, failing provider row still visible | Matches a realistic multi-provider desktop case |
| `Mixed Activity + Offline Overlay` | Canonical persisted activity ordering (`openai` newer than `minimax`) plus one non-live provider state in `viewModel.providers` | Persisted-activity summary stays visible and non-live provider state is overlaid or appended in deterministic order | Checks that current taskbar summarization does not hide degraded provider state |
| `Catching Up` | Requires `ProviderState::Syncing` in the runtime model | Helper-level/runtime-label coverage exists, but not a true provider-fed `AppController` path yet | Important gap to record before the live desktop pass |

## Current Ordering And Fallback Contract

Record and test the current behavior as it exists today:

- `TaskbarPresenter` preserves the order of incoming metrics; it does not sort providers on its own.
- On the quota-driven path, `AppController` currently walks enabled providers in dashboard/provider order and uses the first quota row for each provider.
- On the persisted-activity path, existing persisted-activity summaries remain first, and non-live providers that are not already represented are appended afterward as compact state rows.
- If no provider metrics are available and runtime state is not `Current`, the app emits a single runtime fallback row with `providerKey = runtime`, compact runtime-state text, and the runtime-state status icon.
- If no provider metrics are available and runtime state is still `Current`, the production path has no special runtime fallback row yet; treat that as the current product empty-state boundary rather than promising richer placeholder text.
- In `JISTATS_ENABLE_TEST_HOOKS` builds, `AppController` may still fall back to `buildSampleTaskbarMetrics(...)` before reaching that empty-state boundary, so app-shell tests should either account for the sample fallback explicitly or use a seam that bypasses it before asserting a truly empty `Current` taskbar state.

## Recommended Validation Order

Use this order for a focused live desktop pass:

1. `Current`
2. `Offline`
3. `Stale`
4. `Mixed Live + Offline`
5. `Mixed Activity + Offline Overlay`

Keep `Catching Up` out of the first live pass unless a temporary seam is introduced, because the current provider-fed path does not produce `ProviderState::Syncing`.

## Code-Backed Setup Paths

### 1. Current

Use the canonical fresh live quota shape from the table above on `openai`.

Existing helper paths:
- `makeCurrentQuotaProvider(...)` in `tests/runtime_test_support.hpp`
- `makeStaleQuotaProvider(...)` in `tests/runtime_test_support.hpp`
- `makeOfflineQuotaProvider(...)` in `tests/runtime_test_support.hpp`
- `makeProviderManager(...)` in `tests/runtime_test_support.hpp`

Expected results:
- provider state becomes `Live`
- runtime state becomes `Current`
- tray status becomes `Normal`
- taskbar rows show quota metrics with no degraded-state override

### 2. Offline

Use the canonical offline quota shape from the table above on `openai`.

Existing end-to-end coverage:
- `AppController keeps dashboard, tray, taskbar, and offline notice in sync`

Expected results:
- runtime state becomes `Offline`
- tray status becomes `Offline`
- offline notice is active
- taskbar row for the failing provider shows the shipped offline/degraded compact labeling with the current documented icon treatment instead of legacy shorthand punctuation

### 3. Stale

Use the canonical stale quota shape from the table above on `openai`.

Existing end-to-end coverage:
- `AppController surfaces stale provider runtime state for taskbar validation`

Expected results:
- provider state becomes `Stale`
- runtime state becomes `Stale`
- tray status becomes `Warning`
- taskbar row keeps its quota metric but shifts to the current stale/degraded icon treatment rather than older `~` shorthand

### 4. Mixed Live + Offline

Use:

- `openai` with the canonical fresh live quota shape
- `minimax` with the canonical offline quota shape

Existing end-to-end coverage:
- `AppController keeps dashboard, tray, taskbar, and offline notice in sync`
- `AppController provider path builds live and offline provider states correctly`

Expected results:
- runtime state becomes `Offline`
- the live provider still shows a quota row
- the offline provider still appears with compact degraded-state labeling

### 5. Mixed Activity + Offline Overlay

Use:

- canonical persisted activity ordering where `openai` has the newest `activity_events`
- `minimax` activity remains visible but older than `openai`
- at least one non-live provider state in `viewModel.providers`

Relevant code path:
- `buildRuntimeTaskbarMetrics(...)` prefers persisted-activity summaries when present
- `buildPersistedActivityTaskbarMetrics(...)` orders rows by `MAX(ae.started_at_utc) DESC, p.display_name ASC`
- `overlayProviderStatesOnTaskbarMetrics(...)` overlays non-live indicators or appends missing non-live providers in `viewModel.providers` order

This is a high-value manual scenario because it exercises the current summarized overlay path rather than only the raw quota path.

## Canonical Ordering And Empty-State Contract

Upcoming taskbar tests should use this as the default ordering/fallback contract unless a narrower test says otherwise.

### 1. Persisted-Activity First

If `buildPersistedActivityTaskbarMetrics(...)` returns rows, those rows are the visible taskbar order.

Current shipped ordering:

- sort by newest `activity_events.started_at_utc`
- break ties by `display_name` ascending
- keep at most two provider rows

Canonical expected order for support and test data:

1. `openai`
2. `minimax`

That order should be produced by giving `openai` the newest activity timestamp and `minimax` the older visible timestamp.

### 2. Quota Fallback Preserves Provider Order

If there are no persisted-activity rows, `buildTaskbarMetricsFromViewModel(...)` walks `viewModel.providers` in order and emits:

- quota-backed rows for enabled providers with at least one quota
- provider-state rows for enabled non-live providers without quota rows

Canonical provider-fed order depends on the seam:

- for `AppController` tests that flow through `ProviderManager`, expect the current enabled-provider order from `ProviderManager`, which is provider-key order (`minimax`, then `openai` for the current two-provider support set)
- for narrower tests that construct `viewModel.providers` directly, make the vector order explicit in the test and assert against that same explicit order

### 3. Non-Live Overlay Appends Missing Providers

When persisted-activity rows already exist, `overlayProviderStatesOnTaskbarMetrics(...)`:

- updates the existing metric status icon if that provider is already represented
- appends a new provider-state row only when a non-live provider is missing
- appends those missing non-live rows in `viewModel.providers` order

Canonical mixed overlay contract:

1. keep the existing persisted-activity row order intact
2. overlay degraded status on matching rows in place
3. append any missing non-live provider rows after the existing rows

### 4. Empty-State Runtime Fallback

If no persisted-activity rows and no provider-backed rows are produced, the taskbar falls back to one runtime-only row only when `runtimeState != Current`.

Canonical runtime-only metric shape:

| Field | Expected value |
| --- | --- |
| `providerKey` | `runtime` |
| `providerDisplayName` | `Runtime` |
| `summaryLabel` | `Runtime` |
| `primaryMetric` | `runtimeStateCompactLabel(runtimeState)` |
| `secondaryMetric` | `runtimeStateLabel(runtimeState)` |
| `statusIcon` | `runtimeStateIndicator(runtimeState)` |

If `runtimeState == Current` and there are no provider rows, the production-path expectation is an empty taskbar metric list rather than a synthetic healthy row. In test-hook builds, do not assume that empty result unless the sample fallback path has been bypassed or replaced for the specific test.

## Known Gap: Catching Up

`RuntimeState::CatchingUp` is real in the shared runtime model, but the current provider-fed app path does not produce it from quota fetches.

What is already true:
- `deriveRuntimeState({ ProviderState::Offline, ProviderState::Syncing })` returns `CatchingUp`
- compact label and indicator behavior are defined in `RuntimeStatus`

What is not yet true:
- `determineProviderState(...)` only returns `Offline`, `Stale`, or `Live`
- current `ProviderManager -> AppController -> taskbar` flow has no provider-fed `Syncing` state

For the first live taskbar pass, treat `CatchingUp` as a documented app-shell gap rather than a must-execute desktop scenario.

## Live Desktop Pass Guidance

For the real Windows taskbar validation pass:

- prioritize `Current`, `Offline`, `Stale`, and `Mixed Live + Offline`
- use `Mixed Activity + Offline Overlay` if local activity summaries are already available in the test machine state
- explicitly note whether the desktop session used real providers, mocked test seams, or replay-friendly local data
- record any overlap or truncation differences between quota rows and persisted-activity summary rows
- when checking multi-provider behavior, preserve the canonical order contract above so any visual change can be treated as a regression rather than setup drift
- if overlay toggling is exercised during the session, confirm that disabling removes the surface cleanly and re-enabling repopulates it from the current runtime state rather than only after a later provider refresh

## Exit Condition For This Note

This Lane 1 prep note is good enough for the current checkpoint when:

- the live taskbar pass can pick a scenario from this note without re-reading the app code
- each listed scenario is tied to an existing code path or an explicit gap
- `Catching Up` is documented honestly as not yet provider-fed end to end
