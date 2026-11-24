# MiniMax PAYG Capture

> **Status**: Dedicated PAYG-key validation returned per-request usage

This directory documents the dedicated-key MiniMax PAYG probe. The helper stores raw captures outside the checkout by default.

It is intentionally separate from the MiniMax subscription `coding_plan/remains` and `token_plan/remains` fixture set so JiStats does not mix:

- subscription remains semantics
- PAYG per-request usage evidence
- undocumented endpoint probing

## Local output files

- `completion-<timestamp>.json`
- `completion-<timestamp>.headers.txt`
- `endpoint-probes-<timestamp>.json`
- `capture-<timestamp>.metadata.json`
- `capture-<timestamp>.notes.md`

## Capture Command

Use the repo helper:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\capture_minimax_payg_evidence.ps1
```

The helper:

- reads `MINIMAX_API_KEY`
- sends a minimal OpenAI-compatible `POST /v1/chat/completions`
- archives the returned `usage` payload and response headers
- probes the older candidate history/discovery paths:
  - `GET /v1/usage`
  - `GET /api/v1/usage`
  - `GET /v1/models`
- writes a notes file capturing the current PAYG conclusion plus the derived cost rule from the official pricing page

## Current Conclusion

The current dedicated-key validation shows:

- the PAYG key works on the documented OpenAI-compatible text API
- per-request token usage is returned immediately in the completion response
- the older candidate aggregate/discovery paths above still return `404`

That means the currently validated PAYG usage signal is immediate per-request usage plus published pricing, not a separately validated aggregate usage-history API.

The capture helper writes raw output under `%LOCALAPPDATA%/JiStats/captures/minimax/payg` by default. Use `-OutputDir` for a different location outside the source checkout. Paths inside the source checkout are rejected.
