# Local Observation Binding Model

> **Status**: Current v1.0 baseline documented; future-safe expansion path defined
> **Purpose**: Explain how local observation currently binds to storage rows and how that model can evolve without breaking current persisted data

## Current Binding Rule

Local observation currently binds activity into one synthetic account per:

- `provider_key`
- `provider_surface`

That account key is stored as:

- `local-observation:<providerSurface>`

Examples:

- `local-observation:openai_subscription`
- `local-observation:minimax_coding_plan`

This gives JiStats a deterministic local-only storage target without pretending that local observation already knows the user's real provider account id or real plan id.

## Why The Current Rule Exists

The current v1.0 runtime needs a durable place to store:

- OpenAI/Codex local subscription activity
- future local-only provider activity where no trustworthy provider account id is available

Using one synthetic account per provider surface keeps that path:

- deterministic
- local-first
- cheap to implement
- compatible with existing `providers -> accounts -> plans -> activity_events` storage

## Known Limitation

This binding model collapses multiple real accounts or plans that happen to share the same `provider_surface`.

That means it is not sufficient for future scenarios such as:

- two real OpenAI accounts observed on the same machine
- separate workspace identities under the same provider surface
- plan-aware local observation where Coding Plan and Token Plan need distinct local bindings under one provider family

## Future-Safe Expansion Path

When JiStats grows beyond the current local-only surface binding, local observation should resolve bindings using a richer key built from:

- `provider_key`
- `provider_surface`
- optional local identity hint
- optional provider account hint
- optional provider plan hint

Recommended conceptual model:

1. keep `provider_surface` as the high-level detector/source bucket
2. add a stable observation binding key that can distinguish local identities when available
3. create or resolve a synthetic account row per distinct observation binding, not just per surface
4. attach a synthetic or real plan row only when there is enough evidence to do so honestly

## Practical Next Step

The next implementation step should not replace the current binding immediately.

Instead, it should:

- preserve the current `local-observation:<providerSurface>` path as the fallback
- add a documented seam where richer account or plan hints can be supplied later
- avoid backfilling fake real-account identifiers into existing local-only rows

## Current Decision

For the current v1.0 baseline:

- keep synthetic local observation accounts
- keep one synthetic account per `provider_surface`
- do not invent real provider account or plan identifiers
- treat richer binding as a later provider/data-model refinement, not a blocker for current storage work
