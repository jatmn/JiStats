# JiStats work tracker

Record active work, blockers, decisions, and handoffs here. The [roadmap](plan.md) defines scope. The [release checklist](release-checklist.md) records signoff and dated evidence.

Use [synthetic or redacted fixtures](fixtures/README.md) and keep raw provider captures local. See the [security policy](../SECURITY.md) for private vulnerability reports.

The [pull request lint workflow](../.github/workflows/lint.yml) checks changed C and C++ lines and validates changed GitHub Actions definitions. See the [verification guide](validation/testing.md#pull-request-lint).

## Active work

Follow the [release queue](release-checklist.md#evidence-backed-release-queue) and use the [SQLite quality gate](validation/sqlite-quality-gate.md) for automated build and test results. Phases 0–7 are closed for the current implementation scope.

Verify the packaged SQLite baseline, historical version 12 repair, and the older-database warning in the Windows release-candidate checks. The [release queue](release-checklist.md#evidence-backed-release-queue) tracks that signoff.

The local [Codex SQLite import](providers/openai.md#codex-sqlite-privacy-boundary) can run at startup with OpenAI subscription tracking off. The setting behavior remains an open follow-up under the opt-in policy.

Confirm that [Pullfrog automatic reviews](../CONTRIBUTING.md#pullfrog) run while an owner-authored PR is still a draft.

## Handoffs

Update the tracker when work changes state. At handoff, record what changed, the revision, the verification commands and results, and any blocker or next step. Keep dated release evidence in the release checklist. Record new decisions with their rationale and link to the current contract.

- Stopped committing third-party sources and tools. The checked-in SQLite 3.53.0, spdlog 1.15.1 and Catch2 3.7.1 files and the UPX 5.1.1 Windows binary and source archive were byte-identical to their upstream releases (spdlog's license differed only in line endings). CMake now downloads the same pinned releases with SHA-256 verification, Catch2 only for test builds, and UPX is found on `PATH` or through `JISTATS_UPX_EXECUTABLE`. The [notices](../THIRD_PARTY_NOTICES.md) now carry the spdlog license text that release binaries need. Native Linux tests passed 125 cases and 920 assertions with fetched and system SQLite, and the MinGW-w64 Debug cross-build produced all three executables with the same compile and link flags as before. The minimum CMake version is now 3.20, matching the README, because populating dependencies without their upstream builds needs CMake 3.18. The C/C++ CodeQL job configures CMake so the scan can resolve the Catch2 headers, and a local buildless scan with CodeQL 2.27.5 found no missing third-party includes and only first-party files. See [third-party components](../README.md#-third-party-components).

- Completed the CodeQL cutover after GitHub-managed default setup scanned Markdown-only PR #20. Disabled default setup and removed `CODEQL_UPLOAD_DISABLED`. A manual scan on `eba30e2` published C/C++, JavaScript/TypeScript, and GitHub Actions results through the repository workflow. [CodeQL analysis run](https://github.com/jatmn/JiStats/actions/runs/45069746335)

- Audited the documentation against main at `eba30e2`. Updated the quick start, Codex SQLite behavior, build and CI guidance, and release evidence. The [Linux CMake/Ninja build](../README.md#building-on-linux) passed CTest with bundled SQLite, covering 125 cases and 920 assertions. The Codex SQLite setting behavior remains open.

- Fixed an intermittent shutdown hang seen in the [Windows test stack trace](https://github.com/jatmn/JiStats/actions/runs/45066218501/job/137343424464). AppController now sets the stop flag while holding both worker wait mutexes before waking and joining the workers. A controlled shutdown interleaving reproduced the missed wakeup and verified the fix.

- Updated [CI build detection](validation/testing.md#continuous-integration) to skip native builds for documentation-only PRs and reuse Windows validation for documentation-only pushes with a successful previous push on the same branch. Verified the workflow with `actionlint -color` and exercised its Bash filter against local Git histories and prior push results, including failure, cancellation, and missing proof. The hosted PR run exercises both builds for the workflow change.

- Removed the AI credits section from the [README](../README.md) in [PR #17](https://github.com/jatmn/JiStats/pull/17). Verified 12 local links and anchors. `git diff --check 3bee5a1` passed.

- Switched the [formatting check](validation/testing.md#pull-request-lint) to Git and native LLVM 18 against base `252209f`. Ran `actionlint -color` and exercised `bash scripts/check_changed_format.sh <base-commit>` on local Git histories with clean edits, formatting errors, and deletions.

- Added [repository-managed CodeQL scanning](validation/testing.md#codeql-scanning) after [GitHub's managed scan](https://github.com/jatmn/JiStats/actions/runs/45060965031) ran on docs-only PR #17 (`e18eb23`). The local Git replay skipped analysis for the PR and its merge push (`252209f`) and retained analysis for code, workflows, and fixture payloads.

- Paused uploads from the replacement workflow in [PR #18](https://github.com/jatmn/JiStats/pull/18) after GitHub rejected them with default setup enabled. Default setup published results while the replacement ran with uploads paused.

## Current decisions

| Decision | Where it applies |
| --- | --- |
| Codex SQLite stays local and supplies model attribution. Rollouts supply Codex quota. | [OpenAI sources and privacy](providers/openai.md#sources-and-boundaries) |
| The v1.0 release uses a portable bundle. | [Portable launch and upgrade](quick-start.md#launch) |

## Recently completed

- [PR #5](https://github.com/jatmn/JiStats/pull/5) removed the dormant Dear ImGui build path and trimmed spdlog to its headers and license. [Integrated CI](https://github.com/jatmn/JiStats/actions/runs/45068074135) built the trimmed checkout and passed the full Windows suite.
- [PR #6](https://github.com/jatmn/JiStats/pull/6) consolidated new databases onto schema version 12, added in-place repair for both historical version 12 layouts, and added the startup warning for rejected versions 1–11. Concurrent startup rechecks the version 12 schema inside the write transaction. See the [storage reference](reference/storage.md#migration-behavior).
- [PR #16](https://github.com/jatmn/JiStats/pull/16) received automatic Pullfrog reviews on multiple heads after it was marked ready. The [contributor guide](../CONTRIBUTING.md#pullfrog) describes the authorization path.

- Linux developers can run the portable Catch2 suite with CMake and Ninja. MinGW-w64 builds the Windows release binaries and powers CI for code PRs, protected-branch pushes, and manual runs. The full suite executes on a hosted Windows runner. See the [build instructions](../README.md#building-on-linux) and [CI verification guide](validation/testing.md#continuous-integration).

- Organized the documentation around the [index](README.md) and consolidated duplicate provider, runtime, and UI contracts.
- Audited provider-fed data paths, Codex throughput and SQLite import, Analysis, and fixtures against the source. The audit below records what landed and the remaining verification boundaries.
- Completed the dedicated Analysis workspace and shared export scope, the low-resource tray/taskbar work, and the portable packaging baseline. Reopen any of these lanes when new evidence shows a concrete regression.

- Stabilized the [test suite](https://github.com/jatmn/JiStats/pull/7) with shared test fixtures, controlled provider responses, an isolated startup registry, and an isolated version 9 database for the pre-baseline Analysis upgrade test. Offline dismissal and provider refresh are coordinated without waiting on provider requests. The [runtime reference](reference/runtime.md#offline-and-catch-up-behavior) describes the resulting behavior.
- Added [cost-aware CI and Linux cross-builds](https://github.com/jatmn/JiStats/pull/11), including host UPX discovery, resource paths with spaces, and shared SQLite deployment guidance. See the [Linux build instructions](../README.md#building-on-linux).
- [Consolidated PowerShell script logic](https://github.com/jatmn/JiStats/pull/3) for provider credentials and capture directories, clang source discovery, and taskbar button clicks. Comparisons against the previous scripts matched using synthetic provider responses and simulated clang and Windows shell calls.

## Blocked backlog

- `A0.2d` remains blocked: the first live OpenAI API-platform probe reached both organization endpoints, but the tested key returned `403` with `Missing scopes: api.usage.read`.
- A successful freshness/lag run requires a credential with `api.usage.read`, the correct organization-scoped role, and explicit org context when needed.

## Deferred and follow-up backlog

| Status | Item | Target | Notes |
| --- | --- | --- | --- |
| `⏸️ DEFERRED` | Expand OpenAI API-platform import beyond the initial validation pass | `v1.x` refinement | Keep richer importer UX and broader endpoint coverage separate from the subscription-first OpenAI story. Revisit the adapter's 0–100 quota representation when a new quota surface needs richer fields, and extend provider-surface population beyond the current Codex-local quota path when additional OpenAI sources land. |
| `⏸️ DEFERRED` | Extend OpenAI subscription fixtures and export inputs | `v1.x` refinement | A redacted consumer-dashboard capture is still optional. Raw `.zip` handling and its fixture coverage, and a UI picker for an extracted export directory, are future work. |
| `⏸️ DEFERRED` | Refine the Analysis workspace | `v1.x` refinement | Candidate slices include interactive drill-down, richer scope presets, comparisons, and export preview. The [archived design record](https://github.com/jatmn/JiStats/blob/fd934179646f130fe346a20fb1ca7d82162902f6/docs/in-app-reporting-and-analysis-spike.md) preserves the rationale and acceptance questions. |
| `⏸️ DEFERRED` | Extend MiniMax PAYG support beyond the first real-surface validation pass | `v1.x` refinement | Keep broader PAYG runtime UX and scheduled polling behind the current subscription-first v1.0 baseline. |
| `⏸️ DEFERRED` | Decide post-v1.x provider expansion order beyond the four-provider v1.0 baseline | Post-v1.x | Kilo Code and broader provider expansion stay deferred until OpenAI + MiniMax + Claude + Kimi ship and stabilize. |
| `⚪ TODO` | Redact Codex rollout raw response persistence | Privacy hardening | `OpenAIAdapter` still forwards full local rollout JSONL/file content through `snapshot.rawResponse` / `QuotaResponse::rawResponse`. Replace with parsed quota fields plus minimal sanitized provenance so local Codex session metadata cannot leak into persisted diagnostics or UI/reporting paths. |
| `⚪ TODO` | Hydrate latest quota snapshots per surface/account dimension | Correctness hardening | Startup quota hydration currently selects the provider-level freshest `snapshot_time_utc`, which can drop older-but-current plan/window/account rows when multiple surfaces update at different times. Query latest rows per provider + plan/surface/account-equivalent dimension instead of provider alone. |
| `⚪ TODO` | Consolidate Codex rollout discovery and watcher matching | Code quality | `OpenAIAdapter` and `AppController` each resolve `CODEX_HOME` / `%USERPROFILE%` / `$HOME` and match `rollout-*.jsonl`. Move path resolution and file matching into one shared Codex local helper so watcher/import behavior cannot drift. |
| `⚪ TODO` | Centralize provider-surface/window/confidence labels | Code quality | Dashboard and provider presentation logic still depend on string literals such as `openai_subscription_codex_local`, `kimi_subscription_api`, `weekly`, and confidence labels. Add shared constants or typed mapping helpers before adding more provider surfaces. |
| `⚪ TODO` | Factor shared Dashboard async-action plumbing | UI maintainability | OpenAI import, HTML export, MiniMax actions, offline retry, and offline dismiss use similar thread/in-flight/post-message/completion plumbing. Extract a small helper to reduce copy-paste once release-critical UI behavior is stable. |
| `⚪ TODO` | Refine Codex rollout diagnostics | UX polish | Some live parse errors still say "fixture", and malformed and unsupported files share the top-level error. Preserve the parser detail while making live-source wording and failure distinctions clearer. |
| `⏸️ DEFERRED` | Exercise the real Codex rollout path through AppController | Test follow-up | Use a temporary rollout directory to cover quota fetch, failure, and duplicate-safe token import across application refreshes. Existing controller tests inject provider responses and disable live rollout token import. |
| `⏸️ DEFERRED` | Review Codex import durability | Storage follow-up | Assess database-level rollout deduplication and whether SQLite delta writes and thread checkpoints should share a transaction. Add targeted retry and counter-reset coverage if this work is picked up. |
| `⏸️ DEFERRED` | Run a live Codex SQLite smoke pass | Windows validation | Launch with existing thread totals, check that first scan produces no new chart usage, generate new activity, and confirm OpenAI/HICAP model deltas and unchanged source data. Cover a WAL-only refresh where possible. |
| `⏸️ DEFERRED` | Add a 5-hour Codex throughput window | `v1.x` refinement | The original throughput plan proposed 5-minute, 30-minute, and 5-hour windows. The dashboard and Analysis compute only the 5-minute and 30-minute rates. |
| `⏸️ DEFERRED` | Extract shared test polling helpers | Test cleanup | The suite shares temporary database and directory fixtures. Presenter and controller polling helpers still use different timeouts. |
| `⏸️ DEFERRED` | Extract shared GUI formatting/conversion helpers | UI cleanup | `DashboardPresenter`, `AnalysisPresenter`, and taskbar presenter repeat UTF-8/wide and number/time formatting helpers. Consolidate after chart/reporting surfaces stabilize. |

## Plan audit

Reviewed the four superseded plans and related Analysis and fixture plans against the source, tests, and recorded validation at `710a9dd`.

| Plan | Landed | Open, deferred, or superseded |
| --- | --- | --- |
| Provider-fed data path | [AppController](../src/app/AppController.cpp) registers production adapters and routes quota through ProviderManager. [OpenAIAdapter](../src/providers/openai/OpenAIAdapter.cpp) uses local Codex rollouts and reports missing, empty, and invalid input. [Adapter fixture tests](../tests/openai_fixture_tests.cpp) exercise successful and failed quota fetches. [Provider-path tests](../tests/test_provider_data_path.cpp) exercise application routing with injected adapters. | The optional real-adapter controller fixture and finer parser diagnostics are tracked below. Full provider-history fetch during startup remains outside this slice. |
| Codex rollout throughput and isolation correction | AppController stores timestamped, deduplicated `codex_local_rollout` events. [AnalysisQueryEngine](../src/reporting/AnalysisQueryEngine.cpp) uses persisted events and computes token deltas within each rollout. The subscription report, recovery, and rollup paths exclude these events. The dashboard and Analysis show 5m/30m rates, with [query](../tests/test_analysis_query_engine.cpp) and [rollup](../tests/test_storage_usage_rollups.cpp) coverage. | The direct-file query workaround is superseded. The proposed 5h rate and standalone taskbar gauges remain deferred. The OpenAI hover tooltip can show throughput context. Direct controller import coverage and database-level deduplication remain optional follow-ups. |
| Codex SQLite usage by model | [The SQLite baseline](../db_migrations/012_baseline_schema.sql), the [read-only reader](../src/providers/openai/CodexLocalSqliteReader.cpp), and the [importer](../src/providers/openai/CodexLocalUsageImporter.cpp) are wired into the app. First scan establishes a baseline. Later scans import deltas and eligible new threads for OpenAI and HICAP. The import freshness check includes WAL/SHM metadata, and Overview shows 5-minute model buckets from those events. [Importer tests](../tests/test_codex_local_usage_import.cpp) cover baseline, growth, new threads, and repeat scans. | Overview queries this source directly and general usage rollups exclude it. The proposed dedicated sync checkpoint and shared event/state transaction were never added. MiniMax attribution is deferred. Live Windows first-run, refresh, and chart smoke coverage is unverified. |
| Analysis and reporting | The dashboard launches the dedicated Analysis workspace with persisted scope, history, trends, and provenance. Manual and scheduled HTML export use the shared scope. See the [reporting reference](reference/reporting.md). | The initial slice is closed. Further drill-down, scope presets, comparisons, and export preview remain deferred. |
| OpenAI subscription fixtures | The rate-limit sample, redacted rollout, and extracted ChatGPT export fixture are available. [Fixture replay](../tests/openai_fixture_tests.cpp) covers Codex parsing, and [import tests](../tests/test_openai_export_import.cpp) cover extracted export input and raw archive rejection. | Direct archive support and the optional consumer-dashboard capture remain deferred. The [fixture guide](fixtures/openai/subscription-augmentation-fixture-guide.md) records test expectations. |

## Guardrails for follow-up

- Extend Analysis through `AnalysisPresenter` and `AnalysisQueryEngine`. Keep the current dedicated window and shared export scope.
- Keep the taskbar overlay scoped to subscription quota. Codex throughput belongs in dashboard/Analysis and may provide OpenAI hover context. Local activity and SQLite model attribution stay out of the overlay metrics.
- Keep optional OpenAI organization usage/cost validation and expanded MiniMax PAYG work outside the subscription-first release scope.

Earlier task-by-task closeouts, decisions, and benchmarking are available in the [archived tracker](https://github.com/jatmn/JiStats/blob/fd934179646f130fe346a20fb1ca7d82162902f6/docs/progress.md).
