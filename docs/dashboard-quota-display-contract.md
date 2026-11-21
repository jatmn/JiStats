# Dashboard Quota Display Contract

> **Status**: Lane 2 support contract for the shipped dashboard quota baseline
> **Purpose**: Define the intended user-facing dashboard quota presentation for OpenAI and MiniMax without treating the latest shell fix pass as perpetually unfinished

## Intent

The dashboard should present provider quota state in a way that is:

- consistent across OpenAI and MiniMax
- glanceable without hiding meaning
- explicit about whether each displayed percentage is `used` or `remaining`
- aligned with the taskbar overlay contract
- free of low-value application-status clutter

This contract defines the current shipped target presentation and the baseline future changes must preserve.

## Primary User-Facing Rule

For the current v1.0 shell:

- the provider quota table `Used` field should show an explicit **percentage used**
- the dashboard usage-summary cards should stay explicitly **remaining-first**
- raw counts may still appear, but only as supporting detail
- numeric labels should not force the user to guess whether a number is `used` or `remaining`

## Required Display Semantics

For each provider quota row or summary:

- `used_percent`: should be the primary short-form value shown for the provider quota table `Used` field
- `used_value`: may still appear as supporting detail
- `limit_value`: may still appear as supporting detail
- `remaining_value`: may appear as supporting detail when useful
- `remaining_percent`: should be the primary short-form value shown in the usage-summary cards and their progress bars

Required copy direction:

- acceptable table value: `Used 74%`
- acceptable table value: `74% used`
- acceptable summary value: `26% remaining`
- acceptable summary value: `Remaining 26%`
- acceptable secondary detail: `74 of 100 used`
- avoid: `74` with no unit or meaning
- avoid: a `Used` label next to a raw count with no percentage context

## Summary Row Contract

The dashboard usage-summary area should show:

- provider + plan label
- one short numeric detail line that is internally consistent
- one progress bar using the same semantics for both OpenAI and MiniMax

Recommended default:

- headline label: `{Provider} - {Plan}`
- short detail: `{remaining_percent}% remaining`
- optional secondary detail: `{remaining_value} of {limit_value} left; {used_value} used`

If a supporting numeric detail is shown, it must preserve the distinction between:

- percent used in the table
- amount used
- total limit
- remaining percentage in the summary cards

## Progress-Bar Direction Contract

The dashboard must not invert provider behavior or make the bar disagree with the visible quota wording.

The dashboard currently uses split semantics by surface:

- the provider quota table `Used` value should represent `% used`
- the usage-summary card detail should represent `% remaining`
- the progress bar should fill further to the right as remaining quota increases
- OpenAI and MiniMax should follow the same user-facing rule on each surface even if their upstream source payloads require different normalization logic

The provider-specific inversion risk is an implementation concern, not a reason to ship mixed UI semantics.

## Provider Quota Table Contract

The provider quota list should keep explicit headers such as:

- `Provider`
- `State`
- `Plan`
- `Window`
- `Used`
- `Limit`
- `Remaining`
- `Confidence`
- `Last Sync`

The list may keep numeric `Remaining` values in the table view, but the surrounding summary/progress treatment should still make `Used` percentage easy to understand.

## Application Status Contract

The dashboard application-status block should show live runtime status, not settings clutter.

For the current shell:

- remove `Start With Windows` from the application-status area
- keep startup preference controls in settings/preferences where they belong
- only show app-status fields that help explain live runtime state

## Provider Consistency Rules

OpenAI and MiniMax should follow the same top-level dashboard rules:

- same `Used` percentage semantics in the provider quota table
- same remaining-first summary-card and progress-bar semantics
- same distinction between used values, remaining values, and raw counts

Provider-specific quota dimensions may differ, but the display semantics should not.

## Acceptance Questions

Use these acceptance questions whenever the dashboard quota presentation changes:

- does the provider quota table `Used` field show an explicit percentage instead of an unlabeled raw count?
- do the summary cards say `remaining` only when the number is actually remaining?
- are raw counts reduced to clearly labeled supporting detail?
- do OpenAI and MiniMax summary bars use the same remaining-first meaning?
- do table headers remain readable and explicit?
- is `Start With Windows` gone from the application-status area?

## Related Files

- `docs/taskbar-overlay-display-contract.md`
- `docs/taskbar-manual-validation.md`
- `docs/progress.md`
