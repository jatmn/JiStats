# OpenAI Codex-Local Collection Contract Decision

> **Status**: Decision recorded; baseline landed
> **Purpose**: Choose the first supported Codex-local collection contract for OpenAI subscription augmentation
> **Driven by**: `docs/openai-mirror-spec.md` and the reopened Lane 1 OpenAI subscription augmentation track

## Decision

**Use local Codex rollout session files as the first supported Codex-local collection contract.**

For v1.0, JiStats should begin with read-only parsing of:

- `%USERPROFILE%\\.codex\\sessions\\**\\rollout-*.jsonl`

The parser should only read the minimum fields needed for subscription augmentation:

- `session_meta` for thread/session identity and source context
- `event_msg` records where `payload.type == "token_count"`
- `payload.rate_limits`
- `payload.info.total_token_usage` and `payload.info.last_token_usage` when present
- `payload.rate_limits.plan_type` / credits metadata when present

JiStats should **not** begin with:

- UI automation of the Codex app or VS Code extension
- captured bearer tokens
- undocumented `chatgpt.com/backend-api/*`
- direct dependence on a live `codex app-server` integration

## Why This Is The Best First Contract

### 1. It is already present on the local machine

On the current Windows machine, Codex is already persisting:

- rollout logs under `%USERPROFILE%\\.codex\\sessions\\...`
- a thread index database at `%USERPROFILE%\\.codex\\state_5.sqlite`

The rollout files already contain the useful usage payloads. A sampled local rollout on `2026-04-18` included:

- `session_meta`
- repeated `event_msg` records with `payload.type = "token_count"`
- `payload.rate_limits.primary`
- `payload.rate_limits.secondary`
- `plan_type`

Later inspection found that the local `state_5.sqlite` `threads` table also exposes useful usage attribution metadata:

- `id`
- `model_provider`
- `model`
- `reasoning_effort`
- `tokens_used`
- `created_at_ms`
- `updated_at_ms`

It still does **not** replace rollout JSONL as the quota/rate-limit source. Instead, `threads.tokens_used` is a cumulative per-thread counter suitable for local usage deltas after JiStats establishes a baseline. Sensitive fields such as `title`, `cwd`, prompts, messages, auth material, and API keys remain out of normalized JiStats storage.

### 2. It is passive and local-first

Reading rollout files is:

- read-only
- machine-local
- compatible with the current local-first product story
- isolated from the shared runtime-status work

This makes it the safest first Lane 1 implementation slice.

### 3. It avoids a heavier integration too early

The public `openai/codex` app-server README documents `account/rateLimits/read`, but starting there would require:

- managing a live Codex app-server process or transport
- version/protocol handling
- auth-state assumptions
- more invasive runtime integration than JiStats currently needs

That is a valid later path for fresher data, but it is not the safest first contract.

## Artifact Candidates Evaluated

### Candidate A: Rollout JSONL files

Path:

- `%USERPROFILE%\\.codex\\sessions\\**\\rollout-*.jsonl`

Pros:

- already present locally
- read-only
- contains session metadata plus token/rate-limit events
- works across CLI / VS Code / desktop local-session flows
- can be developed and tested as a parser-only slice

Cons:

- files contain sensitive conversation content, so JiStats must selectively parse and redact fixtures carefully
- some recent Codex builds reportedly emit `token_count` with `rate_limits: null`, so freshness can lag
- schema varies in the wild between snake_case local payloads and camelCase public app-server docs
- early transport failures can produce missing local artifacts for a displayed session id

### Candidate B: `state_5.sqlite`

Path:

- `%USERPROFILE%\\.codex\\state_5.sqlite`

Pros:

- compact local index of threads and rollout paths
- provides provider/model attribution through `threads.model_provider` and `threads.model`
- exposes cumulative per-thread token counters through `threads.tokens_used`
- useful for third-party provider reporting, including `hicap-ai` rows when present

Cons:

- inspected schema did not expose rate-limit state
- not sufficient on its own for subscription quota augmentation
- counters are cumulative, so direct historic import would overcount without baseline/delta logic
- contains sensitive fields outside the safe import subset

Decision:

- use as a local usage attribution source with new-only baseline semantics
- keep rollout JSONL as the quota/rate-limit source

### Candidate C: Local `codex app-server` `account/rateLimits/read`

Interface:

- public `openai/codex` app-server JSON-RPC method `account/rateLimits/read`

Pros:

- official documented contract
- likely the freshest Codex-visible rate-limit state
- better fit for a later high-confidence refresh path

Cons:

- requires a live app-server integration instead of passive local parsing
- more moving parts and version coupling
- larger implementation surface than the current Lane 1 parser slice needs

## First Supported Contract

JiStats should start with this contract:

1. Enumerate rollout files under `%USERPROFILE%\\.codex\\sessions\\`.
2. Read only JSONL records needed for Codex-local usage/rate-limit import.
3. Prefer the newest `token_count` event with non-null `rate_limits`.
4. Normalize both public-doc and local-file field shapes:
   - `usedPercent` or `used_percent`
   - `windowDurationMins` or `window_minutes`
   - `resetsAt` or `resets_at`
5. Treat the result as `Codex-specific imported provider truth`, not general ChatGPT subscription truth.
6. If only stale or null rate-limit data is present, keep that explicit in confidence/freshness labeling.

## Implementation Boundaries

The first implementation should:

- add a parser for rollout `token_count` records
- add fixture coverage for both the public sample shape and a safe redacted local rollout excerpt
- keep parsing narrowly scoped to metadata and rate-limit/token-usage fields
- avoid storing raw conversation content

The first implementation should not:

- launch or control Codex UI surfaces
- depend on `state_5.sqlite` beyond optional file discovery support
- treat local rollout values as universal OpenAI subscription quota truth

## Follow-On Work

After the rollout-file contract lands:

1. Wire the imported Codex-local snapshot into broader runtime/storage surfaces without inventing fake absolute quota totals.
2. Use `state_5.sqlite` only as an optional thread index helper if query pressure justifies it.
3. Reassess whether a later `codex app-server` integration is worth adding for fresher `account/rateLimits/read` refreshes.

## References

- `docs/openai-mirror-spec.md`
- `docs/fixtures/openai/subscription-surface-notes.md`
- `docs/fixtures/openai/codex-rate-limits-shape.sample.json`
- Public `openai/codex` app-server README, reviewed on `2026-04-18`
