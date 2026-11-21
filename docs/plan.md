# 📘 JiStats Plan

## 1. 📌 Summary

JiStats is planned as a lightweight Windows 11 desktop application for monitoring AI usage from a local machine.

OpenAI and Codex remain the primary proof-of-concept provider for v1.0 validation depth, MiniMax remains in initial scope, and Phase 7 now brings Claude Code and Kimi Code into the v1.0 release through bounded local-first support.

Longer-term provider expansion should also leave room for Kilo Code and for deeper provider-backed corroboration paths for Claude Code and Kimi Code after the initial four-provider release is stable.

For OpenAI, the v1.0 planning baseline is now subscription-first tracking rather than assumed API-platform access. ChatGPT subscription billing and OpenAI API billing are separate surfaces, so v1.0 must not assume that a subscriber automatically has API billing or an API key available. The current Phase 0 closeout state reflects that split: the subscription-first baseline is already accepted, while the only remaining blocked advanced-path item is OpenAI API-platform freshness/lag capture after a higher-scope credential is available.

The long-term goal is to support multiple providers, multiple API products, and multiple subscription or billing plans from a unified desktop application, including future subscription-focused tracking for tools such as Kilo Code and stronger corroborated support for Claude Code and Kimi Code beyond the initial local-first v1.0 scope.

The product should help a user answer three questions:

1. What am I using right now?
2. What have I used over time?
3. How close am I to cost or quota pressure?

The primary product objective is to mirror provider quota behavior as accurately as possible across the v1.0 provider set, while being explicit when a subscription surface does not expose enough trustworthy provider-side signals for exact mirroring and must remain local-first, estimated, or unknown.

This plan assumes a native-first Windows desktop application with a local database, small operational footprint, and an architecture that can grow from an OpenAI-plus-MiniMax proof path plus Claude/Kimi local-first additions in v1.0 to broader multi-provider coverage over time.

The default implementation stack for v1.0 is native `C++/C`.

### 1.1 Quick Reference

| Item | Value |
| --- | --- |
| Product Name | `JiStats` |
| Primary Goal | Mirror provider quota behavior as accurately as possible |
| v1.0 Providers | OpenAI / Codex + MiniMax + Claude Code + Kimi Code |
| Platform | Windows 11 |
| Default Stack | Native `C++/C` |
| Primary User Surface | Dashboard + tray + Windows 11 taskbar |
| Primary Current-Data Experience | In-app tables, charts, graphs, and status indicators |
| Key Operational Requirement | Offline-safe, catch-up capable, low-overhead background behavior |

## 2. 🧭 Planning Principles

- Prefer a trustworthy MVP over a feature-rich but speculative first release.
- Design internal data structures so that the first two providers can ship without boxing out future providers.
- Use the fullest trustworthy set of official OpenAI and MiniMax signals plus the validated Claude/Kimi local artifacts and policy boundaries already documented for Phase 7 before inventing fallback behavior.
- Do not assume a provider's consumer subscription automatically grants developer-platform access, API billing, or API keys.
- Prefer direct telemetry, usage, and billing endpoints over model invocations for monitoring workflows.
- If a model invocation is ever required for a provider-specific check, use the lowest-cost acceptable model path and keep the behavior user-visible.
- Treat quota mirroring as the core product problem, not an optional enhancement.
- Prefer native tooling and distribution so the user is not required to install a runtime.
- Avoid adding extra implementation languages unless they solve a validated problem better than `C++/C`.
- Distinguish verified provider signals from locally inferred estimates.
- Keep Claude/Kimi local parser and fixture work inside the approved spike and redaction-contract boundaries unless those docs are updated first.
- Keep all local telemetry and credentials on the local machine.
- Sequence high-risk UI integrations after the data pipeline is proven, even when they are required for v1.0.
- Design for continuity across app downtime, reboot, sleep, and long gaps between launches.
- Treat intermittent or poor connectivity as a normal operating condition, not an edge case.

## 3. 🎯 Product Vision

### 3.1 Near-Term Vision

Version 1.0 should deliver a polished desktop utility that can:

- mirror provider quota state as accurately as possible
- monitor recent usage
- show historical trends
- surface cost or quota pressure
- present current and recent data directly inside the application
- support OpenAI, MiniMax, Claude Code, and Kimi Code from the start, with provider-specific confidence boundaries made explicit
- support subscription-first tracking when that is the user's actual product surface
- live in the tray
- render real-time metrics in the Windows 11 taskbar

### 3.2 Long-Term Vision

The long-term product should become a single control surface for many AI services, including:

- direct API usage
- subscription-backed tools where measurable activity can be captured
- multiple accounts per provider
- multiple plans or billing contexts per provider
- future subscription-oriented coding tools such as Kilo Code, plus deeper corroborated Claude/Kimi coverage when their trustworthy signal paths are better understood

The product should eventually answer:

- which provider am I using most
- which account or plan is nearing a limit
- how usage differs across tools, models, and vendors

### 3.3 Release Strategy

| Release | Focus |
| --- | --- |
| `v1.0` | OpenAI + MiniMax completed product plus bounded Claude Code and Kimi Code local-first support, with in-app live data views, exportable reporting, and taskbar metrics |
| `v1.x` | Refinement, reliability, and stronger provider-backed corroboration for OpenAI, MiniMax, Claude Code, and Kimi Code |
| `v2+` | Additional providers and broader account or subscription coverage, including future investigation of Kilo Code and other provider families |

## 4. ✅ Product Goals

- Show recent usage in a simple desktop dashboard.
- Make the application UI the primary place to inspect current and recent data.
- Preserve enough local event history to support short-term windows and trend views.
- For Codex local usage attribution, use sanitized `state_5.sqlite` thread metadata as a provider/model signal while keeping rollout JSONL as the quota/rate-limit source.
- Import or synchronize official usage or cost data when available.
- Make uncertainty visible when a metric is estimated rather than authoritative.
- Mirror provider quota behavior as accurately as possible from validated signals.
- Support both API-based usage and subscription-backed quota usage where the provider exposes enough trustworthy signals.
- For subscription-only OpenAI users, prefer a truthful local-first experience over assuming inaccessible API imports.
- Run comfortably as a background Windows utility.
- Launch cleanly at Windows startup when enabled by the user.
- Reconcile gaps after downtime so charts and summaries remain trustworthy.
- Minimize the app's own contribution to API cost and usage while monitoring.
- Ship as a native standalone Windows application without requiring end users to install Python, .NET, or another managed runtime.
- Remain stable and informative during offline periods, poor Wi-Fi, or other connectivity failures.
- Support more than one provider over time without rewriting the core data model.
- Support more than one account or plan per provider.

## 5. 🚫 Non-Goals For V1.0

- Exact replication of provider-side quota reset behavior.
- Multi-user collaboration features.
- Cloud sync, hosted dashboards, or remote storage.
- HTML exports as the primary live-data viewing experience.
- Large plugin systems.
- Broad provider support in the first release beyond OpenAI, MiniMax, Claude Code, and Kimi Code.
- Kilo Code implementation work before the four-provider v1.0 baseline is stable enough to generalize safely.
- Raw prompt, tool, or credential ingestion from Claude/Kimi local artifacts.
- Exact Claude/Kimi subscription-quota replication without separately validated corroborating signals.

## 6. 🧱 Core Product Tracks

### 6.1 Provider Abstraction

The core architecture should be provider-agnostic even though the initial release targets OpenAI, MiniMax, Claude Code, and Kimi Code.

The design should account for:

- provider identity
- account identity
- credential profile
- plan or billing context
- model catalog differences
- provider-specific rate-limit semantics

That provider abstraction should remain flexible enough to model the bounded local-first Claude/Kimi v1.0 paths and future subscription-backed coding surfaces such as Kilo Code without forcing the OpenAI/MiniMax assumptions onto them.

### 6.2 Local Activity Tracking

Track locally observed activity that can support:

- short-term recent windows
- per-model summaries
- session-level trends
- local usage estimates

This track is under our control and should be the foundation of the MVP.

### 6.3 Official Usage And Cost Ingestion

Where provider APIs make it possible, ingest official historical usage or cost data for:

- reconciliation
- reporting
- longer-term accuracy
- gap filling after the app has been offline

This track should be treated as authoritative when available.

### 6.4 Quota And Limit Estimation

Provide a clearly labeled estimate of quota or rate-limit pressure using:

- official signals when exposed
- local rolling calculations when not exposed

This track must never pretend to be exact unless exact behavior has been validated.

### 6.5 Provider Quota Mirroring

Provider quota mirroring is the main goal of the product.

For v1.0, the app should attempt to reconstruct and present provider quota state for OpenAI and MiniMax using the best available combination of:

- official usage and cost endpoints
- rate-limit related signals
- local event history
- provider-specific timing and reset logic
- reconciliation after downtime

Where a subscription product does not expose sufficient provider-side signals, the app should degrade to clearly labeled local-observation and confidence-based tracking rather than pretending that exact quota state is known.

This track should drive both the data model and the user experience.

### 6.6 Cost-Aware Monitoring Operations

The monitor should avoid generating unnecessary spend while gathering telemetry.

Operational rules:

- use provider usage, billing, and metadata endpoints before considering any model call
- do not use a general model invocation for routine "check in" behavior if a direct API endpoint exists
- if a provider requires a model-backed probe for a limited scenario, route it through the lowest-cost acceptable model choice
- make model-backed check behavior configurable and easy to disable
- track the monitor's own operational requests separately from observed user activity where possible

### 6.7 Windows Surface Layer

The product should expose usage in three Windows surfaces:

- dashboard
- tray
- taskbar

The taskbar surface is a required v1.0 feature, not an optional bonus.

### 6.8 Lifecycle And Continuity

The app should behave predictably across:

- Windows startup
- user logon
- sleep and resume
- app crashes or forced shutdowns
- several hours, days, or weeks of inactivity

Continuity behavior should include:

- optional run-on-startup
- launch-time refresh of recent metrics
- background reconciliation for missed history
- explicit handling of periods with no observed usage

### 6.9 Offline And Connectivity Resilience

The app should operate correctly for:

- laptop users moving between networks
- users with unstable Wi-Fi
- users with no internet access for short or long periods
- temporary DNS, TLS, or API reachability failures

Offline behavior should include:

- no crashes when connectivity checks fail
- one dismissible notice when the app detects it cannot reach required provider endpoints
- a persistent visual offline indicator until connectivity is restored
- paused or deferred sync behavior instead of repeated failing work
- automatic catch-up and reconciliation when connectivity returns

## 7. ⚠️ Major Risks And Assumptions

### 7.1 Data Availability Risk

The project depends on what provider APIs actually expose for usage, cost, and rate-limit information. The MVP must begin with a validation spike before committing to UX promises.

For OpenAI specifically, the plan must distinguish between ChatGPT/Codex subscription access and Platform API access. These are separate surfaces and may require different credentials, billing setup, and product assumptions.

### 7.2 Mirror Risk

Quota mirroring is a required product capability, but the exact level of achievable fidelity depends on what provider signals can be validated. The plan should treat mirroring as a primary engineering track with explicit confidence and accuracy measurement.

### 7.3 Data Freshness Risk

Official usage or billing APIs may be aggregated, delayed, or shaped differently than local real-time telemetry needs. The plan must support both local near-real-time estimates and later reconciliation.

### 7.4 Taskbar Integration Risk

Windows 11 taskbar integration is required for v1.0, but it is still a high-risk implementation area. The plan should treat it as a dedicated engineering track with its own prototype and validation checkpoints.

### 7.5 Multi-Provider Scope Risk

Trying to support many providers too early could delay the first usable release. The architecture should prepare for multi-provider growth, but v1.0 should remain intentionally bounded to OpenAI, MiniMax, Claude Code, and Kimi Code, with Kilo and broader expansion deferred.

### 7.6 Gap Reconciliation Risk

If the application is not running for long periods, local-only telemetry will produce gaps. The plan must distinguish between:

- known zero usage
- unknown missing data
- reconstructed history imported from official APIs

### 7.7 Monitoring Overhead Risk

If the app uses model calls for routine health checks or telemetry gathering, it could inflate usage and distort the very metrics it is trying to report. Monitoring behavior should be designed to keep self-generated usage negligible.

### 7.8 Connectivity Failure Risk

If connectivity failures are treated as exceptional rather than routine, the app could become noisy, misleading, or unstable for laptop users and unreliable networks. The plan should explicitly classify network failures and define recovery behavior.

## 8. 🏁 Recommended V1.0 Scope

### 8.1 Required Outcomes

The first useful version should:

- support OpenAI as the primary proof provider
- support MiniMax as an initial provider
- support Claude Code as a bounded local-first provider in v1.0
- support Kimi Code as a bounded local-first provider in v1.0
- mirror OpenAI quota state as a primary feature
- mirror MiniMax API and subscription quota state as a primary feature
- prioritize MiniMax Coding Plan subscription support ahead of MiniMax PAYG sequencing
- surface Claude/Kimi local activity and runtime summaries without ingesting raw prompt, tool, or credential content
- label Claude/Kimi local-only values explicitly when provider-exact quota truth is unavailable
- store local usage events
- show recent rolling windows such as 5 minutes, 30 minutes, and 5 hours
- display per-model and daily totals
- present current and recent usage inside the app through dashboard views, tables, charts, graphs, and status indicators
- support a tray icon, compact dashboard, and Windows 11 taskbar metrics
- support manual or scheduled HTML export
- mark each metric as either local, imported, or estimated
- support at least one account or credential profile cleanly
- preserve a provider-neutral internal model for future expansion
- support optional run-on-startup behavior
- refresh and reconcile data on launch after downtime
- avoid routine model usage for monitoring when direct provider endpoints can supply the same information
- show a dismissible notice plus a persistent offline indicator when provider connectivity is unavailable
- recover and catch up automatically after connectivity returns

### 8.2 Explicitly Excludes

The first version should not promise:

- full automation around Windows Scheduler
- complex forecasting beyond simple, explainable estimates
- more than OpenAI, MiniMax, Claude Code, and Kimi Code shipping on day one
- raw transcript or credential ingestion as a requirement for Claude/Kimi support
- exact Claude/Kimi quota mirroring without corroborated provider signals

## 9. 🔌 Provider Expansion Strategy

### 9.0 Initial Provider Requirement Discovery

Before locking v1.0 behavior, the project should extract as much useful structure as possible from OpenAI's and MiniMax's official APIs, pricing surfaces, subscription surfaces, and exposed response signals, plus the validated Claude/Kimi local runtime artifacts and policy boundaries already documented for Phase 7.

This discovery work should drive:

- what metrics are truly available
- which values are authoritative
- which values must be estimated
- what historical backfill is possible on launch
- what gaps can be reconstructed and what gaps must remain explicitly unknown
- which monitoring workflows can be satisfied without any model invocation at all
- what quota mirror can be reconstructed confidently
- what mirror accuracy can be measured against real provider behavior

The initial provider capability inventory should cover:

- usage history
- cost history
- model granularity
- account, project, or organization scoping
- subscription-plan scoping
- rate-limit related signals
- timestamps and freshness expectations
- any identifiers needed for reconciliation
- whether any monitoring action would require a model call and how to minimize that cost
- quota windows, resets, decay, or bucket behavior if inferable

### 9.0.1 Initial Provider Scope

| Provider | Required v1.0 Surfaces | Planning Notes |
| --- | --- | --- |
| OpenAI | Subscription-first quota/status tracking with Codex-local augmentation, plus optional separate API-platform import when distinct billing/credentials exist | Remains the primary proof-of-concept validation path, must not assume API billing for subscription users, and currently leaves only the blocked advanced-path freshness/lag capture after the archived `2026-04-19` authz probe |
| MiniMax | Coding Plan + Token Plan subscription quota mirroring for v1.0, with PAYG deferred to v1.x | Must account for separate credential types, plan types, and quota semantics |
| Claude Code | Bounded local-first activity/runtime summaries from approved `~/.claude` artifacts in v1.0 | Must stay inside `docs/claude-local-surface-spike.md` and `docs/claude-kimi-local-redaction-contract.md`; provider-mode classification remains `Unknown` unless separately validated |
| Kimi Code | Bounded local-first activity/runtime summaries from approved `~/.kimi` artifacts in v1.0, plus provider-exact quota mirroring through the validated `/usages` API path when usable OAuth exists | Must stay inside `docs/kimi-local-parser-spike.md` and `docs/claude-kimi-local-redaction-contract.md`; local artifacts remain activity/history evidence, while provider-exact quota truth is available only through the validated OAuth-backed `/usages` path and should stay empty rather than fabricated when that path is unavailable |

Post-v1.0 provider candidates after the four-provider baseline is stable:

- Kilo Code subscription tracking

Deeper provider-backed corroboration for Claude Code and Kimi Code belongs in `v1.x` hardening, not in the decision about which entirely new provider to onboard next.

### 9.0.2 MiniMax-Specific Planning Notes

MiniMax adds an important second-provider case because it combines direct pay-as-you-go API access with subscription-backed usage plans that use their own keys, quotas, and reset behavior.

For JiStats planning, MiniMax support should explicitly account for:

- Coding Plan subscription as the first-priority v1.0 MiniMax surface
- pay-as-you-go API usage
- Token Plan, Coding Plan, or other subscription-backed usage where available
- provider-specific key separation between subscription-backed and pay-as-you-go access
- request-window quota behavior for supported text plans
- daily-reset style quotas for non-text subscription surfaces when applicable
- provider-specific rate limits and monitoring cost controls

The purpose of MiniMax in initial scope is not just "another provider," but a validation case for provider-specific plan semantics beyond traditional pay-as-you-go billing.

### 9.1 Supported Entity Model

The planning model should assume the app may eventually need to represent:

- provider
- account
- workspace or organization
- API key or credential profile
- billing plan or subscription tier
- model family
- usage source

### 9.2 Provider Adapter Concept

Each provider adapter should be responsible for:

- authentication or credential handling
- API discovery and request formatting
- usage import mapping
- cost mapping
- rate-limit interpretation
- capability reporting back to the app

### 9.3 Capability Matrix

Every provider should eventually be evaluated against a shared capability matrix:

- real-time local capture support
- official historical usage support
- official cost support
- rate-limit visibility
- subscription-plan visibility
- model granularity
- per-account support
- launch-time backfill support
- enough timestamps or cursors for reliable reconciliation
- whether routine monitoring can be done without model inference
- cheapest acceptable probe path if inference is unavoidable
- quota mirroring feasibility
- measurable mirror accuracy

The first version of this matrix must include both OpenAI and MiniMax.

## 10. ⚙️ Technical Direction

### 10.1 App Stack

- Language: C++ with C where appropriate
- Platform: Windows 11
- UI: Native Win32 windows and common controls
- Rendering: DirectX 11 only where a custom surface is justified
- Networking: WinHTTP
- Storage: SQLite
- Secret storage: Windows Credential Manager

### 10.1.1 Language Decision

The default v1.0 stack should remain `C++/C` for these reasons:

- native standalone distribution without requiring a user-installed runtime
- low baseline memory overhead for a long-running desktop utility
- strong fit for deep Windows shell and taskbar integration
- direct alignment with reference material such as TrafficMonitor

Other language options were considered but are not the default recommendation for v1.0:

- `.NET`: rejected as the primary stack because self-contained deployment increases application size and framework-dependent deployment requires a user runtime
- `Go`: not preferred because the runtime and garbage collector are a poor fit for the low-overhead always-running utility goal
- `Delphi`: not preferred because the toolchain and licensing model do not fit the project
- `Rust`: viable as a replacement candidate for selected modules, but not recommended as an added language for v1.0 because it increases architecture and FFI complexity
- `Zig`: interesting future evaluation candidate, but not recommended over `C++/C` for the initial Windows-heavy release

For v1.0, the project should avoid a multi-language architecture unless a later implementation spike proves a clear benefit.

### 10.2 Architecture Layers

- `src/app`: application lifecycle, startup, shutdown, message loop
- `src/providers`: provider adapters, capability descriptors, and request mapping
- `src/observation`: local activity/session observation contracts and capture services
- `src/core`: shared domain models, orchestration, and app services
- `src/db`: schema, migrations, retention, and query helpers
- `src/analytics`: rolling windows, summaries, and estimate logic
- `src/gui`: dashboard, settings, and tray interactions
- `src/taskbar`: Windows 11 taskbar integration and rendering logic
- `src/reporting`: HTML export and report templates
- `src/platform`: Windows-specific integrations such as notifications, shell hooks, and credential plumbing

### 10.2.1 Runtime Components

The runtime should be decomposed into clear components so multiple engineers can work in parallel:

- `AppController`
  - owns startup, shutdown, mode transitions, and major subsystem wiring
- `ProviderManager`
  - loads provider adapters, account profiles, and capability metadata
- `LocalObservationService`
  - captures local subscription/activity signals that do not belong in provider-owned usage imports
- `ObservationPersistenceBridge`
  - binds local observation events to durable provider/account rows and persists them into `activity_events`
- `SyncScheduler`
  - decides when to poll, back off, retry, reconcile, and catch up
- `MirrorEngine`
  - computes mirrored quota state from local observations and imported provider data
- `AnalyticsEngine`
  - computes rolling windows, daily summaries, trend views, and cached aggregates
- `StorageService`
  - owns SQLite access, migrations, transactions, and retention jobs
- `ConnectivityService`
  - classifies offline state, tracks retry timing, and detects recovery
- `NotificationService`
  - owns dismissible notices, cooldowns, and transition-based user messaging
- `DashboardPresenter`
  - maps app state to dashboard view models
- `TaskbarPresenter`
  - maps app state to the Windows 11 taskbar surface
- `TrayController`
  - owns tray icon state, context menu, and quick actions
- `ReportingService`
  - produces exports from persisted data and current mirror state

### 10.2.2 Runtime State Flow

The expected high-level state flow should be:

1. launch
2. load settings, credentials metadata, runtime state, and last-known mirror state
3. bring up tray and basic UI quickly using cached local state
4. perform connectivity classification
5. if online, run startup reconciliation and mirror refresh
6. if offline, enter degraded-sync mode without blocking the user
7. continue with scheduled polling, backoff, and catch-up behavior
8. persist state changes continuously so recovery after termination is clean

The app should support these top-level operating states:

- `starting`
- `steady_online`
- `degraded_offline`
- `syncing`
- `catching_up`
- `error_blocked`
- `shutting_down`

### 10.2.3 Threading And Concurrency Model

The implementation should assume at least these execution domains:

- UI thread
  - Win32 message loop, dashboard rendering, tray interactions, taskbar presentation updates
- network worker
  - provider API requests, retry logic, connectivity checks
- database worker
  - serialized DB writes, migrations, retention, import transactions
- analytics worker
  - mirror recomputation, rollups, report preparation, cache refresh

Concurrency rules:

- no blocking network or database work on the UI thread
- all DB writes should be serialized through one ownership path
- mirror recomputation should operate on immutable input snapshots where practical
- UI surfaces should consume derived view models, not raw provider or DB objects
- background workers must publish state changes through explicit events or message passing

### 10.2.4 Build, Tooling, And Team Conventions

For a multi-engineer team, the project should standardize on:

- Visual Studio 2026 plus MSVC for the default Windows toolchain
- when a filesystem path is needed, remember that Visual Studio 2026 installs under the major-version `18` root
- CMake as the primary build system
- a reproducible dependency strategy for third-party libraries
- `clang-format` for formatting
- `clang-tidy` or equivalent static analysis in CI
- warning levels and compiler flags treated consistently across local and CI builds

Recommended conventions:

- prefer C++20 for v1.0 unless a specific dependency forces a lower target
- keep platform-specific code in `src/platform` and `src/taskbar`
- keep provider-specific code in `src/providers`
- define public headers deliberately; do not let modules reach through internal layers casually
- require small design notes for new subsystems before large implementation begins

### 10.2.5 Dependency Strategy

Each third-party dependency should be justified in writing with:

- purpose
- license compatibility
- binary size or runtime impact
- whether it is replaceable later
- whether it increases installer complexity

Initial dependency categories are expected to include:

- SQLite amalgamation
- JSON parsing support
- optional rendering helpers only if native controls are insufficient
- test libraries

The project should avoid bringing in large frameworks for concerns that can be handled natively.

### 10.3 Taskbar Feature Direction

Taskbar metrics are a required v1.0 surface.

Planning assumptions:

- the taskbar surface should show a compact real-time usage summary
- the taskbar surface should update without opening the main window
- the dashboard and taskbar should share the same underlying metric pipeline
- implementation should be informed later by TrafficMonitor as a reference point, but no repository review is required during the current planning stage

Research input for this feature:

- TrafficMonitor should be treated as an explicit implementation reference for Windows 11 taskbar metrics
- the repository does not need to be reviewed during the current planning pass
- a later research task should study how it anchors to the taskbar, survives Explorer lifecycle events, and updates real-time metrics cleanly

### 10.4 Startup And Recovery Direction

The app should support a lifecycle model that includes:

- clean first launch
- normal manual launch
- optional automatic launch at user sign-in
- recovery after the app has not run for an extended period

Planning assumptions:

- on launch, the app should load the last known local state quickly
- it should then start a reconciliation pass to refresh current metrics
- if official APIs can backfill missing history, the app should import it
- if no backfill is possible, the UI should show an explicit unknown or no-data state rather than inventing values
- if a time span is confidently known to have no usage, it may be represented as zero usage

### 10.4.1 Connectivity Recovery Direction

The app should classify connectivity problems separately from provider-authentication or provider-data issues.

Planning assumptions:

- temporary network failures should transition the app into an offline or degraded-sync state
- the app should back off instead of hammering the network or API endpoints
- the app should remember pending reconciliation work and retry later
- connectivity restoration should trigger an automatic refresh and catch-up pass
- transient failures should not reset local state or corrupt mirror calculations

### 10.5 Low-Cost Check Strategy

The app should separate monitoring operations into three classes:

- direct metadata or usage requests that do not invoke a model
- imported historical reconciliation requests
- rare model-backed probes only when a provider offers no cheaper path

Planning assumptions:

- startup refresh should first use direct provider endpoints
- background polling should prefer the cheapest non-inference path
- any inference-backed probe should have strict frequency limits
- the chosen low-cost probe path should be configurable per provider

### 10.6 Mirror Engine Direction

The mirror engine should be treated as a first-class subsystem.

Planning assumptions:

- it should combine imported provider data with local event observations
- it should support rolling or bucket-based quota logic depending on provider behavior
- it should record confidence and provenance for mirrored values
- it should support reconciliation after downtime and correction when fresher data arrives
- it should be reusable for future providers even if each provider needs custom logic

### 10.6.1 Mirror Accuracy Tiers

To reduce confusion for engineering and UX teams, the mirror should classify its own confidence:

| Tier | Meaning |
| --- | --- |
| `Exact` | Derived from direct validated provider signals with no known inference gap |
| `HighConfidence` | Derived from validated logic with minor inferred timing or lag assumptions |
| `Estimated` | Derived from partial provider signals plus local inference |
| `Unknown` | Not enough information to produce a trustworthy mirrored state |

Every mirrored quota output should carry:

- confidence tier
- basis summary
- last refresh timestamp
- whether the value has been reconciled since the last offline period

Current v1.0 acceptance target:

- validated provider-side counters or imported historical counters must match the authoritative source within normal display-rounding tolerance after refresh or import
- for JiStats v1.0, normal display-rounding tolerance means no more than `1` displayed count unit or `1` percentage point of divergence from the authoritative source
- any mirror state that depends on unvalidated provider semantics, incomplete provider history, or unknown/offline gaps must stay visibly labeled `Estimated` or `Unknown` instead of being presented as exact
- every user-visible quota state must surface freshness plus gap/backfill context whenever those conditions apply

## 11. 🗄️ Data Model Direction

The current plan should move away from keeping only raw logs plus monthly summaries.

The recommended model is:

- `providers`
  - provider registry entries such as OpenAI, MiniMax, and future providers
- `accounts`
  - user-defined provider accounts, orgs, or workspaces
- `credential_profiles`
  - local credential references and metadata
- `plans`
  - subscription tier, billing context, or imported plan snapshot where known
- `usage_events`
  - one row per locally observed event or imported usage unit
  - stores timestamp, provider, account, plan, source, model, request type, token counts if known, and attribution metadata
- `quota_snapshots`
  - mirrored quota state over time, including source confidence and reconciliation markers
- `mirror_rules`
  - provider-specific mirror configuration, discovered semantics, and versioned logic hints
- `usage_rollups_hourly`
  - cached hourly aggregates for fast charts and summaries
- `usage_rollups_daily`
  - longer-term daily summaries for reporting and retention
- `sync_state`
  - cursors, last successful imports, and reconciliation metadata
- `runtime_state`
  - last app launch, last successful sync, last local observation, and recovery markers
- `connectivity_state`
  - last known connectivity status, failure category, retry timing, and dismissed-notice state
- `app_settings`
  - polling interval, retention, reporting preferences, startup preferences, and UI settings
- `provider_capabilities`
  - discovered support for usage endpoints, backfill behavior, and whether model-backed probing is ever required

### 11.1 Minimum Schema Expectations

For team clarity, the following fields should exist or be explicitly justified if omitted.

`providers`

- `id`
- `provider_key`
- `display_name`
- `enabled`
- `created_at_utc`
- `updated_at_utc`

`accounts`

- `id`
- `provider_id`
- `account_key`
- `display_name`
- `scope_type`
- `scope_identifier`
- `enabled`
- `created_at_utc`
- `updated_at_utc`

`credential_profiles`

- `id`
- `provider_id`
- `profile_name`
- `secret_ref`
- `created_at_utc`
- `updated_at_utc`
- `last_validated_at_utc`

`usage_events`

- `id`
- `provider_id`
- `account_id`
- `plan_id`
- `event_time_utc`
- `source_type`
- `source_run_id`
- `model_key`
- `request_type`
- `input_tokens`
- `output_tokens`
- `total_tokens`
- `cost_minor_units`
- `is_backfilled`
- `raw_reference`
- `created_at_utc`

`quota_snapshots`

- `id`
- `provider_id`
- `account_id`
- `snapshot_time_utc`
- `quota_dimension`
- `window_start_utc`
- `window_end_utc`
- `limit_value`
- `used_value`
- `remaining_value`
- `confidence_tier`
- `basis_summary`
- `is_reconciled`
- `created_at_utc`

`sync_state`

- `id`
- `provider_id`
- `account_id`
- `sync_kind`
- `cursor_value`
- `last_attempt_at_utc`
- `last_success_at_utc`
- `last_failure_at_utc`
- `last_failure_category`

`connectivity_state`

- `id`
- `provider_id`
- `account_id`
- `connectivity_status`
- `failure_category`
- `last_checked_at_utc`
- `next_retry_at_utc`
- `notice_dismissed_at_utc`

### 11.2 Indexing And Query Expectations

The schema should support these common query patterns efficiently:

- recent usage by provider and account
- rolling-window recomputation by time range
- latest quota snapshot by provider and account
- catch-up imports since last cursor
- report generation by day, week, model, and provider

At minimum, indexing strategy should be designed for:

- `usage_events(provider_id, account_id, event_time_utc)`
- `quota_snapshots(provider_id, account_id, snapshot_time_utc)`
- `sync_state(provider_id, account_id, sync_kind)`
- `connectivity_state(provider_id, account_id)`

### 11.3 Data Semantics

The team should treat these concepts distinctly:

- observed local usage
- imported provider history
- mirrored quota state
- derived analytics rollups
- stale cached state
- unknown gaps

No table or API contract should blur these concepts casually.

Suggested retention policy:

- current v1.0 landed baseline:
- keep detailed events for 90 days
- keep hourly rollups for 30 days
- keep daily rollups for 365 days
- keep gap markers for 90 days
- treat longer retention windows or user-configurable retention as follow-up work unless the current defaults create a concrete reporting or reconciliation regression

Gap state guidance:

- represent imported backfill separately from live local observations
- preserve enough metadata to explain whether a period is known-empty, reconstructed, or unknown

## 12. 🖥️ UX Direction

### 12.1 Main Dashboard

The dashboard should focus on clarity over density.

The dashboard is the primary live-data surface for JiStats. Users should not need to export HTML just to understand current status, recent usage, mirror pressure, or freshness.

The current compact quota/status shell is a valid baseline, but richer in-app analysis and historical inspection should grow inside the app instead of being pushed into HTML export. See `docs/in-app-reporting-and-analysis-spike.md` for the companion expansion plan.

Recommended tiles:

- current provider and account
- current status
- mirrored quota state
- last 5 minutes
- last 30 minutes
- last 5 hours
- today
- current model mix
- plan or quota status

Every metric should include a small badge:

- `Local`
- `Imported`
- `Estimated`

The dashboard should also show:

- a persistent offline indicator when provider connectivity is unavailable
- last successful sync time

The dashboard should support a mix of:

- summary tables
- compact charts or sparklines
- quota and freshness indicators
- trend visuals that make backfill, stale data, and recovery state obvious

### 12.2 Settings

The settings surface should include:

- provider profiles
- provider enable or disable controls
- credentials
- account or workspace selection
- provider-specific setup state and missing-prerequisite guidance
- import or polling behavior
- low-cost monitoring or probe preferences
- run-on-startup preference
- retention settings
- export schedule preferences
- diagnostic logging toggle
- connectivity retry and notification preferences kept minimal and sensible

For bounded local-first providers such as Claude Code and Kimi Code, settings must also make it possible to:

- see whether the provider is currently active, unavailable, or not yet configured
- enable or disable local ingestion explicitly instead of relying on hidden rollout defaults
- review the approved local source boundary at a high level
- surface the relevant local runtime path or discovery status without exposing secrets
- understand when quota truth is local-first only and therefore `Estimated` or `Unknown`

### 12.3 Tray Experience

Recommended tray actions:

- open dashboard
- switch provider or account
- pause or resume polling
- export report
- exit

### 12.4 Taskbar Experience

The taskbar surface should be compact, glanceable, and dedicated to live metrics.

Recommended v1.0 taskbar content:

- current provider or profile label
- short-window usage intensity
- mirrored quota or plan-pressure indicator
- offline status indicator when sync is unavailable

The taskbar surface should not try to reproduce the full dashboard.

### 12.5 Notification Rules

Notification behavior should be explicit and deterministic:

| Rule | Expected Behavior |
| --- | --- |
| Offline transition | Show one dismissible notice when transitioning from online to offline |
| Same-state suppression | Do not repeat the same notice while the app remains in the same offline state |
| Persistent status | Keep the offline indicator visible even after dismissal |
| Recovery | Clear the offline notice state automatically when connectivity is restored |
| Error separation | Distinguish offline notices from authentication failure notices |
| UX safety | Never block core app usage behind a modal connectivity error |

The first release should prefer low-noise status signaling over aggressive alerts.

### 12.6 Data Freshness And Staleness UX

Each surface should communicate freshness:

| State | Meaning |
| --- | --- |
| `Current` | Fresh provider or local data is available |
| `Stale` | Last known data is still usable, but not fresh |
| `CatchingUp` | The app is actively reconciling missed history |
| `Offline` | Provider connectivity is unavailable |
| `Unknown` | The app cannot produce a trustworthy current state |

This should appear in:

- dashboard status line
- tray icon state
- taskbar indicator state
- report metadata

## 13. 🧾 Reporting

HTML reporting is still a good fit, but it should be scoped tightly.

Reports are export artifacts for sharing, archival, or offline review. They are not the primary interface for viewing current data, recent activity, or live quota state.

Any richer in-app historical analysis or detail-view expansion should therefore be treated as companion dashboard/reporting UX work, not as a reason to overload HTML export. See `docs/in-app-reporting-and-analysis-spike.md`.

V1.0 reporting should support:

- daily summary
- weekly summary
- provider and account breakdown
- per-model breakdown
- recent trend charts
- quota mirror trend or state snapshots
- data source labels so estimated values are obvious
- explicit indication of backfilled or unknown periods when relevant

Avoid building a large report designer in v1.

Offline periods do not need intrusive reporting UX, but exported reports should be able to distinguish stale, backfilled, and current data when relevant.

The core expectation is:

- live and recent usage is visible inside the application
- reports summarize and export already-collected data
- reports should never be required for normal day-to-day monitoring

### 13.1 Report Metadata Requirements

Every exported report should include:

- report generation timestamp
- provider and account scope
- data freshness timestamp
- whether the report contains backfilled data
- whether the report contains unknown gaps
- the mirror confidence policy version used to generate quota sections

## 14. 🛠️ Feasibility-First Roadmap

### Phase 0: Validation Spike

Status in the current repo/docs baseline: closed for the subscription-first v1.0 path. First live OpenAI API-platform authz evidence is now archived, and the only remaining unchecked Phase 0 item is the optional freshness/lag pass once a credential with `api.usage.read` and the correct org-scoped role exists.

Goals:

- define provider-agnostic domain terminology
- verify OpenAI authentication flow
- verify available OpenAI usage and cost endpoints
- verify MiniMax pay-as-you-go and subscription authentication flows
- verify available MiniMax usage, pricing, plan, and quota surfaces
- inspect response freshness and granularity
- verify rate-limit related signals if present
- define what can be known exactly versus estimated
- define the OpenAI quota-mirroring target behavior
- define the MiniMax API and subscription mirroring target behavior
- define how mirror accuracy will be measured
- define the taskbar metric set that v1.0 will surface
- define the launch-time reconciliation strategy for downtime gaps
- identify which monitoring actions can be completed without model inference
- identify the cheapest acceptable fallback path if inference is unavoidable
- define connectivity failure categories and offline recovery behavior

Exit criteria:

- one written capability matrix covering OpenAI and MiniMax, including MiniMax plan-specific surfaces
- one sample-response archive
- one recommended MVP metric list
- one written gap-handling policy for known-zero, reconstructed, and unknown periods
- one written low-cost monitoring policy
- one written mirror accuracy and confidence policy
- one written offline and reconnection policy

Closeout note:

- Phase 0 should stay closed once the subscription-first OpenAI baseline, Codex-local augmentation path, export-import boundary, and MiniMax subscription evidence are documented and accepted.
- The archived `2026-04-19` OpenAI API-platform authz probe is an advanced-path follow-up, not a reason to reopen Phase 0 for the core v1.0 baseline.

### Phase 1: Local Foundation

Status in the current repo/docs baseline: completed for the current v1.0 baseline. The foundational app shell, schema, persistence model, credential storage, startup/runtime-state persistence, and diagnostics scaffolding are in place; later-phase extensions should not be treated as Phase 1 reopen items.

Goals:

- create project skeleton
- define SQLite schema
- define provider, account, and plan entities
- define mirror-state entities and persistence rules
- wire credential storage
- implement logging and diagnostics scaffolding
- define startup registration approach and persisted runtime state
- define provider capability flags for low-cost monitoring behavior
- define connectivity-state persistence and retry metadata

Exit criteria:

- app launches
- database initializes
- credentials can be stored and retrieved
- startup preferences can be stored safely
- monitoring mode preferences can be stored safely
- mirror-state persistence is defined
- offline-state persistence is defined

Closeout note:

- Phase 1 should stay closed while Phase 2 and later work build on the landed app shell, schema, persistence, and settings baseline instead of re-litigating foundation scope.
- If future work expands schema or preferences, treat that as normal later-phase product evolution unless it exposes a real regression in the existing Phase 1 baseline.

### Phase 2: Local Telemetry And Analytics

Status in the current repo/docs baseline: completed for the current v1.0 baseline. Local quantified usage, rollups, mirror calculations, gap/recovery persistence, offline-state handling, and runtime retention are all landed for the shared analytics pipeline that now feeds dashboard, tray, taskbar, and reporting behavior.

Goals:

- record local usage events
- implement rolling-window calculations
- implement the initial quota mirror engine
- build daily and hourly rollups
- establish retention jobs
- expose a shared metric service that can feed dashboard, tray, and taskbar surfaces
- implement gap markers and reconciliation-ready aggregation behavior
- implement offline detection, retry backoff, and catch-up triggers

Exit criteria:

- recent windows render from live local data
- rollups match event totals in test fixtures
- missing periods are classified correctly in test fixtures
- mirror calculations produce repeatable outputs from test fixtures
- offline and reconnect transitions behave correctly in test fixtures

Closeout note:

- Phase 2 should stay closed for the current baseline while later work builds richer UI/reporting/provider support on top of the landed local telemetry model instead of reopening the foundation slice.
- The current landed retention defaults are 90 days for raw usage events, 30 days for hourly rollups, 365 days for daily rollups, and 90 days for gap markers, with a once-per-24h runtime retention pass.
- Longer retention windows, user-configurable retention, or different archival/export behavior should be treated as follow-up product decisions unless the current defaults expose a concrete reporting, recovery, or reconciliation regression.

### Phase 3: Desktop UI And Tray

Status in the current repo/docs baseline: completed for the current v1.0 baseline. The dashboard, settings shell, tray quick actions, provider/account selection, recovery/runtime presentation, and dismissible offline notice behavior are all landed for the shipped desktop shell.

Goals:

- implement dashboard
- implement settings dialog
- implement tray icon and quick actions
- support provider and account selection in the UI
- surface sync status and gap state clearly in the UI
- surface mirror state, confidence, and recovery behavior clearly in the UI
- surface a dismissible offline notice without repeated alert spam

Exit criteria:

- user can review key metrics without opening developer tools or logs
- offline state is clearly visible without blocking app use

Closeout note:

- Phase 3 should stay closed for the current baseline while later work expands in-app analysis and reporting on top of the landed shell instead of reopening core desktop affordances.
- The current baseline includes a real dashboard offline notice surface with `Retry Now` and `Dismiss`, plus matching tray quick actions while the app is offline.
- Dismissing the offline notice keeps the persistent offline indicator visible, persists `notice_dismissed_at_utc` through the existing connectivity records, and clears that dismissed state automatically once connectivity recovers.

### Phase 4: Taskbar Prototype And Integration

Status in the current repo/docs baseline: completed for the current v1.0 baseline. The taskbar surface is live, survives common shell lifecycle events, uses the shared analytics/runtime pipeline, and now has current manual validation evidence for the intended transparent text-first presentation on Windows 11 with TrafficMonitor-aware placement.

Goals:

- prototype Windows 11 taskbar rendering
- bind taskbar metrics to the shared analytics pipeline
- define reliability behavior for shell restarts and taskbar refreshes
- perform a focused implementation review of TrafficMonitor as a taskbar reference before locking the final integration approach

Exit criteria:

- taskbar metrics render consistently during normal desktop use
- taskbar surface survives common shell lifecycle events
- a documented rationale exists for the chosen taskbar integration approach

Closeout note:

- Phase 4 should stay closed for the current baseline while support docs continue to reflect the intended taskbar behavior accurately.
- The current baseline intentionally allows an OpenAI dual-remaining row when more than one relevant subscription window is surfaced, and it intentionally uses a glyph-free text-first presentation instead of a permanent standalone icon column.
- Further live checks for warning/critical cases, toggle reruns, or broader desktop-layout coverage remain useful follow-up validation work, but they should not reopen Phase 4 unless they expose a concrete taskbar regression.

### Phase 5: Import And Reconciliation

Status in the current repo/docs baseline: completed for the current v1.0 baseline. Historical export import, provider-side recovery backfill, mismatch summaries, duplicate-safe reconciliation persistence, low-cost policy alignment, and mirror-accuracy acceptance are all in place for the subscription-first OpenAI plus MiniMax subscription path.

Goals:

- ingest official historical usage or cost data where supported
- label imported versus local values
- reconcile mismatches where possible
- backfill periods after app downtime where supported by the provider
- ensure reconciliation prefers non-inference APIs over model-backed checks
- improve mirror fidelity using imported provider data and reconciliation
- catch up cleanly after connectivity resumes without duplicate or inconsistent imports

Exit criteria:

- historical reports are available
- imported data can be distinguished from local estimates
- launch after long downtime produces a trustworthy reconciled state
- monitoring overhead remains within a defined low-cost policy
- mirror accuracy is measured and within an accepted v1.0 target
- offline recovery completes without user intervention in normal cases

Closeout note:

- Phase 5 should stay closed for the current baseline while later work focuses on targeted follow-up validation rather than reopening the shipped import/reconciliation model.
- Useful follow-up testing still exists: broader live confidence-refresh captures, more desktop/runtime validation across real machines and provider states, and additional evidence for optional advanced paths such as OpenAI API-platform import or wider MiniMax PAYG history.
- Those follow-ups should be tracked as narrow validation or v1.x refinement work unless they expose a concrete regression in the landed historical import, recovery backfill, low-cost, or mirror-accuracy baseline.

### Phase 6: Reporting

Goals:

- implement standalone HTML export
- add optional scheduled export

Exit criteria:

- daily and weekly reports can be generated reliably

Planning note:

- manual and scheduled HTML export remain the core Phase 6 reporting deliverables
- the current companion in-app analysis baseline now includes a dedicated Analysis workspace window launched from the dashboard, with persisted provider/account/date-range scope, daily history rows, provider/account breakdowns, compact trend presentation, provenance/data-quality messaging, and gap visibility on the shared `AnalysisPresenter` + `AnalysisQueryEngine` path
- the remaining companion work is now release signoff and polish rather than missing reporting persistence seams: the retained rollup provenance, structured recovery metadata, quota-snapshot history queries, and export-run metadata are already landed, and the remaining follow-up is broader manual GUI/release validation so the in-app view and HTML export stay equally trustworthy
- use `docs/in-app-reporting-and-analysis-spike.md` to plan that companion UI work without reopening the narrow reporting/export baseline unnecessarily

### Phase 7: Claude Code And Kimi Code V1 Implementation

Goals:

- turn the completed Claude/Kimi research track into shipped v1.0 support
- implement conservative Claude and Kimi local parsers that stay inside the approved spike and redaction-contract boundaries
- map approved local evidence into provider/account/plan/runtime snapshots without ingesting raw transcript, tool, or credential content
- expose Claude and Kimi through a real user-facing activation and configuration path in settings instead of hidden or code-only rollout behavior
- make OpenAI, MiniMax, Claude Code, and Kimi Code all explicitly optional integrations that default to disabled until the user enables them
- extend dashboard, reporting, tray, and taskbar surfaces so Claude/Kimi local evidence is visible with explicit confidence labeling
- add parser fixtures, unit coverage, integration coverage, and release-validation notes for the new providers
- keep Kilo Code and broader provider expansion outside the current v1.0 release

Implementation slices (historical closeout queue for the now-shipped Phase 7 rollout):

- `P7.1` / `P7.1a`: feasibility research baseline complete
- `P7.2`: Claude local-surface boundary complete
- `P7.3`: Kimi local parser boundary complete
- `P7.4`: confidence and low-cost policy alignment complete
- `P7.5`: shared Claude/Kimi redaction contract complete
- `P7.6`: Claude parser, normalized fixtures, and provider registration complete
- `P7.7`: Claude analytics, dashboard/reporting/taskbar integration, and provider-mode labeling complete
- `P7.8`: Kimi parser, normalized fixtures, and provider registration complete
- `P7.9`: Kimi analytics, dashboard/reporting/taskbar integration, and confidence labeling complete
- `P7.10a`: four-provider settings model, persisted enablement state, and provider-discovery plumbing complete
- `P7.10b`: four-provider settings UI, status messaging, and missing-prerequisite guidance complete
- `P7.10c`: four-provider activation-flow validation, startup wiring checks, and focused regression coverage complete
- `P7.11a`: cross-provider glossary, policy, and report-wording closeout for the four-provider v1.0 story complete
- `P7.11b`: taskbar/manual-validation/release-note pass for OpenAI + MiniMax + Claude + Kimi complete
- `P7.11c`: final acceptance closeout, release checklist updates, and handoff-ready Phase 7 completion notes complete

Exit criteria:

- approved Claude and Kimi parser inputs are implemented without widening into raw transcript, tool, or credential ingestion
- redacted fixtures or equivalent parser-validation artifacts exist for the shipped Claude and Kimi inputs
- the shared analytics pipeline can persist and summarize Claude/Kimi local activity/runtime evidence
- supported users can discover, enable, disable, and inspect Claude/Kimi local-provider configuration from the shipped settings surface without editing hidden config or relying on a compile-time rollout default
- OpenAI, MiniMax, Claude Code, and Kimi Code all remain disabled by default on a fresh install until the user opts in
- dashboard, reporting, tray, and taskbar surfaces can show Claude/Kimi local evidence with correct `LocalExact`, `Estimated`, or `Unknown` labeling
- Claude provider-mode classification remains `Unknown` by default unless a separately validated signal can narrow it safely
- Kimi provider-exact quota truth is available through the validated `/usages` API path when usable OAuth credentials or refresh tokens exist; without that path, Kimi quota should remain empty rather than falling back to fabricated local estimates
- release docs explicitly distinguish local-first evidence from provider-exact quota truth

Planning note:

- OpenAI/Codex and MiniMax remain the proof-of-concept depth anchors for quota mirroring and provider-side validation.
- Claude Code and Kimi Code are now in v1.0 scope, but their shipped scope remains intentionally bounded: Claude is still local-first, while Kimi combines bounded local activity/history support with a validated provider-exact quota path when usable OAuth exists.
- Because Phase 7 providers are shipped v1.0 surfaces, settings and provider onboarding must expose their activation state and setup requirements directly; hidden rollout gates are not an acceptable release posture.
- The shipped activation model must never assume any provider is installed or active. OpenAI, MiniMax, Claude Code, and Kimi Code all start disabled until the user explicitly enables them.
- Any widening of the Claude/Kimi parser boundary must update the provider spike doc and `docs/claude-kimi-local-redaction-contract.md` first.
- Kilo Code remains post-v1.x work.

### 14.1 Parallel Engineering Workstreams

To support multiple engineers, work should be parallelized into disjoint workstreams where possible:

| Workstream | Scope |
| --- | --- |
| `A` Platform and app shell | App startup, settings persistence, lifecycle, tray bootstrap |
| `B` Storage and schema | SQLite schema, migrations, retention, query layer |
| `C` Provider integration | OpenAI, MiniMax, Claude Code, and Kimi Code capability mapping, sync flows, import contracts, and bounded local-parser adapters |
| `D` Mirror engine and analytics | Rolling windows, quota snapshots, confidence classification |
| `E` Dashboard, tray, and UX | Settings UI, dashboard cards, status indicators, notices |
| `F` Taskbar integration | Native taskbar surface, shell behavior, explorer lifecycle handling |
| `G` Reporting and exports | HTML output, metadata, backfill labeling |
| `H` Quality and infrastructure | CI, test fixtures, redaction-safe provider fixture archives, static analysis, packaging scripts, optional release-size tooling |

Each workstream should own:

- entry points
- public interfaces
- test responsibilities
- acceptance criteria

Optional Windows release-size reduction may use UPX only as an external packaging tool, not as an embedded project dependency. If enabled, it should stay opt-in, Release-only, based on an official unmodified UPX build, and documented with the matching upstream license handoff for any shipped UPX-packed binaries.

### 14.2 Team Deliverables By Phase

Each phase should produce durable artifacts, not just code.

Required artifact examples:

- capability matrix
- provider fixture archive
- schema document
- mirror accuracy policy
- offline recovery policy
- test plan
- release checklist
- taskbar integration rationale
- third-party packaging/license note for optional external release tools such as UPX

### 14.3 Test Strategy

The test strategy should include all of the following:

- unit tests
  - mirror calculations, classification helpers, schema utilities, retry logic
- fixture replay tests
  - known provider response sets replayed into the mirror engine
- integration tests
  - provider import plus DB plus analytics pipeline
- offline recovery tests
  - simulated DNS failure, timeout, resume, and catch-up
- long-gap tests
  - startup after days or weeks without sync
- UI state tests where practical
  - offline indicator, stale data states, notice cooldown behavior
- manual system tests
  - tray behavior, taskbar behavior, sleep/resume, explorer restart

The project should maintain deterministic test fixtures for:

- normal sync
- delayed provider responses
- partial imports
- duplicate data
- missing periods
- quota resets or bucket decay cases

### 14.4 Quality Gates And Definition Of Done

A feature should not be considered done unless:

- code is merged with tests appropriate to the change
- logging and error paths are covered
- no UI-thread blocking work is introduced
- configuration and persisted-state impacts are documented
- user-visible status behavior is defined
- relevant docs are updated

For milestone-level completion, the release candidate should also meet:

- no known crash path during offline or reconnect flows
- no duplicate import or reconciliation corruption in standard fixtures
- mirror outputs include confidence tier and freshness metadata
- taskbar, tray, and dashboard states agree on provider status
- local startup after prior sync state is reliable
- release packaging behavior is reproducible, license-reviewed, and records whether optional UPX compression is enabled

### 14.5 Operational Observability

The app should produce enough local diagnostics to debug user issues without sending telemetry externally by default.

Recommended local diagnostics include:

- startup timeline events
- sync attempt summaries
- retry and backoff state
- offline transition reasons
- reconciliation summaries
- mirror recalculation summaries
- taskbar surface refresh failures

Logs must avoid storing plaintext credentials or sensitive request bodies.

### 14.6 Execution Tracking And Handoff

For a multi-engineer team, the project should maintain a separate live execution tracker at:

- `docs/progress.md`

That document should be updated whenever work meaningfully changes state.

It should answer, at minimum:

- what is being worked on right now
- what is done
- what is blocked
- what is next
- what decisions were made recently
- what another engineer needs to know to pick up the work safely

The long-form plan should not be used as the only source of day-to-day execution state.

### 14.7 Task Sizing Rules

To avoid overload for engineers and AI agents, tasks should be intentionally small and handoff-friendly.

Recommended task sizing rules:

- one primary objective per task
- one owning workstream per task
- clear input and output expectations
- clear definition of done
- limited file and subsystem spread where practical
- sized so a new engineer can understand the task without rereading the entire project plan

Tasks should usually avoid:

- combining schema work, provider logic, UI changes, and taskbar changes in one unit of work
- mixing exploratory research with broad implementation unless the research output is itself the task
- bundling several unrelated “while we are here” improvements together

Good task examples:

- define the `usage_events` and `quota_snapshots` schema plus migration
- implement offline notice suppression rules in the notification service
- document and fixture-test one OpenAI or MiniMax mirror scenario

Bad task examples:

- build the entire sync system
- implement all UI
- make the OpenAI and MiniMax integrations complete in one task

### 14.8 Definition Of Ready For A Task

A task should be ready to assign when it has:

- a short title
- owner or owning workstream
- purpose
- dependencies
- concrete deliverables
- validation steps
- handoff notes if it spans multiple sessions

### 14.9 Definition Of Done For A Task

A task should not be marked done unless:

- code or document changes are complete
- validation steps were run or explicitly deferred
- blockers and follow-up work are noted
- any relevant decisions are recorded
- `docs/progress.md` is updated

## 15. 🗺️ Versioned Milestones

### Milestone A: Planning Complete

Status in the current repo/docs baseline: achieved.

- capability matrix drafted
- entity model drafted
- v1.0 metric glossary drafted
- language and runtime strategy locked for v1.0
- workstream ownership model drafted
- test strategy drafted

### Milestone B: Internal Alpha

- OpenAI and MiniMax data pipelines work
- initial Claude and Kimi local-ingestion pipelines work inside the approved parser boundaries
- initial mirror engine works
- dashboard and tray are functional
- taskbar prototype is visible
- startup and launch refresh flow is defined end-to-end
- offline and reconnect handling works end-to-end

### Milestone C: V1.0 Release Candidate

- OpenAI, MiniMax, Claude Code, and Kimi Code workflows are stable within their supported confidence boundaries
- quota mirroring is stable
- reporting works
- taskbar integration is reliable
- docs clearly label estimated versus official metrics
- downtime recovery and backfill behavior is trustworthy
- monitoring overhead is demonstrably low and policy-compliant
- offline behavior is stable, non-intrusive, and self-recovering
- release checklist and rollback plan are complete

### Milestone D: Post-Release Expansion

- additional provider candidate selected beyond OpenAI, MiniMax, Claude Code, and Kimi Code
- provider adapter contract validated against real differences after the four-provider v1.0 baseline
- account and plan abstractions adjusted if needed

## 16. 🔒 Decisions To Lock Early

- what "usage event" means in the local schema
- whether local tracking is driven by app-side requests, imported data, or both
- minimum supported Windows version
- retention defaults
- whether scheduled reporting is internal-only or can integrate with Windows Task Scheduler
- what minimum taskbar metric set is required for v1.0
- how provider, account, and plan identifiers are normalized internally
- when a missing period is shown as zero, unknown, or backfilled
- whether run-on-startup is enabled by default, recommended, or opt-in
- what "lowest-cost acceptable" means for any provider-backed probe path
- how the app surfaces and limits any self-generated monitoring usage
- what minimum acceptable mirror accuracy is for v1.0
- how mirror confidence is shown to the user
- whether any non-`C++/C` language is allowed in v1.0 at all
- what connectivity failures are grouped under the offline state
- how often offline notices may be shown before becoming silent

## 17. 🗂️ Proposed Repository Structure

```text
/JiStats
├── /src
│   ├── /app
│   ├── /core
│   ├── /providers
│   ├── /db
│   ├── /analytics
│   ├── /gui
│   ├── /taskbar
│   ├── /reporting
│   └── /platform
├── /thirdparty
├── /docs
│   └── plan.md
└── AGENTS.md
```

## 18. 📚 Reference Notes

- TrafficMonitor is a known reference point for Windows 11 taskbar metrics and should be considered during future implementation planning.
- TrafficMonitor is not just a passive note; it is an explicit future research point for the required taskbar feature.
- No GitHub review or code-level analysis of TrafficMonitor is required during the current planning stage.
- Any later technical review should focus on feasibility, shell integration approach, lifecycle handling, and maintenance risk.

## 19. 🧪 Language Notes

- The default v1.0 stack is `C++/C`.
- The project is intentionally avoiding paid proprietary tooling and managed-runtime-first stacks.
- `Rust` and `Zig` remain possible future evaluation paths, but neither is part of the base v1.0 plan.

## 20. 👥 Engineer Onboarding Notes

New engineers joining the project should be able to answer these questions on day one:

- what subsystem they own
- which data contracts they are allowed to change
- which tests they must run locally
- which UI surfaces consume their outputs
- how their changes affect mirror accuracy, offline handling, and taskbar behavior

The repo should eventually include:

- architecture diagrams
- schema reference
- provider fixture samples
- coding conventions
- release checklist

## 21. ✅ Immediate Next Planning Tasks

This section is now historical context for how Milestone A was closed.

The planning-baseline artifacts below already exist in the repo and should not be treated as missing next steps:

1. Capability matrix: `docs/capability-matrix.md`
2. OpenAI and MiniMax mirror target behavior: `docs/openai-mirror-spec.md`, `docs/minimax-mirror-spec.md`
3. Provider/account/credential/plan/mirror entities: `docs/entity-model.md`
4. Initial SQLite schema and migration order: `docs/sqlite-schema.md`
5. Metric glossary: `docs/metric-glossary.md`
6. Minimum taskbar metric set and overlay contracts: `docs/taskbar-metrics.md`, `docs/taskbar-overlay-display-contract.md`
7. Local observation versus provider-import decisions: `docs/openai-activity-storage-decision.md`, `docs/local-observation-binding-model.md`
8. Gap handling and launch reconciliation policy: `docs/gap-handling-policy.md`
9. Run-on-startup behavior and settings UX: `docs/startup-behavior.md`, `docs/minimax-subscription-credential-ux.md`
10. Low-cost monitoring policy: `docs/low-cost-monitoring-policy.md`
11. Offline, retry, notification, and reconnection policy: `docs/offline-retry-policy.md`
12. Workstream ownership boundaries and interface contracts: `docs/workstream-ownership.md`
13. Test strategy and fixture plan: `docs/test-strategy.md`
14. Release checklist and definition-of-done guidance: `docs/release-checklist.md` plus section `14.9` above

Execution sequencing after this baseline should now come from `docs/progress.md`, not by redoing these planning tasks.
