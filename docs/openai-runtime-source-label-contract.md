# OpenAI Runtime Source Label Contract

> **Status**: Partially implemented runtime/reporting contract
> **Purpose**: Lock the shared OpenAI source-label contract and record the currently shipped runtime/reporting adoption boundary

This note defines the small runtime-facing contract behind the normalized OpenAI source labels already used across the planning docs.

## Normalized Label Set

The concrete OpenAI provider-surface labels are:

- `openai_subscription_local`
- `openai_subscription_codex_local`
- `openai_subscription_export_import`
- `openai_api_import`

`openai_subscription` remains a useful high-level product category, but it is too broad to use as the concrete provenance field for runtime/reporting payloads.

## Runtime Meaning

### `openai_subscription_local`

Use when JiStats is describing:

- locally observed OpenAI or Codex subscription activity captured on this machine
- activity rows or runtime summaries whose truth source is only local observation

This is the current local-observation baseline.

### `openai_subscription_codex_local`

Use when JiStats is describing:

- Codex-local rollout/session metadata
- Codex-linked rate-limit pressure imported from `%USERPROFILE%\.codex\sessions\**\rollout-*.jsonl`
- provider truth that is specific to the Codex-local surface rather than general ChatGPT subscription truth

This is the correct label for the current OpenAI adapter quota augmentation path.

### `openai_subscription_export_import`

Use when JiStats is describing:

- historical OpenAI subscription data loaded from an official ChatGPT export bundle

This is historical enrichment, not a live provider-truth label.

### `openai_api_import`

Use when JiStats is describing:

- separately imported OpenAI API-platform usage or billing data

This must stay distinct from subscription tracking.

## User-Facing Display Rule

Runtime and reporting surfaces should keep the normalized provenance keys internally, but user-facing UI/reporting copy should prefer explicit labels such as:

- `OpenAI Local Observation`
- `OpenAI Codex-local`
- `OpenAI Export Import`
- `OpenAI API Platform Import`

In particular, `openai_api_import` must never collapse back into generic `Subscription` wording in a visible plan/source label.

## Immediate Contract Rule

Runtime and reporting code should not infer the concrete source label from:

- provider key alone
- confidence tier alone
- whether quota data exists

The source label needs its own explicit field; runtime/reporting code should not guess it.

## Current Code Boundary

The current codebase now has a first real runtime/reporting adoption slice:

- docs are normalized on the four labels above
- local observation already has a `provider_surface` concept in storage/docs
- `OpenAIAdapter::fetchQuota()` now uses Codex-local rollout discovery, so its concrete provenance is `openai_subscription_codex_local`
- `QuotaResponse` and `QuotaInfo` now carry an explicit provider-surface/source-label field
- `AppController` preserves that field into the provider-fed dashboard/reporting path
- HTML exports now render source-label metadata plus row-level source columns when provider-surface data is present

That means the normalized label set is both locked and usable on the current OpenAI Codex-local quota path. Broader propagation for future OpenAI import/history surfaces is still follow-on work.

## OpenAI Quota Mapping Note

The current Codex-local quota path returns a bounded pressure-style `QuotaResponse`:

- `used` = rounded used percent
- `limit` = `100`
- `remaining` = `100 - used`
- `windowType` = `interval`

This is a compatibility normalization for the existing generic runtime path.

It should be interpreted as:

- Codex-specific provider pressure on a bounded 0-100 scale
- not an exact absolute quota total
- not general ChatGPT subscription truth

## Remaining Follow-On Adoption Points

Future adoption work should extend this contract into:

1. additional OpenAI runtime sources beyond the current Codex-local quota path
2. future persisted quota/import records that need provenance
3. any broader UI surfaces that should expose source labels outside reports

## Related Files

- `docs/openai-mirror-spec.md`
- `docs/openai-codex-local-contract.md`
- `docs/openai-export-import-contract.md`
- `docs/openai-adapter.md`
- `docs/progress.md`
