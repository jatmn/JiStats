# Windows desktop validation

Use a Windows 11 desktop session and a build of the candidate. Record the revision and build, display layout, taskbar theme and transparency settings, adjacent utilities, provider surfaces exercised, and the observed result for each selected scenario.

## Dashboard and background checks

- Launch normally and with `--background`. Check that the tray opens the dashboard and that closing the dashboard leaves the app available.
- Enable the providers being tested. Compare Overview remaining bars and provider-table used percentages with the returned quota windows. Confirm each bar fills as remaining quota increases. Verify paired windows, missing data, and stale state without treating local activity as quota.
- Check the offline notice, its retry and dismiss actions, and the persistent status through recovery. Verify that Analysis range controls reflect the effective persisted scope and that manual export uses that scope when relevant.
- For MiniMax Settings, exercise the [credential scenarios](../providers/minimax.md#credential-behavior-to-preserve).

## Taskbar checks

- Enable, disable, and re-enable the overlay. Check compact width, readable text, color bands and low-quota emphasis, and placement around the clock, notification area, and TrafficMonitor if present.
- Compare OpenAI, MiniMax, and Kimi subscription rows with the corresponding dashboard quota. Check dual-window formatting where data exists. Confirm the overlay is empty when the enabled integrations supply local activity only or have no quota.
- Check hover text, repeated right-click and dismiss, and hover recovery. Verify the overlay returns after Explorer restarts. Include resizing, auto-hide, or monitor changes when they affect the change under test.
- Use the [taskbar shell signoff script](../../scripts/run_taskbar_shell_signoff.ps1) for repeated menu/click-away and Explorer restart checks in the desktop session. Use [controller](../../tests/test_app_controller.cpp) and [presenter](../../tests/test_taskbar_presenter.cpp) tests for deterministic row filtering, formatting, placement, and state transitions.

## Recorded coverage

| Date | Observed |
| --- | --- |
| Earlier toggle smoke | Repeated overlay off/on, notification-area clearance, and TrafficMonitor coexistence. |
| 2026-04-20 | Live dark-theme display pass with transparency, OpenAI dual-window and MiniMax single-window rows, and TrafficMonitor placement. Low-quota styling, Kimi quota, and Claude activity were outside this pass. |
| 2026-04-21 | Scripted Release shell pass covered three right-click/click-away cycles and Explorer restart recovery. The focused `[taskbar]` suite recorded 230 assertions across 27 cases. Hover suppression and recovery rely on focused test coverage. |

See the [archived taskbar record](https://github.com/jatmn/JiStats-public-prep/blob/fd934179646f130fe346a20fb1ca7d82162902f6/docs/taskbar-manual-validation.md) for the original evidence matrix.
