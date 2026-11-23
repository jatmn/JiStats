# OpenAI and Codex

OpenAI subscription tracking is opt-in. Disabling it stops new rollout quota and throughput collection and preserves persisted history. The setting rehydrates before startup refresh. Enabling schedules a refresh during the running session. Manual ChatGPT export import and local Codex SQLite model attribution are separate paths. Keep late in-flight results from overriding a newly disabled state. Use the [provider-path tests](../../tests/test_provider_data_path.cpp) and [controller tests](../../tests/test_app_controller.cpp) when changing activation or startup behavior.

## Sources and boundaries

| Source | What JiStats uses it for |
| --- | --- |
| Local observation | Activity observed on this computer and persisted using a synthetic local account when no provider identity is available. |
| Codex rollout JSONL | Codex-specific quota pressure from `token_count` / `rate_limits`, plus 5-minute and 30-minute token throughput from persisted rollout activity. |
| Codex `state_5.sqlite` | Read-only, sanitized thread metadata for dashboard model attribution after a first-scan baseline. |
| Extracted ChatGPT export | Historical activity from user-authored messages and a record of the import. |
| OpenAI API platform | Separate optional organization usage/cost validation with separate credentials and billing. |

Quota from [OpenAIAdapter](../../src/providers/openai/OpenAIAdapter.cpp) uses rollout files under `CODEX_HOME/sessions` or the user profile's `.codex/sessions` and the `openai_subscription_codex_local` surface. The percentage scale expresses Codex pressure. General ChatGPT subscription balances are unknown. Keep provider source identity explicit. The source-label contract distinguishes `openai_subscription_local`, `openai_subscription_codex_local`, `openai_subscription_export_import`, and `openai_api_import`. Label the last source as OpenAI API Platform in the UI and reports to distinguish it from subscription data. Keep labels explicit even when a metric has no quota value. Use official exports and validated local artifacts for subscription evidence. Exclude website scraping, captured browser bearer tokens, and undocumented consumer endpoints.

The [throughput query](../../src/reporting/AnalysisQueryEngine.cpp) uses recorded request tokens for both totals and the minute chart. Cumulative counters contribute the portion of each interval beyond recorded requests. Subsecond timestamps order requests and counters within a rollout. When replay finds a request and its previously saved cumulative counter on the same line, it links the two so the request only offsets the interval ending at that counter. Replay also restores fractional timestamps on saved counters that a recorded response supersedes. JiStats also saves the rollout line on each counter. Line order can place whole-second readings when it agrees with the fractional timestamps. When the line is unavailable, JiStats combines possible boundary readings across successive unordered seconds and their surrounding counters. It uses the observed counter range as evidence and chooses the combined sequence with the fewest resets and then the least growth. Each distinct reading splits the corresponding rise or drop before JiStats counts estimated prompts and assigns growth to its observation minute. The nearest timed counters bound which unlinked requests can offset each split. These totals remain estimates. JiStats scans recently updated files, including resumed sessions in older directories, and averages rates over the full window. Quota responses carry parsed values only. These events are separate from subscription reports, recovery, and general usage rollups.

### Codex SQLite privacy boundary

Select **Enable Codex model attribution** in **File → Settings → OpenAI** to let JiStats read the local Codex state database. This setting defaults to off and is independent of subscription tracking. When enabled, the [app controller](../../src/app/AppController.cpp) checks for `state_5.sqlite` under `CODEX_HOME`, then `.codex` under the user profile or home directory, at startup and during Analysis data refreshes.

The [read-only SQLite reader](../../src/providers/openai/CodexLocalSqliteReader.cpp) limits its `threads` query to `id`, `model_provider`, `model`, `reasoning_effort`, `tokens_used`, `created_at_ms`, and `updated_at_ms`. Apply the same allowlist to fixtures. Exclude `title`, `cwd`, prompts, messages, conversation bodies, authentication and configuration data, API keys, and bearer tokens from ingestion, persistence, and fixtures.

On the first scan of each opt-in period, the [importer](../../src/providers/openai/CodexLocalUsageImporter.cpp) saves source and thread baselines in the JiStats database. Later scans persist eligible usage deltas and data for new OpenAI and HICAP threads. Disabling the setting stops source checks and preserves imported history. Re-enabling starts a fresh baseline. The freshness check considers WAL and SHM sidecars. Overview uses 5-minute model buckets from this source.

## Import ChatGPT history

Extract the official export. In **File → Import OpenAI Export…**, select the extracted `conversations.json`. Keep at least one recognized companion file beside it (`chat.html`, `message_feedback.json`, or `user.json`). The [importer](../../src/providers/openai/OpenAIExportImport.cpp) accepts the extracted directory or its `conversations.json`. The app picker selects the JSON file. Raw ZIP import is deferred.

The importer records timestamped user messages as historical activity. Live quota, reset times, and exact token or cost totals come from their own sources. Use the [export fixture and redaction note](../fixtures/openai/chatgpt-export-derived-fixture-note.md) and the [augmentation fixture checklist](../fixtures/openai/subscription-augmentation-fixture-guide.md) when changing the parser or import flow.

## Organization usage and costs

The optional [API-platform validation path](openai-api-platform.md) targets organization usage and costs. Its first live probe reached both endpoints and failed authorization on the required usage scope. See the [roadmap checklist](../plan.md#blocked-backlog) for the dependency.
