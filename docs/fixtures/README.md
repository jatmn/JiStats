# Provider fixture inventory

Fixtures capture a safe input shape or a dated observation. Use the [provider matrix](../capability-matrix.md) for current app behavior.

| Provider | Fixtures and evidence |
| --- | --- |
| OpenAI / Codex | [OpenAI fixture guide](openai/README.md) covers Codex rate limits, rollouts, extracted ChatGPT exports, and the optional API-platform probe. |
| MiniMax | [MiniMax fixture guide](minimax/README.md) covers synthetic remains responses, reset-window findings, and PAYG evidence. |
| Claude Code | [Stats cache](claude/stats-cache.redacted.json) and [settings](claude/settings.redacted.json) are replayed by the [local parser tests](../../tests/test_claude_local_parser.cpp). |
| Kimi Code | [Kimi fixture guide](kimi/README.md) covers the synthetic local runtime and task/notification cases. |

Keep live captures outside the checkout. Commit the minimum synthetic or redacted material needed to exercise the parser. Document provenance and redactions when adding a fixture. Exclude secrets, account identifiers, private paths, and conversation content. Follow the [Claude and Kimi field-level contract](../providers/local-redaction.md) and the [Codex SQLite privacy boundary](../providers/openai.md#codex-sqlite-privacy-boundary) when preparing fixtures or changing accepted inputs for those sources.
