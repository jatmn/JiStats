# Kimi Local Parser Spike

> **Status**: `P7.3` complete
> **Date**: 2026-04-20
> **Purpose**: Define the first redaction-safe JiStats parser contract for `~/.kimi` so Phase 7 Kimi support can extract session metadata and limited runtime evidence without ingesting prompt bodies, tool payloads, or credentials
> **Scope**: Parser-design and evidence for the bounded v1.0 Kimi implementation; this document defines the approved parser boundary, not a promise of provider-exact quota parity

## Outcome

`P7.3` is satisfied by a parser-design contract rather than a full provider implementation.

This spike concludes that JiStats can plausibly build a Kimi local-first parser, but the first parser must stay narrowly scoped to:

- session metadata
- plan-mode and approval-mode state
- task runtime timestamps and completion state
- notification severity and terminal status summaries
- model/provider identifiers from config
- redacted working-directory linkage when needed

The first parser must explicitly avoid raw conversation content, tool inputs and outputs, shell payloads, logs, and credential material.

## Local Input Inventory

Observed top-level `~/.kimi` artifacts on this machine:

- `config.toml`
- `credentials/`
- `device_id`
- `kimi.json`
- `logs/`
- `plans/`
- `sessions/`

Observed session sub-artifacts:

- `baseline/`
- `context.jsonl`
- `context_*.jsonl`
- `notifications/`
- `state.json`
- `tasks/`
- `wire.jsonl`

This is enough evidence to define a first supported parser boundary and a broader set of explicitly excluded artifacts.

## First Supported Parser Contract

JiStats should treat the first supported Kimi parser contract as:

1. Enumerate session directories under `%USERPROFILE%\\.kimi\\sessions\\`.
2. Read only a narrow allowlist of files and fields.
3. Extract only metadata and lightweight runtime outcome signals.
4. Redact or normalize local-path data before persistence unless a future policy explicitly allows raw path storage.
5. Label the result as local-first Kimi evidence rather than authoritative provider-side quota truth.

## Approved Inputs For The First Parser

### `config.toml`

Approved use:

- provider/model defaults
- configured base URLs
- feature and environment-shape checks

Observed safe fields on this machine include:

- `default_model`
- provider mapping under `[models.*]`
- provider `type`
- provider `base_url`
- service `base_url` values for search/fetch

Parsing rules:

- allowlist fields explicitly
- do not read or persist `api_key`, `key`, or other secret-bearing values

Use in JiStats:

- model/provider identification
- parser environment sanity checks
- evidence that the runtime is genuinely Kimi-backed

### `kimi.json`

Approved use:

- working-directory linkage
- last-session linkage

Observed safe fields on this machine include:

- top-level `work_dirs`
- per-entry `last_session_id`
- per-entry `path`, but only after redaction or hashing if persisted

Use in JiStats:

- optional session discovery
- redacted local scope labeling
- correlation between recent sessions and working directories

### `sessions/*/state.json`

Approved use:

- stable session-state metadata
- plan-mode state
- approval-mode metadata

Observed stable fields on this machine include:

- `version`
- `plan_mode`
- `approval`
- `dynamic_subagents`
- `additional_dirs`

Observed nested `approval` fields on this machine include:

- `yolo`
- `auto_approve_actions`

Parsing rules:

- read metadata only
- persist counts or booleans rather than copying raw directory lists where possible
- do not descend into future nested content blobs unless they are separately reviewed and allowlisted

Use in JiStats:

- plan-mode labeling
- approval-mode labeling
- dynamic-subagent count
- additional-directory count

### `sessions/*/tasks/*/runtime.json`

Approved use:

- task lifecycle timing
- task outcome status

Observed stable fields on this machine include:

- `status`
- `started_at`
- `updated_at`
- `finished_at`
- `exit_code`
- `interrupted`
- `timed_out`

Use in JiStats:

- runtime session timelines
- task-success versus task-failure summaries
- local activity density and completion-state evidence

### `sessions/*/notifications/*/event.json`

Approved use:

- notification category and severity
- terminal task-state summaries

Observed stable fields on this machine include:

- `category`
- `type`
- `source_kind`
- `severity`
- `created_at`
- `payload.status`
- `payload.exit_code`
- `payload.interrupted`
- `payload.timed_out`

Observed but not approved for first-parser persistence:

- `payload.terminal_reason`
- freeform descriptive payload text if it appears in future builds

Use in JiStats:

- coarse runtime outcome summaries
- task-failure versus task-timeout labeling
- background-task source classification

### `sessions/*/notifications/*/delivery.json`

Approved use:

- delivery or acknowledgement timing/status only if later needed for UI polish or internal diagnostics

Observed stable fields on this machine include per-sink status values such as:

- `status`

Use in JiStats:

- optional low-priority notification-delivery diagnostics

Note:

- this file is approved but not required for the minimum first parser

## Conditionally Usable, But Not In The Default First Parser

### `context.jsonl` and `context_*.jsonl`

These files are the clearest observed source of token-like counters, but they are also privacy-sensitive.

Observed useful rows on this machine include:

- `_usage` rows with `token_count`

Observed risky content on this machine also includes:

- `content`
- `think`
- tool-call payloads
- full assistant and user bodies

Allowed planning conclusion:

- a future parser may stream these files with a strict row-type and field allowlist that keeps only `_usage.token_count` plus minimal correlation metadata such as `role` and `id`

Default first-parser rule:

- do not ingest these files yet

Reason:

- the safe extractor shape is plausible, but the default parser boundary should not rely on mixed-content transcript files until `P7.5` writes the broader cross-provider redaction contract

## Explicitly Excluded From The First Parser

The first Kimi parser must not read, persist, or fixture-archive the following as normal ingestion inputs:

- `credentials/`
- raw credential values from any file
- `device_id`
- `plans/`
- `logs/`
- `sessions/*/baseline/**`
- `sessions/*/tasks/*/output.log`
- `wire.jsonl`
- raw `context.jsonl` or `context_*.jsonl` conversation rows
- tool-call arguments
- tool results
- prompt text
- assistant response text
- raw shell commands, working directories, or failure bodies when not separately approved

These sources are either privacy-sensitive, too unstable, or both.

## Why `wire.jsonl` Is Out Of Scope For The First Parser

`wire.jsonl` clearly contains potentially useful counters on this machine, including `StatusUpdate` records with:

- `context_usage`
- `context_tokens`
- `max_context_tokens`
- `token_usage`

However, the same file family also contains heterogeneous message types such as:

- `ToolCall`
- `ToolResult`
- `QuestionRequest`
- `ContentPart`

Those records can carry prompt content, tool arguments, or other high-risk payloads.

Planning conclusion:

- `wire.jsonl` is evidence that Kimi exposes richer local runtime counters
- `wire.jsonl` is not approved for the first parser because the privacy surface is too dense for a default v1 extractor

This is an intentional conservative boundary, not evidence that `wire.jsonl` is unusable forever.

## Recommended Normalized Output

The first parser should normalize Kimi local evidence into rows or snapshots shaped roughly like:

```json
{
  "provider_key": "kimi",
  "source_kind": "kimi_local_runtime",
  "session_root_id": "<session-root>",
  "session_id": "<session-id>",
  "observed_at_unix": 0,
  "working_dir_scope": "<redacted-or-hashed-scope>",
  "model_key": "kimi-code/kimi-for-coding",
  "plan_mode": true,
  "approval_mode": {
    "yolo": false,
    "auto_approve_actions": false
  },
  "dynamic_subagent_count": 0,
  "additional_dir_count": 0,
  "task_runtime": {
    "status": "completed",
    "started_at_unix": 0,
    "finished_at_unix": 0,
    "exit_code": 0,
    "interrupted": false,
    "timed_out": false
  },
  "notification_summary": {
    "category": "task",
    "type": "task.completed",
    "severity": "success",
    "source_kind": "background_task"
  },
  "confidence_notes": [
    "Derived from local Kimi runtime artifacts",
    "Not authoritative provider-side quota truth",
    "Does not yet include approved token-count extraction"
  ]
}
```

Notes:

- `working_dir_scope` should be persisted only in a redacted, hashed, or normalized form unless a later privacy decision explicitly allows raw path storage.
- totals should remain explicitly local/runtime-derived until provider-side corroboration exists.
- token counters are intentionally absent from the minimum first-parser output because the cleanest observed sources for them are still mixed-content transcript artifacts.

## Stability Assessment

### Stable Enough For The First Parser Contract

- `config.toml` provider/model/base-URL metadata
- `kimi.json` session-linkage metadata with path redaction
- `state.json` top-level metadata fields
- `tasks/*/runtime.json` lifecycle state and timestamps
- `notifications/*/event.json` category/severity/outcome summaries
- `notifications/*/delivery.json` low-priority delivery state

### Plausible Future Inputs, But Not Yet Accepted

- `_usage.token_count` rows inside `context.jsonl` and `context_*.jsonl`
- filtered `StatusUpdate` records inside `wire.jsonl`

### Not Accepted For The First Parser

- any file under `credentials/`
- `device_id`
- `plans/`
- generic `logs/` parsing
- `baseline/` session content
- `tasks/*/output.log`
- raw transcript or tool-payload ingestion

## Privacy And Redaction Rules For Phase 7 Samples

If a Kimi fixture is committed for Phase 7 or later, it should preserve only:

- file type and version context
- safe field names
- counter-like numeric values
- provider/model identifiers
- plan-mode and approval-mode flags
- redacted or hashed working-directory scope when needed
- task status and notification severity metadata

It must remove or replace:

- prompts
- conversation bodies
- `think` content
- tool-call arguments
- tool outputs
- credential strings
- OAuth material
- device identifiers
- uniquely identifying local paths when not required
- raw `wire.jsonl` or `context*.jsonl` message bodies

This spike does not itself create a full cross-provider redaction contract. That broader work is completed in `docs/claude-kimi-local-redaction-contract.md`.

## Definition Of Done For `P7.3`

`P7.3` is complete when all of the following are true:

- the repo has a written Kimi parser contract
- the approved inputs and explicit exclusions are clear
- the safe and stable file-field inventory is recorded
- token-count and richer counter sources are classified as approved versus conditional versus excluded
- parser output is defined as local-first evidence rather than provider-side truth
- `docs/progress.md` is updated in the same session

This document satisfies that bar.

## Follow-On Work

This spike leaves the following work intentionally open:

1. the concrete Phase 7 parser prototype and tests
2. fixture capture or synthetic parser fixtures
3. any future token-counter extractor for `context*.jsonl`
4. downstream Kimi provider integration and UI/reporting/taskbar wiring

## Related Files

- `docs/phase7-claude-kimi-feasibility.md`
- `docs/progress.md`
- `docs/plan.md`
- `docs/fixtures/README.md`
