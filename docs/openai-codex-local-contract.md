# OpenAI Codex-Local Collection Contract

> **Status**: Lane 1 implementation baseline
> **Purpose**: Lock the first supported Codex-local collection contract for JiStats without coupling it to the active runtime-status work

## First Supported Contract

JiStats should start the OpenAI subscription augmentation track with a **Codex rollout rate-limit snapshot contract** sourced from local Codex session logs.

The first supported baseline is:

- provider: `openai`
- provider surface: `openai_subscription_codex_local`
- artifact class: `%USERPROFILE%\\.codex\\sessions\\**\\rollout-*.jsonl`
- first supported records:
  - `session_meta`
  - `event_msg` with `payload.type = "token_count"` and `payload.rate_limits`
- compatibility fixture shape:
  - `docs/fixtures/openai/codex-rate-limits-shape.sample.json`

This is intentionally narrower than "general ChatGPT subscription usage."

## Why This Contract Comes First

- It stays inside the repo's existing provider/test seam.
- It avoids browser scraping, bearer-token capture, and undocumented `chatgpt.com/backend-api/*` dependencies.
- It matches the documented public `openai/codex` rate-limit shape better than trying to force partial percentage metadata into the generic quota model.
- It does not require touching the currently active runtime-status files.

## Artifact Candidates Considered

### 1. Codex rollout `token_count` / `rate_limits` records

Examples:

- `%USERPROFILE%\\.codex\\sessions\\**\\rollout-*.jsonl`
- `event_msg` payloads carrying `rate_limits.primary`, `rate_limits.secondary`, and `plan_type`

Pros:

- already present locally on this machine
- passive and read-only
- directly relevant to subscription pressure
- coexists well with the current runtime-status work because it stays inside the provider seam

Cons:

- carries percentage/window metadata rather than full quota totals
- is Codex-specific, not universal ChatGPT subscription truth

Decision:

- **Ship this first**

### 2. Local Codex SQLite state index and usage attribution source

Examples:

- `%USERPROFILE%\\.codex\\state_5.sqlite`
- table: `threads`

Validated safe columns:

- `id`
- `model_provider`
- `model`
- `reasoning_effort`
- `tokens_used`
- `created_at_ms`
- `updated_at_ms`

Intentionally excluded fields:

- `title`
- `cwd`
- prompts, messages, and conversation bodies
- auth, config, API keys, and bearer-token material

Pros:

- provides provider/model attribution for local Codex usage
- captures third-party provider splits such as `hicap-ai` when Codex records them locally
- passive and read-only

Cons:

- does not appear to be the primary quota/rate-limit source
- `tokens_used` is cumulative per thread, so JiStats must import deltas rather than raw totals
- first scan must establish a baseline to avoid overcounting historic local usage

Decision:

- use as a usage attribution source, not as quota truth
- keep rollout JSONL as the quota/rate-limit source
- import new-only deltas after JiStats establishes a per-thread baseline

### 3. App-server-readable rate-limit snapshot

Pros:

- officially documented public shape
- likely useful for future freshness work

Cons:

- heavier integration than Lane 1 needs
- not necessary for the first passive local read path

Decision:

- keep as a future path after local rollout parsing

### 4. User-initiated ChatGPT export import

Pros:

- official and user-controlled
- useful for historical enrichment

Cons:

- not live
- not the best first answer to current subscription-pressure augmentation

Decision:

- keep as a follow-on path after Codex-local rate-limit parsing

## Source Labeling Boundary

JiStats should distinguish these OpenAI subscription-related source buckets:

- `openai_subscription_local`
  - local observation only
  - exact for captured events on this machine

- `openai_subscription_codex_local`
  - Codex-local official/account-linked metadata from rollout `rate_limits` snapshots
  - provider truth only for the Codex-specific surface it actually describes

- `openai_subscription_export_import`
  - user-initiated historical import
  - authoritative for the imported artifact, but not live

## Immediate Implementation Rule

Do not force Codex-local percentage/window metadata into generic exact quota totals yet.

The first implementation should use an OpenAI-specific snapshot model that can express:

- `usedPercent`
- `windowDurationMins`
- `resetsAt`
- `planType`
- optional `rateLimitReachedType`

without inventing fake `used` / `limit` absolute totals.

## Current Baseline

The repo now has:

- fixture parsing for the public sample shape
- fixture parsing for a safe redacted local rollout excerpt
- rollout-directory discovery that can pick the newest valid local `token_count` / `rate_limits` snapshot
- a recorded decision that `%USERPROFILE%\\.codex\\state_5.sqlite` should be integrated as a sanitized local usage attribution source, while rollout JSONL remains the quota/rate-limit source

## Follow-up List

After the runtime-ingestion baseline, the next steps are:

1. Decide whether runtime/UI surfaces need a generic provider-pressure abstraction or a Codex-specific mapping layer first.
2. Define how reports and dashboard labels should present `Codex-specific provider truth` versus broader `OpenAI subscription unknown`.
3. Add the export-import contract as a separate historical path.
4. Extend SQLite usage attribution beyond OpenAI/HICAP charts only after provider semantics for additional Codex-backed providers are validated.
5. Keep raw Codex SQLite fields out of JiStats normalized storage unless a future privacy-reviewed archive mode is explicitly added.
