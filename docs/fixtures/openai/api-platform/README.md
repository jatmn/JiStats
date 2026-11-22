# OpenAI API-Platform Capture

This directory documents the optional OpenAI API-platform probe. The initial probe reached both endpoints and received HTTP 403. Usage and cost freshness remain unverified.

It is intentionally separate from the subscription-first OpenAI fixture set so JiStats does not mix:

- `openai_subscription_local`
- `openai_subscription_codex_local`
- `openai_subscription_export_import`
- `openai_api_import`

## Local output files

The helper writes `readiness-<timestamp>.json` before checking whether a credential is available. If it can make the requests, it also writes:

- `usage-<timestamp>.json`
- `costs-<timestamp>.json`
- `capture-<timestamp>.metadata.json`
- `capture-<timestamp>.notes.md`

## Capture Command

Run the helper from the repository root with `-BucketWidth 1d`. The helper sends the same bucket width to both requests, and the Costs endpoint only accepts daily buckets. [Costs API reference](https://developers.openai.com/api/reference/resources/admin/subresources/organization/subresources/usage/methods/costs)

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\capture_openai_api_platform_evidence.ps1 -BucketWidth 1d
```

The helper:

- reads `OPENAI_ADMIN_KEY` first, then falls back to `OPENAI_API_KEY`
- optionally sends `OpenAI-Organization` when `OPENAI_ORG_ID` or `OPENAI_ORGANIZATION` is set
- queries both organization endpoints:
  - `GET /v1/organization/usage/completions`
  - `GET /v1/organization/costs`
- writes raw JSON, a small metadata file recording request parameters and status codes, and a note template for freshness/lag plus any identifier redaction notes

## Current Blocker

The first live probe returned `403` from both organization endpoints because the credential lacked `api.usage.read`.

To validate freshness, use a credential with `api.usage.read`, the correct organization-scoped role, and explicit organization context when needed. See the [provider validation guide](../../../providers/openai-api-platform.md) for what to record.

The capture helper writes raw output under `%LOCALAPPDATA%/JiStats/captures/openai/api-platform` by default. Keep those files local.
