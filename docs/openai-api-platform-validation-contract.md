# OpenAI API-Platform Validation Contract

> **Status**: Official endpoint/auth inventory complete; first authorization result summarized, while successful usage/cost capture still depends on a credential with the required organization usage scopes
> **Purpose**: Define the exact optional OpenAI API-platform probe boundary so JiStats keeps API-import work distinct from the subscription-first OpenAI story

This note closes the Worktree A endpoint-inventory task for the optional OpenAI API-platform path.

It now records both the intended probe boundary and the first live authz result captured on `2026-04-19`.

Current live evidence baseline:

- `GET /v1/organization/usage/completions` and `GET /v1/organization/costs` are reachable with a real API-platform key
- the tested key returned `403` on both endpoints with `Missing scopes: api.usage.read`
- freshness / lag measurement is still blocked until a credential with the required org-scoped read permission exists

## Credential Boundary

The OpenAI API-platform path is separate from ChatGPT or Codex subscription access.

JiStats should assume all of the following are independently required before live validation:

- OpenAI API billing enabled on `platform.openai.com`
- an API credential with permission to read organization usage/cost data
- the correct organization selected for the request when the user belongs to more than one organization

Official OpenAI docs currently show organization usage/cost examples authenticated with a bearer token in the form `OPENAI_ADMIN_KEY`.

JiStats should therefore treat this path as:

- optional
- organization-scoped
- separate from subscription tracking
- blocked until the user has distinct API-platform access

## Minimum Probe Set

JiStats should start with the smallest API-platform probe set that can answer the v1.x import questions cleanly.

### 1. Usage history

- `GET /v1/organization/usage/completions`

Use this as the first required usage-history probe because it covers the primary text/model usage path JiStats already reasons about.

Expected contract from the official reference:

- required query:
  - `start_time`
- optional queries:
  - `end_time`
  - `bucket_width`
  - `group_by`
  - `limit`
  - `page`
  - `project_ids`
  - `user_ids`
  - `api_key_ids`
  - `models`
  - `batch`

Expected response shape:

- page-style top-level object
- time buckets with:
  - `start_time`
  - `end_time`
  - `results`
- grouped usage rows under each bucket

JiStats should treat this endpoint as the first API-platform source for:

- imported request history
- imported token history
- optional model/project grouping

### 2. Cost history

- `GET /v1/organization/costs`

Use this as the first required financial reconciliation probe.

The official reference explicitly says the Costs endpoint is the better source for financial reconciliation because usage and cost recording may differ slightly.

Expected contract from the official reference:

- required query:
  - `start_time`
- optional queries:
  - `end_time`
  - `bucket_width`
  - `group_by`
  - `limit`
  - `page`
  - `project_ids`

Expected response shape:

- page-style top-level object
- time buckets with:
  - `start_time`
  - `end_time`
  - `results`
- cost rows with:
  - `amount.value`
  - `amount.currency`
  - `line_item`
  - `project_id`

JiStats should treat this endpoint as the first API-platform source for:

- imported spend history
- invoice-aligned cost reporting
- project or line-item grouping where available

## Optional Secondary Usage Endpoints

JiStats should only expand beyond the minimum probe set when the user actually uses those surfaces or when v1.x reporting needs them.

Official OpenAI usage docs currently expose additional organization usage endpoints for categories such as:

- embeddings
- moderations
- images
- code interpreter sessions

Inference:

JiStats should not probe every category by default in the first validation pass. The first probe should stay focused on completions plus costs, then expand only when the user has evidence that those categories matter.

## Freshness/Lag Validation Boundary

The next live validation step, once credentials exist, is:

1. call the minimum probe set repeatedly over a short window
2. compare bucket timestamps and returned totals against recent known API activity
3. record the practical lag between activity and visible usage/cost reporting

That remains separate Worktree A task `A0.2`.

## Source-Label Rule

Any data imported from this path must keep the existing `openai_api_import` provenance label.

It must never overwrite or masquerade as:

- `openai_subscription_local`
- `openai_subscription_codex_local`
- `openai_subscription_export_import`

## Fixture / Evidence Target

Once live API-platform access exists, store raw evidence outside the repository. The capture helper defaults to:

- `%LOCALAPPDATA%/JiStats/captures/openai/api-platform/`

That evidence should include:

- one usage response sample
- one costs response sample
- one short freshness/lag note
- any required redaction contract for organization/project identifiers

The repo now includes a capture helper for that step:

- `scripts/capture_openai_api_platform_evidence.ps1`

Use it only when separate API-platform billing and credentials are present. It is meant to make Worktree `A0.2` a one-command evidence/archive pass rather than an ad hoc manual curl session.

The helper now writes:

- `usage-<timestamp>.json`
- `costs-<timestamp>.json`
- `capture-<timestamp>.metadata.json`
- `capture-<timestamp>.notes.md`

The notes file is the intended place to record the short freshness/lag summary plus any organization/project identifier redaction notes after each live capture.

## References

- OpenAI Usage API reference: `https://platform.openai.com/docs/api-reference/usage/costs`
- OpenAI organization/auth guidance: `https://platform.openai.com/docs/guides/production-best-practices/setting-up-your-organization`
