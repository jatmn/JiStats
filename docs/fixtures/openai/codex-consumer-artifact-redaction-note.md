# Codex Consumer Artifact Redaction Note

> **Status**: Safe archived fixture note for Lane 2 support work
> **Purpose**: Record the exact consumer-facing Codex artifact shape JiStats wants to preserve without committing raw account-linked data

This note complements `docs/fixtures/openai/codex-rate-limits-shape.sample.json`.

Unlike the rate-limit sample, this artifact is intentionally **consumer-surface oriented**. It exists to keep future fixture capture and review work disciplined before any parser or import path is widened.

## What This File Is

- a commit-safe note describing the preferred redacted Codex consumer artifact
- a normalized field inventory for a Codex usage-dashboard capture
- a redaction contract future engineers can follow when a safe real capture is available

## What This File Is Not

- not a raw live user export
- not a screenshot containing account-linked data
- not proof that JiStats already has a validated local Codex artifact contract

## Preferred Capture Target

The preferred consumer artifact to archive next is:

- **Codex Settings -> Usage Dashboard**

Reason:

- OpenAI publicly documents this surface as showing credits balance and recent usage
- it is Codex-specific, which matches the current v1.0 augmentation direction
- it avoids pretending that broader ChatGPT subscription truth is already known

Reference:

- [Using Credits for Flexible Usage in ChatGPT (Free/Go/Plus/Pro) & Sora](https://help.openai.com/articles/12642688)

## Required Redaction Rules

Before a real artifact is committed, the following must be removed or replaced:

- account email addresses
- full names
- workspace names when unique to a user or employer
- avatar images
- billing identifiers
- precise credit purchase history if it reveals account linkage not needed for JiStats
- browser URL fragments, tokens, request IDs, cookies, or auth material
- exact timestamps if they are uniquely identifying and not needed for quota-window reasoning

## Safe Fields To Preserve

These are the fields JiStats actually cares about for evidence and planning:

- surface name, such as `Usage Dashboard`
- whether the surface is clearly Codex-specific
- presence of a credits balance field
- presence of recent-usage summaries
- visible quota-window wording if present
- visible reset wording if present
- any explicit labels separating balance, recent usage, and billing/top-up concepts
- relative freshness cues such as `recent usage` rather than user-identifying session detail

## Normalized Capture Template

When a safe real capture is archived later, it should be normalized into a text note or JSON transcript with a shape close to this:

```json
{
  "artifact_type": "codex_usage_dashboard_redacted",
  "capture_date_utc": "YYYY-MM-DD",
  "surface_label": "Codex Settings > Usage Dashboard",
  "account_scope": "[redacted]",
  "workspace_scope": "[redacted-or-none]",
  "shows_credits_balance": true,
  "shows_recent_usage": true,
  "shows_reset_hint": true,
  "visible_fields": [
    "credits_balance",
    "recent_usage",
    "window_label",
    "reset_hint"
  ],
  "redactions_applied": [
    "email",
    "name",
    "workspace_name",
    "precise_identifiers"
  ],
  "notes": [
    "Codex-specific surface, not universal ChatGPT subscription truth"
  ]
}
```

## Why A Note Is Archived First

Lane 2 is support and quality work, so the goal here is to:

- define what a safe capture must look like
- prevent accidental storage of sensitive consumer data
- keep Lane 1 parser work from being blocked on undocumented fixture expectations

This means the repo now has a **fixture-ready redaction contract** even though it still does not contain a raw live consumer artifact.

## Downstream Use

This note is intended to support:

- future Codex-local fixture capture review
- fixture acceptance criteria
- parser/test backlog planning
- clearer separation between `Codex-specific provider truth` and broader `OpenAI subscription unknown`

## Related Files

- `docs/fixtures/openai/subscription-surface-notes.md`
- `docs/fixtures/openai/codex-rate-limits-shape.sample.json`
- [the ChatGPT export import contract](../../providers/openai.md#import-chatgpt-history)
- [Subscription augmentation fixture guide](subscription-augmentation-fixture-guide.md)
