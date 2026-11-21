# Taskbar Overlay Display Contract

> **Status**: Current support contract for the shipped v1.0 baseline
> **Purpose**: Define the intended taskbar overlay value semantics, text treatment, color behavior, and sizing for the current Windows 11 taskbar surface

## Intent

The taskbar overlay should be:

- compact
- glanceable
- aligned with the dashboard quota contract
- visually native on Windows 11 instead of reading like a floating opaque panel
- explicit enough that compact text does not become guesswork

This contract defines the intended shipped behavior for the current baseline.

## Background And Transparency Contract

The overlay should follow the same general presentation spirit as TrafficMonitor on Windows 11:

- the background should read as transparent or blended into the taskbar
- the text, icon, and emphasis colors should remain legible without relying on a solid opaque panel
- any fallback tint, blur, or compositor limitation should stay visually light enough that the overlay still feels native to the taskbar surface

Avoid:

- a large solid rectangle behind the overlay text
- a dashboard-style panel treatment reused on the taskbar
- transparency so weak that the overlay becomes hard to read against a busy taskbar

## Provider Value Contract

For both OpenAI and MiniMax, the visible quota values shown in the overlay should represent:

- **remaining percentage**

The overlay must not:

- show raw used values when the surrounding UI uses remaining percentage
- use one provider's remaining percentage and another provider's used/pressure percentage

If no trustworthy quota percentage exists, the overlay should prefer:

- a clearly degraded/unknown state

rather than displaying a misleading numeric value.

Current provider-specific rule:

- MiniMax uses one remaining-percentage value in the visible row
- OpenAI may intentionally show two remaining-percentage values when both the current interval and the secondary subscription interval are available

That means a row such as `OpenAI: 99% | 50%` is acceptable when both values are meaningful remaining values rather than mixed used-vs-remaining semantics.

## Text Contract

The overlay should avoid redundant state text.

For the current baseline:

- remove the extra `enabled` text
- keep provider row text focused on provider identity + quota state
- prefer compact text-first rows instead of decorative glyph-heavy rows

Recommended row structures:

- `MiniMax: {remaining_percent}%`
- `OpenAI: {primary_remaining_percent}% | {secondary_remaining_percent}%` when both remaining windows are intentionally shown

Standalone glyphs are not required for the current baseline.

Avoid:

- punctuation-only status markers that require memorization
- decorative glyphs that add width without improving comprehension
- unlabeled extra percentages whose meaning is not documented

## Status Treatment Contract

The current baseline should rely primarily on:

- provider label
- remaining-percentage value(s)
- color/emphasis
- compact text when a degraded runtime/provider state needs to be expressed

The overlay does not need a permanently visible standalone glyph column to satisfy the current contract.

## Color Contract

Overlay percentage values should use these pressure bands based on **low remaining state**:

| Remaining % | Suggested Color | Meaning |
| --- | --- | --- |
| `>= 80%` | `blue` | Plenty remaining |
| `>= 50%` and `< 80%` | `green` | Healthy |
| `>= 25%` and `< 50%` | `yellow` | Watch |
| `> 10%` and `< 25%` | `orange` | Low |
| `<= 10%` | `red` | Critical |

For the current user feedback, the exact band edges may be refined in code, but the rendered meaning should remain stable and documented.

## Low-Quota Emphasis Contract

When the remaining value is `10%` or under:

- the number should be bold
- the `%` should be bold as part of the same emphasized value

This emphasis should apply consistently across providers.

## Width And Padding Contract

The overlay should not reserve unnecessary width.

Sizing rule:

- width should track the longest rendered provider line
- include light right padding sufficient for a `100%`-style value

Avoid:

- dashboard-width layouts
- large fixed padding unrelated to the actual longest line

## Alignment Contract

Overlay text alignment should be checked against a TrafficMonitor-informed placement approach.

Required outcome:

- the transparent background treatment should not shift or blur the text into misalignment
- text should remain visually aligned and readable
- text should not drift into the clock/tray region
- compact width should not break right-edge alignment or clipping behavior

## Acceptance Questions

Before the baseline overlay wording/docs pass is considered complete, confirm:

- do both providers show remaining-percentage semantics, not mixed used-vs-remaining semantics?
- if OpenAI shows two values, are both documented remaining values?
- does the overlay background read as transparent/blended instead of as an opaque panel?
- is the redundant `enabled` text gone?
- is the text-first row treatment understandable without retired punctuation shorthand?
- if warning or critical scenarios are exercised, do colors match the intended pressure bands?
- if a `<= 10%` scenario is exercised, is it visibly emphasized?
- is width driven by the longest line instead of a needlessly wide container?

## Related Files

- `docs/dashboard-quota-display-contract.md`
- `docs/taskbar-manual-validation.md`
- `docs/taskbar-metrics.md`
- `docs/progress.md`
