# JiStats documentation

## Use JiStats

- [Portable quick start](quick-start.md) covers launching the app, enabling providers, importing history, and exporting reports.
- [Provider support](capability-matrix.md) shows which integrations supply quota and which supply local activity or historical data.
- [Build and packaging](../README.md#build-and-packaging) covers building from source and assembling portable releases.

## Maintain JiStats

- [Roadmap and work checklist](plan.md), [progress log](progress.md), and [release checklist](release-checklist.md) cover product scope, handoffs, and signoff.
- [Runtime and recovery](reference/runtime.md), [storage](reference/storage.md), [dashboard and taskbar](reference/presentation.md), and [Analysis and reporting](reference/reporting.md) document the shared behavior.
- [TrafficMonitor research](reference/trafficmonitor-research.md) records the taskbar integration patterns JiStats drew on.

## Providers and verification

- [OpenAI and Codex](providers/openai.md), [MiniMax](providers/minimax.md), and [Claude Code and Kimi Code](providers/claude-kimi.md) explain the provider-specific paths.
- [Local redaction rules](providers/local-redaction.md) and the [fixture inventory](fixtures/README.md) cover safe parser inputs and test evidence.
- [Testing](validation/testing.md), the [SQLite quality gate](validation/sqlite-quality-gate.md), and [desktop validation](validation/desktop.md) explain how to verify a change.
- [Provider validation record](validation/provider-evidence-2026-04-21.md) captures dated live checks. [OpenAI API-platform validation](providers/openai-api-platform.md) covers the optional probe.

The [pre-cleanup documentation snapshot](https://github.com/jatmn/JiStats/tree/fd934179646f130fe346a20fb1ca7d82162902f6/docs) contains the original phase plans, design explorations, and detailed closeout records.
