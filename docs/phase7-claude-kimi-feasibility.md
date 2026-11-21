# Phase 7 Claude Code And Kimi Code Feasibility Research

> **Status**: Research baseline complete; now an input to active v1.0 Phase 7 implementation
> **Date**: 2026-04-19
> **Purpose**: Record source-backed findings for Phase 7 Claude Code and Kimi Code planning and implementation, including official documentation/API signals plus safe local-machine reconnaissance
> **Scope**: Research baseline and implementation input; this document does not replace the narrower parser-boundary and redaction-contract docs that define what Phase 7 may actually ingest

## Executive Summary

This research pass found that both Claude Code and Kimi Code are credible JiStats v1.0 providers, but they differ in where trustworthy signals are easiest to obtain and therefore need bounded implementation strategies.

### Claude Code

Claude Code appears feasible, but the strongest future JiStats path is likely a hybrid of:

- local-first observation from the existing `~/.claude` runtime footprint
- optional telemetry export when enabled by the user or organization
- provider-side Console reporting when the user is on API/Console billing rather than only a Claude subscription

The main limitation is that Anthropic’s public docs clearly distinguish:

- API/Console cost reporting
- subscriber usage patterns

That means Claude Code support is likely possible, but the exact subscription-backed quota mirror target still needs a narrower validation pass.

### Kimi Code

Kimi Code looks especially promising for the bounded local-first JiStats integration now planned for Phase 7.

The official docs clearly document:

- subscription-backed coding plans
- API-key and direct-login authentication
- use in official Kimi surfaces and in approved third-party coding agents
- quota refresh behavior

On this machine, Kimi also leaves a substantial local footprint under `~/.kimi`, including:

- sessions
- logs
- plans
- config
- device identity
- credential storage

That local footprint is much closer to a Codex-local-style observation path than a thin IDE wrapper. The main risk is not lack of data, but privacy and sensitivity: some files contain prompts, tool activity, and credentials, so any future parser must be highly selective.

## Research Inputs

### Official Web Sources Verified On 2026-04-19

- Anthropic Claude Code overview: `https://code.claude.com/docs/en/overview`
- Anthropic Claude Code settings: `https://code.claude.com/docs/en/settings`
- Anthropic Claude Code monitoring: `https://code.claude.com/docs/en/monitoring-usage`
- Anthropic Claude Code costs: `https://code.claude.com/docs/en/costs`
- Anthropic pricing: `https://claude.com/pricing`
- Kimi Code membership benefits: `https://www.kimi.com/code/docs/en/`
- Kimi Code benefit description: `https://www.kimi.com/code/docs/en/benefits.html`
- Kimi Code for VS Code guide: `https://www.kimi.com/code/docs/en/kimi-code-for-vscode/guides/getting-started.html`
- Kimi Code sessions/context: `https://www.kimi.com/code/docs/en/kimi-cli/guides/sessions.html`
- Kimi Code third-party agents guide: `https://www.kimi.com/code/docs/en/third-party-agents.html`

### Local Machine Reconnaissance

This pass also inspected local installation footprints on this machine for:

- Claude Code for VS Code
- Kimi Code for VS Code
- `~/.claude`
- `~/.kimi`

Sensitive secrets and conversation contents were intentionally not copied into this repo.

## Claude Code Findings

## Official Product / Subscription / Auth Signals

Anthropic’s current Claude Code overview says Claude Code is available in the terminal, IDE, desktop app, and browser, and that most surfaces require either a Claude subscription or an Anthropic Console account. It also explicitly says the Terminal CLI and VS Code support third-party providers. Source: `https://code.claude.com/docs/en/overview`

That matters for JiStats because Claude Code support must be careful to distinguish at least three user states:

- Claude subscription-backed use
- Anthropic Console / API-backed use
- third-party-provider-backed use through the Claude Code shell

Anthropic’s pricing page currently says:

- Pro includes access to Claude Code directly in the terminal
- Team offers a premium seat tier that includes Claude Code

Source: `https://claude.com/pricing`

This confirms Claude Code is not only an API feature; it also exists as a subscription product surface relevant to JiStats' v1 provider-expansion goals.

## Official Usage / Cost / Monitoring Signals

Anthropic’s cost documentation makes an important distinction:

- `/cost` is intended for API users
- Pro and Max subscribers should use `/stats` to view usage patterns
- `/cost` values are local estimates and authoritative billing lives in the Claude Console for API users

Source: `https://code.claude.com/docs/en/costs`

Anthropic also documents an OpenTelemetry path for Claude Code that can export:

- usage metrics
- cost metrics
- session counters
- code-change counters
- events

Examples explicitly include:

- `claude_code.token.usage`
- `claude_code.cost.usage`
- `claude_code.session.count`

Anthropic also warns that cost metrics are approximations and points users to the Claude Console, Bedrock, or Vertex for authoritative billing. Source: `https://code.claude.com/docs/en/monitoring-usage`

For JiStats, the implication is:

- API or enterprise Claude Code usage has an officially documented telemetry path
- subscriber usage appears visible locally through Claude Code itself, but not as a simple public billing endpoint analogous to OpenAI organization usage APIs

## Official Local Configuration / Storage Signals

Anthropic’s settings documentation explicitly documents:

- user settings in `~/.claude/settings.json`
- project settings in `.claude/settings.json`
- other state in `~/.claude.json`

It also says `~/.claude.json` contains preferences, OAuth session, MCP configurations, per-project state, and caches. Source: `https://code.claude.com/docs/en/settings`

That matters because it confirms local Claude Code persistence is part of the supported product model rather than an accidental byproduct.

## Local Machine Findings

On this machine, Claude Code also has a visible local footprint:

- VS Code extension installed at `%USERPROFILE%\.vscode\extensions\anthropic.claude-code-<version>-win32-x64`
- user directory at `%USERPROFILE%\.claude`

Observed top-level `~/.claude` artifacts:

- `history.jsonl`
- `stats-cache.json`
- `settings.json`
- `sessions/`
- `tasks/`
- `plans/`
- `plugins/`
- `shell-snapshots/`
- `file-history/`

Safe local evidence:

- `~/.claude/settings.json` stores basic user preferences
- the directory layout itself suggests Claude Code persists conversation, task, and session state locally in a structured way

This is a strong indicator that Claude Code can support a local-first JiStats path similar in spirit to the existing OpenAI Codex-local approach.

## Claude Code Assessment

### Strengths

- Official subscription product, not just API billing
- Officially documented local settings and state locations
- Officially documented telemetry export path
- Local machine already has a rich runtime footprint

### Gaps

- Exact subscription quota semantics remain less explicit than Kimi’s published plan/quota docs
- Public docs emphasize usage patterns for subscribers, not a simple authoritative quota mirror endpoint
- We have not yet validated whether local `stats-cache.json`, session metadata, or `/stats` output are stable enough to become JiStats’ primary mirror source

### Recommendation

Claude Code is now in active Phase 7 scope, but only through the conservative local-surface boundary JiStats already documented.

That implementation boundary is captured in:

- `docs/claude-local-surface-spike.md`

The resulting conservative boundary treats `stats-cache.json` and `settings.json` as the clean first-parser inputs, while keeping transcript-like project/history artifacts and raw task files out of the default parser for v1.0.

## Kimi Code Findings

## Official Product / Subscription / Auth Signals

Kimi’s current docs describe Kimi Code as a premium subscription tier for coding. The membership docs currently say:

- it is a premium subscription tier within the Kimi ecosystem
- it supports Kimi Code CLI, Claude Code, and Roo Code
- it offers a 5-hour token quota supporting roughly 300 to 1,200 API calls with max concurrency of 30

Source: `https://www.kimi.com/code/docs/en/`

Kimi’s benefit description goes further and explicitly documents:

- quota is included in the membership plan
- API-key requests deduct from plan quota
- direct account login in CLI or VS Code also deducts from the active subscription plan
- quota refreshes on a rolling 7-day cycle
- unused quota does not carry over

Source: `https://www.kimi.com/code/docs/en/benefits.html`

This is much more explicit than many other subscription-oriented coding tools and is a strong sign that Kimi Code can eventually support a real quota-oriented JiStats story.

## Official API / Third-Party Agent Signals

Kimi’s docs explicitly support generating an API key from the Kimi Code Console for use in:

- third-party agents
- custom scripts

Source: `https://www.kimi.com/code/docs/en/`

Kimi’s third-party agents guide also explicitly documents how to point Claude Code at Kimi by setting:

- `ANTHROPIC_BASE_URL=https://api.kimi.com/coding/`
- `ANTHROPIC_AUTH_TOKEN=<Kimi API key>`

It also documents `https://api.kimi.com/coding/v1` as the OpenAI-compatible endpoint for Roo Code, with model `kimi-for-coding` and context window `262144`.

Source: `https://www.kimi.com/code/docs/en/third-party-agents.html`

For JiStats, this means Kimi already has:

- an official API-facing surface
- an official subscription story
- official compatibility guidance for external coding agents

## Official VS Code / Session Signals

Kimi’s VS Code guide says the extension supports:

- Kimi account subscription or Kimi API key
- browser-based sign-in
- API-key mode
- slash commands
- diff tracking
- logs
- settings

Source: `https://www.kimi.com/code/docs/en/kimi-code-for-vscode/guides/getting-started.html`

Kimi’s sessions guide says the CLI automatically saves conversation history and session state, and that resumed sessions restore:

- approval decisions
- dynamic subagents
- additional directories

Source: `https://www.kimi.com/code/docs/en/kimi-cli/guides/sessions.html`

This is exactly the kind of documentation that makes a local-first JiStats integration more practical.

## Local Machine Findings

Kimi Code is installed on this machine as a VS Code extension:

- `%USERPROFILE%/.vscode/extensions/moonshot-ai.kimi-code-<version>/package.json`

Notable extension findings:

- repository points to `MoonshotAI/kimi-agent-sdk`
- bundled CLI is shipped with the extension
- extension configuration includes settings like `kimi.executablePath`, `kimi.environmentVariables`, `kimi.editorContext`, and `kimi.yoloMode`

The VS Code extension also deploys a bundled CLI/runtime into VS Code global storage:

- `%USERPROFILE%\AppData\Roaming\Code\User\globalStorage\moonshot-ai.kimi-code\bin\kimi`

Much more importantly, this machine has a standalone Kimi runtime directory at:

- `%USERPROFILE%\.kimi`

Observed top-level `~/.kimi` artifacts:

- `config.toml`
- `device_id`
- `kimi.json`
- `credentials/`
- `logs/`
- `plans/`
- `sessions/`

Safe local evidence from `~/.kimi/config.toml`:

- default model is `kimi-code/kimi-for-coding`
- provider type is `kimi`
- base URL is `https://api.kimi.com/coding/v1`
- search and fetch services also exist under `/coding/v1/search` and `/coding/v1/fetch`
- OAuth storage is configured as file-backed under key `oauth/kimi-code`

Safe local evidence from `~/.kimi/kimi.json`:

- it tracks working directories and last-session metadata

Safe local evidence from a sampled session state file such as `~/.kimi/sessions/<project>/<session>/state.json`:

- session state stores approval mode, auto-approved actions, dynamic subagents, additional directories, and plan-mode state

Safe local evidence from sampled runtime files:

- `wire.jsonl` includes status updates with fields such as context usage, token-usage counters, and message identifiers
- logs record session creation, selected provider/model, and config-load details

Observed session directory structure is rich and repeatable, including:

- `context.jsonl`
- `state.json`
- `wire.jsonl`
- `baseline/`
- `notifications/`
- `tasks/`

## Kimi Privacy / Safety Note

Kimi’s local footprint is promising, but it is also highly sensitive.

Some local Kimi files clearly appear capable of containing:

- conversation history
- tool inputs and outputs
- prompt content
- runtime state
- credential material

Future JiStats work should therefore treat the following as sensitive and out of normal ingestion scope:

- raw credential contents
- full conversation transcripts
- full tool payloads
- raw wire logs unless filtered down to explicit usage metadata

The likely safe JiStats path is to parse only narrow metadata such as:

- session IDs
- timestamps
- working-directory scope
- model identifiers
- aggregated token-usage counters when present
- context window usage counters
- plan/thinking mode flags where relevant

## Kimi Code Assessment

### Strengths

- Official subscription product with documented quota refresh behavior
- Official API-key and direct-login flows
- Official support for approved third-party agents
- Local runtime footprint is substantial and structured
- Strong candidate for a Codex-local-style observation path

### Gaps

- I did not find an official standalone Kimi Code usage-reporting endpoint documented in the same style as OpenAI organization usage/cost APIs
- The richest local files are also the most privacy-sensitive
- The shipped parser path therefore depends on the redaction/filtering boundary now captured in `docs/claude-kimi-local-redaction-contract.md`

### Recommendation

Kimi Code is now in active Phase 7 scope and remains the strongest currently researched candidate for bounded local-first support.

That parser-boundary follow-up is now captured in:

- `docs/kimi-local-parser-spike.md`

That follow-up narrows the default first-parser boundary to `config.toml`, `kimi.json`, `state.json`, and task/notification runtime metadata, while keeping `wire.jsonl`, credentials, raw transcripts, and other mixed-content artifacts out of the default parser for v1.0.

The broader shared parser and fixture-redaction rules for both providers are now captured in:

- `docs/claude-kimi-local-redaction-contract.md`

## Comparative Assessment

| Area | Claude Code | Kimi Code |
| --- | --- | --- |
| Subscription product documented | Yes | Yes |
| Official API / provider integration docs | Yes, but split across subscription/API/provider modes | Yes, including approved third-party agent guidance |
| Official local settings/state docs | Yes | Partly; official session persistence is documented, local file layout mostly observed from runtime |
| Rich local footprint on this machine | Yes | Yes |
| Clear published quota refresh semantics | Weaker / less explicit for subscriptions | Stronger / explicitly documented |
| Best likely JiStats path | Local + telemetry + Console/API hybrid | Local-first with optional provider/API corroboration |

## Recommended Follow-On Work

This research pass closed the initial feasibility question, and the immediate research follow-ons are now complete:

1. Claude Code local-surface spike:
   - completed in `docs/claude-local-surface-spike.md`
2. Kimi Code parser spike:
   - completed in `docs/kimi-local-parser-spike.md`
3. Acceptance-policy update:
   - completed in `docs/mirror-accuracy-policy.md`, `docs/low-cost-monitoring-policy.md`, and `docs/metric-glossary.md`
4. Shared redaction contract:
   - completed in `docs/claude-kimi-local-redaction-contract.md`

Phase 7 has now moved from research to shipped bounded implementation:

1. the conservative Claude parser and fixture/test path is landed
2. the conservative Kimi parser and fixture/test path is landed
3. both providers are wired into storage, analytics, dashboard, reporting, tray, and taskbar surfaces within the documented local-first boundary
4. any deeper provider-mode or provider-exact quota claims still remain behind separately validated corroboration work

## Bottom Line

This research pass supports the following planning stance:

- Claude Code is in v1.0 scope, but only through the conservative local-first boundary documented in the Claude spike and redaction contract.
- Kimi Code is in v1.0 scope and remains especially compatible with JiStats' local-first architecture, again within the documented parser and redaction boundaries.
- OpenAI/Codex and MiniMax still anchor the deepest provider-side mirroring story, while Claude/Kimi ship with explicit local-first confidence limits rather than implied parity.
