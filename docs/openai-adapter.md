# OpenAI Adapter Baseline

> **Status**: Partially implemented
> **Purpose**: Describe what the current `OpenAIAdapter` really does, what is already wired into the app, and what still remains scaffolded

## Current Baseline

The OpenAI adapter is no longer just an isolated scaffold. It now participates in the provider-fed app path through:

- `ProviderManager`
- production adapter registration in `AppController`
- focused fixture and provider-path tests

Relevant files:

- `include/providers/openai/OpenAIAdapter.hpp`
- `src/providers/openai/OpenAIAdapter.cpp`
- `tests/openai_fixture_tests.cpp`
- `tests/test_provider_data_path.cpp`

## What Is Implemented

The adapter currently has working support for:

- fixture parsing of OpenAI usage-style sample data
- fixture parsing of Codex-local rate-limit payload shapes
- parsing the public camelCase Codex rate-limit sample shape
- parsing a safe redacted local rollout excerpt
- scanning a rollout directory and selecting the newest valid Codex rollout snapshot
- basic provider identity and enable/disable behavior

## What Is Still Scaffolded

The main remaining gaps are now around richer modeling and credential verification, not the basic local quota hook-up.

Today:

- `fetchUsage()` is still a minimal success-path scaffold with no real provider data
- `fetchQuota()` now uses local Codex rollout discovery, normalizes Codex-local pressure into the generic `QuotaResponse` shape, and carries the explicit provider-surface label `openai_subscription_codex_local`
- credential validation is still lightweight and not a real production verification flow

That means the adapter is now wired into the app with a real local quota-pressure path and explicit Codex-local provenance, but it is not yet the final runtime shape for broader OpenAI-specific modeling.

## Current Role In The App

`AppController` now registers `OpenAIAdapter` in the production provider path. The current provider/runtime behavior is:

1. resolve the local Codex rollout directory from `CODEX_HOME/sessions`, `%USERPROFILE%\\.codex\\sessions`, or `HOME/.codex/sessions`
2. select the newest supported local rollout snapshot
3. map the chosen bucket into a bounded pressure-style `QuotaResponse`
4. preserve actionable diagnostics when rollout files are missing, absent, malformed, or unsupported

The current mapping is intentionally bounded rather than absolute:

- `used` = rounded used percent
- `limit` = `100`
- `remaining` = `100 - used`
- `windowType` = `interval`

This should be read as Codex-local pressure on a 0-100 scale, not as an invented absolute OpenAI subscription quota total.

The remaining provider/runtime follow-up is therefore:

1. decide whether a richer OpenAI-specific runtime shape should replace the temporary 0-100 compatibility mapping
2. extend provider-surface population beyond the current Codex-local quota path when additional OpenAI sources land
3. keep rollout diagnostics actionable when files drift or become unsupported

See also:

- `plans/provider-data-path-plan.md`
- `docs/openai-codex-local-contract.md`
- `docs/openai-runtime-source-label-contract.md`
- `docs/progress.md`
