# OpenAI Subscription Tracking Toggle Behavior

> **Status**: Lane 2 support contract for the shipped settings/runtime toggle
> **Purpose**: Define the expected behavior of the shipped `Enable OpenAI subscription tracking/status` setting so future changes preserve the current contract

## Intent

JiStats has an explicit user-controlled switch for the OpenAI subscription runtime path.
It is one of four shipped provider toggles, and none of those providers are assumed active by default.

This note defines what that shipped switch does so future work does not regress its persistence or runtime behavior.

## Current Baseline

The app now persists the OpenAI subscription tracking toggle alongside the other dashboard preferences.

The shipped settings/runtime model includes:

- taskbar overlay enabled
- start with Windows
- start in background
- OpenAI subscription tracking enabled
- MiniMax subscription tracking enabled
- Claude Code local tracking enabled
- Kimi Code local tracking enabled

## Shipped Setting

Recommended checkbox label:

- `Enable OpenAI subscription tracking/status`

Current placement:

- Settings / General, near the other runtime preferences

## Scope Of The Toggle

This toggle is for the OpenAI subscription runtime/status path only.

Fresh-install baseline:

- all four provider integrations start disabled until the user enables them
- enabling OpenAI must not implicitly enable MiniMax, Claude Code, or Kimi Code

It should control:

- automatic OpenAI subscription quota/status refresh
- local Codex rollout discovery used for OpenAI subscription quota state
- new OpenAI subscription-local observation capture and persistence
- OpenAI taskbar/tray/dashboard live status participation

It should not directly control:

- MiniMax tracking
- the taskbar overlay master toggle
- unrelated app startup behavior
- deletion of previously stored OpenAI history

## When Enabled

When the setting is enabled:

- the enabled state must persist in the normal settings store
- OpenAI subscription runtime refresh should participate in the normal app refresh cycle
- dashboard, tray, and taskbar surfaces may show OpenAI live state according to the normal confidence and provider-truth rules
- enabling the toggle from `off` should schedule a fresh OpenAI runtime refresh without requiring an app restart

## When Disabled

When the setting is disabled:

- the disabled state must persist in the normal settings store
- JiStats must stop automatic OpenAI subscription runtime/status refresh work
- JiStats must stop recording new OpenAI subscription-local observations while the toggle remains off
- taskbar and tray surfaces must stop presenting OpenAI as an active live metric source
- the dashboard must stop presenting stale OpenAI live quota/status values as if tracking were still active

Recommended dashboard presentation while disabled:

- OpenAI may be hidden from live provider surfaces entirely until enabled
- if the settings surface shows OpenAI state while disabled, use a simple disabled message such as `Tracking disabled`
- avoid quota-pressure language or remaining percentages while the setting is off

## Persistence And Restart Rules

- the setting must rehydrate on startup before the first provider refresh pass
- restarting the app with the toggle off must keep OpenAI subscription tracking disabled
- restarting with the toggle on must resume the normal OpenAI subscription runtime path

## Existing Data Rules

Disabling the toggle must not silently delete existing stored OpenAI data.

Expected behavior:

- historical OpenAI rows already in storage remain intact
- user-initiated historical import data remains intact
- re-enabling the toggle resumes forward collection only

## Refresh Edge Cases

If the user disables the toggle while an OpenAI refresh is already in flight:

- the UI should settle to the disabled state when that refresh completes
- no follow-on OpenAI refresh should be scheduled from that completion
- the late result must not leave taskbar or dashboard surfaces looking active again

## Non-Goals

This setting does not need to:

- act as a data-deletion control
- merge OpenAI subscription and optional API-import behavior into one unlabeled path
- override the global taskbar overlay preference

## Regression Guardrails

Future changes should preserve this contract:

- keep the checkbox wired through `DashboardPreferences` and persistence
- keep OpenAI runtime refresh and local observation capture gated on the stored preference
- keep OpenAI taskbar/tray/dashboard live output suppressed while disabled
- keep the setting rehydrated and applied before startup refresh work begins
