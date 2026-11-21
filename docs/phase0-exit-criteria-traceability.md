# Phase 0 Exit-Criteria Traceability Note

> **Status**: Current
> **Purpose**: Map each Phase 0 plan deliverable to the current repo doc or fixture path
> **Driven by**: plan.md Phase 0 Exit Criteria

## Exit Criteria Mapping

Per `docs/plan.md` section "Phase 0: Validation Spike", the exit criteria are:

| Exit Criterion | Repo Path | Status |
|----------------|-----------|--------|
| One written capability matrix covering OpenAI and MiniMax | `docs/capability-matrix.md` | ✅ Complete |
| One sample-response archive | `docs/fixtures/openai/`, `docs/fixtures/minimax/` | ✅ Complete |
| One recommended MVP metric list | `docs/metric-glossary.md`, `docs/taskbar-metrics.md` | ✅ Complete |
| One written gap-handling policy | `docs/gap-handling-policy.md` | ✅ Complete |
| One written low-cost monitoring policy | `docs/low-cost-monitoring-policy.md` | ✅ Complete |
| One written mirror accuracy and confidence policy | `docs/mirror-accuracy-policy.md` | ✅ Complete |
| One written offline and reconnection policy | `docs/offline-retry-policy.md` | ✅ Complete |

## Supporting Artifacts

Additional Phase 0-related artifacts that support the exit criteria:

| Artifact | Repo Path | Notes |
|----------|-----------|-------|
| OpenAI mirror spec | `docs/openai-mirror-spec.md` | Defines OpenAI quota-mirroring target behavior |
| MiniMax mirror spec | `docs/minimax-mirror-spec.md` | Defines MiniMax API and subscription mirroring target behavior |
| OpenAI fixture inventory | `docs/fixtures/openai/README.md` | OpenAI sample-response archive index |
| MiniMax fixture inventory | `docs/fixtures/minimax/README.md` | MiniMax sample-response archive index |
| OpenAI API-platform validation | `docs/fixtures/openai/api-platform/README.md` | API-platform authz probe instructions |
| Taskbar overlay validation scenarios | `docs/taskbar-overlay-validation-scenarios.md` | Taskbar metric validation approach |
| Taskbar manual validation | `docs/taskbar-manual-validation.md` | Manual validation checklist |

## Blocked Advanced-Path Item

The only remaining Phase 0 item that is **blocked** and **not required for v1.0 subscription-first baseline**:

| Item | Status | Blocker |
|------|--------|---------|
| OpenAI API-platform freshness/lag capture (`A0.2d`) | 🔴 BLOCKED | Requires credential with `api.usage.read` scope and correct org-scoped role |

Per `docs/progress.md`:
> A successful freshness/lag run requires a credential with `api.usage.read`, the correct organization-scoped role, and explicit org context when needed.

This item remains in blocked backlog and does not block v1.0.

## Phase 0 Status

**CLOSED** for the subscription-first v1.0 path.

Per `docs/plan.md`:
> Phase 0 should stay closed once the subscription-first OpenAI baseline, Codex-local augmentation path, export-import boundary, and MiniMax subscription evidence are documented and accepted.

All subscription-first deliverables are complete. The only unchecked item is the optional API-platform freshness/lag pass, which is blocked on credential availability and tracked as advanced-path follow-up.

## References

- Phase 0 definition: `docs/plan.md` (Phase 0: Validation Spike)
- Progress tracker: `docs/progress.md`
- Capability matrix: `docs/capability-matrix.md`