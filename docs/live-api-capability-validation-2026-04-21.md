# Live API Capability Validation - 2026-04-21

> **Status**: Current release-signoff evidence for live provider/API claims
> **Purpose**: Record which release-facing provider/API claims were refreshed locally on `2026-04-21`, which ones rely on earlier probes, and which advanced-path items stay explicitly blocked outside the v1.0 baseline

## Scope

This note is the release-signoff companion to:

- `docs/capability-matrix.md`
- `docs/fixtures/README.md`
- `docs/release-checklist.md`

It is intentionally evidence-bound. It records only:

- fresh local probes run on `2026-04-21`
- summaries of earlier provider responses
- explicit blocked advanced-path items that do not belong in the base v1.0 release claim

Use the documented provider workflows to repeat these checks. Keep provider credentials, raw captures, and account-specific output in local storage.

## Evidence Summary

### OpenAI / Codex

- An earlier API-platform probe reached the organization usage and cost endpoints and returned `403` with the required usage scope missing.

Release-signoff conclusion:

- This does **not** block the current v1.0 release story, because OpenAI remains subscription-first by default and API-platform import is still an optional advanced path.
- The blocked freshness/lag item remains the external follow-up tracked as `A0.2d`, not an open release-signoff blocker for the current shipped baseline.

### MiniMax

- Both MiniMax remains endpoints returned usable subscription responses during validation.

Release-signoff conclusion:

- MiniMax live provider-response evidence is current enough for the shipped v1.0 Coding Plan / Token Plan remains baseline.
- The [reset-window findings](fixtures/minimax/reset-window-evidence-log.md) describe the observed cadence.

### Claude Code

- No provider-exact Claude API claim exists in the shipped v1.0 baseline.
- Claude remains a bounded local-first provider.

Release-signoff conclusion:

- No additional live provider API probe was required for Claude, because the release-facing claim is intentionally local-first rather than provider-exact.

### Kimi Code

- The OAuth-backed quota endpoint returned usage and limit fields. The raw response stays local.

Release-signoff conclusion:

- The validation confirmed that Kimi can return provider-exact quota through its OAuth-backed endpoint.
- The shipped release wording should continue to keep Kimi conditional: provider-exact quota is available when usable OAuth exists, and JiStats should still avoid fabricating local fallback quota when that path is unavailable.

## Release Outcome

The current live API capability evidence is sufficient for the shipped v1.0 claims.

What is now considered release-verified:

- MiniMax Coding Plan / Token Plan live provider-response path
- Kimi `/usages` live provider-response path when local OAuth exists
- OpenAI subscription-first baseline remaining separate from the optional API-platform path
- Claude remaining explicitly local-first rather than provider-exact

What remains intentionally outside the release blocker set:

- OpenAI API-platform freshness/lag capture requiring a higher-scope credential with `api.usage.read`
- Broader MiniMax PAYG history/discovery work
- Any future claim that Claude has provider-exact quota parity

## References

- `docs/capability-matrix.md`
- `docs/fixtures/README.md`
- `docs/fixtures/openai/api-platform/README.md`
- `docs/fixtures/minimax/README.md`
- `docs/fixtures/minimax/reset-window-evidence-log.md`
- `docs/release-checklist.md`
- `docs/progress.md`
