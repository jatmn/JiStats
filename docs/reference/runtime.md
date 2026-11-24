# Runtime, startup, and recovery

[AppController](../../src/app/AppController.cpp) coordinates refresh and recovery. The [runtime status model](../../include/core/RuntimeStatus.hpp) drives dashboard and tray status. [SettingsService](../../src/db/SettingsService.cpp) persists user preferences.

## Startup and background operation

Launch with `--background` for tray-first startup. **Start with Windows** is opt-in. [StartupManager](../../src/platform/StartupManager.cpp) stores the executable path and optional `--background` flag under the current user's Windows Run registry key. The entry has no working-directory setting. Turning off the option removes the entry. If rewriting the background flag fails, JiStats keeps the existing entry and saves the effective background preference from it. The [embedded SQLite baseline](storage.md#migration-behavior) works at sign-in regardless of the working directory.

The dashboard and tray show Starting while JiStats opens storage, including during a schema upgrade. The dashboard restores saved placement after storage opens, unless its bounds changed in the meantime. Settings, History, import and export become available after startup publishes its initial view. JiStats restores cached quota when available and otherwise publishes the first refreshed view after provider work completes. Opening Settings triggers asynchronous discovery. Defer startup catch-up to cases that need it. Exiting from the menu stops the app. Closing the dashboard leaves background monitoring available.

Provider toggles govern provider startup refresh and new local observation. A queued opt-out stops new runtime quota and catch-up requests while Settings waits for the refresh lock. Requests already admitted may finish. They preserve saved history. Startup and Analysis refresh scan [Codex SQLite](../providers/openai.md#codex-sqlite-privacy-boundary) only when the separate Codex model attribution setting is enabled. Keep background work small by using changed-file detection, cached state, and update suppression when presenter output is unchanged. The dashboard timer and tray or taskbar open actions queue local data refreshes on the existing interaction worker. The dashboard displays the last available data while the worker waits for an active refresh or reads history. Analysis opens with cached placement or default bounds. The placement worker loads optional Analysis bounds in the background and saves window bounds in SQLite. The placement worker preserves saved bounds when an untouched fallback window closes. Settings and Analysis scope requests save their own preference fields under the provider refresh lock, including when the worker handles both requests together.

Settings keeps pending choices and text edits visible during background refresh. Changes to startup, taskbar visibility, report scheduling, or the selected provider row save on the interaction worker without fetching provider quota. Changes to provider tracking refresh the affected runtime state. If SQLite rejects a save, Settings keeps the draft and offers **Retry save**. A provider turned off stays paused for the current process even if its save failed. A new opt-in takes effect after a successful save. When SQLite was unavailable at launch, Settings identifies changes that apply only for the session.

## Offline and catch-up behavior

| State | Meaning |
| --- | --- |
| Current | Recent usable evidence |
| Stale | Last known evidence has aged |
| Catching up | A refresh or reconciliation is filling a known gap |
| Offline | A provider path is unavailable |

Individual provider rows use **Live** for fresh evidence, **Stale** for aged evidence, **Syncing** during reconciliation, and **Offline** for an unavailable path. **Sample** identifies sample data. The shared runtime state presents provider reconciliation as **Catching up**.

A dismissible offline notice offers retry and dismiss actions. JiStats shows the notice on entry to the offline state and does not repeat it while the state persists. Dismissal starts a 60-minute cooldown. JiStats checks the stored dismissal time when entering offline, including after restart, while the persistent offline status stays visible. Runtime retry state uses a five-minute initial delay and fifteen minutes for continued offline refresh failures.

A dismissal follows the offline episode when the failing provider changes. When no provider reports Offline, a fresh usable provider quota API response clears the saved dismissal for every provider, including omitted providers. A provider that returns fresh recovery evidence from a local read clears only its own dismissal when no provider reports Offline. An omitted provider retains its dismissal in that case. Cached quota, empty refreshes, sample-only results, and local reads by other providers can preserve the dismissal.

Recovery uses connectivity, sync, and checkpoint records and annotates periods of missing coverage. Use local evidence or provider backfill where supported. Preserve unknown gaps when evidence is absent and avoid double counting when sources overlap. Recovery commits its gap markers and completion time together. If either write fails, the original catch-up marker stays saved for retry. [Storage and schema](storage.md) describes the persisted history.

Catch-up rebuilds usage rollups only for UTC hourly and daily buckets that start at or after the raw-event retention cutoff. If the cutoff falls within a bucket, its saved total is preserved. Catch-up accounts for the previous completed retention pass if the system clock moves backward.

## Monitoring cost

Prefer validated local artifacts and direct quota or usage reads. Any model-backed check must be explicit, configurable, and use the lowest-cost suitable path. Keep monitoring activity distinguishable from user activity.

For each direct quota API surface, the runtime uses a five-minute cadence while quota is changing. After thirty minutes of unchanged responses it backs off to thirty-minute checks, and a changed response restores the faster cadence for that surface. Local file watchers use their own cadence. A local refresh cannot bypass the direct API budget and reuses its cached quota response until the next poll is due.
