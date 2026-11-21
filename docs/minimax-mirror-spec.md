# MiniMax Quota Mirroring Target Behavior

> **Status**: v1.0 scope validated for subscription mirrors; dedicated PAYG-key validation now confirms immediate per-request usage on the OpenAI-compatible text API, while separate aggregate PAYG history remains unvalidated for v1.x
> **Scope Change Flag**: PAYG is explicitly deferred from v1.0 requirements
> **Purpose**: Defines what MiniMax quota mirror v1.0 should achieve and how to measure it
> **Complexity**: Higher than OpenAI due to two credential types and multiple plan types
> **Driven by**: `docs/capability-matrix.md` capability hypothesis + Phase 0 API spike
> **API Validation**: 2026-04-17 spike confirmed `coding_plan/remains` and `token_plan/remains`; 2026-04-19 dedicated PAYG validation confirmed per-request completion `usage` plus continued `404` on older candidate history endpoints

## What We Know About MiniMax

From validated fixtures:
- **Subscription endpoints**: `GET /v1/api/openplatform/coding_plan/remains` and `GET /v1/token_plan/remains` return quota payloads (`model_remains`)
- **Key separation**: Subscription keys are valid for subscription-quota endpoints
- **PAYG request path**: dedicated PAYG key works on `POST /v1/chat/completions`, and the response includes `usage.prompt_tokens` / `usage.completion_tokens`
- **PAYG history/discovery paths**: `/v1/usage`, `/api/v1/usage`, and `/v1/models` still returned `404` even with a dedicated PAYG key
- **App path**: JiStats now has an in-app subscription credential path for Coding Plan and Token Plan under Settings / Accounts, including asynchronous validation and retained masked-secret presentation after success
- **Evidence-log support**: `docs/fixtures/minimax/reset-window-evidence-log.md` now records the accepted reset-window closeout for the current runtime lane and remains the structured place to add any future confidence-refresh captures, while `docs/fixtures/minimax/reset-window-capture-inputs.md` defines both the primary direct-capture path and the accepted comparison path

## The Three MiniMax Surfaces

### 1. Pay-As-You-Go API (v1.x Deferred)

Similar model to OpenAI API:
- Direct API access with usage tracking
- Per-request cost calculation
- Rate limits on requests

**Validated signal**: immediate per-request `usage` on successful OpenAI-compatible text completions.

**Current limitation**: no separate aggregate PAYG usage-history endpoint is validated yet.

**Mirror approach (v1.x)**: Track local events and imported per-request `usage`; add periodic provider-side history fetch only if a real aggregate endpoint is later documented and validated.

### 2. Token Plan (Subscription)

Windowed token allowance:
- returns `start_time` / `end_time` counters for the current interval
- also returns weekly fields in the captured remains payload
- exact direct same-surface rollover proof is still incomplete, but current v1.0 runtime parity is now accepted through archived-pre/live-post comparison against the Coding Plan rollover proof

**Validation status**: API endpoint validated (`/v1/token_plan/remains`) with structured remains payload.

Single archived remains payloads already show multiple interval families across models. JiStats should keep period labeling model-aware, but the current runtime lane no longer requires a direct same-night Token Plan boundary run because the archived Token Plan pre-boundary shape plus the current live Token Plan post-boundary capture now line up with the Coding Plan rollover proof.

### 3. Coding Plan (Subscription)

Request-window quota:
- interval/window counters are present in the remains payload
- weekly companion fields are also present
- the `5h` family now has a direct live rollover proof

**Validated signals**: per-model interval totals, interval usage, remaining time, and weekly counters in `model_remains`.

Single archived remains payloads already show multiple interval families across models, so JiStats should keep labels model-aware and avoid collapsing Coding Plan into one plan-wide period beyond the now-confirmed `5h` family.

## Quota Mirror Target Behavior by Surface

### Pay-As-You-Go

| Quota Type | Target Behavior | Confidence Tier |
|------------|-----------------|-----------------|
| Usage history | Imported per-request usage from successful completions; no separate aggregate history API currently validated | `Imported` / `LocalExact` |
| Cost tracking | Per-request calculation from returned token counts plus published pricing | `Estimated` with high-confidence inputs |
| Rate limit visibility | From headers on successful OpenAI-compatible calls if available | `Exact` per request |

### Token Plan (Subscription)

| Quota Type | Target Behavior | Confidence Tier |
|------------|-----------------|-----------------|
| Monthly/interval allowance | Returned in `model_remains` counters | `Exact` |
| Consumed usage | Returned in `current_interval_usage_count` | `Exact` |
| Remaining balance/time | Returned in `remains_time` and derived counters | `Exact` / `Estimated` |
| Reset behavior | Treat the 5h family as high-confidence from Coding Plan live proof plus Token Plan archived/live comparison; keep labels model-aware until contradictory evidence appears | `HighConfidence` |

### Coding Plan (Subscription)

> **v1.0 Priority Surface** - endpoint-level validation completed (2026-04-17)

| Quota Type | Target Behavior | Confidence Tier |
|------------|-----------------|-----------------|
| Request window quota | Read from `current_interval_total_count` | `Exact` |
| Current period usage | Read from `current_interval_usage_count` + local events | `Exact` / `LocalExact` |
| Reset behavior | Treat the `5h` family as confirmed from direct live rollover proof; keep broader plan-wide labels model-aware until more interval families are proven | `HighConfidence` for the `5h` family, `Unknown` for broader generalization |
| Quota limit | Read from response counters, optionally cross-checked with plan metadata | `Exact` |

**Confirmed:** Coding/Token subscription quota APIs exist and return structured quota signals. Previous "no subscription quota API" assumption is invalid.

## Key Differences from OpenAI

| Aspect | OpenAI | MiniMax |
|--------|--------|---------|
| Real-time usage | ❌ None | ⚠️ Windowed counters validated for subscription; current `5h` family parity is confirmed, broader family generalization remains model-aware |
| Subscription quota API | ⚠️ Limited | ✅ Validated for Coding/Token remains |
| Key types | Single API key | Separate PAYG vs subscription keys |
| Plan types | Flat rate | Token Plan + Coding Plan |
| Intra-day tracking | Local events | Local events only |

## MiniMax-Specific Challenges

### Challenge 1: Credential Separation

User may have:
- MiniMax API key for PAYG
- MiniMax subscription account for Token Plan
- MiniMax subscription account for Coding Plan

Each requires different authentication and may have different quota semantics.

**Implication**: JiStats needs to track 3 potential credential types per user, not just 1.

For the current v1.0 path, that means:

- Coding Plan and Token Plan credentials now use a real app-owned settings/accounts flow
- secrets now live in Windows Credential Manager via `secret_ref`
- local persistence should store only metadata and secret references
- the shipped credential UX baseline is now stable enough for repeated live reset-window capture and confidence-refresh work

### Challenge 2: Subscription Quota Visibility

If subscription API access fails for a specific account:
- fall back to local event tracking
- optionally allow manual refresh/check path
- keep confidence tier explicit (`Unknown` or `Estimated`)

### Challenge 3: Plan Complexity

- Token Plan: direct same-surface rollover proof is still a useful confidence refresh, but the current runtime/test checkpoint already accepts the archived-pre/live-post comparison against Coding Plan
- Coding Plan: Request-window semantics are visible in payloads, and the `5h` family rollover is now directly proven; broader interval-family labeling still needs care

Each plan type may need custom mirror logic.

## Intra-Day Tracking Strategy

For Coding Plan subscription (first-priority path for MiniMax v1.0):
1. Track local events for Coding Plan requests
2. Poll `coding_plan/remains` endpoint on schedule
3. Persist quota snapshots from `model_remains` counters
4. Reconcile API snapshots against local request windows

For PAYG (v1.x deferred path):
1. Track local events
2. Import per-request `usage` from successful completions and derive cost from published pricing
3. Add periodic provider-side history sync only if a separate aggregate PAYG endpoint is later documented and validated

For Token Plan and other subscriptions (v1.0 or v1.x based on final scope decision):
1. Use validated `token_plan/remains` endpoint where applicable
2. Track local events for near-real-time overlays
3. Mark confidence per metric based on source (`API` vs `Local`)

## Confidence Classification Rules

For MiniMax, use the same canonical tier set as the OpenAI subscription-first path:

| Condition | Tier |
|-----------|------|
| PAYG: Valid API response with usage data | `Imported` |
| Subscription: API returns counters for the current window | `Exact` for returned counters |
| PAYG: Local events only, no API | `LocalExact` for captured events, `Estimated` for derived mirror state |
| Subscription: Local overlay between validated API snapshots | `LocalExact` / `Estimated` |
| Subscription: Current `5h` family cadence proven, but broader family generalization still unvalidated | `HighConfidence` for current `5h`-family runtime parity, `Unknown` for broader generalization |
| Subscription: No API, dashboard only | `Unknown` |
| Any: Offline gap | `Unknown` |

## Mirror Output Contract

Same structure as OpenAI, with MiniMax-specific fields:
```json
{
  "provider": "minimax",
  "surface_type": "payg | token_plan | coding_plan",
  "dimension": "daily_cost| daily_tokens| requests| remaining_balance",
  "value": <number>,
  "confidence_tier": "LocalExact|Imported|Exact|Estimated|Unknown",
  "basis_summary": "API" | "Local events" | "Dashboard (manual)" | "Unknown",
  "last_refresh_timestamp": <UTC>,
  "is_reconciled": <boolean>,
  "minimax_specific": {
    "plan_type": "token | coding | payg",
    "quota_window": "daily | monthly | rolling"
  }
}
```

## Open Questions for Validation

1. **PAYG API**: Is there any separate aggregate usage-history endpoint beyond the validated per-request completion `usage` payload?
2. **PAYG Auth**: Dedicated PAYG keys work on the OpenAI-compatible text API and remain distinct from subscription remains credentials; if broader hosts/paths exist, they still need documentation.
3. **Reset behavior semantics**: Confirm whether interval families beyond the current accepted `5h` runtime rule can be generalized more broadly or must remain separately labeled.
4. **Webhook availability**: Any real-time signals for usage or quota?

The remaining reset-window question should only be considered reopened if fresh live evidence contradicts the current Coding Plan rollover proof plus Token Plan archived/live comparison recorded in `docs/fixtures/minimax/reset-window-evidence-log.md`.

## v1.0 Scope Decision

**Decision (updated from validated spike)**: MiniMax subscription remains the first-priority v1.0 surface and has usable quota APIs for Coding/Token plan remains.

**Sequencing**:
1. **v1.0 priority**: Implement API-backed subscription mirror using validated remains endpoints
2. **v1.0 implementation**: Combine API quota snapshots with local event overlays
3. **v1.x**: Evaluate PAYG API coverage and add PAYG mirror support

**Coding/Token Plan v1.0 approach**:
- Use `coding_plan/remains` and `token_plan/remains` as primary quota sources
- Keep local event tracking for intra-window visibility and gap handling
- Fall back to manual checks only when endpoint access fails for a given account
- Clearly label confidence tier based on actual signal source
- Use the shipped MiniMax subscription credential-management slice as the baseline prerequisite for clean in-product validation and polling

**PAYG evaluation needed**:
- Decide whether v1.x PAYG mirroring should use per-request captures plus pricing as the primary baseline
- Continue watching for any separately documented aggregate PAYG usage-history endpoint before adding scheduled provider-side PAYG sync

## References

- Capability matrix: `docs/capability-matrix.md`
- OpenAI mirror spec (for comparison): `docs/openai-mirror-spec.md`
- MiniMax-specific planning notes: `docs/plan.md` section 9.0.2
