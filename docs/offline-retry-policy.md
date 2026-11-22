# Offline, Retry, Notification, and Reconnection Policy

> **Status**: Current
> **Purpose**: Define behavior for connectivity failures and recovery
> **Driven by**: plan.md section 10.4.1 (Connectivity Recovery Direction) and section 12.5 (Notification Rules)

## Core Principle

Offline is a **normal operating condition**, not an exception.

From plan.md section 10.4.1:
> Design for intermittent connectivity, laptop sleep/wake cycles, and extended offline periods without crashes or noisy alerts.

## Offline Classification

### Connectivity State Machine

From `docs/plan.md` section 10.2.2 operating states:

```
starting → steady_online ↔ degraded_offline
                ↓
             syncing → catching_up
                ↓
          error_blocked → shutting_down
```

### Connectivity Statuses (`connectivity_state`)

`connectivity_state.status` is stored as text with default `online`; the current runtime-refresh baseline writes a narrower set of persisted statuses than the broader conceptual operating-state diagram.

| State | Meaning | Dashboard Indicator |
|-------|---------|-------------------|
| `online` | Provider/account path is reachable and fresh enough for normal display | Green dot |
| `stale` | Provider/account path still has a last known snapshot, but freshness aged out | Yellow dot |
| `catching_up` | Provider/account path is actively reconciling after a gap or recovery marker | Yellow dot + syncing wording |
| `offline` | Provider unreachable | Red dot |

Current implementation note:

- the broader plan-level operating-state diagram is still useful for architecture discussion, but the persisted per-provider/account runtime refresh states are currently `online`, `stale`, `catching_up`, and `offline`
- the first recovery baseline uses `sync_state(sync_kind = "runtime_refresh")` plus `runtime_state.recovery_marker` to remember pending catch-up work after launch gaps or offline runtime failures
- provider-side catch-up now also writes `sync_state(sync_kind = "runtime_backfill")` per provider/account so JiStats retains the last attempted recovery window plus the most recent backfill outcome (`no_data`, `imported`, or `fetch_failed`)

### Failure Categories

`connectivity_state.failure_category` and `sync_state.last_failure_category` are text fields, not schema-enforced enums. The current runtime-refresh baseline most commonly persists `provider_unreachable` for an offline provider refresh failure, while the wider classification vocabulary below remains the intended direction as error handling gets more specific.

| Category | Meaning | Retry Behavior |
|----------|---------|----------------|
| `provider_unreachable` | Current baseline catch-all when a provider refresh path cannot be reached | Persist offline state and retry with backoff |
| `network` | No internet connection | Exponential backoff |
| `timeout` | Provider not responding | Exponential backoff |
| `auth` | API key invalid or expired | Alert user, stop retry |
| `rate_limit` | Provider rate limited | Wait, then retry |
| `api_error` | Provider server error (5xx) | Exponential backoff |
| `unknown` | Unclassified failure | Exponential backoff |

## Retry Strategy

### Exponential Backoff

From `docs/gap-handling-policy.md`:
```cpp
// Retry timing: 1s, 2s, 4s, 8s, 16s, 32s, 64s, max 5 min
int retry_delay_seconds = min(pow(2, attempt_number), 300);
```

### Retry Limits

| Failure Type | Max Retries | Behavior After Limit |
|--------------|-------------|---------------------|
| Network | Infinite (until online) | Continue in background |
| Timeout | 10, then pause 5 min | Pause, then retry |
| Auth | 0 (fail immediately) | Alert user, stop |
| Rate limit | 5, wait for reset | Pause until reset |
| API error | 10, then pause 15 min | Pause, then retry |

### Retry Persistence

From `docs/sqlite-schema.md` (connectivity_state table):
```sql
next_retry_at_utc TEXT  -- When to retry next attempt
```

Current implementation note:

- the first runtime baseline now persists `last_checked_at_utc`, `next_retry_at_utc`, and `last_failure_category` during provider refreshes
- runtime refresh currently uses a simple persisted retry cadence (`5m`, then `15m` while still offline) rather than the full future scheduler matrix described below

## Notification Rules

From plan.md section 12.5:

### Rule Matrix

| Rule | Expected Behavior |
|------|------------------|
| Offline transition | Show one dismissible notice when transitioning from online to offline |
| Same-state suppression | Do not repeat the same notice while the app remains in the same offline state |
| Persistent status | Keep the offline indicator visible even after dismissal |
| Recovery | Clear the offline notice state automatically when connectivity is restored |
| Error separation | Distinguish offline notices from authentication failure notices |
| UX safety | Never block core app usage behind a modal connectivity error |

### Notice Cooldown

From `docs/sqlite-schema.md` (app_settings seed data):
```sql
('offline_notice_cooldown_minutes', '60'),  -- Don't spam notices
```

**Rule**: After dismissing an offline notice, don't show another for 60 minutes unless state changes (e.g., offline → online → offline again).

The app clears a saved dismissal only when the current refresh confirms recovery and no provider reports Offline. Recovery is confirmed when a provider quota API returns usable quota in that refresh, or when a provider that was offline in the dismissed outage returns a result in that refresh. Local reads by providers that were not part of that outage, such as the Claude Code stats cache or an unchanged Codex rollout, preserve the saved dismissal, as do cached quota, empty refreshes, and sample-only results.

## Offline UI Behavior

### Dashboard Indicator

```
Status: ● Offline
Last sync: 3 hours ago
[Retry Now]
```

- Persistent red dot in dashboard header
- "Offline" badge on metrics
- "Retry Now" button to attempt immediate reconnection

### Tray Icon

```
Tooltip: "JiStats - Offline
Last sync: 3 hours ago"
```

- Red dot in tray when offline
- Double-click still opens dashboard
- Menu includes "Retry Now" and "Settings"

### Notice Display

```
┌────────────────────────────────────────┐
│ ⚠ Unable to reach OpenAI              │
│                                        │
│ JiStats will retry automatically       │
│ when your connection improves.          │
│                                        │
│ [Dismiss]              [Retry Now]     │
└────────────────────────────────────────┘
```

**Key**: One notice only on initial transition. Dismissal shows persistent indicator but no repeat notice.

## Reconnection Behavior

### On Network Recovery

```cpp
// When network becomes available:
1. connectivity_state.status changes to "online"
2. Clear notice_dismissed_at_utc
3. Trigger immediate sync
4. If last sync > threshold (1 hour):
   - Enter "catching_up" state
   - Run reconciliation
   - Show "Syncing..." briefly
5. Clear offline indicator
```

### Catch-Up Reconciliation

From `docs/gap-handling-policy.md`:
- Fetch missed data from provider API
- Mark as `is_reconciled = 1`
- Update confidence tiers
- Show brief "Up to date" confirmation

## Sleep/Wake Cycle Handling

### On System Sleep

```
1. Detect Windows sleep event
2. Stop sync scheduler (no point retrying)
3. Record sleep time in runtime_state
4. Keep tray icon visible (no crash)
```

### On System Wake

```
1. Detect wake event
2. Wait 5 seconds for network to stabilize
3. Run connectivity check
4. If online: trigger sync and catch-up
5. If still offline: show offline indicator
```

## Laptop/Bad Wi-Fi Scenarios

### Scenario: User switches networks

```
1. Detect network change (Windows API)
2. Run connectivity check immediately
3. If new network works: resume sync
4. If no connection: enter offline state
5. No alert spam - just indicator change
```

### Scenario: Intermittent connection

```
1. First failure: retry with backoff
2. Second failure (quick): increase backoff
3. After 3 failures in 5 min: mark as offline
4. Show offline indicator
5. Continue background retry every 5 min
```

### Scenario: VPN toggle

```
1. Same as network switch
2. Quick retry to detect new state
3. Resume normal operation if works
```

## Authentication Failures (Special Case)

### Invalid API Key

```
State: auth failure
Detection: 401 or 403 from provider
Behavior:
  - Do NOT retry (will fail forever)
  - Alert user: "OpenAI API key is invalid"
  - Show in settings with link to fix
  - Stop sync for this account only
```

### Expired Subscription

```
State: auth failure (MiniMax subscription)
Detection: 401 from subscription API
Behavior:
  - Alert user: "MiniMax subscription expired"
  - Link to renewal
  - Stop quota monitoring for subscription
```

## Implementation Notes

### ConnectivityService Component

From `docs/plan.md` section 10.2.1:
```
ConnectivityService
  - classifies offline state
  - tracks retry timing
  - detects recovery
```

### Database State

From `docs/sqlite-schema.md`:
```sql
connectivity_state:
  - status: free-text runtime status such as online|stale|catching_up|offline
  - failure_category: free-text classification such as provider_unreachable|network|timeout|auth|rate_limit|api_error|unknown
  - next_retry_at_utc: when to retry
  - notice_dismissed_at_utc: for cooldown
```

## Edge Cases

### 1. App offline for days

- Continue background retry with 5-min intervals
- After 24 hours, increase retry interval to 15 min
- User sees stale data with clear "Offline" indicator
- On recovery: catch-up reconciliation

### 2. Provider API down for hours

- Treat as offline, not auth error
- Show "OpenAI API temporarily unavailable"
- Retry with backoff (not user-facing errors)
- When API recovers: automatic catch-up

### 3. User dismisses notice, provider comes back

- Clear notice automatically on recovery
- Show brief "Connected" toast (auto-dismiss after 3 sec)

### 4. Multiple accounts in different states

- OpenAI offline but MiniMax online: show both states
- Dashboard: separate indicators per account
- Tray: show worst-case indicator (most important to notice)

## Settings

### Relevant Settings

From `docs/sqlite-schema.md`:
```sql
('offline_notice_cooldown_minutes', '60'),
('polling_interval_seconds', '3600'),
```

### User Configurable

- "Show offline notices": On/Off (default: On)
- "Retry frequency": Every 1/5/15 min (default: 5 min)

## Testing Requirements

From `docs/plan.md` section 14.3:
- Offline recovery tests: DNS failure, timeout, resume, catch-up
- Long-gap tests: startup after days or weeks without sync
- Notification cooldown tests: verify no spam

## References

- Connectivity direction: `docs/plan.md` section 10.4.1
- Notification rules: `docs/plan.md` section 12.5
- Gap handling: `docs/gap-handling-policy.md`
- Connectivity state: `docs/sqlite-schema.md` (connectivity_state table)
