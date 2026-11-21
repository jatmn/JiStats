# Provider Fixture Inventory

This directory is the curated evidence archive for provider capability validation.

Its job is to keep the capability matrix, parser tests, and mirror contracts grounded in captured shapes and archived notes instead of assumption drift.

## Current Provider Coverage

| Provider | Inventory | Current Evidence Focus |
| --- | --- | --- |
| OpenAI | `docs/fixtures/openai/README.md` | Subscription-first OpenAI evidence: Codex-local rollout parsing, historical ChatGPT export import, and the API-platform authz/readiness summary |
| MiniMax | `docs/fixtures/minimax/README.md` | Synthetic remains responses, reset-window findings, and PAYG probe instructions |
| Claude Code | `docs/fixtures/claude/README.md` | Redacted local stats/settings fixtures for the bounded local-first parser path |
| Kimi Code | `docs/fixtures/kimi/README.md` | Redacted local runtime/session fixtures for the bounded local-first parser path |

## How To Use This Archive

- Treat these files as evidence for what JiStats validated or safely modeled at a point in time, not as a promise that every provider surface still behaves the same today.
- Prefer linking planning claims, capability-matrix cells, and adapter tests back to a specific fixture or evidence note.
- Keep secrets, account identifiers, and conversation content redacted before adding anything new.
- Add or update a short inventory note whenever a new artifact changes product assumptions, parser boundaries, or mirror-confidence rules.
- For any Claude Code or Kimi Code fixture work, follow `docs/claude-kimi-local-redaction-contract.md` before committing a sample or widening a parser boundary.

## Current Archive Shape

### OpenAI

The OpenAI archive now covers three distinct evidence buckets:

1. Codex-local subscription evidence
   - public doc-derived rate-limit shape note
   - one synthetic Codex-local rollout example
   - a commit-safe redaction contract for any future Codex consumer artifact

2. Historical export-import evidence
   - one redacted extracted-export fixture directory
   - a provenance/redaction note for that export-derived fixture
   - the import-boundary and augmentation planning notes that keep historical import separate from live provider truth

3. Optional API-platform evidence
   - dedicated [API-platform instructions](openai/api-platform/README.md)
   - first live `2026-04-19` authz/readiness evidence showing both organization endpoints reachable but blocked by missing `api.usage.read`

### MiniMax

The MiniMax archive now covers:

- subscription remains payloads for Coding Plan and Token Plan
- reset-window capture instructions and accepted evidence logs
- synthetic Coding Plan and Token Plan remains examples
- dedicated PAYG request-path capture instructions

## Current Gaps

- OpenAI still has no validated general consumer subscription quota API fixture; the v1.0 OpenAI baseline remains local observation plus Codex-local augmentation plus export import.
- OpenAI API-platform evidence now has a real authz/readiness summary, but it still does not have a successful usage/cost freshness-lag capture.
- Raw live Codex consumer dashboard artifacts are still not committed; only the redaction contract and a synthetic example are tracked.
- ChatGPT export `.zip` handling remains deferred; the current repo fixture path is extracted-export only.
- MiniMax still lacks a separately validated aggregate PAYG usage-history endpoint beyond the dedicated per-request capture path.
- Claude Code and Kimi Code now have bounded redacted fixture archives, and any future local-surface samples must still stay inside `docs/claude-kimi-local-redaction-contract.md`.
