# OpenAI API-Platform Capture

> **Status**: Capture helper ready; the first live API-platform probe returned an authorization error, but usage/cost freshness is still blocked until a credential with `api.usage.read` exists

This directory is reserved for the optional OpenAI API-platform evidence set.

It is intentionally separate from the subscription-first OpenAI fixture set so JiStats does not mix:

- `openai_subscription_local`
- `openai_subscription_codex_local`
- `openai_subscription_export_import`
- `openai_api_import`

## Expected Files

When the optional API-platform path is available, this folder should contain:

- `readiness-<timestamp>.json`
- `usage-<timestamp>.json`
- `costs-<timestamp>.json`
- `capture-<timestamp>.metadata.json`
- `capture-<timestamp>.notes.md`

## Capture Command

Use the repo helper:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\capture_openai_api_platform_evidence.ps1
```

The helper:

- reads `OPENAI_ADMIN_KEY` first, then falls back to `OPENAI_API_KEY`
- optionally sends `OpenAI-Organization` when `OPENAI_ORG_ID` or `OPENAI_ORGANIZATION` is set
- captures the minimum Worktree A probe set:
  - `GET /v1/organization/usage/completions`
  - `GET /v1/organization/costs`
- writes raw JSON, a small metadata file recording request parameters and status codes, and a note template for freshness/lag plus any identifier redaction notes

## Current Blocker

The first live probe returned `403` from both organization endpoints because the credential lacked `api.usage.read`.

That means the directory already serves two purposes:

- a local `readiness-*.json` record for authorization and credential checks
- future successful usage/cost capture once the higher-scope credential exists

Freshness / lag is still blocked because the tested credential lacks:

- `api.usage.read`
- the correct organization-scoped role and, when needed, explicit org context

Successful usage and cost responses still need validation with a credential that has the required scope.

The capture helper writes raw output under `%LOCALAPPDATA%/JiStats/captures/openai/api-platform` by default. Keep those files local.
