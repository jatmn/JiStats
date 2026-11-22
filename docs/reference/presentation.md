# Dashboard, tray, and taskbar

## Dashboard and tray

The dashboard is the main surface for current data and recent activity. **File** contains Settings, **Import OpenAI Export…**, **Export HTML Report…**, and Exit. **Help** contains View Help and About JiStats. Help and About should explain user-facing scope and provider availability. About summarizes the application and license. The tray stays available while the dashboard is closed and carries runtime status and offline actions.

## Quota, freshness, and confidence

Summary bars fill as remaining quota increases. The provider table's **Used** column shows normalized usage percentages for quota rows. Keep supporting counts labeled with their own units. Preserve individual quota windows, with 5-hour and 7-day windows shown together where supported. Include source and freshness and identify stale, backfilled, and unknown periods where applicable.

| Label | Meaning |
| --- | --- |
| `Exact` | Direct, validated provider value |
| `HighConfidence` | Validated behavior with bounded inference |
| `Estimated` | Partial evidence plus inference |
| `Unknown` | Insufficient evidence for a numeric value |
| `LocalExact` | Exact observed local activity or an approved local counter |
| `Imported` | Historical data from a provider or user-export artifact |

Reserve exact quota claims for direct, validated provider values and show their freshness. Label estimates and make their basis visible. Show quota as unknown when the evidence cannot support a number, even if local activity counts are precise. The v1.0 comparison target for authoritative quota values after refresh is within one count unit or one percentage point of the source. Keep source identity, refresh time, basis, and reconciliation context available where the view supports them. The [provider matrix](../capability-matrix.md) describes the available sources.

## Taskbar

The Windows 11 overlay shows subscription-quota rows for enabled providers with eligible data. Each row shows the provider and its primary remaining percentage, with a secondary remaining percentage when available. Status emphasis marks degraded quota rows. The production overlay has no rows when eligible quota is missing. Check the dashboard and tray for runtime status in that case. Keep activity and model usage in the dashboard and Analysis. The OpenAI hover tooltip can add 5-minute and 30-minute Codex throughput context.

The presenter uses blue at 80% remaining and above, then green at 50%, yellow at 25%, orange above 10%, and red with value emphasis at 10% or below. Degraded state can change the color. Use compact, readable text, a transparent or blended background, and placement that clears the clock, notification area, and adjacent taskbar utilities. Preserve hover and context-menu behavior and recovery after Explorer restarts. See the [desktop validation guide](../validation/desktop.md) and [TrafficMonitor research](trafficmonitor-research.md).

Implementation references are [DashboardPresenter](../../src/gui/DashboardPresenter.cpp), [AppController](../../src/app/AppController.cpp), [TrayController](../../src/app/TrayController.cpp), and [TaskbarPresenter](../../src/taskbar/TaskbarPresenter.cpp).
