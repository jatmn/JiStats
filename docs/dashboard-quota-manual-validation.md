# Dashboard Quota Manual Validation

> **Status**: Lane 2 manual validation checklist for the shipped dashboard quota baseline
> **Purpose**: Give future follow-up work a repeatable manual check for dashboard quota wording, progress-bar behavior, and application-status cleanup
> **Related Specs**: `docs/dashboard-quota-display-contract.md`, `docs/progress.md`

## Scope

This checklist is for the quota presentation and app-status baseline only.

It should verify:

- the provider quota table `Used` column displays as a percentage for OpenAI and MiniMax
- the dashboard summary cards display explicitly labeled remaining percentages
- supporting numeric detail is clearly labeled as usage detail
- OpenAI and MiniMax progress bars fill in the same remaining-first direction
- the dashboard does not ask the user to mentally invert raw counts or percentages
- the application-status block no longer shows `Start With Windows`

This checklist does not close:

- taskbar overlay rendering
- the separate MiniMax credential-entry flow
- OpenAI subscription toggle behavior

## Preflight

Before running the pass:

- use a build that includes the current dashboard quota semantics baseline
- have both OpenAI and MiniMax quota rows visible in the dashboard if possible
- record the underlying provider sample or known fixture values used for the check
- keep the current implementation screenshots or notes if a before/after comparison is needed

## Validation Rules

Apply these rules when judging the UI:

- a value under a table `Used` label must be an explicit percentage used
- a value in the summary cards must be explicitly labeled as remaining
- raw counts may appear only as labeled usage detail
- a summary-card bar that fills further to the right should indicate more remaining quota
- OpenAI and MiniMax must follow the same rule set
- `Start With Windows` should not appear in the application-status block

## Provider Checklist

### OpenAI

- [ ] The provider quota table `Used` value is shown as an explicit percentage.
- [ ] The displayed table percentage is the normalized UI-facing used percentage even when the upstream source needs inversion logic internally.
- [ ] The summary card shows an explicitly labeled remaining percentage.
- [ ] Any raw counts are labeled as supporting detail rather than shown as the main percentage value.
- [ ] The progress bar fill increases as remaining quota increases.
- [ ] The OpenAI bar direction now matches MiniMax.

### MiniMax

- [ ] The provider quota table `Used` value is shown as an explicit percentage.
- [ ] The value next to any table `Used` label is not just a raw used count with no `%` meaning.
- [ ] The summary card shows an explicitly labeled remaining percentage.
- [ ] Any raw counts are labeled as supporting detail rather than shown as the main percentage value.
- [ ] The progress bar fill increases as remaining quota increases.
- [ ] The MiniMax bar direction matches OpenAI.

### Application Status

- [ ] `Start With Windows` no longer appears in the application-status block.
- [ ] The remaining application-status fields still read like live runtime status, not settings/preferences.

## Scenario Matrix

Use concrete rows like these when recording evidence.

| Scenario | Provider Input | Expected Main Value | Expected Detail | Expected Bar Fill | Pass/Fail | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| Mid-window healthy state | `used=25`, `limit=100`, `remaining=75` | Table `Used 25%`; summary `75% remaining` | `75 of 100 left; 25 used` | About 75% filled |  |  |
| Warning state | `used=74`, `limit=100`, `remaining=26` | Table `Used 74%`; summary `26% remaining` | `26 of 100 left; 74 used` | About 26% filled |  |  |
| Critical state | `used=91`, `limit=100`, `remaining=9` | Table `Used 91%`; summary `9% remaining` | `9 of 100 left; 91 used` | About 9% filled |  |  |
| Unknown quota state | No trustworthy limit or percent | `Used unknown` or `Quota unknown` | Optional freshness or confidence note | Hidden or disabled |  |  |

## Evidence To Capture

Record enough detail that another engineer can verify the result without repeating the whole session from memory:

- provider
- input values or observed upstream sample
- dashboard table value and summary-card text shown
- whether the progress bar direction matches the contract
- whether `Start With Windows` still appears in application status
- screenshot path or short screenshot description if one was taken
- any mismatch between the visible `Used` or `remaining` wording and the numeric value

## Exit Criteria

Use this validation slice as complete only when:

- both providers pass the table `Used` percentage checks
- both providers pass the summary-card remaining-percentage checks
- both providers pass the progress-bar direction check
- `Start With Windows` is gone from the application-status block
- any remaining mismatch is written down as a concrete follow-up bug rather than a vague note
