# Phase 0: Provider Surface Validation Spike

> **Status**: Closed for the current subscription-first v1.0 baseline; the only remaining blocked advanced-path item is OpenAI API-platform freshness/lag capture with a higher-scope credential
> **Purpose**: Validate provider-surface assumptions before implementation hardens around them
> **Dependencies**: The right credentials for the surface actually being tested

Local documentation and fixture evidence were reconciled on `2026-04-19`.
Official OpenAI subscription-surface research was extended on `2026-04-18` using public OpenAI Help Center pages, `chatgpt.com` Codex pricing/help surfaces, and the public `openai/codex` app-server documentation.
Since then, JiStats has also landed the first Codex-local OpenAI augmentation path by parsing local rollout session files under `%USERPROFILE%\\.codex\\sessions\\**\\rollout-*.jsonl`.
Current status was rechecked on `2026-04-19`: no local repo implementation work remains for Phase 0, and the only still-unchecked item is the optional OpenAI API-platform freshness/lag pass. First live authz evidence is now archived under `docs/fixtures/openai/api-platform/`: a real `OPENAI_API_KEY` reached both organization endpoints, but both returned `403` with `Missing scopes: api.usage.read`, so repeated successful freshness measurement is still blocked on a higher-scope credential with the right org-scoped permission.
Unchecked items below now mean an optional advanced-path validation step or a v1.x follow-on rather than a blocker to closing Phase 0 itself.

## Current Readout

Phase 0 has already answered the main "can we proceed honestly?" question:

- OpenAI subscription tracking should remain local-first, with Codex-local official signal augmentation when available
- automated consumer website scraping is not the product foundation for v1.0
- ChatGPT export import is now a shipped user-initiated historical enrichment path for extracted-export inputs, not the first shipped live quota path
- MiniMax Coding Plan and Token Plan subscription endpoints are validated enough for the current subscription-first v1.0 runtime path
- MiniMax PAYG dedicated-key validation confirmed that the documented OpenAI-compatible text API returns immediate per-request usage payloads, while the older candidate aggregate/discovery paths still return `404`

Phase 0 itself is now closed on the current v1.0 path:

- MiniMax reset-window behavior is decision-ready for current runtime behavior through one full Coding Plan rollover proof plus the accepted Token Plan archived-pre/live-post comparison rule, while broader interval-family semantics remain confidence-refresh work
- the OpenAI export-import contract has already been turned into a shipped extracted-export parser/storage seam with a first dashboard menu trigger
- optional OpenAI API-platform validation remains separate and explicitly non-blocking unless separate API billing/credentials exist

Additional live note from user testing on `2026-04-18`:

- the current OpenAI/Codex subscription-status value shown by JiStats matched the user's expected value during manual testing
- treat that as a useful real-world sanity check for the shipped Codex-local status path, not as proof of broader ChatGPT subscription quota truth beyond the currently supported surface

## Scope Clarification

This spike is no longer "API-first" for OpenAI.

For OpenAI, Phase 0 must validate **the user's real product surface first**:

- ChatGPT / Codex subscription surface
- optional OpenAI API platform surface only if the user separately has API billing and credentials

For MiniMax, v1.0 remains subscription-first and PAYG stays deferred to v1.x.

## Goals

Validate these key assumptions through real product-surface checks, fixtures, and documented evidence.

### OpenAI Subscription Surface (v1.0 default)

- [x] Identify whether any trustworthy subscription-side quota signals are exposed
- [x] Identify whether any account/history/export surfaces exist without API billing
- [x] Document what JiStats can honestly show for a subscription-only user
- [x] Define the correct `Unknown` / `Estimated` UX when provider truth is unavailable
- [x] Capture any available non-API artifacts or response shapes that can be fixture-tested
- [x] Choose the first shipped subscription augmentation path (`Codex`-local rollout parsing)

### OpenAI API Platform Surface (optional advanced path)

- [x] Only if separate API access exists: verify usage/cost endpoints actually available on that surface
- [ ] Measure freshness/lag of any imported API data once a credential with `api.usage.read` and the correct org-scoped role exists
- [x] Confirm how API data should remain separate from subscription tracking

### MiniMax Coding Plan (v1.0 priority surface)

- [x] Verify subscription API exists for Coding Plan
- [x] Discover quota endpoint URL and credential surface
- [x] Capture quota-payload granularity fields
- [x] Measure freshness over repeated live checks
- [x] Identify reset behavior enough for current v1.0 runtime labeling (model-aware mixed interval families, with a confirmed `5h` rollover proof and separate `24h` / weekly companion counters)
- [x] Capture sample response format

### MiniMax PAYG (v1.x deferred track)

- [x] Probe likely usage API endpoints with available credential (`/v1/usage`, `/api/v1/usage`)
- [x] Measure freshness/granularity
- [x] Verify cost calculation approach
- [x] Capture probe response format

### MiniMax Token Plan (v1.0 subscription scope)

- [x] Verify subscription API exists for Token Plan
- [x] Discover remains endpoint URL
- [x] Identify reset behavior enough for current v1.0 runtime parity through the accepted archived-pre/live-post comparison rule
- [x] Capture sample response format

## Required Inputs

### OpenAI Subscription

- A real OpenAI subscriber workflow to reason from
- Access to any subscription-visible settings, history, export, or quota surfaces the user can inspect
- Confirmation of whether the user also has separate API-platform access

### OpenAI API Platform (optional)

- Separate API billing enabled on `platform.openai.com`
- API credentials only if this optional path is being validated
- Expected responses: usage/cost/history surfaces if available

### MiniMax

- Coding Plan subscription credentials
- PAYG API key (optional, v1.x scope)
- Token Plan subscription credentials (if available)
- Expected responses: quota snapshots, usage data

## MiniMax Subscription Credential Requirements

These requirements are now satisfied by the shipped Settings / Accounts flow, Windows Credential Manager `secret_ref` storage, and runtime polling/validation support in the app. This section is retained as the Phase 0 decision record for why those requirements existed, not as an active implementation backlog.

For the current v1.0 MiniMax scope, JiStats needs these requirements in place:

### 1. Credential entry

- the app needs a Preferences / Accounts flow where the user can add a MiniMax subscription credential
- the first supported path should target MiniMax subscription credentials, not PAYG
- the UI should distinguish:
  - Coding Plan subscription key
  - Token Plan subscription key
  - PAYG API key as a later or separate path

### 2. Secure storage

- raw MiniMax secrets should not be stored in SQLite or plain-text settings
- the secret should be stored in Windows Credential Manager
- SQLite should only store:
  - provider/account metadata
  - credential type
  - secret reference (`secret_ref`)
  - plan association when known

### 3. Validation on entry

- when the user enters a Coding Plan credential, JiStats should validate it against `GET /v1/api/openplatform/coding_plan/remains`
- when the user enters a Token Plan credential, JiStats should validate it against `GET /v1/token_plan/remains`
- validation should detect and surface:
  - rejected credential
  - wrong surface / credential mismatch
  - network failure
  - temporarily unavailable provider endpoint

### 4. Runtime usage

- after validation, the sync/runtime layer must be able to retrieve the secret from Windows Credential Manager
- MiniMax subscription polling should use the stored credential reference rather than requiring manual re-entry
- the credential metadata must be rich enough to tell the scheduler which endpoint family to poll

### 5. UX behavior

- Preferences should show whether a MiniMax subscription credential is:
  - present
  - validated
  - stale / failed
  - missing
- JiStats should never imply that MiniMax subscription validation is complete if the user has not yet supplied the relevant credential

### 6. Scope sequencing

- Coding Plan subscription credential support is the first required MiniMax credential-management slice
- Token Plan support should follow closely because reset-window validation still depends on it
- PAYG API key entry can remain deferred behind the subscription-first v1.0 path

## Execution Steps

### Step 1: OpenAI Subscription Surface Validation

```
1. Inventory what the subscription user can actually access
   - account/billing pages
   - usage/history pages
   - exports
   - Codex-visible usage or quota surfaces

2. For each discovered surface
   - document whether it is provider truth, partial truth, or UI-only
   - capture fixture notes or screenshots/response artifacts where practical

3. Classify the subscription mirror outcome
   - exact signal available
   - estimated/local-only
   - unknown / unavailable

4. Update planning docs
   - mirror spec
   - capability matrix
   - metric glossary
   - collection strategy notes
```

### Step 2: OpenAI API Platform Validation (Optional)

```
1. Only run if separate API-platform access exists
2. Validate usage/cost/history endpoints on that distinct surface
3. Capture fixtures and freshness notes
4. Keep findings explicitly labeled as API-platform behavior, not subscription behavior
```

### Step 3: MiniMax Coding Plan Validation

```
1. Call GET /v1/api/openplatform/coding_plan/remains with subscription credential
   - Result: 200 + model_remains payload

2. Keep raw headers and responses in local storage
   - The fixture directory includes a synthetic Coding Plan example

3. Follow-up
   - Determine reset semantics from start_time/end_time behavior across intervals
```

### Step 4: MiniMax PAYG Validation (Deferred to v1.x)

```
1. Validate the dedicated PAYG-key request path
   - POST /v1/chat/completions (MiniMax-M2.7) -> 200 + immediate `usage` payload
   - GET /v1/usage -> 404
   - GET /api/v1/usage -> 404
   - GET /v1/models -> 404

2. Keep raw probe artifacts in local storage

3. Current conclusion
   - The validated PAYG signal is immediate per-request usage plus published pricing
   - No separate aggregate PAYG usage-history endpoint is currently validated
```

### Step 5: MiniMax Token Plan Validation

```
1. Call GET /v1/token_plan/remains with subscription credential
   - Result: 200 + model_remains payload

2. Keep raw artifacts in local storage
   - The fixture directory includes a synthetic Token Plan example
```

## Sample Response / Evidence Archive

See the [fixture inventory](fixtures/README.md) for the example payloads. Store provider captures in local storage and commit only synthetic or redacted artifacts that have been reviewed.

## Success Criteria

### OpenAI Subscription (must have for honest v1.0 scope)

- [x] The current subscription-first product behavior is documented accurately enough to avoid fake API assumptions
- [x] We know what JiStats can and cannot truthfully mirror in the current local-first subscription story
- [x] Unknown provider-side quota states are treated as a product behavior, not a hidden failure
- [x] The repo now records that Codex has official account-linked usage surfaces, but that consumer website scraping is still not the right product foundation
- [x] Phase 0 is sufficient to proceed to Phase 1 with local observation plus Codex-local augmentation as the first shipped OpenAI path

### OpenAI API Platform (optional)

- [x] API import is documented as a separate optional path rather than part of the base subscription story
- [x] API findings are not conflated with the base subscription story
- [ ] Successful repeated freshness/lag capture is still blocked on a higher-scope credential with `api.usage.read`

### MiniMax Coding Plan (must have for v1.0)

- [x] Subscription API exists and returns quota data (`coding_plan/remains`)
- [x] Token Plan remains endpoint also returns quota data (`token_plan/remains`)
- [x] Confirm reset-window semantics enough for current v1.0 runtime behavior through the accepted comparison-based evidence rule

### MiniMax PAYG (v1.x follow-on)

- [x] Validated PAYG request path returns usable per-request usage payload with a dedicated PAYG key
- [x] Can calculate per-request cost from returned token counts plus published pricing

## Go/No-Go Decision

After spike execution:

| Scenario | Decision |
|----------|----------|
| OpenAI subscription surface is documented enough for honest local-first tracking + MiniMax subscription remains APIs work | ✅ Proceed to Phase 1 with OpenAI subscription-first tracking + MiniMax subscription mirror |
| OpenAI subscription truth surface remains too unclear to describe honestly | ⚠️ Narrow OpenAI UX promises further, but do not fall back to fake API assumptions |
| Separate OpenAI API platform also validates | ✅ Add optional API-import path as a later enhancement |
| MiniMax subscription APIs fail for target accounts | ⚠️ Use local/manual fallback; reassess v1.0 scope impact |

Current outcome: the first scenario above is satisfied, so Phase 0 is a `Go` for the current OpenAI subscription-first + MiniMax subscription-first v1.0 baseline. The archived `2026-04-19` OpenAI API-platform authz probe is explicitly advanced-path follow-up rather than a reason to reopen Phase 0.

## Next Steps After Spike

1. Keep the OpenAI v1.0 baseline on the now-validated direction:
   - local observation baseline
   - Codex-local official signal augmentation
   - optional user-initiated ChatGPT export import
2. Treat broader OpenAI export-import work as follow-on refinement on top of the already-shipped extracted-export menu action and parser/storage seam:
   - optional `.zip` support
   - optional extracted-directory chooser support
   - any later reporting/polish around historical-import presentation
3. Treat any future direct Token Plan pre/post boundary run as confidence-refresh evidence, not a blocker to the current runtime lane.
4. Keep OpenAI API-platform validation explicitly optional and separate unless separate API billing/credentials are available; the only remaining blocked item there is successful freshness/lag capture after a higher-scope credential is available.
5. Keep broader MiniMax PAYG runtime support deferred until v1.x implementation work begins; the dedicated-key request path has been validated.
6. Do not pursue automated consumer website scraping as the primary v1.0 collection approach.

## References

- Phase 0 in `docs/plan.md`
- Capability matrix: `docs/capability-matrix.md`
- OpenAI mirror spec: `docs/openai-mirror-spec.md`
- MiniMax mirror spec: `docs/minimax-mirror-spec.md`
