# Scheduled Export Decision

**Date**: 2026-04-19
**Status**: Decided - v1.0 Implementation Selected

## Decision Summary

For v1.0, JiStats will use an **internal timer-based approach** for scheduled HTML exports rather than integrating with Windows Task Scheduler. This decision prioritizes simplicity, portability, and minimal runtime overhead.

## Options Considered

### Option 1: Windows Task Scheduler Integration
- **Pros**: Native Windows scheduling, survives app restart, familiar UX for power users
- **Cons**: Complex COM APIs, requires elevation for some operations, harder to test, adds Windows-specific coupling

### Option 2: Internal Timer-Based Approach
- **Pros**: Simple implementation, portable across Windows versions, self-contained, easy to test
- **Cons**: Export runs only when app is running, requires app to be active for scheduled export

## Selected Approach: Internal Timer (v1.0)

For v1.0, an internal timer-based approach will be implemented:

1. **Simple internal timer** in the app that triggers exports at configured intervals
2. **Configuration** stored in `DashboardPreferences` struct (schedule interval, export path)
3. **No background service** - export runs when app is running
4. **Minimal seam** - first implementation hooks into existing `HtmlExporter` without bloating runtime

### Future Enhancement (post-v1.0)
- Consider Windows Task Scheduler integration for the case where user wants exports even when app is not running
- This would be implemented as an optional feature toggle

## Implementation Notes

- Add `scheduledExportEnabled`, `scheduledExportIntervalMinutes`, and `scheduledExportPath` fields to `DashboardPreferences`
- Create `ScheduledExportService` class to manage the timer and trigger exports
- Use existing `HtmlExporter::exportToFile()` as the export mechanism
- Log export events using existing logging infrastructure

## References

- Phase 6 goals: `docs/plan.md` Section 6
- HTML Export existing implementation: `src/reporting/HtmlExporter.cpp`
