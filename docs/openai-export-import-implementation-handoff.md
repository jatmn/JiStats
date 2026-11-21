# OpenAI Export-Import Implementation Handoff

> **Status**: Support handoff for the landed parser/storage seam and the shipped extracted-export menu trigger
> **Purpose**: Turn the export-import concept into a support-ready note that matches the code currently in this repo

## Intent

OpenAI export import is a historical enrichment path, not the shipped live OpenAI subscription path.

The implementation should preserve that distinction all the way through parser, storage, reporting, and UX labeling.

## Current Landed Scope

The currently landed first export-import slice supports:

- extracted official export directory input
- `conversations.json` input only when provenance checks are strong enough
- a real dashboard menu action for choosing an extracted-export `conversations.json`

It does not try to import every file OpenAI may include in an export bundle.

The next follow-on refinement may add:

- official ChatGPT export `.zip` input

## Parser Boundary

The parser should own:

- input-type validation
- provenance checks
- schema/layout validation
- historical conversation coverage extraction when possible

The parser should not own:

- UI wording
- live quota interpretation
- provider-truth claims

## Current Runtime Boundary

The codebase now has a landed parser/storage seam plus a narrow shipped end-user trigger.

What is already true:

- `OpenAIExportImport` accepts extracted export directories and provenance-checked `conversations.json`
- focused tests cover accepted input, rejection behavior, and historical activity persistence
- imported records are labeled under `openai_subscription_export_import`
- `File -> Import OpenAI Export...` routes through the running app and calls the seam

What is not yet true:

- raw `.zip` input is still deferred
- the first runtime affordance is still file-picker based and limited to extracted-export `conversations.json`

## Storage Boundary

The first storage path should preserve:

- import source type
- import timestamp
- original artifact name when available
- parser version
- detected coverage window when inferable

Imported historical data should remain distinguishable from:

- local observation
- Codex-local imported signals
- optional API-platform imports

## Required Source Labeling

Use:

- `openai_subscription_export_import`

Do not collapse export-import into:

- `openai_subscription_local`
- `openai_subscription_codex_local`
- `openai_api_import`

## Reporting / UX Rules

Required wording themes:

- historical
- user-initiated
- snapshot
- not live

Acceptable examples:

- `Historical OpenAI subscription data imported from official ChatGPT export`
- `Point-in-time import; not live provider quota`

Avoid:

- `OpenAI quota synced`
- `Current remaining balance imported`
- `Provider truth restored`

## First Fixture Expectations

The landed seam already has:

- one safe export-derived fixture or schema excerpt
- one companion redaction/provenance note describing what the committed sample preserves and what it must not claim
- one parser rejection test for malformed or incompatible export input
- focused parser/storage and shell-routing coverage for the historical import path, while keeping the export-import source label distinct in the implementation contract

Current fixture package and note:

- `docs/fixtures/openai/chatgpt-export-redacted/`
- `docs/fixtures/openai/chatgpt-export-derived-fixture-note.md`

## Dependencies On Other Work

The export-import slice depends on:

- the normalized OpenAI source-label contract already in the repo
- a parser/storage mapping decision for imported historical entities

It does not depend on:

- optional OpenAI API-platform validation
- consumer website scraping

## Done Criteria For The First Implementation Slice

The landed parser/storage seam already satisfies these criteria:

- accepted input types match `docs/openai-export-import-contract.md`
- parser failures are safe and explicit
- stored metadata preserves provenance
- reporting/UX keeps export-import labeled as historical-only
- no live quota claim is introduced by the import path

Remaining follow-up before the feature can be described as a broader import workflow:

- add explicit `.zip` handling if that slice is ever prioritized
- decide whether the runtime affordance should also accept selecting the extracted export directory root
- keep `.zip` support explicitly deferred until it is implemented rather than implied
