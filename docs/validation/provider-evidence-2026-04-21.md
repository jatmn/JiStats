# Provider validation record, 2026-04-21

These are dated observations. Keep credentials, raw captures, and account-specific output in local storage when repeating the checks.

| Surface | Evidence and limits |
| --- | --- |
| MiniMax | Both subscription remains endpoints returned usable responses in the 2026-04-21 validation. The [reset-window findings](../fixtures/minimax/reset-window-evidence-log.md) record accepted five-hour family evidence. Separate [PAYG evidence](../fixtures/minimax/README.md) covers immediate per-request usage only. |
| Kimi Code | The OAuth-backed quota endpoint returned usage and limit fields on 2026-04-21. Quota depends on usable OAuth. Local runtime evidence supplies activity and history. |
| Claude Code | The baseline uses approved local artifacts. This pass did not exercise a live Claude quota API. |
| OpenAI / Codex | The earlier OpenAI organization usage/cost probe reached both endpoints and received HTTP 403 for the missing `api.usage.read` scope. Successful freshness and lag measurement remains open for the optional [API-platform path](../providers/openai-api-platform.md). The baseline uses subscription-local sources. |

See [provider support](../capability-matrix.md) for the implementation scope, [fixtures](../fixtures/README.md) for replayable data, and the [release checklist](../release-checklist.md) for overall signoff.
