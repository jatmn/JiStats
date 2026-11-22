# OpenAI Subscription Augmentation Fixture Guide

Use this guide when changing Codex-local parsing or ChatGPT export import. Follow-ups are recorded in the [roadmap checklist](../../plan.md#remaining-work).

## Fixture Families

### 1. Codex Local / Account-Linked Signals

Primary target:

- Codex rate-limit metadata shaped like `docs/fixtures/openai/codex-rate-limits-shape.sample.json`

Supporting target:

- a safe redacted local rollout excerpt represented through
  `docs/fixtures/openai/codex-rollout-rate-limits.redacted.jsonl`

Minimum parser expectations:

- parse `usedPercent`
- parse `windowDurationMins`
- parse `resetsAt`
- reject missing required fields
- reject impossible values such as negative window durations
- preserve the fact that this source is **Codex-specific**, not universal ChatGPT truth

### 2. ChatGPT Export Import

Primary target:

- official export provenance for files from an extracted archive
- extracted export directory support in the parser
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
- replay the archived export-style `conversations.json` fixture
- verify mixed-source labeling remains stable when local + imported sources coexist

### Integration Tests

Required before the change is complete:

- import flow records metadata without pretending imported history is live
- Codex-local parsed fields map to the correct source label and confidence tier
- reporting surfaces can distinguish `Codex-local Imported` from `Export Imported`

## Available Fixtures

- `docs/fixtures/openai/codex-rate-limits-shape.sample.json`
- `docs/fixtures/openai/codex-rollout-rate-limits.redacted.jsonl`
- `docs/fixtures/openai/codex-consumer-artifact-redaction-note.md`
- `docs/fixtures/openai/chatgpt-export-redacted/`
- `docs/fixtures/openai/chatgpt-export-derived-fixture-note.md`

## Disallowed Fixture Sources

- undocumented `backend-api/*` payloads
- intercepted bearer-token traffic
- browser DOM dumps with account-linked secrets
- edited screenshots without a matching normalized field transcript

## Parser Acceptance Checklist

Before a Codex-local parsing change is complete:

- fixture source is documented
- source is labeled as Codex-specific
- parser rejects malformed required fields
- parser output can carry `Imported` confidence without claiming broader OpenAI truth

Before an export-import change is complete:

- accepted input types match [the ChatGPT export import contract](../../providers/openai.md#import-chatgpt-history)
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
- [the ChatGPT export import contract](../../providers/openai.md#import-chatgpt-history)
