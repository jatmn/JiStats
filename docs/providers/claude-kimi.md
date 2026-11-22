# Claude Code and Kimi Code

Both integrations are opt-in. Use [the local redaction contract](local-redaction.md) to change accepted inputs or prepare fixtures. Keep local activity and provider quota evidence distinct.

## Claude Code

[ClaudeCodeLocalParser](../../src/providers/claude/ClaudeCodeLocalParser.cpp) reads aggregate activity and token data from `~/.claude/stats-cache.json` and bounded metadata from `~/.claude/settings.json`. [ClaudeCodeAdapter](../../src/providers/claude/ClaudeCodeAdapter.cpp) uses these inputs for local activity and usage. The stats cache can support exact local counters. Provider quota, billing mode, and reset behavior remain unverified.

Replay the [Claude fixtures](../fixtures/claude/) with [the Claude parser tests](../../tests/test_claude_local_parser.cpp). Keep transcripts, tool payloads, and credentials outside the parser and fixture scope.

## Kimi Code

[KimiAdapter](../../src/providers/kimi/KimiAdapter.cpp) reads selected configuration and runtime metadata under `~/.kimi`. An existing root can indicate the client is installed before session activity is available. The local path supports activity and history from approved session, task, and notification metadata.

Subscription quota comes from the validated OAuth-backed `/coding/v1/usages` endpoint. The adapter uses the provider-owned `~/.kimi/credentials/kimi-code.json` credential and can refresh it when needed. Keep credential contents out of activity ingestion, app storage, logs, and fixtures. Without usable OAuth, quota stays empty. The 5-hour and 7-day percentages describe the returned subscription windows.

Replay the [Kimi fixtures](../fixtures/kimi/README.md) with [the Kimi adapter tests](../../tests/test_kimi_adapter.cpp). The [dated provider validation](../validation/provider-evidence-2026-04-21.md) records the live OAuth-backed quota check.
