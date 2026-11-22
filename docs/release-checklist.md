# V1.0 release checklist

Release signoff is open. The [roadmap](plan.md) defines the intended scope. The [work tracker](progress.md) owns follow-up work.

## Evidence-backed release queue

| Gate | Remaining work |
| --- | --- |
| Windows CI | Run the [Windows SQLite quality gate](validation/sqlite-quality-gate.md) on the release candidate. [PR #10](https://github.com/jatmn/JiStats/pull/10) replaced the timing assertion with a test that holds the live provider response while checking persisted state. The replacement passed in [run 44975908262](https://github.com/jatmn/JiStats/actions/runs/44975908262) on main at `9b6d520`. Before that fix, [run 44961330523](https://github.com/jatmn/JiStats-public-prep/actions/runs/44961330523) on `e5230ef` built the test target, then reported 578 ms against a 300 ms limit. [Run 44960685972](https://github.com/jatmn/JiStats-public-prep/actions/runs/44960685972) on `722ae18` failed the same assertion at 456 ms. |
| Static analysis and formatting | Resolve the [clang gate](../scripts/run_clang_quality_gate.ps1) findings or record the release decision on the backlog. The recorded pass found formatting violations in 101 repo-owned files and tidy findings in 58 translation units. |
| Coverage and integration | Confirm and meet the release thresholds and full-flow integration scope. The previous checklist proposed over 80% unit and over 60% integration coverage, with manual checks complete. |
| Upgrade | Validate or explicitly defer the broader v0.x upgrade claim against representative real data. Existing migration and settings-preservation tests cover specific paths. See [migration behavior](reference/storage.md#migration-behavior). |
| Release package | Assemble and smoke-test the portable bundle, including migrations and licenses, with a clean profile and the intended launch directory. Use the [fresh database check](validation/desktop.md#fresh-packaged-database) to verify both migration `010` effects and the [desktop sign-in check](validation/desktop.md#dashboard-and-background-checks) to verify that a pending migration applies at Windows sign-in. |

The optional OpenAI API-platform freshness probe remains blocked under [A0.2d](progress.md#blocked-backlog). The subscription-first release scope can proceed independently.

## Recorded verification

The entries below capture earlier evidence. Record the revision and scope of each future candidate check.

| Area | Evidence |
| --- | --- |
| Build and tests | Debug, Release, and warning-gated builds passed locally on 2026-04-21. The warning-gated suite recorded 4,623 assertions across 443 cases and passed CTest. Focused reruns covered provider fixtures, offline recovery, settings, migration, and credential paths. |
| First run and packaging | The 2026-04-21 isolated-profile launch created `%LOCALAPPDATA%\JiStats\jistats.db`. Standard and side-by-side UPX Release builds were recorded. The fresh profile used opt-in provider defaults. |
| Desktop | The [desktop record](validation/desktop.md#recorded-coverage) captures the 2026-04-20 quota display pass and the 2026-04-21 repeated menu/click-away and Explorer-recovery pass. Earlier Analysis smoke exercised scope switching, charting, and responsiveness, and found an effective-range fallback mismatch that was fixed. |
| Provider evidence | The [2026-04-21 validation record](validation/provider-evidence-2026-04-21.md) covers MiniMax remains and OAuth-backed Kimi quota. Claude Code support is bounded local activity. Codex uses local rollouts for quota. |
| Credentials and SQL | The 2026-04-21 review covered MiniMax Credential Manager storage, provider-owned Kimi OAuth, log handling, and bound SQL parameters in the paths reviewed. The focused credential suite recorded 147 assertions across 21 cases. |
| Performance | The 2026-04-21 local Release pass measured startup readiness at 311.2 ms average, launch-to-dashboard at 315.7 ms average, and steady-state memory at about 22.57 MB working set. Targets are under 2 seconds for startup, under 1 second to load the local dashboard, under 100 MB typical memory, and non-blocking UI during sync. |

See the [archived release record](https://github.com/jatmn/JiStats-public-prep/blob/fd934179646f130fe346a20fb1ca7d82162902f6/docs/release-checklist.md) for the original commands, test counts, and detailed evidence.

## Before tagging

- [ ] Merge the release candidate to main and pass Windows CI.
- [ ] Resolve the open gates above and record the final test and evidence scope.
- [ ] Check version 1.0.0 consistency across the build, manifest, and About.
- [ ] Update the changelog and verify user docs against the packaged behavior.
- [ ] Assemble and smoke-test the portable bundle. For the packed variant, record the exact UPX version and include its matching license and copying texts.

## Release and follow-up

- [ ] Create the GitHub release, attach the verified bundle, add the download link to the README, and prepare the announcement.
- [ ] Monitor crash reports and user feedback. Use those results to prioritize maintenance work.

For a critical issue, branch from the release tag, verify the fix, and publish a patch release. If restoring an earlier binary, confirm database schema compatibility with that binary or restore a compatible pre-upgrade backup.
