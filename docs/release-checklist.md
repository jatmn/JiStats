# Release Checklist and Definition of Done

> **Status**: Evidence-backed release-signoff queue; not yet signed off for release
> **Purpose**: Define what "done" means for v1.0 release
> **Driven by**: plan.md section 14.4 (Quality Gates and Definition of Done) and section 15 (Versioned Milestones)

## Current Evidence Snapshot

Current repo-backed evidence as of `2026-04-21`:

- Local verification rerun succeeded:
  - `cmake --build build --config Debug --target JiStats`
  - `cmake --build build --config Debug --target JiStatsTests`
  - `.\build\bin\Debug\JiStatsTests.exe --reporter compact`
  - `cmake --build build --config Release --target JiStats`
- Warning-gated verification rerun also succeeded:
  - `cmake --build build-w4 --config Debug --target JiStats`
  - `cmake --build build-w4 --config Debug --target JiStatsTests`
  - `.\build-w4\bin\Debug\JiStatsTests.exe --reporter compact`
- The latest warning-gated full-suite rerun is also green:
  - `.\build-w4\bin\Debug\JiStatsTests.exe --reporter compact` (`4623` assertions across `443` test cases)
  - `ctest --test-dir build-w4 -C Debug --output-on-failure`
- Focused upgrade/settings preservation reruns also succeeded:
  - `.\build-w4\bin\Debug\JiStatsTests.exe "[schema][migration]" --reporter compact`
  - `.\build-w4\bin\Debug\JiStatsTests.exe "AppController rehydrates persisted provider tracking preferences,AppController persists and rehydrates the last selected provider scope,AppController persists and rehydrates analysis scope preferences" --reporter compact`
- The previously reopened MiniMax async/settings-shell and rollup-backed quantified-summary failures were repaired and rerun cleanly on `2026-04-21`.
- `docs/progress.md` already records the shipped dedicated Analysis workspace, landed reporting persistence (`quota_snapshot_rollups_daily`, `recovery_checkpoints`, `export_runs`), packaging/UPX closeout, and the closed Phase 7 four-provider rollout.
- The former tracker blockers are now closed: the final taskbar shell pass is recorded via `scripts/run_taskbar_shell_signoff.ps1`, and the first real clang findings pass is recorded via `scripts/run_clang_quality_gate.ps1`.
- The remaining hard release-signoff work is now follow-through rather than missing evidence: address the recorded clang backlog if a clean clang gate is required for release, decide the explicit automated coverage/integration threshold, prove or defer the broader `v0.x` migration claim, and complete the remaining CI/release-day checklist items.

## Evidence-Backed Release Queue

| Area | Status | Current Evidence | Remaining Work |
| --- | --- | --- | --- |
| Docs accuracy | `Verified` | `docs/plan.md`, `docs/progress.md`, `README.md`, `docs/metric-glossary.md`, and the reporting spike now match the shipped Analysis workspace, four-provider opt-in model, and Kimi `/usages` quota path | Keep aligned only if later release-signoff work changes behavior |
| Local build + automated tests | `Verified locally` | Debug app build, Debug test build, Release app build, the `build-w4` warning-gate app/test builds, the full `build-w4` suite (`4623` assertions across `443` test cases), and `ctest --test-dir build-w4 -C Debug --output-on-failure` all pass on `2026-04-21` | Re-check only if later release-signoff work changes code or test coverage |
| Packaging / distribution baseline | `Verified locally` | Standard Release builds and the side-by-side `-upx` packaging path are already documented in `docs/progress.md`; the normal Release app build succeeded again on `2026-04-21`, and the current v1.0 claim is now explicitly portable Release binaries rather than an installer | Re-check only if the shipped artifact set changes or an installer is added later |
| Taskbar shell interactions | `Verified locally` | `docs/taskbar-manual-validation.md` now includes the 2026-04-20 live display pass plus a 2026-04-21 scripted shell-signoff pass (`scripts/run_taskbar_shell_signoff.ps1`) covering repeated right-click dismiss/click-away recovery and Explorer restart recovery; focused `[taskbar]` tests reran cleanly at `230` assertions across `27` cases | Re-check only if taskbar shell interaction behavior changes again before release |
| Security / credential handling | `Verified in repo` | MiniMax secrets are stored in Windows Credential Manager with only `secret_ref` metadata in SQLite, Kimi quota uses the provider-owned OAuth file under `~/.kimi/credentials/kimi-code.json`, provider/auth paths do not emit token-bearing log calls, and storage/query paths use bound parameters with the remaining dynamic table names constrained to internal helpers like `rollupTableName()` | Re-check only if credential-storage or provider logging behavior changes before release |
| Install / first-run behavior | `Verified locally` | An isolated `LOCALAPPDATA` Release launch created `JiStats\\jistats.db` on first run, fresh-install provider defaults were revalidated, startup/background wiring passed again, and dashboard shell tests still cover native-close safety plus the provider settings surface | Re-check only if startup/bootstrap behavior or distribution posture changes before release |
| Live API capability evidence | `Verified for current v1.0 claims` | The [validation summary](live-api-capability-validation-2026-04-21.md) records MiniMax subscription and Kimi quota results. OpenAI API-platform freshness remains an optional advanced path, blocked (`A0.2d`) until a credential with the usage scope is available, and is not a v1.0 release blocker. | Re-check only if release-facing provider/API claims expand beyond the current shipped baseline |

## Definition of Done for v1.0

### Feature Completion

From plan.md section 14.4:

A feature is not done unless:

- code is merged with tests appropriate to the change
- logging and error paths are covered
- no UI-thread blocking is introduced
- configuration and persisted-state impacts are documented
- user-visible status behavior is defined
- relevant docs are updated

### Release Candidate Criteria

From plan.md section 14.4 (Milestone C criteria):

| Requirement | Status | Evidence / Remaining Gap |
| --- | --- | --- |
| OpenAI / Codex subscription workflows are stable and clearly separated from optional OpenAI API-platform import | `Verified in repo` | Current docs, settings, import paths, and tests reflect the subscription-first split; optional API-platform freshness/lag evidence remains an advanced-path follow-on rather than a v1.0 blocker |
| MiniMax subscription workflows are stable across the current Coding Plan / Token Plan remains baseline | `Verified in repo` | Current runtime/docs baseline treats Coding Plan + Token Plan remains as the shipped MiniMax mirror anchors |
| Claude Code and Kimi Code are user-activatable from the shipped settings surface and stay inside their documented bounded scope | `Verified in repo` | Phase 7 closeout plus settings/runtime coverage already landed; Claude remains local-first and Kimi combines local activity/history with validated `/usages` quota truth when OAuth exists |
| Quota mirroring is stable and accurate within defined targets for validated provider-backed surfaces, and local-first providers remain clearly labeled when provider-exact truth is unavailable | `Verified in repo` | Current provider docs, UI wording, and tests all preserve the exact-vs-local boundary; remaining live/manual release evidence belongs to taskbar/security/install signoff rather than mirror-engine implementation |
| Dashboard shows current/recent usage with correct badges | `Verified in repo` | Dashboard presenter/tests plus the shipped Analysis/reporting surfaces cover current badge/freshness/provenance behavior |
| Tray icon works with status indicators across the shipped four-provider baseline | `Verified in repo` | Automated tray coverage exists; final manual shell-interaction confirmation still belongs to the taskbar/tray signoff pass |
| Taskbar metrics render and pass the current TrafficMonitor/Explorer coexistence checks without overstating Claude/Kimi as provider-exact quota mirrors | `Verified locally` | The live display pass, scripted shell-signoff pass, and focused taskbar tests together cover the current taskbar behavior and keep Claude/Kimi scoped as local-first rather than provider-exact quota rows |
| HTML reports generate correctly | `Verified locally` | The 2026-04-21 automated suite includes manual and scheduled HTML export coverage, and export paths are already persisted through `export_runs` |
| No known crash path during offline or reconnect flows | `Verified in repo` | Offline/recovery policy and automated coverage exist; no active crash blocker is recorded in `docs/progress.md` |
| No duplicate import or reconciliation corruption in standard fixtures | `Verified in repo` | Import/reconciliation persistence and duplicate-safe local-provider behavior are covered by landed tests and Phase 5 closeout |
| Mirror outputs include confidence tier and freshness metadata | `Verified in repo` | Current metric glossary, presenters, reporting, and provider-path wording all preserve confidence/freshness metadata |
| Taskbar, tray, and dashboard states agree on provider status | `Verified in repo` | Shared provider pipeline is landed; remaining work is manual shell confirmation rather than a known state-model mismatch |
| Local startup after prior sync state is reliable | `Verified locally` | Startup/recovery paths are heavily exercised in the current automated suite and passed again on `2026-04-21` |
| Downtime recovery and backfill behavior is trustworthy | `Verified in repo` | Recovery persistence, backfill checkpoints, gap labeling, and automated recovery coverage are landed; no open blocker is recorded |
| Monitoring overhead is demonstrably low and policy-compliant | `Verified in repo` | `docs/progress.md` records the steady-state background sanity run and the low-resource implementation closeout |
| Offline behavior is stable, non-intrusive, and self-recovering | `Verified in repo` | Offline notice retry/dismiss flows and recovery clearing are shipped and documented |
| Release checklist and rollback plan are complete | `Pending signoff completion` | This document and the rollback section exist; completion now depends on the remaining open quality/release-day gates rather than missing taskbar shell evidence |

### Phase 7 Four-Provider Closeout

- [x] Settings, onboarding, and activation flows expose OpenAI / Codex, MiniMax, Claude Code, and Kimi Code honestly
- [x] Release-facing docs make the four-provider v1.0 story explicit without claiming provider-exact Claude/Kimi parity
- [x] Glossary, taskbar validation, and checklist wording all agree on local-first versus provider-exact boundaries
- [x] Final acceptance notes record which live/manual passes covered OpenAI/MiniMax quota rows versus Claude/Kimi local-first behavior

Phase 7 closeout note:

- See `docs/phase7-v1-closeout-notes.md` for the four-provider acceptance summary, the manual-validation boundary for OpenAI/MiniMax versus Claude/Kimi, and the explicit note that remaining unchecked items in this checklist are broader v1.0 release gates rather than open Phase 7 implementation tasks.

## Pre-Release Checklist

### Documentation

- [x] `docs/plan.md` is accurate and reflects implemented behavior
- [x] `docs/progress.md` shows all tasks complete
- [x] API capability matrix validated with real API responses
- [x] User-facing docs (README, quick start, release-facing support docs) reflect the four-provider v1.0 UI and keep Claude local-first wording plus Kimi's bounded local-activity and validated `/usages` quota wording honest

Documentation note:

- `docs/quick-start.md` now covers the shipped portable Release-binary path directly, including first launch, `%LOCALAPPDATA%` storage, optional `--background` startup, provider opt-in defaults, and the optional `JiStats-upx.exe` variant.
- Keep this item closed only while `README.md`, the quick start, and release-facing support docs stay aligned with the actual portable-no-installer baseline.

### Code Quality

- [x] All compiler warnings resolved (MSVC /W4)
- [x] No known release-blocking functional regressions remain in the current automated suite
- [x] MiniMax async/settings-shell regressions are fixed and rerun cleanly
- [x] Rollup-backed quantified-summary regressions are fixed and rerun cleanly
- [ ] Static analysis (clang-tidy) shows no errors
- [ ] Code formatted with clang-format
- [x] No TODO comments remaining in code
- [x] No hardcoded credentials or secrets

Code-quality note:

- Final release-quality verification on `2026-04-21` rebuilt the warning-gated tree, reran the full `build-w4` suite (`4623` assertions across `443` test cases), reran `ctest --test-dir build-w4 -C Debug --output-on-failure` cleanly, confirmed there are no repo-owned `TODO` comments in `src/`, `include/`, `tests/`, or `scripts/`, and confirmed there are no `sk-...` style credential literals in `src/`, `scripts/`, or `db_migrations/`.
- The remaining unchecked code-quality items are now strictly the clang-tooling outputs themselves. The repo-owned config/bootstrap is present and the first real findings pass is recorded: `clang-format` reported violations in `101` repo-owned files, while `clang-tidy` reported findings in `58` translation units. The biggest tidy buckets were `bugprone-throwing-static-initialization`, `performance-enum-size`, `bugprone-unchecked-optional-access`, `performance-no-int-to-ptr`, and `bugprone-easily-swappable-parameters`.
- `scripts/run_clang_quality_gate.ps1` now auto-detects user-local LLVM installs, and this machine used a local `%LOCALAPPDATA%\\Programs\\LLVM-22.1.3` archive extraction for the first pass rather than requiring an admin-level LLVM install.
- MiniMax provider-side usage-history import is now explicitly treated as a deferred future path rather than an accidental open v1.0 blocker; the shipped subscription-first baseline remains the validated remains-endpoint quota mirror.
- A focused `2026-04-21` source scan found no hardcoded `sk-*` style credential literals in `src/`, `scripts/`, or `db_migrations/`; the remaining token-related hits in those paths are field names, environment-variable wiring, or runtime header assembly rather than embedded live secrets. Synthetic placeholder secrets remain in tests/fixtures by design and are not release credentials.

### Testing

- [ ] Automated test/coverage gate is explicitly satisfied for the chosen v1.0 threshold
- [x] Fixture replay tests pass for OpenAI
- [x] Focused Claude and Kimi local-parser/runtime fixture coverage passes for the approved bounded inputs
- [ ] Integration tests pass for complete flows
- [x] Offline recovery tests pass
- [x] Long-gap tests pass (simulated 3-day and 7-day gaps)
- [x] UI state tests pass (indicator visibility, notice suppression, provider activation visibility)
- [x] Manual tray and taskbar tests completed with explicit notes for provider-exact versus local-first coverage, using `docs/taskbar-manual-validation.md` to record repeated context-menu dismiss, hover return, and Explorer/taskbar recovery behavior

Testing note:

- The current local automated evidence is strong: Debug app build, Debug test build, the full `JiStatsTests` suite, the separate `build-w4` warning-gated suite, and `ctest --test-dir build-w4 -C Debug --output-on-failure` all passed on `2026-04-21` (`4623` assertions across `443` test cases in the latest warning-gated full rerun).
- The final tray/taskbar shell evidence is now recorded too: `scripts/run_taskbar_shell_signoff.ps1` passed three repeated overlay right-click/click-away cycles plus Explorer restart recovery against the Release app on `2026-04-21`, and focused `.\build\bin\Debug\JiStatsTests.exe "[taskbar]" --reporter compact` reran cleanly at `230` assertions across `27` test cases.
- Focused release-checklist reruns on `2026-04-21` also passed cleanly:
  - `JiStatsTests.exe "[openai][fixtures]" --reporter compact` (`108` assertions across `15` test cases)
  - `JiStatsTests.exe "[claude]" --reporter compact` (`109` assertions across `17` test cases)
  - `JiStatsTests.exe "[kimi]~[app]" --reporter compact` (`152` assertions across `18` test cases)
  - `JiStatsTests.exe "[app][runtime][recovery],[app][runtime][offline_notice]" --reporter compact` (`331` assertions across `8` test cases)
  - `JiStatsTests.exe "[schema][migration]" --reporter compact` (`116` assertions across `7` test cases)
  - `JiStatsTests.exe "DashboardPresenter shows offline notice content and routes retry and dismiss actions,AppController dashboard dismiss and tray retry keep offline notice low-noise across recovery transitions,AppController defaults all shipped provider tracking to disabled on a fresh install,AppController rehydrates persisted provider tracking preferences" --reporter compact` (`84` assertions across `4` test cases)
  - `JiStatsTests.exe "AppController rehydrates persisted provider tracking preferences,AppController persists and rehydrates the last selected provider scope,AppController persists and rehydrates analysis scope preferences" --reporter compact` (`83` assertions across `3` test cases)
  - `JiStatsTests.exe "*offline notice in sync*" --reporter compact` (`60` assertions in `1` test case)
- The MiniMax async/settings-shell and rollup-backed quantified-summary regressions that briefly reopened this section were fixed and rerun cleanly; the remaining open testing work is now broader policy/signoff work such as the explicit automated coverage threshold and full-flow integration-test decision, not missing tray/taskbar shell evidence.

### Security

- [x] Credentials stored in Windows Credential Manager (not plaintext)
- [x] No sensitive data in logs (per plan.md section 14.5)
- [x] API keys not logged or displayed in UI
- [x] No SQL injection vulnerabilities

Security note:

- Release security/credential review completed on `2026-04-21`.
- Repo evidence confirms MiniMax secrets are written through `WindowsCredentialStore` and persisted in SQLite only as `credential_profiles.secret_ref` metadata (`db_migrations/001_initial_schema.sql`, `src/platform/WindowsCredentialStore.cpp`, `src/db/StorageService.cpp`).
- Kimi exact-quota access uses the provider-owned OAuth credential file at `~/.kimi/credentials/kimi-code.json`; JiStats does not mirror those bearer tokens into its own database.
- Provider/auth code paths do not call the logger with raw bearer tokens or secret values, and focused local tests still pass for credential handling: `JiStatsTests.exe "[credential_store],[minimax][credentials],[app][shell][minimax]" --reporter compact` (`147` assertions across `21` test cases).
- SQL review found the live query paths using prepared statements and bound parameters; the remaining concatenated identifiers are internal-only helper outputs such as `rollupTableName(UsageRollupResolution)`, not user-controlled table names.

### Performance

- [x] App startup < 2 seconds
- [x] Dashboard load < 1 second (from local data)
- [x] Sync operations don't block UI
- [x] Memory usage < 100MB typical

Performance note:

- Final `2026-04-21` Release measurements against isolated `LOCALAPPDATA` roots on the current release-candidate tree came in comfortably inside the checklist thresholds:
  - App startup readiness (`WaitForInputIdle`) averaged `311.2 ms` across 5 runs, with a max of `321.5 ms`
  - Launch-to-dashboard readiness averaged `315.7 ms` across 5 runs, with a max of `326.7 ms`
  - Steady-state background memory after 60 seconds was about `22.57 MB` working set and `9.25 MB` private memory
- Focused non-blocking/UI async coverage also reran cleanly on `2026-04-21` for `DashboardPresenter runs OpenAI import asynchronously`, `DashboardPresenter runs offline notice actions asynchronously`, `DashboardPresenter manual HTML export runs asynchronously`, `AnalysisPresenter surfaces a pending refresh status while async analysis work is in flight`, `DashboardPresenter runs MiniMax credential actions asynchronously`, `AppController keeps MiniMax validation asynchronous and preserves the masked secret field`, and `AppController surfaces MiniMax validation failures through the settings shell`.
- These numbers are now the release-candidate rerun, so the checklist thresholds are considered satisfied for the current tree.

### Installation / Distribution Posture

- [x] The v1.0 distribution posture is explicitly chosen and documented (`portable Release binaries`)
- [x] Installer-specific validation is not part of the current v1.0 claim; no installer assets are present in the repo baseline
- [x] Release-facing docs avoid promising installer-specific behavior such as shortcuts or uninstallers

Installation note:

- The current repo-backed v1.0 posture is portable Release binaries from `build/bin/Release/`.
- If an installer is later introduced, reopen this section and add explicit install/uninstall validation rather than treating that future packaging work as already covered.

### Packaging / Distribution

- Preferred local Release build command is `pwsh -File .\scripts\build_release.ps1`, which stops any running JiStats binaries before invoking `cmake --build build --config Release`
- [x] Standard Release binaries build successfully
- [x] Optional UPX-packed `JiStats-upx.exe` builds successfully when `JISTATS_ENABLE_UPX=ON`
- [x] `JiStats.exe` and `JiStats-upx.exe` coexist side by side without overwriting each other
- [x] If shipping `JiStats-upx.exe`, the exact official UPX version used is recorded
- [x] If shipping `JiStats-upx.exe`, matching upstream UPX `LICENSE` and `COPYING` texts are included

Packaging note:

- The current bundled official UPX tool is `tools/upx/upx.exe` version `5.1.1`.
- The matching upstream redistribution texts already live in `tools/upx/LICENSE` and `tools/upx/COPYING`.

### First-Run Experience

- [x] Clean first-run creates database and schema under `LOCALAPPDATA\\JiStats`
- [x] First-run help/settings surfaces cover provider activation and setup honestly
- [x] Background/tray startup behavior is covered by current startup and shell tests
- [x] The fresh-install dashboard state handles the empty/default case without assuming enabled providers or existing data

First-run note:

- Local validation on `2026-04-21` launched `build/bin/Release/JiStats.exe --background` against an isolated `LOCALAPPDATA` root and confirmed first-run database creation at `%LOCALAPPDATA%\\JiStats\\jistats.db`.
- Focused automation reruns also passed on `2026-04-21`:
  - `JiStatsTests.exe "[schema][storage],[app][runtime]" --reporter compact` (`1113` assertions across `36` test cases)
  - `JiStatsTests.exe "[startup]" --reporter compact` (`189` assertions across `13` test cases)
  - `JiStatsTests.exe "DashboardPresenter keeps quota bars compact and handles native close safely" --reporter compact` (`10` assertions in `1` test case)
- Those checks revalidated `LOCALAPPDATA` database redirection, fresh-install provider opt-in defaults, startup/background registry wiring, and native-close shell behavior without reopening an installer-specific release path.

### Migration

- [ ] Upgrade from v0.x doesn't corrupt existing data
- [x] Schema migrations run correctly
- [x] Settings are preserved across upgrades

Migration note:

- Focused upgrade evidence was rerun on `2026-04-21`: `JiStatsTests.exe "[schema][migration]" --reporter compact` now passes at `116` assertions across `7` test cases, including explicit coverage that settings rows survive the version `10 -> 11` schema upgrade path.
- Settings persistence is also covered at the app level: `JiStatsTests.exe "AppController rehydrates persisted provider tracking preferences,AppController persists and rehydrates the last selected provider scope,AppController persists and rehydrates analysis scope preferences" --reporter compact` passed at `83` assertions across `3` test cases.
- The still-open migration question is the broader `v0.x` data-corruption claim across real prior release baselines, which remains separate from the now-verified settings-preservation path.

### Versioning

- [x] Version strings are consistently at `1.0.0` across the shipped app/runtime surfaces

Versioning note:

- `CMakeLists.txt` already declares `project(JiStats VERSION 1.0.0 ...)`, the manifest uses `1.0.0.0`, and the dashboard presenter/default UI version text is `1.0.0`.
- `src/app/AppController.cpp` no longer contains the stale `"0.1.0-spike"` fallback assignments; the app-owned runtime/view-model paths now align with the shipped `1.0.0` baseline.

## Release Gate Summary

| Gate | Pass Criteria |
|------|---------------|
| Code Complete | All features implemented per spec |
| Tests Pass | Unit >80%, integration >60%, manual tests complete |
| Quality | No crashes, no corruption, mirrors accurate |
| Security | No exposed secrets, no injection vulnerabilities |
| Performance | Startup <2s, UI non-blocking, <100MB RAM |
| Docs | User docs accurate, API matrix validated |
| Install | Portable first-run/bootstrap validated; no installer-specific promises unless an installer is added later |

## Milestone Alignment

From plan.md section 15:

| Milestone | Criteria | Status |
|-----------|----------|--------|
| **A: Planning Complete** | Capability matrix, entity model, v1.0 metrics, workstream ownership, test strategy | ✅ Planning docs done |
| **B: Internal Alpha** | Data pipelines work, mirror engine works, dashboard functional, taskbar prototype visible | ✅ Surpassed by the current shipped baseline |
| **C: V1.0 Release Candidate** | All Milestone B + taskbar reliable, offline stable, monitoring low-cost, release checklist complete | 🟡 Release-signoff queue still open |

## v1.0 Final Handoff Checklist

When marking v1.0 as ready for release:

### Before Tag

- [ ] All code merged to main
- [ ] All tests passing on CI
- [ ] Version bumped to 1.0.0
- [ ] CHANGELOG.md updated
- [ ] Portable release bundle assembled and smoke-tested

### Release Day

- [ ] GitHub release created
- [ ] Portable release bundle uploaded to release
- [ ] README updated with download link
- [ ] Announcement prepared

### Post-Release

- [ ] Monitor for crash reports
- [ ] Monitor for user feedback
- [ ] Plan v1.1 fixes based on feedback

## Rollback Plan

If critical issue found post-release:

1. **Hotfix Process**:
   - Create hotfix branch from v1.0 tag
   - Fix issue
   - Test
   - Release v1.0.1

2. **Rollback to Previous Version**:
   - Users can replace the portable v1.0 binaries with the last stable release if needed
   - Database schema allows v1.0.1 to migrate properly

## References

- Quality gates: `docs/plan.md` section 14.4
- Versioned milestones: `docs/plan.md` section 15
- Test strategy: `docs/test-strategy.md`
- Low-cost monitoring: `docs/low-cost-monitoring-policy.md`
- Offline behavior: `docs/offline-retry-policy.md`
