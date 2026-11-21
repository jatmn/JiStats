# ChatGPT Export-Derived Fixture Note

> **Status**: Durable fixture note for the shipped extracted-export parser/storage seam
> **Purpose**: Define the redaction and provenance rules for the committed ChatGPT export-derived fixture package used by the first historical import slice

## What This Artifact Is

This note describes the minimum acceptable contract for the committed export-derived sample used by the landed OpenAI export-import parser/storage seam.

It travels with the first redacted export-derived fixture package in this repo.

The current committed fixture package is:

- `docs/fixtures/openai/chatgpt-export-redacted/`

## What This Artifact Is Not

This note is not:

- a raw user export archive
- proof of live provider quota truth
- a replacement for the Codex-local live subscription path
- permission to commit personally identifying conversation content

This note also does not broaden the runtime feature beyond its current narrow affordance. The app now exposes `File -> Import OpenAI Export...`, but that shipped action is still limited to extracted-export inputs and does not imply broad `.zip` archive support or live quota sync.

## Provenance Wording

Any committed export-derived sample should be described as:

- historical
- user-initiated
- redacted
- export-derived

Safe example wording:

- `Redacted schema excerpt derived from an official ChatGPT data export`
- `Historical export-derived fixture for parser coverage; not a live quota signal`

## Redaction Rules

The first committed fixture must remove or replace:

- account identifiers
- conversation titles that expose personal or project information
- free-form prompt and response content
- attachment names or URLs
- exact external links when they identify the original user or workspace

Preserve structure, not content, whenever possible.

## Safe Fields To Preserve

The first fixture may preserve schema-level fields that are necessary for parser coverage, such as:

- top-level file names or section names
- conversation object structure
- message role/type fields
- timestamps
- stable schema keys
- redacted IDs or deterministic placeholders

If a field is not required for parser/storage coverage, prefer removing it.

## Historical-Only Labeling Rules

Anything derived from ChatGPT export import must remain visibly separate from:

- `openai_subscription_local`
- `openai_subscription_codex_local`
- `openai_api_import`

Use:

- `openai_subscription_export_import`

The fixture and any related tests should reinforce that this is a historical, user-initiated import path, not a live provider sync.

## Fixture Acceptance Boundary

The first committed export-derived fixture is acceptable only if:

- it is commit-safe after redaction
- it is sufficient for parser rejection/acceptance tests
- it does not imply live quota semantics
- it does not preserve recoverable personal conversation content
- it keeps provenance obvious to future contributors

The current fixture package meets that boundary by using an extracted export directory rather than a raw `.zip` archive.

That extracted-directory boundary also matches the currently shipped parser/storage seam in code today:

- accepted now: extracted export directory, or a provenance-checked `conversations.json` within that directory
- not accepted yet: raw `.zip` archive input

## Related Files

- `docs/openai-export-import-contract.md`
- `docs/openai-export-import-implementation-handoff.md`
- `docs/fixtures/openai/README.md`
- `docs/fixtures/openai/chatgpt-export-redacted/`
