# Gap-Handling and Launch-Reconciliation Policy

> **Status**: Current
> **Purpose**: Define how JiStats handles gaps in data (downtime, offline periods, missing data)
> **Driven by**: plan.md section 10.4 (Startup and Recovery Direction) and section 6.6 (Gap Reconciliation Risk)

## Shipped Runtime Baseline

The following gap-handling features are implemented and tested in the current v1.0 runtime:

### Database Schema

**runtime_state table** (migration 005):
- `last_launch_at_utc` - When the app last started
- `last_successful_sync_utc` - Last successful provider sync
- `last_local_observation_utc` - Last local activity observation
- `recovery_marker` - Checkpoint for gap detection

**gap_markers table** (migration 008):
- `provider_id` - Provider identifier
- `account_id` - Account identifier
- `period_start_utc` / `period_end_utc` - Gap time range
- `classification` - One of: `KnownZero`, `Reconstructed`, `Unknown`
- `basis_summary` - Human-readable explanation
- `source_kind` - Origin of the classification

### Core Classes

- **GapClassifier** (`include/analytics/GapClassifier.hpp`): Implements three-state classification enum (`KnownZero`, `Reconstructed`, `Unknown`), with `GapClassifier::classify()` method using `GapClassificationInput` struct and returning `GapClassificationResult`
- **StorageService** (`include/db/StorageService.hpp`): Methods `upsertGapMarker()`, `getGapMarkers()`, `deleteGapMarkersOlderThan()`, `upsertRuntimeState()`, `getRuntimeState()`

### Test Coverage

- `tests/test_storage_gap_markers.cpp` validates classifier logic and persistence

## The Problem

JiStats will have gaps in data when:
1. App wasn't running for a period
2. Network was offline
3. Provider API was unreachable
4. Sync failed repeatedly

These gaps must be handled correctly so users don't see:
- Fabricated data
- Incorrect "zero usage" when there was usage
- Confusion about what's known vs unknown

## The Three States

From plan.md section 11.3 (Data Semantics):

| State | Meaning | Display |
|-------|---------|---------|
| **Known-Zero** | Confirmed no usage occurred | "0" with `Exact` badge |
| **Reconstructed** | Backfilled from provider API | Shows as `Imported` |
| **Unknown** | Can't determine what happened | "?" with `Unknown` badge |

## Gap Classification Rules

### Rule 1: Known-Zero Periods

A period is "known-zero" when:
- App was running AND
- Provider API confirms no activity AND
- Local events show no activity

**Display**: "0" with `Exact` badge
**Example**: "No OpenAI usage on 2024-01-15 (verified)"

### Rule 2: Reconstructed Periods (Backfill)

A period is "reconstructed" when:
- Provider API backfill data is available
- Marked with `is_backfilled = 1` in `usage_events`

**Display**: Normal numbers with `Imported` badge
**Example**: "42 requests imported from OpenAI for Jan 10-14"

### Rule 3: Unknown Periods

A period is "unknown" when:
- App wasn't running AND
- No provider backfill available
- Or: Provider API returned insufficient data

**Display**: "?" with `Unknown` badge, no numbers
**Example**: "Usage unknown for Jan 10-14 (app was offline)"

## Launch Reconciliation Flow

From plan.md section 10.4:

On app launch after downtime:

```
1. Load last known state from runtime_state table
2. Check last_successful_sync_utc
3. If gap > threshold (e.g., > 1 hour):
   a. Enter "catching_up" state
   b. Attempt provider API backfill
   c. Import available historical data
   d. Compare imported vs local events
   e. Update confidence tiers
   f. Mark reconciled where possible
4. If backfill succeeded:
   a. Show "Catching up..." indicator briefly
   b. Update dashboard with reconciled data
   c. Clear catching_up state
5. If backfill failed:
   a. Mark affected periods as "Unknown"
   b. Show offline indicator
   c. Continue with last known state
```

## Gap Detection Logic

### Detection Algorithm

```cpp
// On launch, check for gaps:
Time lastSync = runtime_state.last_successful_sync;
Time now = current_time();
Duration gap = now - lastSync;

if (gap > threshold) {
    // Gap detected
    for each day in [lastSync, now] {
        if (provider_api.hasData(day)) {
            import as reconstructed (is_backfilled = 1)
        } else if (local_events.exists(day)) {
            // Local events exist but we missed sync
            mark as local with lower confidence
        } else {
            // Neither provider nor local
            mark as Known-Zero if app was running
            mark as Unknown if app was not running
        }
    }
}
```

### Runtime State Check

From `docs/sqlite-schema.md`:
```
runtime_state:
  - last_launch_at_utc
  - last_successful_sync_utc
  - last_local_observation_utc
  - recovery_marker (checkpoint for gap detection)
```

## Reconciliation with Provider Data

### OpenAI Reconciliation

From `docs/openai-mirror-spec.md`:
- Compare local events sum vs imported daily total
- If within 1%: mark `HighConfidence`
- If mismatch > 1%: investigate duplicate or missing events
- If gap in provider data: mark `Unknown`

### MiniMax Reconciliation

Same approach as OpenAI, but:
- v1.0 uses subscription remains endpoints as provider truth source
- PAYG is deferred to v1.x; local-only reconciliation applies until PAYG API is validated
- Mark as `Estimated` for local-only periods

## UI Indicators for Gap States

### Dashboard Display

| State | Indicator | Color |
|-------|-----------|-------|
| Fresh (no gap) | None | - |
| Catching up | "Syncing..." with spinner | Yellow |
| Stale (gap > 1hr) | "Stale" badge | Orange |
| Unknown period | "?" on affected dates | Gray |
| Offline | Red dot + "Offline" | Red |

### Gap in Time Series Charts

When showing charts (e.g., daily usage over time):
- Known-zero: Show as "0" bar (gray)
- Reconstructed: Show normal bar with `Imported` badge
- Unknown: Gap in chart, no bar, tooltip explains

### Report Metadata

Reports (from `docs/plan.md` section 13.1) must include:
- "Contains backfilled data: Yes/No"
- "Contains unknown periods: Yes/No"
- "Last reconciled: {timestamp}"

## Gap Handling by Scenario

### Scenario 1: App was closed for 3 days

```
State: Launch after 3-day gap
Detection: last_successful_sync was 3 days ago
Action:
  - Attempt OpenAI API backfill for 3 days
  - If successful: show reconstructed data with Imported badges
  - If failed: show "Unknown" for 3 days with offline indicator
Outcome:
  - User sees "? for Jan 10-12 (offline)" or "Import for Jan 10-12"
  - Dashboard shows "Catching up..." during reconciliation
```

### Scenario 2: Network was down for 2 hours during sync

```
State: Sync failed, then network restored
Detection: sync_state.last_failure_category = "network"
Action:
  - On network restore, retry sync
  - If successful: clear offline indicator
  - If failed again: backoff and show offline
Outcome:
  - Last sync shown as "Last sync: 2 hours ago" with yellow indicator
  - Quota metrics marked as "Estimated" until next successful sync
```

### Scenario 3: Provider API returned incomplete data

```
State: OpenAI usage API returned partial data
Detection: Imported total < local events sum
Action:
  - Accept imported as authoritative
  - Mark local-only events as "reconciling"
  - Update confidence tier for affected period
Outcome:
  - Some events may be marked duplicate or missing
  - User sees "Some events may not be captured" notice
```

### Scenario 4: App crash during sync

```
State: App crashed, sync was in progress
Detection: runtime_state shows in-progress sync (but no completion)
Action:
  - On restart, check sync_state.last_attempt without last_success
  - Re-run sync from last cursor
  - Don't assume data was written - re-sync
Outcome:
  - No duplicate events (sync_state prevents re-import)
  - Clean recovery from crash state
```

## Database Persistence for Gap Tracking

### usage_events Fields

```sql
is_backfilled INTEGER DEFAULT 0  -- 1 = from provider backfill
source_type TEXT                 -- 'local', 'imported', 'backfill'
```

### quota_snapshots Fields

```sql
is_reconciled INTEGER DEFAULT 0  -- 1 = verified against provider
confidence_tier TEXT             -- 'Exact', 'HighConfidence', 'Estimated', 'Unknown'
```

### sync_state Fields

```sql
last_failure_category TEXT  -- 'network', 'auth', 'rate_limit', 'unknown'
```

## Testing Gap Handling

Test cases needed:
1. Launch after 3-day gap with successful backfill
2. Launch after 3-day gap with failed backfill (show "Unknown")
3. Sync interrupted mid-import (clean restart)
4. Provider API returns incomplete data
5. App crash during sync (verify no duplicates)

See `docs/plan.md` section 14.3 (Test Strategy) for fixture requirements.

## Open Questions

1. **Threshold for gap detection**: 1 hour? 30 minutes? Configurable?
2. **How to show "Known-Zero" in dashboard**: "0" with Exact badge or no display?
3. **Backfill depth**: How far back should we attempt backfill? (90 days? 30?)
4. **User notification**: Should user be notified of gaps? Only if significant (>1 day)?

## References

- Startup and Recovery: `docs/plan.md` section 10.4
- Connectivity Recovery: `docs/plan.md` section 10.4.1
- Data Semantics: `docs/plan.md` section 11.3
- Gap Reconciliation Risk: `docs/plan.md` section 7.6
- OpenAI mirror spec: `docs/openai-mirror-spec.md` (reconciliation section)
