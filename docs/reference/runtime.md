# Runtime, startup, and recovery

[AppController](../../src/app/AppController.cpp) coordinates refresh and recovery. The [runtime status model](../../include/core/RuntimeStatus.hpp) drives dashboard and tray status. [SettingsService](../../src/db/SettingsService.cpp) persists user preferences.

## Startup and background operation

Launch with `--background` for tray-first startup. **Start with Windows** is opt-in. [StartupManager](../../src/platform/StartupManager.cpp) registers the background launch under the current user's Windows Run registry key.

Hydrated startup restores preferences and cached quota before first paint. Keep provider discovery and slow imports off the first-paint path. Opening Settings triggers asynchronous discovery. Defer startup catch-up to cases that need it. Exiting from the menu stops the app. Closing the dashboard leaves background monitoring available.

Provider toggles govern startup refresh and new local observation. They preserve saved history. Keep background work small by using changed-file detection, cached state, and update suppression when presenter output is unchanged.

## Offline and catch-up behavior

| State | Meaning |
| --- | --- |
| Current | Recent usable evidence |
| Stale | Last known evidence has aged |
| Catching up | A refresh or reconciliation is filling a known gap |
| Offline | A provider path is unavailable |

A dismissible offline notice offers retry and dismiss actions. Dismissal suppresses the notice while the persistent status can still show offline. Runtime retry state uses a five-minute initial delay and fifteen minutes for continued offline refresh failures.

A dismissal follows the offline episode when the failing provider changes. When no provider reports Offline, a fresh usable provider quota API response clears the saved dismissal for every provider, including omitted providers. A provider that returns fresh recovery evidence from a local read clears only its own dismissal when no provider reports Offline. An omitted provider retains its dismissal in that case. Cached quota, empty refreshes, sample-only results, and local reads by other providers can preserve the dismissal.

Recovery uses connectivity, sync, and checkpoint records and annotates periods of missing coverage. Use local evidence or provider backfill where supported. Preserve unknown gaps when evidence is absent and avoid double counting when sources overlap. [Storage and schema](storage.md) describes the persisted history.

## Monitoring cost

Prefer validated local artifacts and direct quota or usage reads. Any model-backed check must be explicit, configurable, and use the lowest-cost suitable path. Keep monitoring activity distinguishable from user activity.

For each direct quota API surface, the runtime uses a five-minute cadence while quota is changing. After thirty minutes of unchanged responses it backs off to thirty-minute checks, and a changed response restores the faster cadence for that surface. Local file watchers use their own cadence.
