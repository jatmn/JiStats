# OpenAI Export-Import Contract

> **Status**: Parser/storage seam and app-facing menu trigger shipped for extracted-export inputs
> **Purpose**: Define the user-initiated ChatGPT export-import contract for historical OpenAI subscription enrichment without overstating its historical-only scope

This contract is intentionally scoped as a **follow-on enrichment path** after local observation and any Codex-local official signal augmentation.

It is not the primary live OpenAI subscription signal.

## Product Intent

The export-import path exists so a user can add **historical** OpenAI subscription context without:

- requiring API billing
- relying on undocumented consumer endpoints
- pretending that imported history is live provider truth

## Accepted User Inputs

JiStats v1.0 should accept only these user-provided artifacts:

1. An extracted official export directory that preserves the original export contents.
2. A directly selected `conversations.json` file only when it clearly originated from an official export bundle.
3. A future `.zip` archive path only after the import slice grows explicit archive handling instead of guessing at compressed input.

JiStats v1.0 should reject:

- ad hoc browser network captures
- copied HTML from the ChatGPT UI
- manually edited JSON with no provenance
- undocumented `backend-api/*` payloads
- mixed folders that do not preserve official export structure clearly enough for provenance checks

## Current Landed Input Boundary

The currently landed parser/storage seam in this repo accepts:

- an extracted export directory with marker siblings such as `chat.html` and `message_feedback.json`
- a provenance-checked `conversations.json` selected from that extracted export directory

The currently landed seam does **not** accept:

- raw `.zip` archives

`.zip` handling remains a follow-on refinement so the first implementation can stay explicit about provenance and failure behavior.

The currently landed seam is now wired to a shipped dashboard menu action:

- `File -> Import OpenAI Export...`

That runtime affordance currently targets extracted-export inputs only, even though the file picker is centered on the extracted export's `conversations.json`.

## Official Basis

OpenAI’s Help Center states that ChatGPT data exports are delivered as a `.zip` file and that the file includes chat history plus other relevant account data.

OpenAI also states, in its account-transfer guidance, that the export `.zip` contains `conversations.json` among other files.

References:

- [How do I export my ChatGPT history and data?](https://help.openai.com/en/articles/7260999-how-do-i-export-my-chatgpthistory-and-data)
- [Transferring Conversations from 1 ChatGPT account to another ChatGPT account](https://help.openai.com/en/articles/9106926-transferring-conversations-from-1-chatgpt-account-to-another-chatgpt-account)

## MVP Parsing Boundary

For the current planning baseline, JiStats should treat the export-import parser boundary as:

- **required**: conversation-history payloads needed for historical enrichment
- **optional**: ancillary account metadata from the export bundle when it is clearly useful and low-risk
- **out of scope for v1.0**: importing every file OpenAI may include in the export bundle

This keeps the contract stable even if OpenAI adjusts secondary export files over time.

## Current Shipped Seam

The first landed implementation is currently a single `OpenAIExportImport` seam that combines:

1. input-path validation and provenance checks for extracted-export inputs
2. parser logic for official export `conversations.json` structure
3. conservative mapping of user-authored historical messages into JiStats activity storage
4. import-run metadata persistence that keeps export-import evidence distinct from local observation and Codex-local imports

It currently accepts only:

- extracted export directories
- provenance-checked `conversations.json` selections from those directories

It deliberately does not yet accept:

- raw `.zip` archives

If the implementation is later split into parser/mapper/store types, that refactor should preserve the same truth boundary rather than broadening accepted inputs by assumption.

## First Durable Model Mapping

The first durable mapping should stay conservative:

- use export-derived conversation/message timing only for historical activity-style enrichment
- do not map export data into `quota_snapshots`
- do not map export data into exact `usage_events` token/cost rows unless the export shape later proves those numbers are authoritative

For the first shippable slice, the cleanest default mapping is:

- imported historical activity -> `activity_events`
- `source_type = imported`
- `provider_surface = openai_subscription_export_import`
- `activity_kind = message_send` only when the export record clearly represents a user/authored message timestamp
- `raw_reference` retains the export conversation/message identifier when present

This keeps the import path additive and honest while avoiding false precision.

## Runtime Trigger Boundary

The current shipped seam is now a narrow but real app-facing workflow.

Today that means:

- parser/storage behavior is implemented
- rejection/acceptance and storage behavior are covered by focused tests
- a shipped dashboard menu action can launch the import from the running app
- the runtime affordance currently prompts for an extracted-export `conversations.json`

Support and planning docs should describe this as a shipped historical-import action for extracted-export inputs, not as a broad “import anything OpenAI exports” feature.

## Freshness Semantics

Imported export data must be labeled as:

- user-initiated
- historical
- point-in-time
- not live

JiStats must not imply that export-imported history reflects the user’s current provider-side quota state.

Required semantics:

- freshness source: `User Export`
- freshness mode: `Snapshot`
- live quota status after import: unchanged unless corroborated by another validated source
- unknown recent activity after export cutoff: remains `Unknown` or local-only

## Confidence Labeling

For OpenAI subscription enrichment, imported export data should use:

- provider surface: `openai_subscription_export_import`
- confidence tier: `Imported`
- basis summary: `User-initiated official export`

JiStats should not collapse this into the same label used for:

- local observation
- Codex-local official signal imports
- optional OpenAI API-platform imports

The normalized OpenAI source-label set is:

- `openai_subscription_local`
- `openai_subscription_codex_local`
- `openai_subscription_export_import`
- `openai_api_import`

## Scope Labeling Rules

The UI and reporting path should make these distinctions explicit:

- `Local Observation`: activity JiStats captured on this machine
- `Codex-local Imported`: Codex-specific account-linked or local official signals
- `Export Imported`: historical data loaded from an official ChatGPT export
- `Unknown`: provider truth not available from a validated source

## Import Safety Rules

The import flow should:

- preserve the original file timestamp when available as import metadata
- record the import time separately from the export’s own time coverage
- reject clearly malformed or schema-incompatible files
- version-gate the parser so unknown export layouts fail safely instead of being guessed

## Minimal Metadata To Persist

If JiStats stores import metadata, it should retain at least:

- import source type: `chatgpt_export`
- user action timestamp
- original export artifact name
- parser version
- success/failure outcome
- detected coverage window when inferable

## Explicit Non-Claims

Even after an import succeeds, JiStats must not claim:

- exact current subscription quota remaining
- exact current reset time
- exact live provider-side recent usage

The export path is historical enrichment, not a live mirror.

## Recommended v1.0 UX Copy

Examples of acceptable wording:

- `Historical OpenAI subscription data imported from official ChatGPT export`
- `Import is point-in-time and does not represent live provider quota`
- `Recent activity after the export date may still be local-only or unknown`

Examples to avoid:

- `OpenAI quota synced`
- `Current remaining balance imported`
- `Provider truth restored`

## Follow-On Dependencies

This contract now assumes:

- the Codex-local augmentation path remains separate and already labeled as `openai_subscription_codex_local`
- the shared source-label contract remains authoritative
- the implementation details stay aligned with `docs/openai-export-import-implementation-handoff.md`

## Related Files

- `docs/openai-mirror-spec.md`
- `docs/capability-matrix.md`
- `docs/openai-export-import-implementation-handoff.md`
- `docs/fixtures/openai/subscription-surface-notes.md`
- `docs/fixtures/openai/subscription-augmentation-fixture-plan.md`
- `docs/fixtures/openai/chatgpt-export-derived-fixture-note.md`
