# Provider-Fed Data Path Follow-up Plan

## Purpose

This note replaces the older bootstrap plan for introducing a provider-fed app path.

That bootstrap work is mostly landed already. The current purpose of this file is to
capture the remaining work needed to make the existing provider-fed runtime path
trustworthy for real OpenAI/Codex subscription augmentation and future provider-backed
runtime state.

This plan is aligned to the current active work in [`docs/progress.md`](../docs/progress.md).

## Current Reality

The repo already has a real provider-fed path in place:

- `AppController` registers production adapters for OpenAI and MiniMax during init.
- `AppController` can build a `DashboardViewModel` from `ProviderManager`.
- tests can inject a `ProviderManager` through `setProviderManagerForTesting()`.
- focused provider-path tests already exist in `tests/test_provider_data_path.cpp`.
- production startup uses the provider-fed builder, while the older sample/mirror path
  is kept under `JISTATS_ENABLE_TEST_HOOKS`.

Relevant code:

- [`src/app/AppController.cpp`](../src/app/AppController.cpp)
- [`include/app/AppController.hpp`](../include/app/AppController.hpp)
- [`include/providers/ProviderManager.hpp`](../include/providers/ProviderManager.hpp)
- [`src/providers/ProviderManager.cpp`](../src/providers/ProviderManager.cpp)
- [`src/providers/openai/OpenAIAdapter.cpp`](../src/providers/openai/OpenAIAdapter.cpp)
- [`tests/test_provider_data_path.cpp`](../tests/test_provider_data_path.cpp)

## What Is Already Done

These items should no longer be treated as open plan work:

1. ProviderManager test injection into `AppController`
2. provider-fed `DashboardViewModel` construction path
3. production registration of OpenAI and MiniMax adapters
4. basic provider-path tests around enabled/disabled providers and quota-fed runtime state
5. retention of the older sample path behind test hooks only

## Remaining Gaps

### 1. OpenAI provider runtime path is still scaffolded

The main remaining gap is not in `AppController`; it is in `OpenAIAdapter`.

Today:

- `AppController` calls into `ProviderManager`
- `ProviderManager` calls `OpenAIAdapter::fetchQuota()`
- `OpenAIAdapter::fetchQuota()` still returns the scaffold error

That means the production provider-fed path exists structurally, but the OpenAI runtime
signal is not yet real.

### 2. Codex rollout parsing is still test-only

The repo now has:

- Codex rollout snapshot parsing
- rollout-directory discovery
- fixture coverage for those parsing helpers

But that parsing is not yet used by the real OpenAI adapter fetch path. The runtime app
still cannot surface Codex-local OpenAI subscription state through the production
provider pipeline.

### 3. Rollout diagnostics are too generic

If a rollout directory exists but all files are malformed, unsupported, or drifted,
the current helper collapses those cases into a generic "no supported snapshot found"
result.

The remaining provider-path work should preserve enough detail for runtime diagnostics
and future UI/reporting visibility.

### 4. Provider-fed runtime state is quota/status-first, not full provider-history ingest

The current provider-fed startup path is intentionally narrow:

- quota/status data feeds dashboard provider rows and runtime state
- persisted local activity feeds dashboard summaries and taskbar recent activity

That is acceptable for the current checkpoint. This plan should not expand scope into
full usage import unless a concrete runtime or reporting consumer appears.

## Active Follow-up Work

### Work Item A: Land the real OpenAI quota fetch path

Goal:

- implement `OpenAIAdapter::fetchQuota()` so the production provider-fed runtime path
  can use Codex-local rollout discovery instead of returning a scaffold error

Expected behavior:

- attempt to discover the latest supported Codex rollout snapshot from the local rollout
  directory
- map the parsed snapshot into `QuotaResponse`
- set `snapshotTime`, success/error state, and meaningful remaining/used/window fields
- avoid inventing fake absolute quota totals when the source only provides pressure-style
  or bounded window data

This maps directly to the active work item:

- `Wire the Codex rollout ingestion path into the real OpenAI provider/runtime flow`

### Work Item B: Improve rollout failure diagnostics

Goal:

- distinguish these cases cleanly:
  - rollout directory missing
  - rollout directory empty
  - rollout files found but malformed
  - rollout files found but unsupported/drifted

Expected behavior:

- runtime logs and adapter error strings should help explain why OpenAI runtime state is
  unavailable
- future UI/reporting work should be able to reuse those distinctions if needed

This maps directly to the active work item:

- `Surface actionable diagnostics when Codex rollout directory parsing finds only malformed or drifted files`

### Work Item C: Keep the current fallback boundary explicit

Goal:

- keep the provider-fed production path real
- keep the old sample/mirror path limited to test-hook scaffolding
- do not accidentally let test-only fallback logic mask production-provider regressions

Expected behavior:

- provider-path tests continue to inject `ProviderManager`
- production init continues to build from real adapters
- sample helpers remain clearly transitional and test-only

## Not In Scope For This Plan

These are related, but should not be folded into this note unless they become active
provider-path blockers:

- report freshness fixes after provider filtering
- startup Run-entry rewrite reconciliation
- OpenAI source-label normalization across docs/specs
- full provider usage/history ingestion during startup
- persistence of Codex rollout snapshots as first-class history
- replacing persisted local activity summaries with provider usage import

Those belong to other active tasks in `docs/progress.md`.

## Recommended Execution Order

1. implement the real `OpenAIAdapter::fetchQuota()` path from Codex rollout discovery
2. tighten rollout diagnostics so runtime failures are distinguishable
3. extend provider-path tests to cover:
   - successful OpenAI quota fetch from a rollout-derived path
   - expected failure when directory is missing
   - expected failure when files are present but malformed/unsupported
4. only then decide whether any remaining sample-path scaffolding can be reduced further

## File Map

Primary files for this follow-up:

- [`src/providers/openai/OpenAIAdapter.cpp`](../src/providers/openai/OpenAIAdapter.cpp)
- [`include/providers/openai/OpenAIAdapter.hpp`](../include/providers/openai/OpenAIAdapter.hpp)
- [`src/app/AppController.cpp`](../src/app/AppController.cpp)
- [`tests/openai_fixture_tests.cpp`](../tests/openai_fixture_tests.cpp)
- [`tests/test_provider_data_path.cpp`](../tests/test_provider_data_path.cpp)
- [`docs/progress.md`](../docs/progress.md)

## Success Criteria

This follow-up should be considered complete when:

1. production `AppController -> ProviderManager -> OpenAIAdapter::fetchQuota()` uses the
   Codex-local rollout path rather than returning a scaffold error
2. OpenAI runtime state in the provider-fed path is driven by real local Codex evidence
3. rollout parse failures produce actionable diagnostics instead of a generic empty result
4. provider-path tests cover both success and failure cases for the OpenAI runtime path
5. this file no longer describes already-landed bootstrap work as if it were still open
