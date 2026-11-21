# Workstream Ownership and Interface Contracts

> **Status**: Living reference aligned to the current repo layout
> **Purpose**: Define clear ownership boundaries for parallel engineering work without depending on components or sequencing that no longer match the current tree
> **Driven by**: `docs/plan.md` section 14.1 plus the current `include/`, `src/`, and `tests/` layout

## Workstream Overview

From plan.md section 14.1:

| Workstream | Scope | Owner |
|------------|-------|-------|
| `A` Platform and app shell | App startup, settings persistence, lifecycle, tray bootstrap | Platform |
| `B` Storage and schema | SQLite schema, migrations, retention, query layer | Storage |
| `C` Provider integration | OpenAI and MiniMax capability mapping, sync flows, import contracts | Provider |
| `D` Mirror engine and analytics | Rolling windows, quota snapshots, confidence classification | Analytics |
| `E` Dashboard, tray, and UX | Settings UI, dashboard cards, status indicators, notices | UX |
| `F` Taskbar integration | Native taskbar surface, shell behavior, explorer lifecycle handling | Platform |
| `G` Reporting and exports | HTML output, metadata, backfill labeling | Reporting |
| `H` Quality and infrastructure | CI, test fixtures, static analysis, packaging scripts | Quality |

## Component Ownership Matrix

### Platform Layer (Workstream A)

| Component | Files | Public Interface |
|-----------|-------|------------------|
| AppController | `src/app/AppController.cpp/hpp` | `init()`, `shutdown()`, `getState()` |
| TrayController | `src/app/TrayController.cpp/hpp` | `showIcon()`, `setStatus()`, `showMenu()` |
| SettingsService | `src/db/SettingsService.cpp/hpp` | `getBool()`, `setBool()`, `getString()`, `setString()` |
| StartupManager | `src/platform/StartupManager.cpp/hpp` | `enable()`, `disable()`, `isEnabled()` |

### Storage Layer (Workstream B)

| Component | Files | Public Interface |
|-----------|-------|------------------|
| StorageService | `src/db/StorageService.cpp/hpp` | All DB operations, single serialization point |
| SchemaManager | `src/db/SchemaManager.cpp/hpp` | `migrate()`, `getVersion()` |

**Important**: StorageService is the ONLY write path to DB. No other component writes directly.

### Provider Layer (Workstream C)

| Component | Files | Public Interface |
|-----------|-------|------------------|
| ProviderManager | `src/providers/ProviderManager.cpp/hpp` | `addProvider()`, `getProvider()`, `getAllProviders()`, `getEnabledProviders()` |
| OpenAIAdapter | `src/providers/openai/OpenAIAdapter.cpp/hpp` | `fetchUsage()`, `fetchQuota()` |
| MiniMaxAdapter | `src/providers/minimax/MiniMaxAdapter.cpp/hpp` | `fetchUsage()`, `fetchQuota()` |
| LocalObservationService | `src/observation/LocalObservationService.cpp/hpp` | Emits local subscription/activity events independent of provider API imports |

Current note:

- `ProviderManager` currently exposes `getAllProviders()` and `getEnabledProviders()` directly rather than a single aggregate `listProviders()` helper.

**Interface contract**: Provider adapter must implement `IProviderAdapter`:
```cpp
class IProviderAdapter {
    virtual UsageResponse fetchUsage(time_t from, time_t to) = 0;
    virtual QuotaResponse fetchQuota() = 0;
    virtual bool validateCredentials() = 0;
    virtual std::vector<Capability> getCapabilities() = 0;
};
```

**Boundary rule**: `IProviderAdapter` is for provider-owned usage/quota surfaces. Local foreground/session observation must not be forced through `fetchUsage()` or `UsageEvent` if it is not a quantified usage unit.

### Analytics Layer (Workstream D)

| Component | Files | Public Interface |
|-----------|-------|------------------|
| MirrorEngine | `src/analytics/MirrorEngine.cpp/hpp` | `computeMirror()`, `reconcile()` |
| AnalyticsEngine | `src/analytics/AnalyticsEngine.cpp/hpp` | `computeDailyUsage()`, `computeModelUsage()`, `computeCostBreakdown()`, `getUsageTrend()`, `computePeakHours()`, `estimateMonthlyCost()` |
| SyncScheduler | `src/core/SyncScheduler.cpp/hpp` | `scheduleSync()`, `pause()`, `resume()` |

**Interface contract**: MirrorEngine input/output:
```cpp
struct MirrorInput {
    std::vector<UsageEvent> localEvents;
    std::vector<UsageEvent> importedEvents;
    time_t windowStart;
    time_t windowEnd;
};

struct MirrorOutput {
    QuotaSnapshot snapshot;
    ConfidenceTier tier;
    std::string basisSummary;
};
```

### UX Layer (Workstream E)

| Component | Files | Public Interface |
|-----------|-------|------------------|
| DashboardPresenter | `src/gui/DashboardPresenter.cpp/hpp` | `getViewModel()` returning DashboardViewModel |
| NotificationService | `src/gui/NotificationService.cpp/hpp` | `showNotice()`, `dismissNotice()`, `suppressUntil()` |

Current note:

- the current shipped settings flow is presenter-owned and app-controller-routed; there is no separate `SettingsDialog` component in the tree today

**Rule**: UI consumes derived view models, never raw DB/provider objects.

### Taskbar Layer (Workstream F)

| Component | Files | Public Interface |
|-----------|-------|------------------|
| TaskbarPresenter | `src/taskbar/TaskbarPresenter.cpp/hpp` | `updateMetrics(TaskbarMetrics)` |

Current note:

- the current shipped overlay path is centered on `TaskbarPresenter`; there is no separate `TaskbarSurface` component in the repo today

**Note**: Implementation approach requires TrafficMonitor review (Phase 4).

### Reporting Layer (Workstream G)

| Component | Files | Public Interface |
|-----------|-------|------------------|
| ReportingService | `src/reporting/ReportingService.cpp/hpp` | `generateReport(ReportRequest)` |
| HtmlExporter | `src/reporting/HtmlExporter.cpp/hpp` | `exportToFile(...)`, `generateHtml(...)` over `DashboardViewModel` and optional `ReportMetadata` |

## Cross-Component Communication

Current repo reality is more direct than the original planning draft:

- `AppController` is the main composition root
- presenters consume normalized view models and callbacks
- providers and storage are typically coordinated through `AppController`
- background work is still callback/thread based in several places rather than event-bus driven

Keep the boundary rule, not the old mechanism assumption:

- avoid ad hoc direct coupling between unrelated workstreams
- prefer explicit service interfaces, callback hooks, and view models
- treat `AppController` as the place where cross-subsystem coordination is expected

## Interface Stability Requirements

### Stable Interfaces (Do Not Change Without Review)

1. `IProviderAdapter` - All provider adapters implement this
2. `StorageService` public methods - All DB writes go through this
3. `SyncScheduler` scheduling interface - Shared runtime scheduling contract; currently not the same thing as `ProviderManager`
4. `DashboardViewModel` - Consumed by all UI components

### Internal Interfaces (Can Change Freely Within Workstream)

- `MirrorEngine` internal logic
- `AnalyticsEngine` computation methods
- `TrayController` implementation details

## Handoff Checklist

Before workstream task is considered complete:
- [ ] Public interface header files reviewed by affected workstreams
- [ ] Test interfaces documented
- [ ] Integration points tested with real data (fixture if no provider access)
- [ ] Error handling paths reviewed
- [ ] Logging points identified

## Dependency Graph

```
AppController
    ├── TrayController (A)
    ├── ProviderManager (C)
    │   ├── OpenAIAdapter (C)
    │   └── MiniMaxAdapter (C)
    ├── StorageService (B)
    ├── DashboardPresenter (E)
    │   └── NotificationService (E)
    └── TaskbarPresenter (F)
```

Supporting runtime/storage components that also exist in the current repo:

- `SettingsService` (B)
- `LocalObservationService` / `ObservationPersistenceBridge` (B/C boundary)
- `ReportingService` (G)
- `SyncScheduler` and `AnalyticsEngine` (D), currently not central to the active taskbar/runtime slice

**Note**: `StorageService` is the DB serialization point. All writes flow through it.

## Sequencing Note

This document should no longer be read as a strict untouched Phase 1 sequence.

The repo already contains meaningful work across:

- storage and migrations
- startup/tray shell
- OpenAI and MiniMax provider paths
- dashboard and taskbar surfaces
- reporting
- quality-gate automation

Use this file for ownership boundaries and integration cautions, not as a claim that later workstreams have not started yet.

## References

- Parallel workstreams: `docs/plan.md` section 14.1
- Runtime components: `docs/plan.md` section 10.2.1
- Entity model: `docs/entity-model.md`
- Threading model: `docs/plan.md` section 10.2.3
