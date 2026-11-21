# Claude Local Surface Spike

> **Status**: `P7.2` complete
> **Date**: 2026-04-20
> **Purpose**: Define the first redaction-safe JiStats interpretation of Claude Code local artifacts so Phase 7 Claude support can summarize local usage patterns without ingesting prompts, tool payloads, or account secrets
> **Scope**: Local-surface validation and parser-boundary design for the bounded v1.0 Claude implementation; this document defines what may be parsed, not a promise of provider-exact mirror parity

## Outcome

`P7.2` is satisfied by a local-surface spike rather than a provider implementation.

This spike concludes that Claude Code can support a bounded JiStats v1.0 local-first usage summary path, but the cleanest local artifacts only support:

- aggregate local activity summaries
- aggregate token counters by model
- local session/message timing summaries
- default model or settings context

The clean local artifacts do **not** currently prove:

- authoritative subscription quota truth
- clean separation between Claude subscription-backed use and Anthropic Console/API-backed use
- clean separation between first-party Claude use and third-party-provider-backed runs through the Claude shell

## Official Product And Monitoring Context

Current Anthropic docs still matter for how JiStats should interpret the local files:

- Claude Code overview says most surfaces require either a Claude subscription or an Anthropic Console account, and also says the terminal CLI and VS Code support third-party providers.
- Claude Code costs documentation says `/cost` is intended for API users, while Pro and Max subscribers should use `/stats` for usage patterns.
- Claude Code monitoring docs describe OpenTelemetry export for metrics such as session count, token usage, and cost usage.
- Claude Code settings docs confirm official local settings locations such as `~/.claude/settings.json` and `~/.claude.json`.

Planning implication:

- Claude has a real subscription product surface
- Claude also has a real API/Console path
- Claude also supports third-party providers
- JiStats must therefore treat local Claude artifacts as local runtime evidence first, not automatic proof of one billing or provider mode

## Local Input Inventory

Observed local Claude artifacts on this machine include:

- `~/.claude/stats-cache.json`
- `~/.claude/settings.json`
- `~/.claude/history.jsonl`
- `~/.claude/tasks/`
- `~/.claude/projects/`
- `~/.claude/shell-snapshots/`
- `~/.claude/file-history/`
- `~/.claude/plans/`
- `~/.claude/plugins/`
- `~/.claude.json`

This is enough evidence to define a conservative first-parser boundary.

## First Supported Claude Parser Contract

JiStats should treat the first supported Claude local contract as:

1. Read only aggregate or narrowly scoped metadata artifacts.
2. Prefer files that already summarize activity over transcript or tool-log files.
3. Extract only local usage-pattern signals.
4. Treat billing or provider-mode classification as `Unknown` unless a later validated surface can separate subscription, Console/API, and third-party-provider-backed runs safely.
5. Label the result as local-first Claude evidence rather than provider-exact quota truth.

## Approved Inputs For The First Parser

### `~/.claude/stats-cache.json`

Approved use:

- daily aggregate activity
- per-model token totals
- total session/message counts
- high-level session timing summaries

Observed stable fields on this machine include:

- `version`
- `lastComputedDate`
- `dailyActivity[].date`
- `dailyActivity[].messageCount`
- `dailyActivity[].sessionCount`
- `dailyActivity[].toolCallCount`
- `dailyModelTokens[].date`
- `dailyModelTokens[].tokensByModel`
- `modelUsage`
- `totalSessions`
- `totalMessages`
- `longestSession.duration`
- `longestSession.messageCount`
- `longestSession.timestamp`
- `firstSessionDate`
- `hourCounts`
- `totalSpeculationTimeSavedMs`

Use in JiStats:

- local daily usage summaries
- token-volume trend summaries
- per-model activity summaries
- coarse session-density reporting

Why this file matters:

- it is already aggregate
- it avoids raw prompt and tool bodies
- it looks like the cleanest first source for future Claude local summaries

### `~/.claude/settings.json`

Approved use:

- default model context
- update-channel context

Observed fields on this machine include:

- `model`
- `autoUpdatesChannel`

Use in JiStats:

- parser environment sanity checks
- model-default labeling when useful

## Conditionally Usable, But Not In The Default First Parser

### `~/.claude/tasks/**/*.json`

These files expose some stable metadata, including:

- `id`
- `status`
- `blocks`
- `blockedBy`

However, sampled task files also carry:

- `subject`
- `description`
- `activeForm`

Default first-parser rule:

- do not ingest these files yet

Planning conclusion:

- a future parser may add a strict field-level extractor if JiStats later wants task-status counts
- the current first parser should not rely on full task JSON files

### `~/.claude/projects/**/*.jsonl`

These files can contain useful metadata such as:

- `sessionId`
- `entrypoint`
- `version`
- `gitBranch`
- `permissionMode`
- some system or API-error events that may hint at routing/provider mode

However, the same files also contain:

- full user prompts
- assistant output
- tool calls and tool results
- local command output
- project paths
- error bodies and provider-routing details

Default first-parser rule:

- do not ingest these files yet

Planning conclusion:

- these files may become a future conditional source if JiStats later defines a strict filter for system-only or error-only event rows
- they are too content-dense for the default parser boundary

## Explicitly Excluded From The First Parser

The first Claude parser must not read, persist, or fixture-archive the following as normal ingestion inputs:

- `~/.claude/history.jsonl`
- `~/.claude/projects/**/*.jsonl`
- `~/.claude/tasks/**/*.json` as raw full-file input
- `~/.claude/shell-snapshots/`
- `~/.claude/file-history/`
- `~/.claude/plans/`
- `~/.claude/downloads/`
- `~/.claude/cache/`
- `~/.claude/paste-cache/`
- `~/.claude/session-env/`
- `~/.claude/plugins/`
- `~/.claude.json`
- prompt text
- assistant text
- tool payloads
- local command output
- OAuth/session material
- MCP configuration secrets or connector state

These artifacts are either privacy-sensitive, too mixed-content, or too weakly tied to JiStats' first usage-summary goal.

## What Claude Local Evidence Can And Cannot Separate

### What The Clean Approved Files Can Support

- local activity summaries
- local token counters by model
- a local-first understanding of recent Claude Code usage patterns

### What The Clean Approved Files Cannot Reliably Separate

- Claude subscription-backed usage versus Anthropic Console/API-backed usage
- first-party Claude use versus third-party-provider-backed use through the Claude shell
- authoritative quota-remaining state

Important reasoning:

- `stats-cache.json` is excellent for usage-pattern summaries but does not identify billing mode cleanly
- `settings.json` identifies the local default model, not the authoritative billing surface
- mixed-content project transcripts sometimes appear to contain provider-routing or API-error clues, but they are not acceptable first-parser inputs

Planning rule:

- if JiStats later shows Claude local values from this contract, quota state should remain `Estimated` or `Unknown` unless a separately validated provider or telemetry surface can corroborate it
- activity summaries derived from approved local artifacts may still be labeled `LocalExact`

## Recommended Normalized Output

The first parser should normalize Claude local evidence into rows or snapshots shaped roughly like:

```json
{
  "provider_key": "claude_code",
  "source_kind": "claude_code_local_stats",
  "observed_at_unix": 0,
  "default_model": "opus[1m]",
  "last_computed_date": "2026-04-19",
  "daily_activity": [
    {
      "date": "2026-04-17",
      "message_count": 1154,
      "session_count": 23,
      "tool_call_count": 472
    }
  ],
  "model_usage": {
    "claude-opus-4-7": {
      "input_tokens": 0,
      "output_tokens": 0,
      "cache_read_input_tokens": 0,
      "cache_creation_input_tokens": 0,
      "cost_usd": 0
    }
  },
  "confidence_notes": [
    "Derived from local Claude Code runtime artifacts",
    "Supports local usage-pattern summaries",
    "Does not by itself separate subscription, Console/API, or third-party provider modes",
    "Not authoritative provider-side quota truth"
  ]
}
```

## Stability Assessment

### Stable Enough For The First Parser Contract

- `stats-cache.json` aggregate usage summaries
- `settings.json` simple model/update settings

### Plausible Future Inputs, But Not Yet Accepted

- a field-level task-status extractor over `tasks/**/*.json`
- system-only or error-only filtered rows from `projects/**/*.jsonl`
- official telemetry export if the user already enables it

### Not Accepted For The First Parser

- `history.jsonl`
- raw project transcript files
- raw task files
- `.claude.json`
- shell, file-history, and other transcript-like artifact families

## Privacy And Redaction Rules For Phase 7 Samples

If a Claude fixture is committed for Phase 7 or later, it should preserve only:

- aggregate counts and totals
- model identifiers
- timestamps or dates needed for trend reasoning
- explicitly redacted local-path context only when necessary

It must remove or replace:

- prompts
- conversation bodies
- tool calls and tool results
- local command output
- project paths when not required
- OAuth/session material
- MCP secrets or connection details
- raw provider or router error bodies unless separately normalized into a safe note

This spike does not itself create the broader shared redaction policy. That work is completed separately in `docs/claude-kimi-local-redaction-contract.md`.

## Definition Of Done For `P7.2`

`P7.2` is complete when all of the following are true:

- the repo has a written Claude local-surface spike
- the approved, conditional, and excluded inputs are explicit
- the doc says what Claude local evidence can and cannot prove
- the doc records whether local artifacts can safely separate subscription, API/Console, and third-party-provider-backed modes
- `docs/progress.md` is updated in the same session

This document satisfies that bar.

## Follow-On Work

This spike leaves the following intentionally open:

1. the concrete Phase 7 parser implementation and tests
2. safe fixture capture or synthetic parser fixtures
3. any telemetry-backed or provider-backed corroboration path
4. any narrowing of Claude billing/provider-mode classification beyond `Unknown`

## Related Files

- `docs/phase7-claude-kimi-feasibility.md`
- `docs/claude-kimi-local-redaction-contract.md`
- `docs/progress.md`
- `docs/plan.md`
