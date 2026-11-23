# JiStats roadmap

JiStats is a local-first Windows 11 utility for tracking current and historical AI provider activity and supported subscription quota. This page tracks implementation and remaining work. Record verification and handoffs in the [progress log](progress.md), and complete candidate signoff in the [release checklist](release-checklist.md).

## V1.0 scope

Ship a portable native C++17 app with a small background footprint, optional Windows startup, persisted state, downtime recovery, and a low-noise offline notice with retry. Keep provider collection local and opt-in, and prefer direct, low-cost evidence. Show source, freshness, and confidence wherever the evidence supports them. Preserve provider, account, and plan identity in SQLite, with synthetic bindings for local sources that lack reliable account identity.

The dashboard shows quota and activity, the tray shows runtime status, and the taskbar overlay shows subscription quota. Dashboard and Analysis show Codex throughput, and Dashboard Overview includes Codex model attribution. The OpenAI taskbar tooltip can include throughput context. The dedicated Analysis workspace and manual and scheduled HTML reports share provider, account, and date scope.

The in-scope integrations are OpenAI / Codex subscription tracking, MiniMax Coding Plan and Token Plan quota, bounded Claude Code local activity, Kimi Code local activity and subscription quota, and Grok and Cursor subscription quota. See the [provider matrix](capability-matrix.md) and [privacy rules](../AGENTS.md#provider-and-privacy-boundaries).

General ChatGPT and provider-exact Claude quota need validated signals. OpenAI organization usage and cost, expanded MiniMax PAYG, Windows Task Scheduler integration, richer forecasting, a report designer, cloud sync, multi-user collaboration, a plugin system, and broader provider expansion are outside V1.0.

## Implementation audit

Audited against [main at 326dcd4](https://github.com/jatmn/JiStats/tree/326dcd46dd29ee23a01c7204934105d66d24df9a), using production paths, tests, and GitHub run history. Each row describes the implemented baseline and its limits. Windows desktop, live provider, and packaged release results belong to the candidate that was tested. See the [audit handoff](progress.md#roadmap-audit).

| Phase | Implemented | Evidence and limits |
| --- | --- | --- |
| 0 — Feasibility | The initial provider boundaries are reflected in the adapters. Codex reads local rate-limit snapshots, MiniMax queries its subscription surfaces, Claude summarizes bounded local data, and Kimi can fetch subscription quota with usable OAuth. | [Codex fixture tests](../tests/openai_fixture_tests.cpp), [MiniMax adapter tests](../tests/test_minimax_adapter.cpp), [Claude parser tests](../tests/test_claude_local_parser.cpp), and [Kimi adapter tests](../tests/test_kimi_adapter.cpp). These establish behavior on controlled inputs. Organization API freshness is still [blocked](#blocked-backlog). |
| 1 — Local foundation | The app initializes an embedded SQLite v12 baseline with settings, provider/account/plan identity, usage and quota history, and recovery state. Windows startup, credential storage, and local logging have implementations. | [Schema](../db_migrations/012_baseline_schema.sql), [migration tests](../tests/test_schema_manager.cpp), [startup tests](../tests/test_startup_manager.cpp), and [credential tests](../tests/test_windows_credential_store.cpp). Both historical v12 repairs and rejection of older databases have automated coverage. Packaged behavior is open. |
| 2 — Telemetry and analytics | Local observations, quota mirroring, gap classification, usage rollups and retention, and offline/retry state are wired into the runtime. Recorded Codex requests supply 5m/30m throughput, with cumulative deltas for older rollouts. The OpenAI/HICAP model charts use read-only Codex SQLite deltas after an initial baseline. | [Controller](../src/app/AppController.cpp), [mirror tests](../tests/test_mirror_engine.cpp), [rollup tests](../tests/test_storage_usage_rollups.cpp), [Analysis query tests](../tests/test_analysis_query_engine.cpp), and [SQLite importer tests](../tests/test_codex_local_usage_import.cpp). Codex rollout and SQLite usage are excluded from general usage rollups. The SQLite path still bypasses the OpenAI tracking toggle. |
| 3 — Dashboard and tray | The dashboard has settings, quota and activity views, source and freshness labels, and recovery state. The dashboard and tray route retry and dismissal while retaining persistent offline status. | [Dashboard](../src/gui/DashboardPresenter.cpp), [tray](../src/app/TrayController.cpp), and [controller tests](../tests/test_app_controller.cpp). The cached startup view still selects quota rows by one timestamp per provider. |
| 4 — Taskbar | The Windows overlay renders compact OpenAI, MiniMax, and Kimi subscription quota. The OpenAI hover can include throughput context. Local activity stays in the dashboard and Analysis. | [Taskbar presenter](../src/taskbar/TaskbarPresenter.cpp) and [presenter tests](../tests/test_taskbar_presenter.cpp). The [desktop record](validation/desktop.md#recorded-coverage) covers specific shell scenarios and captures earlier results. Candidate desktop verification remains open. |
| 5 — Import and reconciliation | The dashboard imports extracted ChatGPT history. Recovery records gaps, compares available backfill with local evidence, and avoids duplicate writes in the exercised paths. | [Export import tests](../tests/test_openai_export_import.cpp) and [controller recovery tests](../tests/test_app_controller.cpp). The recovery tests inject provider history. The production [OpenAI adapter](../src/providers/openai/OpenAIAdapter.cpp) returns no remote history, and the [MiniMax adapter](../src/providers/minimax/MiniMaxAdapter.cpp) defers it. Gaps without evidence can remain unknown. The GUI selects `conversations.json`, and the parser also accepts an extracted directory. |
| 6 — Analysis and reporting | The dedicated workspace has persisted scope, daily history, breakdowns, quota series selection, comparison summaries, and chart bucket details. Manual and in-app scheduled HTML export use the shared analysis scope. | [Analysis presenter tests](../tests/test_analysis_presenter.cpp), [query tests](../tests/test_analysis_query_engine.cpp), [report tests](../tests/test_reporting_service.cpp), and [controller export tests](../tests/test_app_controller.cpp). Broader Analysis enhancements are deferred. |
| 7 — Claude and Kimi integration | Settings default all four tracking toggles to off. Claude and Kimi activation, bounded local collection, and source labels are wired into the shared app. Kimi has a separate OAuth-backed quota route. | [Controller and settings tests](../tests/test_app_controller.cpp), [provider routing tests](../tests/test_provider_data_path.cpp), and [provider boundaries](providers/claude-kimi.md). The toggle test covers settings defaults. The Codex SQLite exception described above still needs resolution. |
| Packaging and CI | CMake installs the portable app and notices, supports optional UPX packing, and builds the MiniMax developer CLI only when enabled. Linux portable tests and MinGW cross-builds feed the Windows CI suite. | [CMake](../CMakeLists.txt), [install targets](../src/CMakeLists.txt), and [CI workflow](../.github/workflows/ci.yml). Documentation-only changes can reuse earlier CI results. Current [release signoff](release-checklist.md) is open. |

## Architecture

[AppController](../src/app/AppController.cpp) coordinates the lifecycle and refreshes. [Provider adapters](../src/providers/) and [local observation](../src/observation/) collect evidence. [Storage](reference/storage.md) preserves source and identity, and [analytics](../src/analytics/) reconciles quota and derives usage metrics. [Desktop presentation](reference/presentation.md) and [reporting](reference/reporting.md) consume these views.

Keep blocking network and storage work off the UI thread. Extend Analysis through `AnalysisPresenter` and `AnalysisQueryEngine` while preserving the shared export scope. Preserve quota provenance and the taskbar's subscription-quota boundary.

## Remaining work

### V1.0 implementation and signoff

- [ ] **Release blocker.** Resolve the Codex SQLite opt-in gap. Confirm whether the existing OpenAI setting or a separate explicit opt-in controls model attribution. Exercise a fresh profile with `state_5.sqlite` present and verify that startup and refresh leave the source unread and import no deltas until the user opts in. Cover disabling the setting again. [Current import path](../src/app/AppController.cpp#L6480)
- [ ] Preserve the latest cached quota for each provider, account, plan/surface, and window when their snapshot times differ. Seed staggered snapshots, block live refresh, and verify that startup keeps the newest row for each independent dimension. [Current hydration query](../src/app/AppController.cpp#L2555)
- [x] Keep raw Codex rollout payloads out of quota responses. The [Codex fixture tests](../tests/openai_fixture_tests.cpp) verify quota fields and an empty raw response.
- [ ] Complete the [release checklist](release-checklist.md#evidence-backed-release-queue) on the intended candidate, including the actual Windows suite, desktop smoke, and the portable bundle.

### Follow-up reliability and maintenance

- [ ] Exercise real rollout quota, failure handling, and duplicate-safe token persistence together across AppController refreshes. Existing tests cover adapter parsing, a real adapter in an offline scenario, and file watcher refresh with an injected quota response. [Offline adapter test](../tests/test_app_controller_offline_notice.cpp) · [Controller watcher tests](../tests/test_app_controller.cpp)
- [ ] Review Codex import retry behavior. Verify that an interrupted SQLite delta/checkpoint update and a counter reset produce correct totals. Evaluate a database-level rollout identity and a shared SQLite event/checkpoint transaction if the demonstrated failure warrants either. [Usage importer](../src/providers/openai/CodexLocalUsageImporter.cpp) · [Storage](../src/db/StorageService.cpp)
- [ ] Run the live Codex SQLite smoke on Windows. Begin with existing threads, generate new activity, and verify first-scan baselining, OpenAI/HICAP chart deltas, WAL-only refresh where possible, and an unchanged source database. [Reader and import contract](providers/openai.md#codex-sqlite-privacy-boundary)
- [ ] Share Codex rollout directory resolution and file matching between the adapter and AppController watcher. Exercise the same supported home-directory overrides in both paths. [Adapter](../src/providers/openai/OpenAIAdapter.cpp) · [Watcher](../src/app/AppController.cpp)
- [ ] Improve live rollout diagnostics. Use live-source wording and distinguish malformed input from valid input without supported rate limits. [Parser fixture tests](../tests/openai_fixture_tests.cpp)
- [ ] Centralize provider surface, window, and confidence labels used by dashboard and provider presentation before adding more provider surfaces.
- [ ] Evaluate shared dashboard background-action plumbing for import, export, MiniMax actions, retry, and dismissal after release behavior is stable. Preserve the current completion and error behavior.
- [ ] Consolidate repeated test polling and GUI formatting helpers when the affected tests or presenters next need changes. Temporary file and database fixtures are already shared.

### Blocked backlog

- [ ] Complete the optional OpenAI organization usage/cost freshness probe (A0.2d). The [2026-04-19 probe record](providers/openai-api-platform.md) reports HTTP 403 with `Missing scopes: api.usage.read` from both endpoints. A successful rerun requires that scope, an appropriate organization role, and organization context where needed. The subscription-first release can proceed independently.

### Deferred to V1.x or later

- [ ] Evaluate an OpenAI organization usage/cost importer after A0.2d, including endpoint coverage, import UX, source labels, and richer quota fields if future surfaces require them.
- [ ] Extend ChatGPT export input if prioritized. Add ZIP parsing and fixture coverage, and consider an extracted-directory picker alongside the current file picker. A redacted consumer dashboard fixture remains optional.
- [ ] Extend Analysis beyond the current trend inspection, comparisons, and date presets. Evaluate richer scope presets, comparisons or drill-down, and an export preview. [Archived design](https://github.com/jatmn/JiStats/blob/fd934179646f130fe346a20fb1ca7d82162902f6/docs/in-app-reporting-and-analysis-spike.md)
- [ ] Decide whether dashboard and Analysis need a 5-hour Codex throughput rate alongside 5m/30m. Keep taskbar rows scoped to subscription quota.
- [ ] Evaluate broader MiniMax PAYG monitoring, history, and opt-in polling after the subscription-first release. Include MiniMax in Codex SQLite model attribution only if a supported mapping is established.
- [ ] Revisit Kilo Code and broader provider/account/plan coverage after the first four integrations stabilize.
