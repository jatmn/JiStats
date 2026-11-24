# Claude Code and Kimi Code

Both integrations are opt-in. Use [the local redaction contract](local-redaction.md) to change accepted inputs or prepare fixtures. Keep local activity and provider quota evidence distinct.

## Claude Code

[ClaudeCodeLocalParser](../../src/providers/claude/ClaudeCodeLocalParser.cpp) reads aggregate activity and token data from `~/.claude/stats-cache.json` and bounded metadata from `~/.claude/settings.json`. [ClaudeCodeAdapter](../../src/providers/claude/ClaudeCodeAdapter.cpp) uses these inputs for local activity and usage. Set `CLAUDE_CONFIG_DIR` before starting JiStats to use a different Claude configuration directory. Daily model totals use UTC reporting dates. Recovery uses dated daily totals only. Activity and usage references use an opaque identifier for the stats-cache path. The bundled version 13 upgrade replaces legacy Claude paths in place when JiStats opens the database, even when the cache has changed or is unavailable. The stats cache can support exact local counters. Provider quota, billing mode, and reset behavior remain unverified.

Existing Claude backfill rows retain their original timestamps. Replaying a day previously saved at local midnight can add a UTC-dated row for the same totals. Reports that include both timestamps can count those tokens twice.

Replay the [Claude fixtures](../fixtures/claude/) with [the Claude parser tests](../../tests/test_claude_local_parser.cpp). Keep transcripts, tool payloads, and credentials outside the parser and fixture scope.

An activity day counts once per cache and account across cache revisions.

The local aggregate requires a recorded token or message counter. An explicit zero is a valid measurement. If the token sum is out of range, the adapter uses the recorded message count when available. When neither counter is usable, the aggregate is unavailable.

## Kimi Code

[KimiAdapter](../../src/providers/kimi/KimiAdapter.cpp) reads selected configuration and runtime metadata under `~/.kimi`. An existing root can indicate the client is installed before session activity is available. The local path reports activity from approved session, task, and notification metadata. Those records carry no token usage.

Subscription quota comes from the validated OAuth-backed `/coding/v1/usages` endpoint. The adapter reads the Kimi client's `~/.kimi/credentials/kimi-code.json` file. It can refresh when the access token is unusable or quota returns HTTP 401. A successful refresh writes the rotated credentials back to the same file before using them. A write failure causes the quota fetch to fail. Keep credential contents out of activity ingestion, app storage, logs, and fixtures. Without usable OAuth, quota stays empty. The 5-hour and 7-day percentages describe the returned subscription windows.

The 7-day balance stays unknown until Kimi reports a positive allowance and either used or remaining quota. JiStats displays and records a weekly balance once it has those values, including an explicit zero remaining.

Replay the [Kimi fixtures](../fixtures/kimi/README.md) with [the Kimi adapter tests](../../tests/test_kimi_adapter.cpp). The [dated provider validation](../validation/provider-evidence-2026-04-21.md) records the live OAuth-backed quota check.
