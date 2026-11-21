# MiniMax Subscription-Plan and Pay-As-You-Go Account Handling

> **Status**: Lane 1 provider/storage baseline defined; app-shell credential entry still pending
> **Purpose**: Define how JiStats handles MiniMax's three distinct surfaces
> **Driven by**: `docs/minimax-mirror-spec.md` and `docs/entity-model.md`

## MiniMax Has Three Surfaces

From `docs/minimax-mirror-spec.md`:

| Surface | Credential Type | Quota Type | v1.0 Priority |
|---------|----------------|------------|---------------|
| Pay-As-You-Go (PAYG) | API key | Unlimited, per-request cost | Deferred (v1.x) |
| Token Plan | Subscription key | Windowed subscription quota (exact period label still under validation) | Secondary |
| Coding Plan | Subscription key | Windowed subscription quota (exact period label still under validation) | **Primary** |

## Key Challenge: Credential Separation

MiniMax uses **different credentials** for PAYG vs subscription:
- PAYG: Uses MiniMax API key (standard Bearer token)
- Subscriptions: Use separate subscription keys (tested with `sk-cp-*` bearer format)

This means ONE MiniMax user might need:
- 1 PAYG credential profile (API key)
- 1 Token Plan credential (subscription account)
- 1 Coding Plan credential (subscription account)

## Entity Model Support

From `docs/entity-model.md`:

### credential_profiles Table

```
credential_profiles:
  - id, provider_id, account_id
  - profile_name (e.g., "PAYG API Key", "Token Plan", "Coding Plan")
  - secret_ref (reference to Windows Credential Manager)
  - credential_type: "api_key" | "subscription_key" | "oauth"
```

### plans Table

```
plans:
  - id, account_id
  - plan_key (MiniMax's plan identifier)
  - plan_type: "subscription" | "payg"
  - quota_window: "daily" | "monthly" | "unlimited"
  - limit_value (null if unlimited)
```

## First Subscription Credential Profile Contract

The first concrete MiniMax subscription credential contract should stay intentionally narrow and should map cleanly onto the already-shipped `accounts`, `credential_profiles`, and `plans` tables plus Windows Credential Manager.

### Runtime/Profile Fields

The current runtime-facing contract is:

- `providerKey`
  - must be `minimax`
- `accountKey`
  - stable JiStats account identifier for the subscription scope
- `accountDisplayName`
  - user-visible account label
- `scopeType`
  - first supported value is `subscription`
- `scopeIdentifier`
  - stable subscription-scope identifier; default to `accountKey` when nothing better is available
- `profileName`
  - user-visible credential profile label such as `MiniMax Coding Plan`
- `planKey`
  - first supported values are `coding_plan` and `token_plan`
- `planDisplayName`
  - user-visible plan label such as `MiniMax Coding Plan`
- `credentialType`
  - first supported value is `subscription_key`
- `secretRef`
  - Windows Credential Manager target name
- `endpointFamily`
  - first supported values are `coding_plan_remains` and `token_plan_remains`
- `validationState`
  - runtime status such as `unknown`, `pending`, `valid`, `invalid`, or `unreachable`
- `lastValidatedAt`
  - last validation timestamp
- `lastValidationError`
  - last runtime validation error text

### Storage Mapping

The first practical storage split should be:

- `accounts`
  - `account_key` <- `accountKey`
  - `display_name` <- `accountDisplayName`
  - `scope_type` <- `scopeType`
  - `scope_identifier` <- `scopeIdentifier`
- `credential_profiles`
  - `profile_name` <- `profileName`
  - `secret_ref` <- `secretRef`
  - `credential_type` <- `credentialType`
  - `last_validated_at_utc` <- `lastValidatedAt`
- `plans`
  - `plan_key` <- `planKey`
  - `display_name` <- `planDisplayName`
  - `plan_type` <- `subscription`
  - `quota_window` <- provider-confirmed window label once repeated reset-window captures are complete

The current SQLite schema does not yet have dedicated columns for `validationState` or `lastValidationError`.

For the first shipped slice, those two fields should remain runtime/provider metadata rather than forcing a speculative migration.

### Secret Reference Contract

The first supported MiniMax subscription secret target should be:

- `JiStats/minimax/subscription/<plan_key>/<account_key>`

Examples:

- `JiStats/minimax/subscription/coding_plan/default`
- `JiStats/minimax/subscription/token_plan/work-account`

This keeps the secret reference stable across restarts while allowing one MiniMax account scope to hold more than one subscription credential profile over time.

### Polling Contract

The first runtime polling seam should resolve three things together from the stored profile:

- normalized MiniMax subscription profile metadata
- the Windows Credential Manager bearer token referenced by `secretRef`
- the endpoint path derived from `endpointFamily`

That means the scheduler/runtime path can:

1. load MiniMax credential metadata from `credential_profiles`
2. reconstruct the MiniMax subscription profile
3. resolve the bearer token from Windows Credential Manager
4. poll the right remains endpoint without persisting the raw secret in SQLite

### First-Shipped Scope

The first shipped MiniMax credential-management slice should support:

- Coding Plan subscription profile creation
- Token Plan subscription profile creation
- Windows Credential Manager storage for the raw subscription secret
- persisted credential metadata in `credential_profiles`
- runtime retrieval of bearer token + endpoint family for quota polling

It should not yet imply:

- PAYG credential parity
- automatic surface detection beyond the validated remains endpoints
- exact user-visible reset labels before repeated live capture confirms the semantics

## Account Structure for MiniMax

```
MiniMax Account
├── PAYG Surface
│   └── credential_profile: credential_type="api_key"
│   └── plan: plan_type="payg", quota_window="unlimited"
│
├── Token Plan (Subscription)
│   └── credential_profile: credential_type="subscription_key"
│   └── plan: plan_type="subscription", quota_window="<provider-confirmed interval>"
│
└── Coding Plan (Subscription)
    └── credential_profile: credential_type="subscription_key"
    └── plan: plan_type="subscription", quota_window="<provider-confirmed interval>"
```

## v1.0 Scope Recommendation

**Coding Plan subscription first** is now the planning priority for MiniMax v1.0.

Rationale for this direction:
1. Product direction now prioritizes subscription quota mirroring for MiniMax before PAYG parity.
2. Coding Plan-first validates multi-plan quota semantics earlier.
3. PAYG support can remain in-scope but should not be the first implementation priority.

**Working rollout direction**:
- v1.0 priority path: Coding Plan
- v1.0 secondary path: Token Plan
- v1.x: PAYG or remaining MiniMax surfaces not validated for v1.0

## UI Flow for Adding MiniMax Account

### Step 1: User selects "Add MiniMax Account"

UI shows two paths:
- "I use MiniMax API (pay-as-you-go)"
- "I have a MiniMax subscription (Token Plan or Coding Plan)"

### Step 2a: Coding Plan Subscription Path (v1.0 priority)
1. User enters Coding Plan subscription key (Bearer)
2. JiStats validates against `GET /v1/api/openplatform/coding_plan/remains`
3. On success: create account + credential_profile + plan (subscription/coding)
4. Show "Coding Plan account added"

### Step 2b: PAYG Path (v1.x deferred)
1. User enters API key
2. JiStats validates key by calling MiniMax API
3. On success: create account + credential_profile + plan (payg)
4. Show "PAYG account added"

### Step 2c: Additional Subscription Surfaces (future expansion)
1. User enters additional subscription credentials (e.g., Token Plan)
2. JiStats validates against the corresponding MiniMax subscription API
3. On success: create additional plan + credential profile mapping
4. Show plan-specific success state

## Sync Model for MiniMax Surfaces

### For Coding Plan subscription (v1.0 priority):

1. **Track local events**:
   - Capture local request events for Coding Plan usage
   - Store usage events with plan linkage

2. **Import quota status**:
   - Poll MiniMax subscription quota endpoints (`coding_plan/remains`, `token_plan/remains`)
   - Import as `source_type = 'imported'`

3. **Reconcile**:
   - Compare local usage vs imported plan quota snapshots
   - Update confidence tier and mirror basis

### For PAYG (v1.x deferred path):

1. **Track local events** (same as OpenAI):
   - Applications using the API key generate usage events
   - Local event tracking captures: timestamp, model, tokens, cost

2. **Import from MiniMax API** (if usage API available):
   - Poll MiniMax usage endpoint
   - Import as `source_type = 'imported'`

3. **Reconcile**:
   - Compare local vs imported
   - Update confidence tier

### For other subscriptions (v1.0+):

1. **Track local events** (same as PAYG):
   - Local event tracking for subscription requests

2. **Quota status** (if API available):
   - Poll MiniMax subscription API for remaining balance
   - Update `quota_snapshots`

3. **If endpoint unavailable for account**:
   - Mark quota metrics as `Unknown`
   - User can trigger manual refresh/retry
   - Keep fallback path explicit in UI

## MiniMax-Specific Sync Kind Values

From `docs/sqlite-schema.md`:

```
sync_state.sync_kind values:
  - "usage" (for PAYG usage sync)
  - "subscription_quota" (for Coding Plan and other subscription quota sync)
  - "reconciliation" (for gap reconciliation)
```

## Storage Requirements

### For PAYG (v1.x scope):

Per user per month:
- ~100-1000 usage events per day
- ~3k-30k events per month
- Retention: 90 days events = ~9k-90k events

### For subscription-heavy usage (v1.0 priority path and later expansion):

Same as PAYG for local events. Additional:
- Subscription quota snapshots (interval-based)
- Different sync_state per subscription

## Dashboard Display by MiniMax Surface

For v1.0 Coding Plan accounts:
- Show quota remaining/used for active Coding Plan window (if API available)
- Show confidence and freshness badges clearly
- Show fallback `Unknown` with explicit reason if provider signal is unavailable

For PAYG accounts (v1.x):
- Show usage metrics from local events
- Show imported usage (if API available)
- Badge: Local / Imported
- Quota pressure: N/A for unlimited PAYG (show "Unlimited")

For later subscription surfaces:
- Show usage metrics from local events
- Show quota remaining from subscription API
- Badge: Local / Imported / Exact
- Quota pressure: (used / limit) as percentage

## Error Handling

### Invalid API Key
- Show error: "MiniMax API key rejected"
- Don't create account

### Subscription API Unavailable
- If Token Plan API fails: show "Token Plan API unavailable"
- Don't block user, but mark subscription metrics as `Unknown`
- Suggest manual dashboard check

### Credential Mismatch
- If user tries to use subscription key for PAYG endpoint: error
- Clear error: "This credential is for subscriptions, not API access"

## Implementation Notes

1. **Multiple credentials per account**: MiniMax requires this pattern
2. **Plan-based quota tracking**: Use `plans` table for quota_window and limit_value
3. **Sync scheduler**: Must handle different sync intervals per surface (PAYG hourly, subscription cadence tied to provider freshness and reset-window evidence rather than assumed daily/monthly labels)
4. **Connectivity state**: Per account, so PAYG could be online while subscription offline

## Deterministic Reset-Window Evidence Capture Inputs

The repeated-live cadence, required payload fields, comparison checks, and provider-model mapping rules now live in:

- `docs/fixtures/minimax/reset-window-capture-inputs.md`

Use that file as the execution note for the remaining Coding Plan and Token Plan reset-window validation instead of redefining the capture contract ad hoc in multiple docs.

## Open Questions for Validation

1. **PAYG API**: What is the exact endpoint for usage history?
2. **PAYG Auth**: Does PAYG require a different key shape/issuer from subscription keys?
3. **Reset semantics**: How should `start_time` / `end_time` windows map to user-visible periods?
4. **Credential UX**: Should UI auto-detect subscription vs PAYG by validation response?

## References

- MiniMax mirror spec: `docs/minimax-mirror-spec.md`
- Entity model: `docs/entity-model.md`
- OpenAI scope decision: `docs/openai-scope-decision.md` (for comparison)
