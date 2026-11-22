# TrafficMonitor Research Notes

> **Status**: Completed (source-verified)
> **Date**: 2026-04-17
> **Purpose**: Extract concrete, source-backed taskbar integration patterns for JiStats
> **Reference Repo**: `https://github.com/zhongyang219/TrafficMonitor` (analyzed at local clone `master` commit `7901ce0`)

## Executive Summary

TrafficMonitor does **not** use `ITaskbarList3` overlay/progress APIs for its main taskbar metrics surface.
It renders a dedicated window (`CTaskBarDlg`) and embeds it into taskbar window hierarchy using `SetParent(...)`, with OS-specific placement logic.

This is the key pattern to replicate for a DU Meter-like realtime taskbar text surface.

## Source-Verified Findings

### 1) Taskbar embedding mechanism is child-window insertion

Evidence:
- `TaskBarDlg.cpp` calls `SetParent(this->m_hWnd, GetParentHwnd())` during init to insert its dialog into taskbar parent hierarchy.
- `TaskBarDlg.cpp` also marks insertion failure via `m_connot_insert_to_task_bar` and applies fallback positioning if insertion fails.

Key lines:
- `TrafficMonitor/TaskBarDlg.cpp:0999`
- `TrafficMonitor/TaskBarDlg.cpp:0550-0561`

### 2) Different taskbar implementations are selected per OS

Evidence:
- `TrafficMonitorDlg.cpp` chooses `CWin11TaskbarDlg`, `CClassicalTaskbarDlg`, or `CWineTaskbarDlg` in `OpenTaskBarWnd()`.

Key lines:
- `TrafficMonitor/TrafficMonitorDlg.cpp:0566-0575`

### 3) Classic taskbar placement depends on shell internal window classes

Evidence:
- Finds `ReBarWindow32`/`WorkerW`, then `MSTaskSwWClass` or `MSTaskListWClass`.
- Resizes task list area with `MoveWindow(...)` to make room for TrafficMonitor's embedded window.

Key lines:
- `TrafficMonitor/ClassicalTaskbarDlg.cpp:0072-0077`
- `TrafficMonitor/ClassicalTaskbarDlg.cpp:0024-0031`

### 4) Win11 placement uses `Shell_TrayWnd` child anchors

Evidence:
- Finds `TrayNotifyWnd` and `Start` window.
- Positions taskbar window near notify/start regions with special handling for centered alignment, widgets, and secondary displays.

Key lines:
- `TrafficMonitor/Win11TaskbarDlg.cpp:0091-0093`
- `TrafficMonitor/Win11TaskbarDlg.cpp:0014-0047`
- `TrafficMonitor/Win11TaskbarDlg.cpp:0053-0063`

### 5) Explorer restart resilience is explicit

Evidence:
- Registers `TaskbarCreated` message.
- On `OnTaskBarCreated`, recreates tray icon and reopens taskbar window.

Key lines:
- `TrafficMonitor/TrafficMonitorDlg.cpp:0031`
- `TrafficMonitor/TrafficMonitorDlg.cpp:0097`
- `TrafficMonitor/TrafficMonitorDlg.cpp:2537-2553`

### 6) Retry and self-healing loops are built in

Evidence:
- Every 10 seconds it checks whether embedding failed and retries by closing/reopening taskbar window.
- Tracks retry count and logs warning/error after repeated failures.

Key lines:
- `TrafficMonitor/TrafficMonitorDlg.cpp:1689-1703`

### 7) Update cadence is timer-driven (multiple loops)

Evidence:
- Main timer: 1000ms.
- Monitor timer: configurable `monitor_time_span`.
- Taskbar-adjust timer in main dialog: 100ms.
- Taskbar dialog timer: 1000ms (recompute width/reflow).

Key lines:
- `TrafficMonitor/TrafficMonitorDlg.cpp:1113-1115`
- `TrafficMonitor/TrafficMonitorDlg.cpp:1142`
- `TrafficMonitor/TrafficMonitorDlg.cpp:1928-1968`
- `TrafficMonitor/TaskBarDlg.cpp:1037`
- `TrafficMonitor/TaskBarDlg.cpp:1233-1249`

### 8) Sleep/wake recovery is handled

Evidence:
- On resume from suspend, it re-initializes network connections via delayed/retry timer (10s interval, up to 20 checks).

Key lines:
- `TrafficMonitor/TrafficMonitorDlg.cpp:3023-3052`

## Integration implications

- `ITaskbarList3`-only approach was incorrect for TrafficMonitor parity.
- “Do NOT review source code” was incorrect for this task; source review is required to extract actual mechanism.
- Overlay/progress APIs are still useful for lightweight indicators, but they are **not** how TrafficMonitor renders dense realtime taskbar text metrics.

## JiStats Recommendation (v1.0 Taskbar Spike)

Implement a `TaskbarSurface` spike using the same architectural pattern:

1. Create a dedicated taskbar surface window class (not just tray icon overlays).
2. Embed with `SetParent(...)` into detected taskbar parent.
3. Implement OS-specific placement strategies (Win11 vs classic fallback).
4. Handle `TaskbarCreated` and recreate surface after Explorer restart.
5. Add retry/fallback path:
   - Retry open/attach on failure.
   - If repeated failure, degrade to tray-only indicator + dashboard.

## JiStats implementation

For the implementation that followed this research, see [TaskbarPresenter](../../src/taskbar/TaskbarPresenter.cpp), the [taskbar reference](presentation.md#taskbar), and the [Windows desktop validation record](../validation/desktop.md#recorded-coverage).

## Primary Source Links

- TrafficMonitor repository: https://github.com/zhongyang219/TrafficMonitor
- Taskbar window creation + class selection:
  - https://github.com/zhongyang219/TrafficMonitor/blob/7901ce07727ee2b220dc5666f934fe69be03f3af/TrafficMonitor/TrafficMonitorDlg.cpp#L566-L575
- Taskbar embedding via `SetParent(...)`:
  - https://github.com/zhongyang219/TrafficMonitor/blob/7901ce07727ee2b220dc5666f934fe69be03f3af/TrafficMonitor/TaskBarDlg.cpp#L999
- Classic taskbar handles (`ReBarWindow32`, `MSTaskSwWClass`, `MSTaskListWClass`):
  - https://github.com/zhongyang219/TrafficMonitor/blob/7901ce07727ee2b220dc5666f934fe69be03f3af/TrafficMonitor/ClassicalTaskbarDlg.cpp#L72-L77
- Win11 taskbar anchors (`TrayNotifyWnd`, `Start`):
  - https://github.com/zhongyang219/TrafficMonitor/blob/7901ce07727ee2b220dc5666f934fe69be03f3af/TrafficMonitor/Win11TaskbarDlg.cpp#L91-L92
- Explorer restart handling (`TaskbarCreated` + reopen):
  - https://github.com/zhongyang219/TrafficMonitor/blob/7901ce07727ee2b220dc5666f934fe69be03f3af/TrafficMonitor/TrafficMonitorDlg.cpp#L31
  - https://github.com/zhongyang219/TrafficMonitor/blob/7901ce07727ee2b220dc5666f934fe69be03f3af/TrafficMonitor/TrafficMonitorDlg.cpp#L2537-L2553
- Timer cadence (`MAIN_TIMER`, `MONITOR_TIMER`, `TASKBAR_TIMER`):
  - https://github.com/zhongyang219/TrafficMonitor/blob/7901ce07727ee2b220dc5666f934fe69be03f3af/TrafficMonitor/TrafficMonitorDlg.cpp#L1113-L1115
  - https://github.com/zhongyang219/TrafficMonitor/blob/7901ce07727ee2b220dc5666f934fe69be03f3af/TrafficMonitor/TrafficMonitorDlg.cpp#L1142
  - https://github.com/zhongyang219/TrafficMonitor/blob/7901ce07727ee2b220dc5666f934fe69be03f3af/TrafficMonitor/TaskBarDlg.cpp#L1037
