# Windows desktop validation

Use a Windows 11 desktop session and a build of the candidate. Record the revision and build, display layout, taskbar theme and transparency settings, adjacent utilities, provider surfaces exercised, and the observed result for each selected scenario.

## Dashboard and background checks

- Launch normally and with `--background`. Check that the tray opens the dashboard and that closing the dashboard leaves the app available.
- Test **Start with Windows** with the packaged build in an isolated Windows profile. Enable the setting and exit JiStats. Move the disposable test database and any companion files aside, then sign out and back in without launching the app manually. Verify that the tray appears and the new database passes the [fresh packaged database check](#fresh-packaged-database). Repeat with a disposable version 12 database and verify that it reaches schema version 17 after sign-in.
- Hold a WAL read transaction open on a disposable version 12 database while launching JiStats. Verify that the dashboard and tray show Starting before releasing the reader and that storage then upgrades to schema version 17. Repeat with a subscription enabled, no saved quota, and a delayed provider response. Confirm that the tray opens the dashboard during a background launch, the first completed refresh publishes the correct status, and the [release timing targets](../release-checklist.md#recorded-verification) are recorded on Windows 11.
- Enable several subscriptions. Compare each Overview remaining meter, reset time, and update time with its provider. Include paired windows, a stale reading, an account without quota, and a provider with local activity only. Select an account in Provider Details and check that Overview still shows every enabled subscription. Compare the provider table’s used percentages with the same quota windows.
- Resize the dashboard to a compact window and scroll Overview to the last chart using the scrollbar, mouse wheel, and Page Up or Page Down. Check that the navigation and footer stay in view and that model charts stack on narrow windows. Repeat on a high-DPI display and with Windows contrast themes if available.
- Focus the dashboard tabs and use Left and Right to switch between Overview and Provider Details. From both views, open History and Settings with the keyboard and confirm that each opens its own window.
- In Settings, press Tab and Shift+Tab to cycle through the selected page and Close in both directions. Use the arrow keys on the tab strip to select OpenAI, then reach both tracking and Codex attribution and toggle each with Space. On MiniMax, reach its tracking checkbox, plan selector, account and credential fields, and enabled action buttons. Verify that hidden controls on other pages and disabled controls on the current page are skipped. Start MiniMax validation from the focused Validate button and check that focus moves to Close and Tab still works while validation is pending.
- Open Settings with the dashboard on a 100% display, then move Settings to a 200% display and back. Check the tabs, General checkboxes and export fields, MiniMax inputs and action buttons, and Close for readable text, adequate space, and keyboard access. Close Settings and reopen it from the dashboard on the 200% display to check the initial layout there. With a short work area, use the scrollbar arrows and drag its thumb to reach Last Run on the General page. Confirm that Close stays in view. Move back to the roomier display and confirm Settings returns to its usual size.
- Save the dashboard at its minimum size at 100% scaling. Exit, change the same display to 200% scaling, and relaunch. Use a display with enough physical space for the minimum at 200%. Check that the tabs and header actions fit. Repeat on a display with a work area shorter than the scaled minimum. Compare the restored window bounds to the work area and verify the footer and header actions remain visible.
- Enable a Windows contrast theme and show several model series, including a series with one point. Match each trace and isolated point to its legend by the shared marker shape. Include two short traces that start at the same point and diverge within 32 DIPs, alongside a longer series. Verify both shape identifiers are visible. Hover a marker displaced by a nearby point and check that its tooltip shows the marker’s model, token total, and original bucket. Hover a sample in the unpainted corner of a nearby circle’s bounding box and check that the sample still supplies the tooltip. Repeat with a standard theme.
- Check the offline notice, its retry and dismiss actions, and the persistent status through recovery. Verify that Analysis range controls reflect the effective persisted scope and that manual export uses that scope when relevant.
- For MiniMax Settings, exercise the [credential scenarios](../providers/minimax.md#credential-behavior-to-preserve).

### Fresh packaged database

In a separate isolated Windows profile with no existing JiStats database, launch the packaged executable with a working directory outside the portable folder and without an external migration directory. Leave all integrations disabled so provider activity cannot create the Kimi row. Exit JiStats and open the new `%LOCALAPPDATA%\JiStats\jistats.db` read-only in a SQLite client. Run:

```sql
SELECT
    (SELECT MAX(version) FROM schema_version) AS schema_version,
    EXISTS(SELECT 1 FROM sqlite_schema WHERE type = 'table' AND name = 'export_runs') AS has_export_runs,
    EXISTS(SELECT 1 FROM providers WHERE provider_key = 'kimi') AS has_kimi_provider;
```

Record all three results. The schema version must be `17`, and both existence checks must be `1`.

## Taskbar checks

- Enable, disable, and re-enable the overlay. Check compact width, readable text, color bands and low-quota emphasis, and placement around the clock, notification area, and TrafficMonitor if present.
- Compare OpenAI, MiniMax, and Kimi subscription rows with the corresponding dashboard quota. Check dual-window formatting where data exists. With local activity only or no eligible quota, verify the overlay has no rows even in a degraded runtime state. Check that the dashboard and tray continue to show that state.
- Check hover text, repeated right-click and dismiss, and hover recovery. Verify the overlay returns to its tray-relative position after Explorer restarts, even while quota data is unchanged. Include resizing, auto-hide, or monitor changes when they affect the change under test.
- Use the [taskbar shell signoff script](../../scripts/run_taskbar_shell_signoff.ps1) for repeated menu/click-away and Explorer restart checks in the desktop session. Use [controller](../../tests/test_app_controller.cpp) and [presenter](../../tests/test_taskbar_presenter.cpp) tests for deterministic row filtering, formatting, placement, and state transitions.

## Recorded coverage

| Date | Observed |
| --- | --- |
| Earlier toggle smoke | Repeated overlay off/on, notification-area clearance, and TrafficMonitor coexistence. |
| 2026-04-20 | Live dark-theme display pass with transparency, OpenAI dual-window and MiniMax single-window rows, and TrafficMonitor placement. Low-quota styling, Kimi quota, and Claude activity were outside this pass. |
| 2026-04-21 | Scripted Release shell pass covered three right-click/click-away cycles and Explorer restart recovery. The focused `[taskbar]` suite recorded 230 assertions across 27 cases. Hover suppression and recovery rely on focused test coverage. |

See the [archived taskbar record](https://github.com/jatmn/JiStats/blob/fd934179646f130fe346a20fb1ca7d82162902f6/docs/taskbar-manual-validation.md) for the original evidence matrix.
