# AGENTS.md

This file provides guidance to ask agents when researching this repository.

## Project Overview

JiStats is a Windows 11 native desktop app for tracking OpenAI, Codex, and MiniMax usage with provider quota mirroring as the primary product goal. The repo is in early implementation mode, so research should assume both live code and live planning docs matter.

## Key Documentation

| File | Purpose |
|------|---------|
| `AGENTS.md` (root) | Current product boundaries and working agreement |
| `CLAUDE.md` | Secondary build and implementation summary |
| `docs/plan.md` | Architecture, roadmap, and long-form technical direction |
| `docs/progress.md` | Current execution state, blockers, and handoff notes |
| `docs/test-strategy.md` | Testing approach |
| `docs/entity-model.md` | Provider-agnostic data model |
| `docs/sqlite-schema.md` | Database schema |
| `docs/capability-matrix.md` | Provider capabilities and validation status |

## Counterintuitive Structure

- `include/` mirrors `src/` closely rather than acting like a broad public SDK include tree.
- `src/platform/` holds Windows-specific startup, shell, and credential code.
- `src/taskbar/` is a required Windows 11 product surface, not an experiment.
- `src/core/` and shared models should stay provider-agnostic.
- `src/providers/` contains provider-specific adapters and import seams.

## Provider Scope

- OpenAI / Codex is the primary proof-of-concept provider.
- Research should assume a subscription-first OpenAI/Codex stance, not automatic OpenAI API-platform billing access.
- Do not assume a ChatGPT/Codex subscription implies API billing, API keys, or API usage endpoints.
- MiniMax is in v1.0 scope primarily to validate subscription-plan semantics beyond simple PAYG usage.
- MiniMax subscription-backed paths come first; PAYG validation is follow-on work unless the task explicitly reopens it.

## Research Constraints

- Do not assume provider quota behavior can be mirrored exactly before validating the available signals.
- Prefer official provider surfaces and exposed signals over inferred behavior.
- Keep monitoring cost-aware; research should avoid recommending routine model calls when direct endpoints exist.
- Treat TrafficMonitor as a future reference for taskbar work unless the user explicitly asks for repository review or technical analysis.

## Data Freshness States

Each metric may be labeled as `Local`, `Imported`, or `Estimated`.

Dashboard/runtime freshness states include `Current`, `Stale`, `CatchingUp`, `Offline`, and `Unknown`.

## Mirror Confidence Tiers

- `Exact` - direct validated provider signals
- `HighConfidence` - validated logic with minor timing or lag assumptions
- `Estimated` - partial signals plus local inference
- `Unknown` - insufficient information

## Architecture Constraints

- Platform-specific code belongs in `src/platform/` and `src/taskbar/`.
- Provider-specific logic belongs in `src/providers/`.
- Core/shared logic should remain provider-agnostic.
- UI surfaces consume view models, never raw DB or provider objects.
- DB writes funnel through `StorageService`.
- No blocking network or DB work should happen on the UI thread.
