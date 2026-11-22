# V1.0 release checklist

Release signoff is open. The [roadmap](plan.md) defines the intended scope and tracks implementation work. Use this checklist to record the revision, environment, and evidence for the candidate that will ship.

## Evidence-backed release queue

- [ ] Run the [SQLite quality gate](validation/sqlite-quality-gate.md) on the release candidate. Confirm that the candidate's CI run actually executes the full cross-built suite on Windows. Use manual dispatch if change detection skips the build.
- [ ] Run the [clang quality gate](../scripts/run_clang_quality_gate.ps1) on the candidate. Resolve findings or record the release decision and any remaining backlog. The earlier run reported formatting violations in 101 repo-owned files and tidy findings in 58 translation units.
- [ ] Confirm the coverage and integration requirements for release, then record measured results and any gaps. The earlier plan proposed greater than 80% unit and 60% integration coverage. The current CI workflow runs tests without enforcing those thresholds.
- [ ] Verify the packaged SQLite baseline and Windows sign-in launch using a clean profile. Check that both historical v12 layouts reopen with their settings intact. Use a disposable pre-baseline database to verify the startup warning and check that the file stays unchanged. [Packaged database and sign-in checks](validation/desktop.md#fresh-packaged-database) · [Migration behavior](reference/storage.md#migration-behavior)
- [ ] Run the [Windows desktop checks](validation/desktop.md) for dashboard, tray and taskbar quota, offline retry/dismiss and recovery, Explorer restart, and sleep/resume. Check Analysis scope and manual and scheduled HTML output on the candidate. Record which provider surfaces and layouts were exercised.
- [ ] Check startup time, local dashboard time, memory, and responsiveness during sync on the candidate. Record the machine, build, and results against the targets below.

The optional [OpenAI API-platform freshness probe](plan.md#blocked-backlog) remains blocked. The subscription-first release can proceed independently.

## Recorded verification

These entries are historical or development evidence. Record the revision and scope of each release-candidate check separately.

| Area | Evidence |
| --- | --- |
| Build and tests | Debug, Release, and warning-gated builds passed locally on 2026-04-21. The warning-gated suite recorded 4,623 assertions across 443 cases and passed CTest. Focused reruns covered provider fixtures, offline recovery, settings, migration, and credential paths. |
| Windows CI | The full cross-built Windows suite passed in [the PR #16 run](https://github.com/jatmn/JiStats/actions/runs/45067254166) at `1205402` and in [the integrated main build](https://github.com/jatmn/JiStats/actions/runs/45068074135) at `eba30e2`. Earlier, [PR #10](https://github.com/jatmn/JiStats/pull/10) replaced a 300 ms timing threshold with a test that holds the provider response while checking persisted state. The replacement passed on main at `9b6d520` in [run 44975908262](https://github.com/jatmn/JiStats/actions/runs/44975908262). The prior assertion had failed at 578 ms and 456 ms in [run 44961330523](https://github.com/jatmn/JiStats/actions/runs/44961330523) and [run 44960685972](https://github.com/jatmn/JiStats/actions/runs/44960685972). |
| Latest observed full Windows run | [Main at `6cd5610`](https://github.com/jatmn/JiStats/actions/runs/45076271119) ran the Linux and Windows jobs. The Windows suite passed 448 cases and 5,189 assertions. [Main at `326dcd4`](https://github.com/jatmn/JiStats/actions/runs/45079671212) passed the aggregate CI check by reusing the predecessor result and skipped both build and test jobs. |
| First run and packaging | The 2026-04-21 isolated-profile launch created `%LOCALAPPDATA%\JiStats\jistats.db`. Standard and side-by-side UPX Release builds were recorded. The fresh profile used opt-in provider defaults. |
| Desktop | The [desktop record](validation/desktop.md#recorded-coverage) captures the 2026-04-20 quota display pass and the 2026-04-21 repeated menu/click-away and Explorer-recovery pass. Earlier Analysis smoke exercised scope switching, charting, and responsiveness, and found an effective-range fallback mismatch that was fixed. |
| Provider evidence | The [2026-04-21 validation record](validation/provider-evidence-2026-04-21.md) covers MiniMax remains and OAuth-backed Kimi quota. Claude Code support is bounded local activity. Codex uses local rollouts for quota. |
| Credentials and SQL | The 2026-04-21 review covered MiniMax Credential Manager storage, provider-owned Kimi OAuth, log handling, and bound SQL parameters in the paths reviewed. The focused credential suite recorded 147 assertions across 21 cases. |
| Performance | The 2026-04-21 local Release pass measured startup readiness at 311.2 ms average, launch-to-dashboard at 315.7 ms average, and steady-state memory at about 22.57 MB working set. Targets are under 2 seconds for startup, under 1 second to load the local dashboard, under 100 MB typical memory, and non-blocking UI during sync. |

See the [archived release record](https://github.com/jatmn/JiStats/blob/fd934179646f130fe346a20fb1ca7d82162902f6/docs/release-checklist.md) for the original commands, test counts, and detailed evidence.

## Before tagging

- [ ] Merge the release candidate to main, complete the queue above on the intended release revision, and record the test and evidence scope.
- [ ] Confirm version 1.0.0 across the packaged build, manifest, and About. The audited source declares 1.0.0 in CMake and AppController and 1.0.0.0 in the Windows manifest.
- [ ] Create the initial changelog and verify user docs against the packaged behavior.
- [ ] Assemble and smoke-test the portable bundle. For MinGW builds, include the [runtime license files from the toolchain used](../THIRD_PARTY_NOTICES.md#mingw-w64-and-winpthreads). For the packed variant, record the exact UPX version and include its matching license and copying texts.

## Release and follow-up

- [ ] Create the GitHub release, attach the verified bundle, add the download link to the README, and prepare the announcement.
- [ ] Monitor crash reports and user feedback. Use those results to prioritize maintenance work.

For a critical issue, branch from the release tag, verify the fix, and publish a patch release. If restoring an earlier binary, confirm database schema compatibility with that binary or restore a compatible pre-upgrade backup.
