# Provider support

All provider tracking toggles start off. Enable the providers you use in **File → Settings**. [Codex SQLite model attribution](providers/openai.md#codex-sqlite-privacy-boundary) can run with OpenAI subscription tracking off.

| Integration | Quota in the app | Activity and history | Setup |
| --- | --- | --- | --- |
| [OpenAI / Codex](providers/openai.md) | Codex rate-limit windows from local rollouts when present | Local observation, Codex rollout throughput, model attribution from local SQLite, and historical ChatGPT export import | Enable OpenAI subscription tracking for rollout quota and throughput. Import an extracted ChatGPT export when you want historical activity. |
| [MiniMax](providers/minimax.md) | Provider-reported remaining quota for Coding Plan and Token Plan | Quota snapshots collected by JiStats | Add and validate the key for the selected subscription surface. |
| [Claude Code](providers/claude-kimi.md#claude-code) | Local aggregates support activity summaries. Provider-exact quota is unavailable. | Approved local stats and settings | Enable Claude Code with a local stats cache available. |
| [Kimi Code](providers/claude-kimi.md#kimi-code) | Provider-reported subscription percentages when usable OAuth is available | Approved local runtime and activity metadata | Enable Kimi Code. JiStats reads the Kimi client's OAuth credential file and writes rotated credentials back when refreshing them. |
| [Grok](providers/grok-cursor.md) | Remaining subscription allowance and the reported reset time | Saved quota snapshots | Sign in with the Grok CLI, then turn on Grok tracking. |
| [Cursor](providers/grok-cursor.md) | Included subscription usage when the account reports a percentage | Saved quota snapshots | Set `CURSOR_API_KEY` or `CURSOR_AUTH_TOKEN` for JiStats, then turn on Cursor tracking. |

Codex rollout pressure applies to the Codex window recorded in the source. General ChatGPT subscription quota is unknown. ChatGPT exports enrich historical activity. OpenAI organization usage and cost are a [separate advanced path](providers/openai-api-platform.md). MiniMax PAYG per-request usage was validated separately. Aggregate PAYG history and runtime integration remain deferred.

The [provider validation record](validation/provider-evidence-2026-04-21.md) captures which live responses were checked. The [fixture inventory](fixtures/README.md) describes synthetic and redacted test data. See [quota, freshness, and confidence](reference/presentation.md#quota-freshness-and-confidence) for how to interpret the values.
