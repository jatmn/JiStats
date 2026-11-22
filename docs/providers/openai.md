# OpenAI and Codex

OpenAI tracking is opt-in and subscription-first. Disabling it stops new subscription collection and preserves persisted history. The setting rehydrates before startup refresh. Enabling schedules a refresh during the running session. Keep late in-flight results from overriding a newly disabled state. Use the [provider-path tests](../../tests/test_provider_data_path.cpp) and [controller tests](../../tests/test_app_controller.cpp) when changing activation or startup behavior.

## Sources and boundaries

| Source | What JiStats uses it for |
| --- | --- |
| Local observation | Activity observed on this computer and persisted using a synthetic local account when no provider identity is available. |
| Codex rollout JSONL | Codex-specific quota pressure from `token_count` / `rate_limits`, plus 5-minute and 30-minute token throughput from persisted rollout activity. |
| Codex `state_5.sqlite` | Read-only, sanitized thread metadata for dashboard model attribution after a first-scan baseline. |
| Extracted ChatGPT export | Historical activity from user-authored messages and a record of the import. |
| OpenAI API platform | Separate optional organization usage/cost validation with separate credentials and billing. |

Quota from [OpenAIAdapter](../../src/providers/openai/OpenAIAdapter.cpp) uses rollout files under `CODEX_HOME/sessions` or the user profile's `.codex/sessions` and the `openai_subscription_codex_local` surface. The percentage scale expresses Codex pressure. General ChatGPT subscription balances are unknown. Keep provider source identity explicit. The source-label contract distinguishes `openai_subscription_local`, `openai_subscription_codex_local`, `openai_subscription_export_import`, and `openai_api_import`. Use readable labels in the UI and reports. Keep labels explicit even when a metric has no quota value.

The [throughput query](../../src/reporting/AnalysisQueryEngine.cpp) calculates deltas within each rollout and averages rates over the full window. These events are separate from subscription reports, recovery, and general usage rollups. The [SQLite reader](../../src/providers/openai/CodexLocalSqliteReader.cpp) and [importer](../../src/providers/openai/CodexLocalUsageImporter.cpp) use thread IDs, provider/model, reasoning effort, counters, and timestamps. The first scan establishes a baseline. Later scans import eligible OpenAI and HICAP deltas and new threads. The freshness check considers WAL and SHM sidecars. Overview uses 5-minute model buckets from this source.

## Import ChatGPT history

Extract the official export. In **File → Import OpenAI Export…**, select the extracted `conversations.json` and keep it with the export's companion files. The [importer](../../src/providers/openai/OpenAIExportImport.cpp) accepts the extracted directory or its `conversations.json` when a recognized companion file is present. The app picker selects the JSON file. Raw ZIP import is deferred.

The importer records timestamped user messages as historical activity. Live quota, reset times, and exact token or cost totals come from their own sources. Use the [export fixture and redaction note](../fixtures/openai/chatgpt-export-derived-fixture-note.md) and the [augmentation fixture checklist](../fixtures/openai/subscription-augmentation-fixture-guide.md) when changing the parser or import flow.

## Organization usage and costs

The optional [API-platform validation path](openai-api-platform.md) targets organization usage and costs. Its first live probe reached both endpoints and failed authorization on the required usage scope. See the [work tracker](../progress.md#blocked-backlog) for the dependency.
