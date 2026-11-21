# JiStats

## Project Status

This repository is in early implementation mode.

The current goal is to define a practical Windows-native v1.0 for tracking OpenAI, Codex, and MiniMax usage with a strong local-first bias, starting from the subscription-backed surfaces users actually have.

OpenAI remains the primary proof-of-concept provider for v1.0 validation depth, and the current planning baseline is subscription-first tracking for ChatGPT/Codex rather than assumed OpenAI API-platform billing access.

MiniMax is now also an explicit in-scope v1.0 provider, with subscription-backed tracking first and PAYG API validation kept as a follow-on refinement.

The long-term product goal is to support multiple providers, API products, and subscription plans through a provider-agnostic core architecture.

The detailed implementation plan lives in `docs/plan.md`.

The default v1.0 implementation stack is native `C++/C`.

## Working Agreement

- Treat `docs/plan.md` as the source of truth for roadmap, scope, and architecture.
- Treat `docs/progress.md` as the source of truth for current execution state, task status, blockers, and handoff notes.
- Keep implementation changes aligned with `docs/plan.md` and `docs/progress.md`, and update those documents in the same session when the repo state changes.
- When code and docs disagree, treat that as drift to fix rather than assuming either side is automatically correct.
- Keep implementation sequencing realistic even when a feature is required for v1.0.
- When a task is completed, partially completed, blocked, or re-scoped, update `docs/progress.md` in the same work session.
- Keep tasks small enough for a single engineer or AI agent to complete with low context switching and a clear definition of done.
- Prefer vertically sliced, handoff-friendly tasks over broad ambiguous work items.

## Product Direction

The intended product is a lightweight Windows 11 desktop app that helps a user understand:

- recent local activity
- longer-term historical usage
- estimated budget or quota pressure
- active provider or account status
- mirrored provider quota state

The app should prioritize:

- native performance
- minimal runtime dependencies
- local storage of user data
- provider-agnostic internal models where practical
- capability-driven requirements, especially for the OpenAI-plus-MiniMax v1.0 scope
- cost-aware background behavior so monitoring does not create avoidable usage
- provider quota mirroring as a first-class product capability
- native standalone distribution without requiring users to install a runtime
- explicit handling of uncertainty when provider-side limits cannot be mirrored exactly

## Important Constraints

- Do not assume provider-side quota behavior can be replicated exactly without validating the available APIs and signals first.
- Use official OpenAI and MiniMax APIs, pricing surfaces, and exposed signals to drive v1.0 requirements wherever possible instead of inventing behavior ad hoc.
- Do not assume a ChatGPT/Codex subscription implies separate OpenAI API billing, API-key access, or API usage endpoints.
- Prefer non-model API endpoints for telemetry, status, usage, and reconciliation work whenever they are available.
- If any provider workflow requires a model call for a lightweight check, use the lowest-cost acceptable model path and make that behavior explicit and configurable.
- Separate confirmed features from experimental features in plans and future implementation.
- Treat provider quota mirroring as the central product requirement for v1.0, beginning with OpenAI subscription-first tracking and MiniMax subscription usage; keep MiniMax PAYG validation as follow-on work until it is actually validated.
- Keep the v1.0 implementation stack native and low-overhead; do not add extra languages or managed runtimes unless they solve a proven problem.
- Treat Windows 11 taskbar metrics as a required v1.0 feature, but sequence them after core data collection and analytics are stable.
- Use TrafficMonitor only as a future implementation reference unless the user explicitly asks for repository review or technical analysis.
- Keep TrafficMonitor listed as an explicit research input for the required taskbar feature, even when repository review is deferred.
- Plan for continuity behaviors such as run-on-startup, launch-time refresh, and gap reconciliation after long idle periods.
- Design for intermittent connectivity, laptop sleep/wake cycles, and extended offline periods without crashes or noisy alerts.

## Planning Priorities

1. Validate what usage, cost, and rate-limit signals are actually available.
2. Define the quota-mirroring target behavior for OpenAI and MiniMax v1.0 as precisely as possible.
3. Define a provider-agnostic internal data model that still ships OpenAI first while bringing MiniMax into initial scope.
4. Build v1.0 requirements around the maximum trustworthy OpenAI and MiniMax data we can retrieve rather than improvised assumptions.
5. Keep monitoring operations cost-aware so the tracker does not meaningfully increase spend or quota pressure.
6. Build a trustworthy path to a Windows 11 dashboard, tray, and taskbar surface.
7. Keep the base architecture native and minimal with `C++/C` as the default stack.
8. Ensure the app degrades gracefully offline and catches up cleanly when connectivity returns.

## Repo Notes

- Root AI context file: `AGENTS.md`
- Detailed project plan: `docs/plan.md`
- Live execution tracker: `docs/progress.md`
