# OpenAI Subscription Surface Notes

This note records the current Phase 0 evidence status for the OpenAI subscription-first path.

## Current Evidence Status

- Official OpenAI public sources now confirm consumer subscription-visible account, export, and Codex usage surfaces.
- `docs/fixtures/openai/codex-rate-limits-shape.sample.json` records the public `openai/codex` ChatGPT rate-limit response shape for future fixture/parsing work.
- `docs/fixtures/openai/codex-rollout-rate-limits.redacted.jsonl` now provides a synthetic local rollout example for the selected `token_count` / `rate_limits` contract.
- `docs/fixtures/openai/codex-consumer-artifact-redaction-note.md` now records the commit-safe field inventory and redaction contract for a future Codex usage-dashboard capture.
- No raw live consumer artifact is committed yet from a real ChatGPT/Codex account.
- There is still no validated public consumer API for general ChatGPT subscription usage/quota.

## Confirmed Official Subscription-Visible Surfaces

- ChatGPT account/billing pages for subscription users
- ChatGPT Data Export through account settings or the Privacy Portal
- Codex Settings -> Usage Dashboard, which OpenAI documents as showing credits balance and recent usage
- Official `openai/codex` app-server ChatGPT rate-limit fields:
  - `usedPercent`
  - `windowDurationMins`
  - `resetsAt`

## What This Changes

- The OpenAI subscription blocker is no longer "we do not know whether any subscription-visible surface exists."
- The real product constraint is now narrower:
  - there are official Codex-specific usage surfaces
  - there is official user-export history
  - there is still no validated general public consumer API for ChatGPT subscription usage/quota
  - automated consumer website scraping is not an acceptable collection foundation

OpenAI Terms of Use published `2026-01-01` prohibit automatically or programmatically extracting data from the consumer service, so JiStats should not build v1.0 around website scraping or undocumented `backend-api/*` endpoints.

## What JiStats Can Honestly Claim Today

- JiStats can track locally observed subscription activity on this device.
- JiStats can preserve locally observed subscription activity across launches where the app captured it.
- JiStats now has an honest path to augment local observation with:
  - Codex-local official usage/rate-limit signals when available
  - user-initiated ChatGPT export import for historical data
- the first Codex-local runtime path is already landed through local rollout session parsing under `%USERPROFILE%\\.codex\\sessions\\**\\rollout-*.jsonl`
- the first export-import historical path is already landed for extracted-export inputs, including the shipped `File -> Import OpenAI Export...` dashboard action
- JiStats must still treat general provider-side OpenAI subscription quota remaining and cross-ChatGPT usage totals as `Unknown` unless a validated provider surface is present.

## Optional Follow-on Confidence Work

- A real redacted Codex usage-dashboard or equivalent consumer artifact that can be archived safely using the redaction contract.
- Optional follow-up validation of whether a later local `codex app-server` `account/rateLimits/read` integration is worth adding for freshness.
- If a separate OpenAI API-platform path is later validated, a distinct archive for that API surface so it stays separate from the subscription story.

## Codex-Local Contract Decision

As of `2026-04-18`, the preferred first Codex-local JiStats contract is:

- local rollout session files under `%USERPROFILE%\\.codex\\sessions\\**\\rollout-*.jsonl`

Reasoning:

- they are already present locally
- they can be parsed read-only
- they already carry `session_meta` and `token_count` / `rate_limits` payloads
- they avoid dependence on UI scraping or a live app-server integration for the first implementation slice

Supporting note:

- `%USERPROFILE%\\.codex\\state_5.sqlite` is still useful as a possible index/helper source, but the inspected local `threads` table did not expose rate-limit fields and should not be the primary contract
- `docs/openai-codex-sqlite-helper-evaluation.md` now records the current decision that SQLite indexing is not worth shipping yet because rollout-directory discovery already covers the current query pressure

## Open-Source Reference Direction

- Worth studying:
  - `xiangz19/codex-ratelimit` and `xiangz19/codex-ratelimit-vscode` for local Codex session parsing
  - `Loongphy/codex-auth` local-only mode for account-linked local collection ideas
  - ChatGPT export parsers such as `slyubarskiy/chatgpt-conversation-extractor` for historical import handling
- Avoid as a product foundation:
  - undocumented `chatgpt.com/backend-api/*` and `/wham/usage` paths
  - bearer-token interception
  - DOM scraping / fetch-hook relays

## Downstream References

- `docs/phase0-spike.md`
- `docs/capability-matrix.md`
- `docs/openai-mirror-spec.md`
- `docs/openai-codex-local-contract-decision.md`
- `docs/openai-codex-sqlite-helper-evaluation.md`
- `docs/openai-export-import-contract.md`
- `docs/fixtures/openai/subscription-augmentation-fixture-plan.md`
