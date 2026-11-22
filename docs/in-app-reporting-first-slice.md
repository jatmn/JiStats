# In-App Reporting First Slice: Scope Controls And History Surface

> **Status**: Historical design for the shipped scope controls and history views. The dashboard launches a dedicated Analysis window, which shares its scope with manual and scheduled HTML export. See the [work tracker](progress.md) for follow-ups.
> **Purpose**: Preserve the original first-slice design and acceptance criteria.

## Selected Slice

The first in-app reporting expansion combines the spike's **Slice B** (scope controls) and **Slice C** (history/breakdown views) into one vertical slice because scope controls have no user value without something to scope over.

This avoids shipping scope UI that does nothing, or shipping history views that lack explicit scope.

## What This Slice Delivers

### 1. Explicit Scope Bar

A visible provider/account/date-range selector inside the main dashboard window:

- Provider dropdown: filters all in-app analysis views to a single provider or "All Providers"
- Account dropdown: filters to a specific account within the selected provider, or "All Accounts"
- Date-range selector: presets plus custom range
  - `Today`
  - `Last 24 Hours`
  - `Last 7 Days`
  - `This Month`
  - `Custom` (date picker pair)

The scope bar lives at the top of the analysis/history surface, not the overview tab.

The currently selected scope should be persisted in preferences (extend existing `lastSelectedProviderId`/`lastSelectedAccountId`) and used by manual HTML export when the user triggers `File -> Export HTML Report...`.

### 2. Daily Summary View

A table showing one row per day within the selected date range:

- Date label
- Request/activity count (from `usage_rollups_daily` or fallback to hourly aggregation)
- Source badge (local / imported / mixed)
- Gap/unknown indicator if the day has incomplete coverage

This is the minimum table that makes historical inspection useful inside the app.

### 3. Provider/Account Breakdown

Below the daily summary table, a compact breakdown section:

- Provider-level totals within the selected range
- Account-level totals when a single provider is selected
- Source attribution per row (local, imported, backfilled)

This reuses the same aggregation queries that feed `ReportAggregateData` for HTML export.

### 4. Trend Visual

A compact daily bar or step chart showing activity volume across the selected date range:

- One bar per day
- Height proportional to daily activity count
- Uses daily rollups for performance
- Native owner-drawn control (no external charting library)

This is the first visual element beyond the existing usage progress bars.

## View Model Additions

### AnalysisScopeState

New struct to track the current in-app analysis scope:

```cpp
struct AnalysisScopeState {
    std::string selectedProviderId;  // empty = all
    std::string selectedAccountId;   // empty = all
    DateRangePreset dateRangePreset = DateRangePreset::Last7Days;
    std::chrono::system_clock::time_point customRangeStart;
    std::chrono::system_clock::time_point customRangeEnd;
};

enum class DateRangePreset {
    Today,
    Last24Hours,
    Last7Days,
    ThisMonth,
    Custom
};
```

### AnalysisViewModel

New struct for the analysis/history surface, separate from `DashboardViewModel`:

```cpp
struct AnalysisDailySummary {
    std::string dateLabel;
    long long activityCount = 0;
    std::string sourceLabel;     // "Local", "Imported", "Mixed"
    bool hasGaps = false;
};

struct AnalysisBreakdownRow {
    std::string key;
    std::string label;
    long long activityCount = 0;
    std::string sourceLabel;
};

struct AnalysisTrendPoint {
    std::string dateLabel;
    long long activityCount = 0;
};

struct AnalysisViewModel {
    AnalysisScopeState scope;
    std::vector<AnalysisDailySummary> dailySummaries;
    std::vector<AnalysisBreakdownRow> providerBreakdown;
    std::vector<AnalysisBreakdownRow> accountBreakdown;
    std::vector<AnalysisTrendPoint> trendPoints;
    std::string scopeLabel;       // "OpenAI / All Accounts / Last 7 Days"
    std::string dataQualityNote;  // "Contains 2 days with incomplete coverage"
};
```

## Query Seams Needed

### Daily rollup fetch by scope

```
SELECT date_label, SUM(request_count), ...
FROM usage_rollups_daily
WHERE provider_key = ? AND account_id = ?
  AND date_label BETWEEN ? AND ?
GROUP BY date_label
ORDER BY date_label
```

This query family already exists in the rollup infrastructure; the new work is exposing it through a reusable function that both the in-app view and the HTML export aggregate-data provider can call.

### Provider/account breakdown by date range

```
SELECT provider_key, account_id, SUM(request_count), ...
FROM usage_rollups_daily
WHERE date_label BETWEEN ? AND ?
GROUP BY provider_key, account_id
```

### Gap coverage check by date range

```
SELECT DISTINCT date(period_start_utc) as gap_date
FROM gap_markers
WHERE provider_key = ?
  AND period_start_utc >= ? AND period_end_utc <= ?
```

## Original Implementation Sequence

### Step 1: Scope state and persistence

- Add `AnalysisScopeState` and `DateRangePreset` to a new `include/gui/AnalysisViewModel.hpp`
- Extend `DashboardPreferences` with `analysisDateRangePreset` and custom range fields
- Persist and load scope state in `AppController` preferences

### Step 2: Analysis query layer

- Add `fetchDailySummaries(scope)` and `fetchBreakdown(scope)` to a reusable query module
- These functions query `usage_rollups_daily` and `gap_markers` by provider/account/date range
- Wire the same functions into `ReportingService` aggregate-data provider for reuse

### Step 3: Analysis view model population

- Add `AnalysisViewModel` struct
- Add `buildAnalysisViewModel(scope, storageService)` function
- Wire into `AppController` so the dashboard can request analysis data for the current scope

### Step 4: Dashboard analysis surface

- Add a new tab or panel in `DashboardPresenter` for the analysis/history view
- Render scope bar controls (provider/account dropdowns, date-range presets)
- Render daily summary table
- Render provider/account breakdown rows
- Render compact daily trend bars using owner-drawn native control

### Step 5: Manual export scope alignment

- When `File -> Export HTML Report...` is triggered, use the current analysis scope (if set) for the `ReportRequest` provider/account filter and date range
- Fall back to the existing behavior (dashboard selection) if no analysis scope is active

### Step 6: Tests

- Unit tests for daily rollup fetch with scope filters
- Unit tests for breakdown aggregation
- Dashboard presenter tests for scope control state persistence
- Integration test proving manual export uses the visible analysis scope

## Acceptance Criteria

From the spike's acceptance questions:

1. Can the user inspect meaningful historical data in-app without generating HTML first? **Yes** — daily summary table and trend bars.
2. Is the currently viewed provider/account/date-range scope obvious? **Yes** — explicit scope bar with visible selections.
3. Are local, imported, backfilled, estimated, and unknown values still clearly labeled? **Yes** — source badges on daily rows and breakdown rows, gap indicators.
4. Does the HTML export remain a portable artifact rather than the only detailed viewer? **Yes** — in-app analysis is the primary inspection surface; export is for portability.
5. Are rollups and retention behavior respected so the UI does not depend on indefinite raw-event storage? **Yes** — all queries use `usage_rollups_daily` not raw events.
6. Does the overview remain compact enough to serve as the primary live dashboard? **Yes** — analysis surface is a separate tab/panel, not an expansion of the overview.

## What This Slice Does Not Include

- Model-level breakdown (deferred until per-model data is trustworthy)
- Recovery/data-quality detail panel (Slice D in the spike)
- Drill-down interactions on individual rows
- Custom charting beyond simple daily bars
- Export preview showing exactly what the current scope would produce
- Hourly detail views (prefer daily rollups for the first pass)

## Related Files

- `docs/in-app-reporting-and-analysis-spike.md` — parent spike
- `docs/plan.md` — section 13 (reporting)
- `include/gui/DashboardPresenter.hpp` — dashboard types
- `src/reporting/ReportingService.cpp` — aggregate data pipeline
- `src/db/StorageService.cpp` — rollup and gap marker queries
- `src/app/AppController.cpp` — preference persistence and data wiring
