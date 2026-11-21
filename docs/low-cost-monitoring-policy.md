# Low-Cost Monitoring Policy

> **Status**: Current
> **Purpose**: Define how JiStats monitoring operations should not distort usage metrics
> **Driven by**: plan.md section 6.6 (Cost-Aware Monitoring Operations)

## The Problem

A monitoring app that makes API calls to track usage could **inflate the very usage it's tracking** if it uses model calls for monitoring.

For example:
- If monitoring requires 10 GPT-4 calls per day = $10/month in monitoring cost
- This distorts the user's actual usage with monitoring overhead

## Core Principle

**Monitoring operations should be negligible cost compared to user activity**

From plan.md section 6.6:
> The monitor should avoid generating unnecessary spend while gathering telemetry.

## Monitoring Operations Taxonomy

### Class 1: Direct API Requests (No Model Call)

✅ **Preferred** - Use these whenever possible

| Operation | API Endpoint | Cost |
|-----------|--------------|------|
| Fetch usage history | Provider usage endpoint (for OpenAI API-platform: `GET /v1/organization/usage/completions`) | Free |
| Fetch cost history | Provider cost endpoint (for OpenAI API-platform: `GET /v1/organization/costs`) | Free |
| Fetch account or organization metadata | Provider account metadata endpoint | Free |
| Rate limit headers | On any API response | Free |
| Check subscription status | Provider subscription/account API | Free |

### Class 2: Historical Import Requests (No Model Call)

✅ **Good** - Use for backfill

| Operation | Purpose | Cost |
|-----------|---------|------|
| Import daily usage | Backfill history | Free (no model) |
| Fetch billing history | Cost tracking | Free |

### Class 3: Model-Backed Probes (AVOID)

⚠️ **Last Resort** - Only when no Class 1 or 2 option exists

| Operation | When Required | Cost |
|-----------|---------------|------|
| Model invocation for health check | Only if no direct endpoint available | Expensive |
| Model invocation for quota check | Only if provider has no quota API | Expensive |

## Provider-Specific Monitoring Paths

### Claude And Kimi Local-First Provider Rule

For the current Phase 7 Claude Code and Kimi Code local-runtime implementation path:

- prefer local artifact parsing before any model-backed probe
- keep the default path zero-cost
- treat local parser output as `Local tracking only` unless separately corroborated by validated provider surfaces
- do not add model probes just to convert a local-first provider into an artificial exact-quota story

This keeps Phase 7 provider implementation aligned with the same low-overhead principle already used for the subscription-first OpenAI path.

### OpenAI Monitoring Path (Subscription-First)

v1.0 uses a subscription-first monitoring baseline. ChatGPT subscription billing and OpenAI API billing are separate surfaces, so the app does not assume API platform access.

**Primary path (v1.0):**

1. **Local activity observation**: Watch Codex `rollout-*.jsonl` files for subscription quota signals
   - Confidence: `LocalExact` for captured activity
   - Cost: **$0** (no API calls)

2. **User-initiated ChatGPT export import**: On-demand historical enrichment
   - Confidence: `Imported` (exact for exported artifact, not live quota truth)
   - Cost: **$0** (manual user action)

3. **Codex-local rate-limit metadata**: From official Codex Settings/Usage Dashboard
   - Confidence: `Imported` when sourced from official Codex-local signal
   - Cost: **$0**

**Optional API Platform path (requires separate API billing + `api.usage.read` scope):**

```
If user has OpenAI API platform billing setup:
1. Usage history: `GET /v1/organization/usage/completions` (free, daily)
2. Cost history: `GET /v1/organization/costs` (free, daily or on-demand)
3. Organization metadata/context: only when needed for account/org scoping
4. Rate limits: From headers on actual API calls (free)

Note: The 2026-04-19 authz probe showed api.usage.read scope is required.
      This path is currently blocked for advanced-path follow-up until a 
      higher-scope credential is available.
```

**Conclusion**: OpenAI subscription-first monitoring is zero-cost (no model calls needed, local observation driven). API platform monitoring is optional and also zero-cost when available.

### MiniMax PAYG Monitoring Path (v1.x Deferred)

```
Preferred path:
1. Usage API: (endpoint TBD, assumed free)
2. No organization-level API (simpler than OpenAI)
3. Rate limits: From headers if available

If usage API unavailable:
- Fall back to local-only monitoring
- Mark confidence as Estimated
- Consider manual refresh option
```

**Conclusion**: PAYG monitoring remains a v1.x task and does not block v1.0.

### MiniMax Subscription Monitoring Path (v1.0)

```
Validated endpoints:
- GET /v1/api/openplatform/coding_plan/remains
- GET /v1/token_plan/remains

Options:
1. Use subscription API endpoints as primary monitoring path (free)
2. If endpoint unavailable for a specific account:
   a. Local event tracking only (free)
   b. User manually checks dashboard (zero cost to us)
   c. Model-backed probe: LAST RESORT only
   
Model probe only if:
- No usable endpoint for the account AND
- User explicitly enables "subscription quota tracking" AND  
- We use the provider's lowest-cost acceptable model
- We limit frequency to once per hour max
```

### Claude Code Local-First Path (v1.0)

Preferred path for the shipped bounded Claude Code local-first implementation:

1. Parse approved local aggregate artifacts such as:
   - `~/.claude/stats-cache.json`
   - `~/.claude/settings.json`
2. Use the data for local activity and runtime summaries only
3. Show `Local tracking only` when the artifact path does not prove provider-exact quota truth
4. Use official telemetry export only if the user already enables it and JiStats later documents a safe ingest contract

Model probe policy:

- disabled by default
- not justified for the first local parser path

Conclusion:

- Claude local monitoring should remain zero-cost by default
- local parsing may support usage patterns without pretending to know authoritative billing or quota state

### Kimi Local-First Path (v1.0)

Preferred path for the shipped bounded Kimi local-first implementation:

1. Parse only the approved local metadata/runtime artifacts documented in `docs/kimi-local-parser-spike.md`
2. Use the data for local activity and runtime summaries
3. Keep provider-exact quota truth tied to validated remains endpoints when available
4. Show `Local tracking only` when JiStats is relying only on local parser output

Model probe policy:

- disabled by default
- not justified for the first local parser path

Conclusion:

- Kimi local-runtime parsing should remain a zero-cost supplement, not a reason to add model-backed monitoring

## Polling Intervals (Cost Control)

From `docs/sqlite-schema.md` (app_settings seed data):
```sql
('polling_interval_seconds', '3600'),  -- 1 hour default
```

### OpenAI Polling

| Operation | Interval | Cost/Month |
|-----------|----------|------------|
| Usage history | Hourly | ~720 calls = free |
| Cost history | Daily | ~30 calls = free |

**Total OpenAI monitoring cost: $0/month**

### MiniMax Polling

v1.0 subscription path: poll remains endpoints on schedule.
v1.x PAYG path: poll PAYG endpoint once validated; until then local-only.

### Local Parser Refresh Rules

For the shipped Claude and Kimi local parsers:

- refresh should be file-change-driven or low-frequency scan driven
- do not poll provider APIs merely to accompany a local parser refresh
- treat parser cadence as a freshness concern, not as a reason to add model-backed checks
- if no safe local file changed recently, JiStats may keep the prior local snapshot and mark it stale rather than generating extra provider traffic

### Direct Quota API Polling Standard

For any provider path that checks a direct quota API rather than a local artifact:

- throttle each direct quota API surface independently rather than using one global provider timer
- poll at most once every 5 minutes while the returned quota response is changing
- if the returned quota stays unchanged for 30 minutes, back that specific API surface off to at most once every 30 minutes
- if a later response shows changed usage or remaining quota, drop only that specific API surface back to the 5-minute cadence
- reuse the last successful direct quota response during faster local refresh cycles instead of re-hitting the network

Examples:

- MiniMax Coding Plan and Token Plan remains endpoints should each keep their own adaptive timer
- Kimi `/usages` should keep its own adaptive timer independent from MiniMax or any other provider
- a file-driven Claude or Kimi local refresh may still run frequently, but that local pass must not force a matching direct API poll

### Rate Limit Monitoring

Rate limit info comes from **actual user API call headers**:
- No extra API calls needed
- Just read `X-RateLimit-*` headers from responses
- Already captured in local events

## Model-Backed Probe Policy (Last Resort)

If a provider ever requires model invocation for monitoring:

```
Rules:
1. Must be configurable per provider
2. User must explicitly enable
3. Use lowest-cost acceptable model for that provider
4. Limit to max 1 probe per hour
5. Track probe usage separately in diagnostics
6. Show probe cost separately in dashboard

Implementation:
- provider_capabilities table has "model_probe_cost" field
- SyncScheduler tracks "monitoring_requests" vs "user_requests"
- UI shows "Monitoring overhead: $X.XX this month"
```

## Self-Monitoring Overhead Tracking

JiStats should track its own monitoring cost:

### Tracking Fields

In usage_events or separate tracking:
```
monitoring_request: boolean  -- true if this is a JiStats self-request
```

### Dashboard Display

Show monitoring overhead separately:

```
This month's usage:
- User activity: $45.67
- Monitoring overhead: $0.02
- Total: $45.69

Monitoring is operating within policy (free tier)
```

Or if model probe was used:
```
This month's usage:
- User activity: $45.67
- Monitoring overhead: $0.15 (1 probe)
- Total: $45.82
```

## Configuration Options

### Settings

From `docs/sqlite-schema.md`:
```sql
('polling_interval_seconds', '3600'),
('monitoring_probe_enabled', 'false'),  -- Disabled by default
('monitoring_probe_model', ''),  -- Provider-specific, set only if probe is enabled
```

### Provider Capability Flags

From `docs/entity-model.md` (provider_capabilities table):
```
capability_key: "uses_model_probe"
supported: 0/1
config_json: { "model": "...", "max_per_hour": 1 }
```

## Notification When Probe Required

If monitoring requires model probe:

```
⚠️ Monitoring using model probe
Cost: ~$0.01 per check
Enable: Settings → Monitoring → "Track quota via model probe"
Or disable to save cost
```

## Open Questions

1. Should we ever use model probe even if free endpoints exist?
   - No, always prefer free endpoints
2. What's the maximum acceptable monitoring cost as % of user cost?
   - Recommendation: < 1% of user's monthly spend
3. If provider has no API at all (local-only):
   - Is that acceptable or should we warn user?
   - Recommendation: Acceptable, show "Local tracking only" badge

## Policy Summary

| Provider | Monitoring Path | Expected Cost |
|----------|-----------------|---------------|
| OpenAI (v1.0 subscription-first) | Local observation + Codex-local + export-import | $0/month |
| OpenAI API Platform (optional) | Usage API (requires `api.usage.read` scope) | $0/month |
| MiniMax Subscription (v1.0) | Remains API endpoints | $0/month |
| MiniMax PAYG (v1.x) | Usage API if available; local-only until validated | $0/month |
| Claude Code (v1.0 bounded local-first) | Local aggregate/runtime parsing, no model probe by default | $0/month |
| Kimi Code (v1.0 bounded local-first) | Local runtime parsing, no model probe by default | $0/month |

**Rule**: If monitoring costs are > 1% of user spend, reduce polling frequency or disable monitoring features.

## References

- Cost-aware monitoring: `docs/plan.md` section 6.6
- OpenAI mirror spec: `docs/openai-mirror-spec.md`
- MiniMax mirror spec: `docs/minimax-mirror-spec.md`
- App settings: `docs/sqlite-schema.md`
- Provider capabilities: `docs/entity-model.md`
- Claude local-surface spike: `docs/claude-local-surface-spike.md`
- Kimi local parser spike: `docs/kimi-local-parser-spike.md`
