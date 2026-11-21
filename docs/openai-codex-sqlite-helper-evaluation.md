# OpenAI Codex SQLite Helper Evaluation

> **Status**: Lane 1 provider-only follow-up
> **Purpose**: Record whether `%USERPROFILE%\\.codex\\state_5.sqlite` is worth integrating as a rollout discovery/index helper

## Decision

**Do not integrate `state_5.sqlite` yet.**

For the current OpenAI subscription augmentation checkpoint, JiStats should continue to treat local rollout files under `%USERPROFILE%\\.codex\\sessions\\**\\rollout-*.jsonl` as the only required source.

`state_5.sqlite` remains a possible optimization later, but it is not justified for the current runtime/query pressure.

## Evidence Reviewed

The current machine-local `threads` table in `%USERPROFILE%\\.codex\\state_5.sqlite` exposes fields such as:

- `id`
- `rollout_path`
- `created_at`
- `updated_at`
- `source`
- `model_provider`
- `cwd`
- `title`
- `tokens_used`
- git/source metadata

It does **not** expose the Codex rate-limit snapshot fields JiStats currently needs, such as:

- `rate_limits.primary.used_percent`
- `rate_limits.primary.window_minutes`
- `rate_limits.primary.resets_at`
- `rate_limits.secondary.*`
- `plan_type`

That means SQLite can only help JiStats discover or deduplicate rollout files. It cannot replace rollout parsing as the provider-truth source.

On the current machine-level micro-spike reviewed on `2026-04-18`:

- `%USERPROFILE%\\.codex\\state_5.sqlite` contained `86` `threads` rows
- `%USERPROFILE%\\.codex\\sessions\\**\\rollout-*.jsonl` contained `86` rollout files
- recursive rollout discovery took about `3ms`
- a direct SQLite `rollout_path` lookup took about `1ms`

That delta is too small to justify taking on a second artifact dependency yet.

## Why It Is Not Worth Adding Yet

- The current parser already supports direct recursive discovery of `rollout-*.jsonl` files.
- Lane 1 query pressure is still tiny: the repo only needs a first runtime-ingestion seam, not a high-scale index.
- Adding SQLite lookup now would increase moving parts without adding new provider truth.
- The current active runtime/UI work does not need a second discovery layer yet.

## When To Reopen This

Revisit `state_5.sqlite` only if one of these becomes true:

1. rollout discovery becomes too slow on real user caches
2. deduplication across large Codex session trees becomes a measurable issue
3. runtime import needs stable thread-to-rollout lookup that directory walking cannot provide cheaply

## Current Rule

- source of truth for Codex-local rate-limit snapshots: rollout JSONL files
- optional future helper for discovery only: `state_5.sqlite`
- not justified to ship in the current checkpoint: SQLite-first indexing logic

## Related Files

- `docs/openai-codex-local-contract.md`
- `docs/openai-codex-local-contract-decision.md`
- `docs/fixtures/openai/codex-rollout-rate-limits.redacted.jsonl`
