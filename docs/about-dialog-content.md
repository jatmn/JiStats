# About Dialog Content Contract

> **Status**: Lane 2 support contract
> **Purpose**: Define the first accurate About dialog content for JiStats v1.0

## Intent

The About dialog should present a trustworthy summary of the product without overstating maturity or provider coverage.

Its content should be sourced from current repo truth:

- `README.md`
- `docs/plan.md`
- `docs/progress.md`
- `LICENSE`
- `THIRD_PARTY_NOTICES.md`

## Required Sections

### 1. Product Identity

Show:

- `JiStats`
- a short subtitle such as:
  - `Windows 11 desktop app for AI quota and usage tracking`

### 2. Version / Build Status

Show:

- the current app version when available from runtime state
- a conservative status line such as:
  - `Status: Early implementation`

Avoid:

- `Stable`
- `Production ready`
- `Complete`

### 3. Current Provider Scope

Show:

- `Current provider scope: OpenAI / Codex + MiniMax + Claude Code + Kimi Code`

Optional supporting lines:

- `All provider integrations are optional and disabled by default until enabled in Settings`
- `OpenAI and MiniMax ship with subscription-level support in v1.0`
- `OpenAI API-platform import and broader MiniMax PAYG support remain follow-on refinement`
- `Claude Code and Kimi Code are shipped in a bounded local-first mode for v1.0`

Avoid:

- implying Claude Code or Kimi Code have provider-exact quota parity with OpenAI or MiniMax
- implying Kilo Code is implemented now

### 4. License Summary

Show:

- `First-party code: Apache-2.0 with Commons Clause`
- `See LICENSE and THIRD_PARTY_NOTICES.md for details`

Do not try to summarize every third-party license in the About dialog itself.

### 5. Attribution / Credits

Optional but acceptable:

- a short line noting the project is AI-assisted / vibecoded

If included, keep it short and secondary. Do not let it replace the product summary or license content.

## Suggested First-Pass Layout

Recommended field set:

- Product name
- Short subtitle
- Version
- Status
- Provider scope
- License pointer

Recommended layout:

- top-left app icon
- product title and subtitle in a short header block
- grouped body copy for provider scope and license notes

## Explicit Non-Claims

The About dialog must not claim:

- full provider parity
- provider-exact Claude Code or Kimi Code quota truth
- exact provider-side truth for every subscription surface
- completed MiniMax credential/account management
- live OpenAI API-platform support by default
- a finished or stable release

## Implementation Handoff

Lane 1 / implementation should build `About JiStats` around this content contract and prefer runtime-backed values where available:

- use runtime app version when present
- keep other content static only where it matches current repo truth
- make the dialog easy to refresh later as scope changes
