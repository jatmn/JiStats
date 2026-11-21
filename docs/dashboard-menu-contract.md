# Dashboard Menu Contract

> **Status**: Lane 2 support contract
> **Purpose**: Define the first required desktop menu structure and common action behavior for JiStats v1.0
> **Related Specs**: `docs/dashboard-quota-display-contract.md`, `docs/dashboard-quota-manual-validation.md`

## Intent

JiStats should behave like a normal Windows desktop utility instead of relying on an inline `Preferences...` button for core app actions.

The v1.0 dashboard shell should therefore ship with a real menu bar and standard desktop actions.

This document only defines shell/menu behavior.
Quota cards, percentage wording, and progress-bar semantics are intentionally defined in the separate dashboard quota display contract so the menu shell and the quota presentation can evolve independently.

## First Shipped Menu Structure

Top-level menus for the first pass:

- `File`
- `Help`

The first pass should not add extra top-level menus until the app has enough real actions to justify them.

Menus that do **not** need to exist yet:

- `View`
- `Tools`
- `Window`
- `Accounts`

## Required File Menu Actions

### `File -> Settings`

Behavior:

- opens the existing settings/preferences surface
- if the settings window is already open, brings it to the foreground instead of opening a duplicate
- replaces the current inline dashboard `Preferences...` button as the primary shell entry path

Labeling rule:

- use `Settings`
- avoid mixed naming such as `Preferences...` in one place and `Settings` in another

### `File -> Import OpenAI Export`

Behavior:

- opens a file picker for an extracted ChatGPT export `conversations.json`
- routes through the app-owned historical import path rather than a test-only seam
- persists imported historical activity under the shipped OpenAI export-import surface
- reports completion or failure back through the dashboard shell status text

Scope rule:

- this first shipped action is for extracted export inputs only
- `.zip` archive handling remains intentionally deferred from the initial runtime affordance

### `File -> Export HTML Report`

Behavior:

- opens a save dialog for the user to choose an output path
- generates an HTML report using the same `ReportingService` pipeline as scheduled export
- uses the current dashboard provider/account selection for scope
- runs export on a worker thread to avoid blocking the UI
- reports success (with output path) or failure through the dashboard shell status text

Default behavior:

- suggested filename: `jistats-report.html`
- default extension: `.html`
- overwrite prompt if the file already exists

### `File -> Exit`

Behavior:

- requests a real application shutdown
- routes through the same app-owned shutdown path as tray exit
- does not behave like `Hide`, `Close Window`, or `Minimize to Tray`

User expectation:

- if a user selects `Exit`, the app should stop running rather than merely hiding the dashboard window

## Required Help Menu Actions

### `Help -> Help`

Behavior:

- opens a simple local help surface
- the first pass may use a lightweight dialog or window rather than a full documentation browser
- explains the current product at a practical level:
  - what JiStats tracks
  - what the current provider scope is
  - where to find planning/progress docs locally

This first pass should remain local-first:

- do not make it depend on web access
- do not imply online documentation is required for basic use

### `Help -> About`

Behavior:

- opens a dedicated About dialog
- presents product identity, version/status, provider scope, stack, license pointers, and local docs pointers
- stays accurate to the current repo state and conservative about unfinished features

## First-Pass Interaction Rules

- Menu actions must work even if the dashboard content is stale.
- `Settings` and `About` should be single-instance surfaces.
- `Help` and `About` should have the dashboard as their owner window.
- Closing `Settings`, `Help`, or `About` should never shut down the whole app.
- `Exit` should always use the app shutdown path, not a window-only close path.

## Content / Wording Rules

- Use `Settings`, not `Preferences`, as the primary menu label.
- Use `Help`, not `Documentation`, for the top-level menu name.
- Use `About JiStats` as the About dialog title.
- Keep wording aligned with `README.md`, `docs/plan.md`, and `docs/progress.md`.

Avoid:

- claiming the product is fully complete or stable
- implying broad provider parity that does not exist
- mixing temporary validation wording into long-term menu labels

## Non-Goals For This Menu Slice

The first menu slice does not need to include:

- recent files
- account management
- credential editing
- advanced diagnostics
- developer tools

Those can be added later when they represent real user-facing workflows.

## Implementation Handoff

Lane 1 / implementation should treat this as the minimum expected UI behavior:

- add a real menu bar to the dashboard window
- route `File -> Settings` to the current settings/preferences surface
- route `File -> Import OpenAI Export` to the shipped historical import path
- route `File -> Exit` through `AppController::requestShutdown()`
- add lightweight local `Help` and `About` surfaces
- remove the old inline `Preferences...` button once menu routing is in place
