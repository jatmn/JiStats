# MiniMax Subscription Credential UX Contract

> **Status**: Lane 2 support contract for the current settings follow-up
> **Purpose**: Define the shipped MiniMax credential-entry baseline plus the remaining expected validation UX behavior for MiniMax subscription support

## Intent

JiStats needs a real app path for MiniMax subscription credentials before the remaining v1.0 MiniMax validation work can be completed cleanly.

This contract defines how that path should look from the user's point of view.

## Current Shipped Baseline

The first Settings / Accounts credential flow already exists in the app:

- `Coding Plan` and `Token Plan` can be entered from the dashboard settings shell
- secrets are stored in Windows Credential Manager
- SQLite stores only credential metadata, secret references, and last validation timestamps
- endpoint-specific validation maps invalid, wrong-surface, and unreachable cases into distinct user-visible states

The remaining settings follow-up is specifically about interaction quality, not inventing a new credential model:

- validation must stop blocking the whole application while the network call runs
- the secret field must stop clearing to a blank edit box after successful validation

## First Supported Scope

The first credential-management slice should support:

- MiniMax Coding Plan subscription key
- MiniMax Token Plan subscription key

It should not require PAYG support before the subscription-first v1.0 work can proceed.

## Entry Surface

The first UI entry should live under the app settings/accounts path, not as an invisible implementation detail.

Recommended first-pass wording:

- `Add MiniMax Subscription`

The flow should ask the user which subscription surface they are adding:

- `Coding Plan`
- `Token Plan`

## Required User Inputs

For the first pass:

- provider: `MiniMax`
- subscription surface: `Coding Plan` or `Token Plan`
- subscription key
- optional friendly label / account name if the user wants one

## Validation Behavior

### Coding Plan

Validate against:

- `GET /v1/api/openplatform/coding_plan/remains`

### Token Plan

Validate against:

- `GET /v1/token_plan/remains`

## Validation Interaction Contract

The `Validate` action is part of the settings flow, so it must behave like a background check rather than a modal app-wide stall.

Required behavior:

- start the MiniMax validation request off the UI thread
- keep the rest of the application responsive while validation is in progress
- allow the settings window to repaint and remain interactive during validation
- update visible status/feedback only after marshaling the completion result back to the window thread
- keep the selected MiniMax surface and account label intact while validation runs

Acceptable temporary UI states while validation is running:

- disable only the credential-specific `Save` / `Validate` controls if needed
- show a short in-progress message such as `Validating...`
- keep the rest of the dashboard, tray, and app shell usable

Non-goal:

- do not freeze the whole application until the network call returns

## Required User-Visible States

Each MiniMax subscription credential should expose a simple status:

- `Missing`
- `Present`
- `Validated`
- `Validation Failed`
- `Unavailable / Retry Needed`

## Required Error Categories

### Invalid Credential

Show:

- `MiniMax subscription key rejected`

### Wrong Surface / Credential Mismatch

Show:

- `This credential does not match the selected MiniMax subscription surface`

Examples:

- Coding Plan key entered for Token Plan validation
- non-subscription credential entered for subscription validation

### Network Failure

Show:

- `Could not reach MiniMax to validate this credential`

This should not be phrased as a credential rejection.

### Provider Endpoint Temporarily Unavailable

Show:

- `MiniMax validation endpoint is temporarily unavailable`

This should remain distinct from both invalid credential and network-offline cases.

## Success Behavior

On success:

- store the secret in Windows Credential Manager
- persist only the metadata/reference locally
- show the credential as `Validated`
- show which surface it is tied to

## Secret Field Retention Rules

After a successful save or validation round-trip, the secret edit control must continue to look like a stored password field instead of an empty text box.

Required behavior:

- once a secret has been stored, the UI may replace the live input with a masked placeholder state
- the visible field should continue to show password-style masking rather than a blank value
- re-opening Settings / Accounts for a stored credential should still present a masked field, not the raw secret
- revalidating a stored credential must not require the user to retype the secret just because the prior validation succeeded

Not allowed:

- clearing the secret field to a visibly empty control immediately after success
- revealing the raw secret in the UI after hydration or validation
- treating a masked stored-secret presentation as if no credential exists

## Settings / Accounts Display Rules

The settings/accounts UI should show enough information to answer:

- Is there a MiniMax subscription credential?
- Which surface does it belong to?
- Has it validated successfully?
- Is it currently failing or unavailable?

Minimum displayed metadata:

- provider name
- surface name
- validation status
- last validation result or last checked time when available
- whether validation is in progress when a background check is currently running

## Explicit Non-Claims

The UI must not imply:

- that MiniMax subscription validation is complete before the user provides a key
- that Coding Plan and Token Plan are interchangeable
- that a stored credential automatically proves full quota semantics are already validated

## Implementation Handoff

Lane 1 / implementation should use this contract when building the first credential slice:

- settings/accounts entry flow
- endpoint-specific validation
- non-blocking validation completion
- masked secret retention after successful validation
- clear wrong-surface messaging
- persisted validation state
- Windows Credential Manager storage with local metadata only
