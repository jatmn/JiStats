# Mirror Accuracy and Confidence Policy

> **Status**: Current
> **Purpose**: Define the v1.0 acceptance target for quota mirroring accuracy and confidence labeling
> **Driven by**: plan.md section 10.6.1 (Mirror Accuracy Tiers)

## The Problem

JiStats mirrors provider quota state, but the achievable fidelity depends on what provider signals can be validated. Users must be able to trust what they see and understand when a value is exact versus estimated.

## Core Principle

**Never present unvalidated quota state as exact.**

From plan.md section 10.6.1:
> Every mirrored quota output should carry confidence tier, basis summary, last refresh timestamp, and whether the value has been reconciled since the last offline period.

## Acceptance Target (v1.0)

The v1.0 minimum acceptable mirror-accuracy target is locked:

| Criterion | Requirement |
|-----------|-------------|
| Display-rounding tolerance | `<= 1` count unit OR `<= 1` percentage point |
| Unvalidated quota | Must display as `Estimated` or `Unknown` |
| Reconciled state | Must surface freshness + gap/backfill context |

### Tolerance Definition

For JiStats v1.0, normal display-rounding tolerance means:

- **Count values**: The mirrored value displayed to the user must match the authoritative source within `<= 1` count unit after refresh or import
- **Percentage values**: The mirrored percentage displayed to the user must match the authoritative source within `<= 1` percentage point after refresh or import

Example: If the provider shows "852 remaining" and JiStats shows "851" or "853", that is within tolerance. If JiStats shows "849" or "855", that is outside tolerance.

## Confidence Tiers and Display Labels

Mirror acceptance still uses the four core tiers defined in `docs/plan.md` section 10.6.1:

| Tier | Meaning | When to Use |
|------|---------|-------------|
| `Exact` | Derived from direct validated provider signals with no known inference gap | Provider API response directly populates the value, no transformation or inference |
| `HighConfidence` | Derived from validated logic with minor inferred timing or lag assumptions | Provider value adjusted for known lag, or reconciled within tolerance |
| `Estimated` | Derived from partial provider signals plus local inference | Local observation only, or provider data incomplete/stale |
| `Unknown` | Not enough information to produce a trustworthy mirrored state | Offline period with no backfill, provider endpoint unavailable |

The current repo display vocabulary also includes provenance-qualified exact-source labels from `docs/metric-glossary.md`:

| Label | Kind | Meaning |
|-------|------|---------|
| `LocalExact` | Provenance-qualified exact label | Exact for activity JiStats directly observed on this machine |
| `Imported` | Provenance-qualified imported label | Loaded from a separately validated provider or user-export surface |

Use these rules together:

- keep `Exact`, `HighConfidence`, `Estimated`, and `Unknown` as the core mirror-acceptance tiers
- use `LocalExact` and `Imported` when the display needs to preserve how an otherwise trustworthy value entered JiStats
- do not collapse `Estimated` or `Unknown` into `LocalExact`, `Imported`, or `Exact`

### Local-First Provider Evidence Rule

Local runtime artifacts can justify trustworthy local activity summaries before they justify provider-exact quota truth.

Use these rules:

- `LocalExact` may be used when JiStats is exact about activity it directly observed or about aggregate counters read from an approved local artifact
- `LocalExact` does **not** by itself mean the provider's quota-remaining state is exact
- if a provider's local artifacts do not cleanly separate billing mode, provider mode, or quota window semantics, quota state must remain `Estimated` or `Unknown`
- `Exact` remains reserved for direct validated provider-returned quota or limit signals
- `HighConfidence` may be used only when validated provider behavior plus bounded local inference keeps the displayed value within the accepted tolerance target

Practical consequence:

- local Claude or Kimi runtime evidence may support exact local activity counters while still leaving quota state non-exact
- JiStats must not promote those local counters into provider-exact quota truth without separate corroboration

## Display Rules

### Required Labels

| Label | Required Display |
|-------|------------------|
| `LocalExact` | May show as exact local activity/import context, but must still include freshness or capture context |
| `Imported` | May show as imported/provider-export context, but must still include freshness or artifact context |
| `Exact` | May show without qualification, but must include freshness timestamp |
| `HighConfidence` | Show value with "`HighConfidence`" or equivalent indicator |
| `Estimated` | Show value with "`Estimated`" badge; user must be able to see basis |
| `Unknown` | Show "`?`" or "`Unknown`" instead of a numeric value |

### Freshness Context

All quota displays must include:

- Last refresh timestamp (when the value was last updated from a provider or local source)
- Gap/backfill context if applicable (e.g., "Includes 2 days of backfilled data")

## Reconciliation Behavior

### OpenAI Reconciliation

From `docs/openai-mirror-spec.md`:

- Compare local events sum vs imported daily total
- If within `<= 1` count unit or `<= 1%`: mark `HighConfidence`
- If mismatch exceeds tolerance: investigate duplicate or missing events
- If gap in provider data: mark affected periods `Unknown`

### MiniMax Reconciliation

From `docs/minimax-mirror-spec.md`:

- Subscription remains endpoints are authoritative quota truth
- Compare mirrored remaining vs provider remaining after refresh
- If within tolerance: mark `Exact` or `HighConfidence` as appropriate
- If tolerance exceeded: mark `Estimated` and log diagnostic

## Provider-Specific Accuracy Notes

### OpenAI (Subscription-First)

| Source | Display Label | Accuracy Basis |
|--------|---------------|----------------|
| Local Codex rollout observation | `LocalExact` | Exact for captured activity events |
| ChatGPT export import | `Imported` | Exact for exported artifact, not live quota |
| API platform usage (optional) | `Exact` | Direct from provider API |

### MiniMax Subscription

| Source | Display Label | Accuracy Basis |
|--------|---------------|----------------|
| Coding Plan remains | `Exact` | Direct from `/v1/api/openplatform/coding_plan/remains` |
| Token Plan remains | `Exact` | Direct from `/v1/token_plan/remains` |
| Local observation only | `Estimated` | No direct provider confirmation |

### Claude Code Local-First Path

| Source | Display Label | Accuracy Basis |
|--------|---------------|----------------|
| `~/.claude/stats-cache.json` aggregates | `LocalExact` | Exact for locally recorded aggregate activity and token counters |
| Claude local-only quota interpretation | `Estimated` or `Unknown` | Local artifacts do not yet prove authoritative quota truth or billing/provider mode separation |

### Kimi Local-First Path

| Source | Display Label | Accuracy Basis |
|--------|---------------|----------------|
| Approved Kimi runtime/task/notification metadata | `LocalExact` | Exact for the local runtime metadata JiStats can read safely |
| Kimi local-only token or quota interpretation without `/usages` corroboration | Not surfaced as quota truth | Local artifacts alone do not justify provider-exact quota truth, so JiStats treats them as activity/history evidence only |

## What This Policy Prohibits

- Presenting a locally inferred quota value as `Exact`
- Presenting a local activity aggregate as if it automatically proves provider-side remaining quota
- Displaying a value derived from stale provider data without freshness indicator
- Hiding the `Estimated` or `Unknown` label when conditions require it
- Claiming tolerance compliance when the displayed value differs by more than `1` unit or `1` percentage point

## Measurement and Validation

Mirror accuracy should be validated through:

1. **Fixture tests**: Known provider response sets replayed through the mirror engine
2. **Live comparison**: Manual or automated comparison of mirrored values vs provider dashboard
3. **Reconciliation logs**: Automated logging of tolerance-check results

## References

- Mirror accuracy tiers: `docs/plan.md` section 10.6.1
- OpenAI mirror spec: `docs/openai-mirror-spec.md`
- MiniMax mirror spec: `docs/minimax-mirror-spec.md`
- Metric glossary (confidence vocabulary): `docs/metric-glossary.md`
- Claude local-surface spike: `docs/claude-local-surface-spike.md`
- Kimi local parser spike: `docs/kimi-local-parser-spike.md`
