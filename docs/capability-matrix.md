# OpenAI + MiniMax Capability Matrix

> **Status**: Release evidence refreshed on `2026-04-21`; validated enough for the current subscription-first v1.0 baseline, with one blocked advanced-path OpenAI API-platform item still open
> **Purpose**: Drives v1.0 scope and mirror feasibility decisions
> **Scope Change Flag**: OpenAI is now treated as subscription-first by default; OpenAI API import is optional and separate

Fixture archive references:

- Shared inventory: `docs/fixtures/README.md`
- OpenAI inventory: `docs/fixtures/openai/README.md`
- MiniMax inventory: `docs/fixtures/minimax/README.md`
- Release-signoff refresh note: `docs/live-api-capability-validation-2026-04-21.md`

Release evidence refresh on `2026-04-21`:

- MiniMax validation confirmed credential readiness and usable quota responses for both subscription plans.
- The OAuth-backed Kimi quota endpoint returned usage and limit fields.
- A higher-scope credential is still required for an OpenAI API-platform freshness measurement.

## OpenAI

### OpenAI Surface Split

JiStats must treat OpenAI as two distinct surfaces:

1. **OpenAI subscription surface**
   - ChatGPT / Codex / workspace-style subscription usage
   - Default OpenAI v1.0 path
   - Does not imply API billing or API keys

2. **OpenAI API platform surface**
   - Separate billing and credentials
   - Optional advanced import path
   - Valuable when available, but not the default OpenAI assumption

### OpenAI Subscription Surface

Evidence status: official public docs now confirm consumer subscription-visible account/export/Codex usage surfaces, and `docs/fixtures/openai/` now includes a public doc-derived Codex rate-limit shape sample, a safe redacted local rollout excerpt, and a commit-safe redaction contract for a future Codex consumer artifact. No raw live consumer dashboard artifact is committed yet, and there is still no validated public consumer subscription API. This is enough for the current subscription-first baseline, not proof of a universal ChatGPT subscription quota ledger.

| Capability | Supported | Signal | Freshness | Notes |
|------------|-----------|--------|-----------|-------|
| Local activity tracking | ✅ | JiStats local observation | Near real time | Base v1.0 path |
| Historical local usage | ✅ | JiStats local storage | Immediate | Limited to observed periods |
| Account / history / export surfaces | ✅ | ChatGPT account pages + Data Export | User initiated / on demand | Exists without API billing |
| Codex recent-usage surface | ⚠️ Partial | Codex Settings -> Usage Dashboard | Near real time | Official Codex-specific surface; not a public consumer API |
| Codex quota-window metadata | ⚠️ Partial | Official `openai/codex` ChatGPT `account/rateLimits/read` shape plus local rollout `token_count` / `rate_limits` snapshots | Near real time when available through Codex runtime | Public schema and one safe redacted local rollout capture are archived; broader consumer dashboard evidence is still pending |
| Provider-side subscription quota | ⚠️ Partial | Codex-specific only | Near real time for Codex only | Must not be generalized to all ChatGPT subscription usage |
| Provider-side usage totals | ⚠️ Partial | Codex recent usage UI + user export for history | Mixed | No full authoritative cross-surface consumer ledger is validated |
| Exact reset window | ⚠️ Partial | Codex rate-limit metadata + some UI reset hints | Near real time for exposed surfaces | Must not be assumed outside validated surfaces |
| Real-time provider truth | ⚠️ Partial | Codex dashboard / rate-limit surfaces | Near real time for Codex only | Use explicit uncertainty outside Codex-visible signals |

### OpenAI API Platform Surface

Evidence status. The [validation contract](openai-api-platform-validation-contract.md) documents the endpoint/auth inventory and the observed authorization result.

| Capability | Supported | Endpoint / Signal | Freshness | Notes |
|------------|-----------|------------------|-----------|-------|
| Usage history | ⚠️ Optional | `GET /v1/organization/usage/completions` (inventory complete; first live probe returned `403 Missing scopes: api.usage.read`) | Unknown until a higher-scope credential succeeds | Separate API billing required |
| Cost history | ⚠️ Optional | `GET /v1/organization/costs` (inventory complete; first live probe returned `403 Missing scopes: api.usage.read`) | Unknown until a higher-scope credential succeeds | Not part of base subscription path |
| Per-model breakdown | ⚠️ Optional | `group_by=model` on organization usage endpoints | Unknown until validated | Requires separate API access |
| Per-project breakdown | ⚠️ Optional | Organization usage/cost grouping once a higher-scope credential succeeds | Unknown until validated | Requires separate API setup |
| Rate limit signals | ⚠️ Optional | Unverified API response headers | Per-request if available | Relevant only when using API surface |
| Webhook for usage | ❌ | Not validated | - | No live API-platform validation is archived yet |

### OpenAI Quota Mirroring Feasibility

| OpenAI Dimension | Mirrorable | Confidence | Notes |
|------------------|------------|------------|-------|
| Locally observed subscription activity | ✅ | High | Exact for captured events |
| Historical local subscription usage | ✅ | High | Exact for observed periods |
| User-initiated subscription history import | ⚠️ | Medium | Official data export exists, but it is on-demand rather than live |
| Codex subscription pressure / recent usage | ⚠️ | Medium | Official surfaces exist for Codex-specific usage and credits, but not for all ChatGPT subscription activity |
| General ChatGPT subscription quota remaining | ⚠️ | Unknown | No validated public consumer API or universal remaining-budget counter |
| Subscription reset timing | ⚠️ | Partial | Some Codex and model-picker reset hints exist, but not a universal subscription truth source |
| API usage import | ⚠️ Optional | Blocked advanced path | Separate from subscription path; first live authz probe proved the endpoint family but not the required scopes |

### OpenAI Authentication Reality

- Subscription access is **not** the same as API-platform access
- ChatGPT/Codex billing and API billing are separate surfaces
- A subscription user may have no API key at all
- API import must be modeled as a separate credential path, not a default requirement

### OpenAI Monitoring Cost

- Subscription-first local tracking: zero provider call cost
- Codex-local file/session parsing: zero provider call cost once the local artifact exists
- User-initiated export import: zero provider call cost
- Optional API import: still blocked for successful freshness/lag capture until a higher-scope credential is available, but it should prefer free non-model endpoints if available
- Recommendation: ship local-first subscription tracking first, use Codex-local official signals where available, and keep API import optional

---

## MiniMax

### MiniMax v1.0 Planning Priority

- First-priority surface: Coding Plan subscription
- PAYG runtime implementation remains deferred to v1.x, but dedicated-key validation confirmed per-request usage
- The subscription rows summarize live validation. The fixture directory contains synthetic response examples.
- The curated MiniMax fixture list lives in `docs/fixtures/minimax/README.md`

### Pay-As-You-Go API

| Capability | Supported | Endpoint / Signal | Freshness | Notes |
|------------|-----------|------------------|-----------|-------|
| Per-request usage | ✅ Partial | `POST /v1/chat/completions` returns `usage` with dedicated PAYG key | Immediate / per-request | Validated for `MiniMax-M2.7`; the response includes token counts on success |
| Usage history | ⚠️ Partial | `/v1/usage` and `/api/v1/usage` probed; both `404` with dedicated PAYG key | Unknown / not exposed | No separate aggregate history endpoint is currently validated; only the per-request completion payload is decision-ready today |
| Cost calculation | ✅ Partial | Returned token counts + published PAYG pricing | Immediate / per-request | Cost is derivable per request; no separate provider-side cost-history endpoint is validated |
| Per-model breakdown | ⚠️ Partial | Successful completion response identifies model; no separate history grouping endpoint validated | Per-request only | Valid for captured requests, not for provider-side historical rollups |
| Token counts | ✅ Partial | `usage.prompt_tokens` + `usage.completion_tokens` on successful completions | Immediate / per-request | Observed in dedicated-key validation |
| Rate limit signals | ⚠️ Partial | Headers on OpenAI-compatible calls | Per-request | Dedicated-key evidence confirms headers exist on successful OpenAI-compatible calls |

### Subscription Plans (Token Plan, Coding Plan)

| Capability | Supported | Signal | Notes |
|------------|-----------|--------|-------|
| Subscription quota | ✅ | `GET /v1/api/openplatform/coding_plan/remains` | Validated with subscription key; returns `model_remains` |
| Request-window quota | ✅ | `current_interval_total_count` / `current_interval_usage_count` | Returned per model in subscription response |
| Remaining balance/time | ✅ | `remains_time` + interval/week fields | Returned in subscription response |
| Usage included in plan | ✅ | `model_remains` counters | Validated for coding/token remains endpoints |
| Mixed interval-family evidence | ⚠️ Partial | Archived remains snapshots plus Coding Plan rollover proof and accepted Token Plan archived/live comparison | Live validation showed multiple per-model interval spans plus weekly fields, and the current runtime lane is decision-ready for accepted `5h`-family parity; broader period semantics still should not be overstated |
| In-app credential entry path | ✅ | Shipped Settings / Accounts flow | Coding Plan and Token Plan credential entry, save, and validate actions now exist in-product, validation now completes asynchronously, and the stored secret keeps a masked presentation after success |
| Secure credential storage | ✅ | Windows Credential Manager `secret_ref` path | Raw MiniMax subscription secrets stay out of SQLite; local persistence stores metadata plus secret reference |
| Repeated-live reset evidence log | ✅ Sufficient for current v1.0 runtime parity | `docs/fixtures/minimax/reset-window-evidence-log.md` | Coding Plan now has a full live pre/post sequence, Token Plan now has archived-pre plus live-post evidence matching the same 5h family shape, and the repo now treats that comparison-based condition as sufficient for the current runtime/test checkpoint |

Subscription scope note:

- The shipped MiniMax v1.0 path is quota-mirroring through the validated remains endpoints.
- Broader provider-side MiniMax usage-history import is intentionally deferred to a future scoped follow-on instead of being treated as an unfinished part of the current release baseline.

### Key Separation

- Pay-as-you-go: Uses API key
- Subscription: Uses separate subscription key or account credentials
- **Impact**: May need separate credential profiles per usage type

### MiniMax Specific Risks

1. **Documentation gap**: MiniMax API docs less mature than OpenAI
2. **PAYG usage surface**: separate aggregate history endpoint still unvalidated; current validated signal is per-request completion `usage`
3. **Key management**: Two credential types may require different sync logic

---

## Comparison Matrix

| Feature | OpenAI Subscription | OpenAI API Platform | MiniMax (PAYG) | MiniMax (Subscription) |
|---------|---------------------|---------------------|----------------|------------------------|
| Local recent activity | ✅ | ⚠️ Separate concern | ⚠️ Partial | ✅ With remains-backed subscription context |
| Provider-side history | ⚠️ Partial / surface-limited | ⚠️ Optional / blocked advanced path | ⚠️ Partial | ✅ Windowed counters validated |
| Cost history | ⚠️ Partial / export-plus-surface-limited | ⚠️ Optional / blocked advanced path | ⚠️ Partial | ⚠️ Not explicit in remains payload |
| Per-model granularity | ⚠️ Unvalidated | ⚠️ Optional / unvalidated | ⚠️ Partial | ✅ Per-model remains entries |
| Real-time provider truth | ⚠️ Partial / Codex-surface-limited | ⚠️ Optional / blocked advanced path | ⚠️ Partial | ⚠️ Windowed near-real-time counters |
| Quota mirroring feasible | ⚠️ Partial / honest-unknown | ⚠️ Pending validation | ⚠️ Partial / per-request only | ✅ For remains-based subscription mirrors |
| Requires separate developer billing | ❌ | ✅ | ✅ | ❌ |

---

## Open Questions / Validation Required

1. **OpenAI API platform**:
   - What is the lag/freshness of API usage data once a credential with `api.usage.read` and the right org-scoped role exists?
   - What response-header/rate-limit detail is preserved on successful organization usage/cost calls?
   - How should API data remain visibly separate from subscription tracking when that advanced path is enabled?

2. **MiniMax PAYG**:
   - Is there any separate aggregate PAYG usage-history endpoint beyond the validated per-request completion `usage` payload?
   - How should v1.x mirror PAYG history when the currently validated signal is per-request usage plus published pricing rather than provider-side aggregate history?
   - Should JiStats treat PAYG freshness as immediate request-time capture unless a separate history endpoint is later documented?

3. **MiniMax Subscription**:
   - Confirm stable reset semantics from `start_time` / `end_time` fields across plan types and interval families if fresh contradictory evidence appears
   - Confirm whether remains endpoints cover all subscription models consistently
   - Confirm cost attribution strategy when remains payload lacks explicit currency fields
   - Keep user-facing quota-period labels model-aware unless fresh contradictory evidence appears; future direct Token Plan pre/post capture is now confidence-refresh work rather than a runtime blocker

---

## Exit Criteria

This matrix should be considered accurate enough for v1.0 implementation when:

1. OpenAI subscription-first behavior is fully documented without API assumptions
2. OpenAI API import is clearly separated as an optional path
3. MiniMax subscription endpoints remain validated (completed)
4. Response shapes are captured in fixture archives where available
5. Remaining unknowns are reflected as explicit limits, not hidden assumptions

**Current confidence**: Medium-high for the current v1.0 baseline. OpenAI subscription remains subscription-first and surface-limited rather than universally mirrorable, the archived `2026-04-19` API-platform probe leaves only one blocked advanced-path freshness item, and MiniMax subscription is validated enough for the current runtime lane without claiming every interval-family semantic is fully closed.
