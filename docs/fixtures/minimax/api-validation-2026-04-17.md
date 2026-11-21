# MiniMax API Validation Results

**Date:** 2026-04-17
**Credential Handling:** API key used during live validation and redacted from all stored artifacts.
**Historical note:** the PAYG-specific conclusion in this file is superseded by the dedicated-key validation summarized in the [fixture inventory](README.md).

## API Base URL

- Confirmed hosts used for live tests: `https://api.minimax.io` and `https://www.minimax.io`
- Reference docs:
  - OpenAI-compatible text API: `https://platform.minimax.io/docs/api-reference/text-openai-api`
  - Coding/Token plan FAQ endpoint reference: `https://platform.minimax.io/docs/coding-plan/faq`

## Validation Summary

| Endpoint | Method | HTTP | Result Summary |
| --- | --- | --- | --- |
| `/v1/api/openplatform/coding_plan/remains` | GET | 200 | Returns `model_remains` + `base_resp.status_code=0` |
| `/v1/token_plan/remains` | GET | 200 | Returns `model_remains` + `base_resp.status_code=0` |
| `/v1/usage` | GET | 404 | Not found for tested credential path |
| `/api/v1/usage` | GET | 404 | Not found for tested credential path |
| `/v1/models` | GET | 404 | Not found |
| `/v1/chat/completions` (`model=MiniMax-Text-01`) | POST | 500 | Auth accepted, model not supported for this plan (`2061`) |
| `/v1/chat/completions` (`model=MiniMax-M2.7`) | POST | 200 | Successful completion + usage payload returned |

## Confirmed Findings

1. MiniMax subscription quota endpoints are real and callable with the tested subscription credential.
2. Both coding/token remains endpoints returned structured quota payloads (per-model window totals, usage, and remaining time).
3. OpenAI-compatible chat endpoint is usable with documented model naming (`MiniMax-M2.7`).
4. The prior claim "no subscription quota API exists" is invalid.
5. PAYG usage endpoint discovery is still incomplete because no dedicated PAYG key was provided for this spike.

## Examples

See the [fixture inventory](README.md) for synthetic remains payloads and local capture instructions.

## v1.0 Planning Impact

1. Keep MiniMax Coding/Token subscription as v1.0 first-priority surface.
2. Build subscription quota mirror from `coding_plan/remains` and `token_plan/remains` responses.
3. Treat PAYG endpoint discovery as v1.x follow-on, not a v1.0 requirement.
