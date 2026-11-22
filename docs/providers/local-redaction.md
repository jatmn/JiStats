# Claude And Kimi Local Redaction Contract

Use this contract for Claude and Kimi local activity ingestion and fixture work. It permits bounded metadata and aggregate counters. Keep raw transcripts, tool payloads, and credentials outside this path.

## Shared Principles

All Claude/Kimi local parser and fixture work must follow these rules:

1. Prefer aggregate artifacts over transcript artifacts.
2. Prefer field-level allowlists over file-level trust.
3. Do not persist content just because it is present locally.
4. Treat local paths, account-linked metadata, and provider-routing details as sensitive unless there is a documented reason to keep them.
5. Distinguish clearly between:
   - local activity evidence
   - local runtime metadata
   - provider-exact quota truth
6. Update the provider appendix in this document before widening any parser or fixture boundary.

## Shared Parser Rules

Future local parsers for Claude or Kimi must:

- enumerate only the currently approved files for the provider
- read only explicitly approved fields
- normalize local paths into redacted, hashed, or scope-label forms unless a later policy explicitly allows raw path storage
- avoid storing prompt text, assistant text, tool inputs, tool outputs, shell output, or credential material
- treat mixed-content transcript files as excluded unless a later provider-specific appendix explicitly approves a row-level allowlist extractor

Future local parsers must not:

- read credential stores as a normal ingestion path
- persist OAuth material, session tokens, or auth headers
- persist MCP connector secrets or external-service account identifiers
- use transcript files as a shortcut for aggregate counters unless a documented safe extractor exists first

## Shared Fixture Rules

If future Claude or Kimi fixture material is ever committed, it must:

- preserve only the minimum fields needed for parser or policy validation
- include a short provenance note describing the source and the redactions applied
- normalize or redact account-linked identifiers
- normalize or redact local paths unless those paths are necessary to validate scope behavior
- remove freeform content bodies, tool payloads, and command outputs

No fixture may include:

- prompts
- conversation transcripts
- assistant responses
- tool-call arguments
- tool results
- shell command output bodies
- raw credential strings
- OAuth/session tokens
- browser cookies
- account email addresses unless a separate note explicitly proves they are required and safely redacted

## Normalized Redaction Metadata Template

Future fixture notes should preserve a small metadata block shaped roughly like:

```json
{
  "artifact_type": "provider_local_redacted",
  "provider_key": "claude_code|kimi",
  "capture_date_utc": "YYYY-MM-DD",
  "source_kind": "local_runtime_artifact",
  "approved_fields_only": true,
  "redactions_applied": [
    "prompt_content",
    "assistant_content",
    "tool_payloads",
    "credentials",
    "account_identifiers",
    "sensitive_paths"
  ],
  "notes": [
    "Local-first evidence, not provider-exact quota truth"
  ]
}
```

## Shared Allowlist Categories

These categories are generally acceptable when explicitly approved by a provider appendix:

- aggregate activity counts
- aggregate token counters
- session or task timestamps
- status and severity labels
- provider/model identifiers
- bounded version or settings metadata
- redacted working-directory or project-scope labels
- parser-environment sanity-check fields

## Shared Denylist Categories

These categories are denied by default for both providers:

- prompts
- assistant text
- transcript message bodies
- tool-call payloads
- tool results
- raw shell output
- raw local command output
- OAuth/session material
- credential storage
- browser or connector auth material
- raw MCP server secrets or connector payloads
- raw provider/router error bodies that expose account linkage or hidden transport details

## Claude Appendix

### Approved Inputs

- `~/.claude/stats-cache.json`
  - approved fields: aggregate daily activity, aggregate model token usage, total sessions/messages, session-duration summaries, hour-count summaries
- `~/.claude/settings.json`
  - approved fields: `model`, `autoUpdatesChannel`

### Conditionally Usable, But Not Approved By Default

- `~/.claude/tasks/**/*.json`
  - possible future use only through a field-level extractor that keeps status metadata and drops descriptive task text
- `~/.claude/projects/**/*.jsonl`
  - possible future use only through a provider-specific row-level filter for system-only metadata
- official telemetry export already enabled by the user
  - possible future corroboration path if JiStats later documents a safe ingest contract

### Explicitly Excluded

- `~/.claude/history.jsonl`
- `~/.claude/projects/**/*.jsonl` as raw transcript input
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

### Claude Provider-Mode Rule

Claude local artifacts may support local activity or runtime summaries before they support provider-mode classification.

Default rule:

- if approved local artifacts cannot safely separate subscription-backed, Anthropic Console/API-backed, and third-party-provider-backed runs, JiStats must classify that dimension as `Unknown`

## Kimi Appendix

The [Kimi quota adapter](claude-kimi.md#kimi-code) uses a separate OAuth path. Credential files remain excluded from local activity parsing and fixtures. Session-relative entries below also apply to supported nested session directories.

### Approved Inputs

- `~/.kimi/config.toml`
  - approved fields: provider/model defaults, provider `type`, provider/service `base_url`
- `~/.kimi/kimi.json`
  - approved fields: `work_dirs`, `last_session_id`, with path redaction or hashing
- `~/.kimi/sessions/*/state.json`
  - approved fields: `version`, `plan_mode`, `approval`, `dynamic_subagents`, `additional_dirs`
- `~/.kimi/sessions/*/tasks/*/runtime.json`
  - approved fields: `status`, `started_at`, `updated_at`, `finished_at`, `exit_code`, `interrupted`, `timed_out`
- `~/.kimi/sessions/*/notifications/*/event.json`
  - approved fields: `category`, `type`, `source_kind`, `severity`, `created_at`, plus bounded payload outcome fields
- `~/.kimi/sessions/*/notifications/*/delivery.json`
  - approved fields: delivery-status metadata only

### Conditionally Usable, But Not Approved By Default

- `~/.kimi/sessions/*/context.jsonl`
- `~/.kimi/sessions/*/context_*.jsonl`
  - possible future use only through a strict `_usage.token_count` extractor with minimal correlation metadata
- `~/.kimi/sessions/*/wire.jsonl`
  - possible future use only through a separately approved record-type filter

### Explicitly Excluded

- `~/.kimi/credentials/`
- `~/.kimi/device_id`
- `~/.kimi/logs/`
- `~/.kimi/plans/`
- `~/.kimi/sessions/*/baseline/**`
- `~/.kimi/sessions/*/tasks/*/output.log`
- raw transcript or tool-payload rows from `context*.jsonl`
- raw `wire.jsonl`

## Change-Control Rule

Before any Claude or Kimi local parser grows beyond the boundaries above, update:

1. the [provider reference](claude-kimi.md)
2. this shared contract
3. the [work tracker](../progress.md)

Do not widen parser or fixture work first and document the boundary later.

## Relationship To Confidence Policy

This redaction contract does not decide whether a value is `Exact`, `HighConfidence`, `Estimated`, `LocalExact`, `Imported`, or `Unknown`.

See [quota, freshness, and confidence](../reference/presentation.md#quota-freshness-and-confidence) for evidence labels and [runtime policy](../reference/runtime.md#monitoring-cost) for monitoring cost.

This contract decides only:

- what local material may be parsed
- what local material may be archived safely
- what must remain out of scope

## Related Files

- [Fixture inventory](../fixtures/README.md)
- [Original feasibility research](https://github.com/jatmn/JiStats/blob/fd934179646f130fe346a20fb1ca7d82162902f6/docs/phase7-claude-kimi-feasibility.md)
