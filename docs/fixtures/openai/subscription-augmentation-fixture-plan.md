# OpenAI Subscription Augmentation Fixture Plan

> **Status**: Lane 2 quality/support plan
> **Purpose**: Pin down fixture and test expectations for Codex-local parsing and ChatGPT export import before implementation spreads into runtime code

This note is the quality checklist for the current OpenAI subscription augmentation track.

## Goals

- make fixture expectations explicit before Lane 1 parser work begins
- keep Codex-local and export-import evidence clearly separated
- define what must be tested versus what is only nice to have

## Fixture Families

### 1. Codex Local / Account-Linked Signals

Primary target:

- Codex rate-limit metadata shaped like `docs/fixtures/openai/codex-rate-limits-shape.sample.json`

Supporting target:

- a safe redacted local rollout excerpt represented through
  `docs/fixtures/openai/codex-rollout-rate-limits.redacted.jsonl`

Future support target:

- a safe redacted Codex usage-dashboard capture represented through
  `docs/fixtures/openai/codex-consumer-artifact-redaction-note.md`

Minimum parser expectations:

- parse `usedPercent`
- parse `windowDurationMins`
- parse `resetsAt`
- reject missing required fields
- reject impossible values such as negative window durations
- preserve the fact that this source is **Codex-specific**, not universal ChatGPT truth

### 2. ChatGPT Export Import

Primary target:

- official export `.zip` provenance
- extracted export directory support
- `conversations.json` historical import path

Minimum parser expectations:

- validate provenance before import when only a single file is selected
- reject malformed JSON cleanly
- reject unknown export layout without guessing
- derive historical coverage only when the source supports it
- preserve import metadata separate from exported conversation timestamps

## Required Test Layers

### Unit Tests

Required:

- field-level validation for Codex rate-limit metadata
- export provenance and input-type validation
- malformed JSON and malformed timestamp rejection
- confidence/source labeling derivation

### Fixture Replay Tests

Required:

- replay public Codex rate-limit sample into the parser
- replay at least one export-style `conversations.json` fixture once archived
- verify mixed-source labeling remains stable when local + imported sources coexist

### Integration Tests

Required before the feature is called done:

- import flow records metadata without pretending imported history is live
- Codex-local parsed fields map to the correct source label and confidence tier
- reporting surfaces can distinguish `Codex-local Imported` from `Export Imported`

## Fixture Backlog

### Ready Now

- `docs/fixtures/openai/codex-rate-limits-shape.sample.json`
- `docs/fixtures/openai/codex-rollout-rate-limits.redacted.jsonl`
- `docs/fixtures/openai/codex-consumer-artifact-redaction-note.md`
- `docs/fixtures/openai/chatgpt-export-redacted/`
- `docs/fixtures/openai/chatgpt-export-derived-fixture-note.md`

### Needed Next

- `.zip` archive handling coverage once the parser grows explicit compressed-export support
- one safe redacted Codex usage-dashboard capture that confirms whether the consumer dashboard surface exposes additional usable fields beyond the local rollout and public rate-limit shapes

### Explicitly Not Accepted As Fixtures

- undocumented `backend-api/*` payloads
- intercepted bearer-token traffic
- browser DOM dumps with account-linked secrets
- edited screenshots without a matching normalized field transcript

## Parser Acceptance Checklist

Before Lane 1 marks the Codex-local parsing slice done:

- fixture source is documented
- source is labeled as Codex-specific
- parser rejects malformed required fields
- parser output can carry `Imported` confidence without claiming broader OpenAI truth

Before export-import is considered implementation-ready:

- accepted input types match `docs/openai-export-import-contract.md`
- parser rejects unknown layouts safely
- imported history remains historical-only in reporting and UX labels
- gap/freshness semantics stay explicit after import

## Reporting / UX Checks

The following user-visible distinctions must remain testable:

- `Local Observation` versus `Codex-local Imported`
- `Codex-local Imported` versus `Export Imported`
- `Imported historical snapshot` versus `live provider state`
- `OpenAI subscription unknown` when no validated provider-side truth exists

## Related Files

- `docs/fixtures/openai/README.md`
- `docs/fixtures/openai/subscription-surface-notes.md`
- `docs/fixtures/openai/codex-consumer-artifact-redaction-note.md`
- `docs/openai-export-import-contract.md`
