# OpenAI Subscription Tracking And Mirror Target Behavior

> **Status**: Re-scoped planning - subscription-first, with Codex-specific official usage surfaces now identified
> **Purpose**: Defines what OpenAI v1.0 should achieve for ChatGPT/Codex subscription tracking without assuming API-platform access
> **Driven by**: `docs/capability-matrix.md` and the product requirement to support subscription usage tracking honestly

## Scope Clarification

For OpenAI, JiStats must distinguish between two different surfaces:

1. **ChatGPT / Codex subscription surface**
   - Consumer or workspace subscription access
   - The user's primary product in scope for v1.0
   - Does **not** automatically imply API billing or an API key

2. **OpenAI API platform surface**
   - Separate billing and credentials
   - Optional advanced import path
   - Useful when available, but not the default OpenAI assumption for v1.0

This document defines the **base OpenAI v1.0 behavior** around the subscription surface first.

## Confirmed Official Subscription-Visible Surfaces

As of `2026-04-18`, the repo has public-source evidence for these OpenAI subscription-visible surfaces:

- ChatGPT account and billing pages for subscription users
- ChatGPT Data Export, available through account settings and the Privacy Portal for consumer plans
- Codex Settings -> Usage Dashboard, which OpenAI documents as showing credits balance and recent usage
- The public `openai/codex` app-server ChatGPT rate-limit shape, which documents `usedPercent`, `windowDurationMins`, and `resetsAt`

Important limits on that evidence:

- there is still no validated public consumer API for general ChatGPT subscription usage/quota
- the official Enterprise/Edu Compliance API and workspace analytics are separate workspace features, not the default consumer path
- OpenAI Terms of Use prohibit automatically or programmatically extracting data from the consumer service, so automated website scraping is not an acceptable product foundation

## OpenAI Subscription Mirror Goal

JiStats should help a subscriber understand:

- recent locally observed OpenAI activity
- longer-term local usage history
- whether current provider-side quota state is known, estimated, or unknown
- whether any optional imported API data is present and clearly distinct from subscription data

The app must not present API-derived organization history as if it were the user's default subscription truth source.

## What We Can Reliably Track In v1.0

### Exact Local Observations

| Dimension | Target Behavior | Confidence Tier |
|-----------|-----------------|-----------------|
| Recent locally observed activity | Exact for activity JiStats directly captures on this machine | `LocalExact` |
| Historical locally observed activity | Exact within periods the app observed and stored | `LocalExact` |
| User-initiated export import | Exact for the exported historical artifact, but not live | `Imported` |
| Codex-local usage / window metadata | Imported only when sourced from an official Codex-local/account-linked signal | `Imported` |
| Launch gaps / offline periods | Explicitly labeled as known, reconstructed, or unknown | `GapAware` |

### Truthful Unknowns

| Dimension | Target Behavior | Confidence Tier |
|-----------|-----------------|-----------------|
| General ChatGPT subscription quota remaining | Show `Unknown` unless a validated provider-side signal exists | `Unknown` |
| Reset-window pressure outside validated Codex/UI signals | Show `Unknown` until validated from official subscription behavior | `Unknown` |
| Provider-side usage totals | Show `Unknown` unless imported from a validated, explicitly separate source | `Unknown` |

## What We Must Not Pretend To Mirror

Until validated otherwise, JiStats must not claim exact OpenAI subscription-side values for:

- messages remaining
- exact quota remaining
- exact reset time
- exact provider-side usage totals
- exact provider-side cost totals

If a signal is not exposed or validated, the correct product behavior is:

- keep the local timeline useful
- mark provider truth as unavailable
- explain the confidence level instead of inventing a mirror

## Recommended v1.0 Collection Strategy

1. **Local observation baseline**
   - Keep `LocalObservationService` as the always-available OpenAI subscription baseline.

2. **Codex-local official signal augmentation**
   - Prefer official local/account-linked Codex signals when available.
   - Acceptable reference targets include documented local Codex rate-limit metadata and other official Codex-local artifacts.
   - Treat those signals as `Codex-specific`, not universal ChatGPT subscription truth.

3. **User-initiated ChatGPT export import**
   - Support import of official ChatGPT exports as a historical enrichment path.
   - Treat import freshness as on-demand and user-driven rather than live.
   - Follow `docs/openai-export-import-contract.md` for accepted inputs, provenance checks, and confidence labeling.

4. **Explicit non-goal**
   - Do not build v1.0 around automated scraping of `chatgpt.com`, captured bearer tokens, or undocumented `backend-api/*` endpoints.

## Optional Advanced Path: Separate API Import

If a user independently has OpenAI API-platform access, JiStats may later support:

- imported organization or project history
- imported API cost and usage reconciliation
- separation between API usage and subscription usage in both storage and UI

That path is **optional** and must remain clearly distinct from subscription-only tracking.

## Confidence Classification Rules

| Condition | Tier |
|-----------|------|
| Directly observed on this device with no gap | `LocalExact` |
| Reconstructed from local evidence with a known gap policy | `Estimated` |
| Imported from a separately validated provider surface | `Imported` |
| No trustworthy provider-side or local basis exists | `Unknown` |

## v1.0 OpenAI UX Contract

For OpenAI subscription users, the app should:

1. show recent local activity confidently
2. preserve local history across launches where possible
3. surface provider-side quota state only when validated
4. otherwise show `Unknown` or `Estimated`, never a fake exact quota state
5. distinguish optional API imports from subscription tracking if both ever coexist

## Accuracy Goals

v1.0 targets for the OpenAI subscription path:

- **Locally observed activity accuracy**: exact for captured events
- **Gap handling accuracy**: explicit distinction between known, reconstructed, and unknown periods
- **Provider-side quota honesty**: never label an unvalidated subscription quota value as exact

## Mirror Output Contract

Every OpenAI output should include a concrete source label.

The current normalized provider-surface set is:

- `openai_subscription_local`
- `openai_subscription_codex_local`
- `openai_subscription_export_import`
- `openai_api_import`

`openai_subscription` remains a useful high-level product category, but it is too broad to use as the concrete provenance label in mirror output.

Every OpenAI output should include:

```json
{
  "provider_surface": "openai_subscription_local|openai_subscription_codex_local|openai_subscription_export_import|openai_api_import",
  "dimension": "recent_activity|local_usage_total|quota_remaining|reset_window_pressure",
  "value": "<number or null>",
  "confidence_tier": "LocalExact|Imported|Estimated|Unknown",
  "basis_summary": "Local observation|Imported provider data|No validated provider-side signal",
  "last_refresh_timestamp": "<UTC>",
  "is_reconciled": false
}
```

Current implementation note:

- the documentation now uses the normalized source-label set above
- runtime/reporting now has a first landed adoption slice for explicit provider-surface propagation on the current Codex-local quota path plus HTML export/report output
- broader adoption is still follow-on work for future OpenAI import/history paths and any remaining surfaces that do not yet preserve the label end to end

## Open Questions

1. Which OpenAI subscription augmentation should ship first: Codex-local rate-limit/session parsing, user-export import, or both?
2. What redacted consumer artifact can be safely archived next from the Codex usage dashboard to complement the public doc-derived rate-limit shape?
3. How should the UI distinguish `Codex-specific provider truth` from broader `OpenAI subscription unknown` states?
4. If API import is enabled separately, how should the UI keep subscription and API usage distinct?

## References

- Capability matrix: `docs/capability-matrix.md`
- Scope decision: `docs/openai-scope-decision.md`
- Gap policy: `docs/gap-handling-policy.md`
- Export/import contract: `docs/openai-export-import-contract.md`
