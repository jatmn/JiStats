# JiStats roadmap

JiStats is a local-first Windows 11 utility for answering what you are using, what you used over time, and how close you are to a provider quota. The [work tracker](progress.md) owns open tasks. The [release checklist](release-checklist.md) owns release signoff.

## V1.0 scope

- Keep the native C++17 desktop app usable as a portable release with a small background footprint. Support optional Windows startup, persisted state, recovery after downtime, and a low-noise offline notice with retry.
- Ship four opt-in integrations. OpenAI / Codex is the primary proof path and tracks subscription activity first. MiniMax focuses on Coding Plan and Token Plan quota. Claude Code uses bounded local evidence. Kimi Code combines bounded local evidence with subscription quota when valid OAuth is available. The [provider matrix](capability-matrix.md) records the supported signals.
- Show current usage and quota in the dashboard, status in the tray, and compact subscription quota in the taskbar. Keep throughput and model attribution in dashboard and Analysis views, with optional throughput context in the OpenAI taskbar tooltip.
- Keep source, freshness, and confidence visible. Preserve useful history in SQLite, identify gaps, and reconcile with provider data where available.
- Use the dedicated Analysis workspace to inspect history, trends, provider/account breakdowns, and coverage. Manual and scheduled HTML reports use the shared analysis scope.
- Keep collection local and minimize monitoring overhead and cost. Prefer direct or local evidence, with the existing [privacy rules](../AGENTS.md#provider-and-privacy-boundaries) and [runtime policy](reference/runtime.md).

V1.0 excludes cloud sync, multi-user collaboration, a plugin system, and broad provider expansion. General ChatGPT subscription quotas and Claude provider-exact quotas require validated signals. OpenAI organization usage/cost import and expanded MiniMax PAYG support remain separate follow-ons. Windows Task Scheduler integration, richer forecasting, and a report designer are outside the first release.

## Architecture

| Area | Responsibilities |
| --- | --- |
| [App controller](../src/app/AppController.cpp) | Startup, lifecycle, refresh coordination, and projection into dashboard, tray, and taskbar views |
| [Provider adapters](../src/providers/) and [local observation](../src/observation/) | Provider-specific collection and bounded local ingestion |
| [Storage](reference/storage.md) | Provider/account/plan identity, events and quota snapshots, migrations, recovery state, and rollups |
| [Analytics](../src/analytics/) | Provider-neutral quota reconciliation and derived usage metrics |
| [Desktop presentation](reference/presentation.md) and [reporting](reference/reporting.md) | Native UI, scoped queries, and HTML export |

Keep blocking network and storage work off the UI thread. Pass provider information through shared models with explicit source identity. Keep support for multiple account and plan identities in the model. Local-only evidence can use synthetic bindings where the source lacks a trustworthy account identity.

## Milestones

| Milestone | Scope |
| --- | --- |
| V1.0 | The feasibility, foundation, telemetry, desktop, taskbar, import, reporting, and Claude/Kimi implementation phases are closed. The release checklist tracks remaining signoff. |
| V1.x | Improve reliability and Analysis, validate broader provider-backed signals for existing integrations, and evaluate advanced import and PAYG paths. |
| Later | Consider Kilo Code and broader provider, account, and plan coverage after the first four integrations stabilize. |
