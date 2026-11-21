# MiniMax Credential Entry Acceptance Matrix

> **Status**: Durable Lane 2 support contract
> **Purpose**: Turn the MiniMax credential UX contract into a concrete acceptance matrix for the shipped settings/accounts flow and its current UX follow-up work

## Scope

This matrix covers the first in-app entry flow for MiniMax subscription credentials:

- add a credential
- edit or replace a credential
- revalidate a stored credential
- remove a credential

It applies only to:

- `Coding Plan`
- `Token Plan`

It does not cover:

- PAYG API keys
- automatic quota-semantic validation beyond endpoint-specific credential validation
- richer account management such as multi-user sharing or bulk credential import

## Shared Expectations

- Secrets must be stored in Windows Credential Manager, not SQLite or plain-text settings.
- Local persistence should keep only profile metadata, validation state, and secret references.
- The UI must always show which MiniMax subscription surface a credential belongs to.
- Validation must not freeze the whole application while the network check runs.
- A stored secret should remain visually represented after save or validate success with a masked password-style placeholder, not an empty-looking field.
- Reopening Settings / Accounts after a successful save or validation should continue to show the selected surface, stored-label metadata, and the masked-secret placeholder for the stored credential.
- Validation must stay endpoint-family-specific:
  - Coding Plan: `GET /v1/api/openplatform/coding_plan/remains`
  - Token Plan: `GET /v1/token_plan/remains`

## Required Visible States

| State | Meaning | Minimum UI Expectation |
| --- | --- | --- |
| `Missing` | No credential stored for that surface | Show add action and no validated provider access claim |
| `Present` | Credential metadata exists but current validation result is not yet known | Show stored surface plus a validate/retry action |
| `Validating` | Validation is currently in flight | Keep the window responsive, show in-progress feedback, and avoid blocking unrelated app interaction |
| `Validated` | Credential was accepted by the correct MiniMax subscription endpoint | Show surface, successful state, and last checked time when available |
| `Validation Failed` | MiniMax rejected the credential or it did not match the selected surface | Keep the credential editable; do not imply provider outage |
| `Unavailable / Retry Needed` | Validation could not complete because MiniMax or the network was unavailable | Keep the credential stored and retryable; do not imply credential rejection |

## Interaction Expectations During Validation

- Pressing `Validate` must move the credential row into a visible in-progress state instead of stalling the entire UI thread.
- The settings window may disable only the credential action currently in flight, but the rest of the app should remain responsive.
- Completion should marshal back into the normal credential state model (`Validated`, `Validation Failed`, or `Unavailable / Retry Needed`) without clearing the stored-secret presentation.
- A successful validation may rotate or overwrite the stored secret reference internally, but the user-facing textbox should still render a masked stored-secret placeholder after the round-trip completes.

## Acceptance Matrix

| Flow | Preconditions | Expected Stored Outcome | Expected Visible State | Expected User-Facing Result |
| --- | --- | --- | --- | --- |
| Add Coding Plan credential, validation succeeds | No Coding Plan credential present | New Coding Plan profile metadata plus Windows Credential Manager secret reference | `Validated` | `MiniMax Coding Plan credential saved` |
| Add Token Plan credential, validation succeeds | No Token Plan credential present | New Token Plan profile metadata plus Windows Credential Manager secret reference | `Validated` | `MiniMax Token Plan credential saved` |
| Add credential but skip immediate validation | No credential present | Profile metadata plus secret reference stored | `Present` | `MiniMax subscription credential saved` |
| Add credential, MiniMax rejects it | No credential present | Profile metadata plus secret reference may remain if save-first flow is used; validation result must persist as failed | `Validation Failed` | `MiniMax subscription key rejected` |
| Add credential for wrong surface | No credential present | Same storage rule as above, but failed state must preserve selected surface | `Validation Failed` | `This credential does not match the selected MiniMax subscription surface` |
| Add credential, network unreachable | No credential present | Profile metadata plus secret reference may remain, with unavailable validation result | `Unavailable / Retry Needed` | `Could not reach MiniMax to validate this credential` |
| Add credential, provider endpoint unavailable | No credential present | Profile metadata plus secret reference may remain, with unavailable validation result | `Unavailable / Retry Needed` | `MiniMax validation endpoint is temporarily unavailable` |
| Replace existing credential and validation succeeds | Existing credential present for same surface | Secret reference updated or replaced; metadata preserved for that surface | `Validated` | `MiniMax subscription credential updated` |
| Replace existing credential and validation fails | Existing credential present for same surface | New validation result persisted without losing surface binding | `Validation Failed` or `Unavailable / Retry Needed` | Failure wording must match the real category |
| Revalidate stored credential, success | Existing credential present | No secret change required; last validation result refreshed | `Validated` | `MiniMax subscription credential validated` |
| Revalidate stored credential, provider/network unavailable | Existing credential present | Stored credential remains intact; unavailable result refreshed | `Unavailable / Retry Needed` | `MiniMax validation endpoint is temporarily unavailable` or `Could not reach MiniMax to validate this credential` |
| Validate credential while other app surfaces remain open | Existing credential present and user presses `Validate` | Validation work runs without blocking unrelated app state | `Validating` then terminal result | The settings surface stays responsive and the app does not appear frozen during the network round-trip |
| Successful save or validation with stored secret rehydrated into the UI | Existing or newly stored credential present | Stored metadata and secret reference remain intact | `Present` or `Validated` with masked secret shown | The secret field keeps a masked stored-secret placeholder instead of looking blank |
| Remove stored credential | Existing credential present | Local metadata removed and referenced secret deleted from Windows Credential Manager | `Missing` | `MiniMax subscription credential removed` |

## Error Category Mapping

| Category | Result State | Required Wording |
| --- | --- | --- |
| Invalid credential | `Validation Failed` | `MiniMax subscription key rejected` |
| Wrong surface / credential mismatch | `Validation Failed` | `This credential does not match the selected MiniMax subscription surface` |
| Network failure | `Unavailable / Retry Needed` | `Could not reach MiniMax to validate this credential` |
| Provider endpoint temporarily unavailable | `Unavailable / Retry Needed` | `MiniMax validation endpoint is temporarily unavailable` |

## Non-Claims

The first settings/accounts pass must not imply:

- that a validated credential proves all MiniMax quota-window semantics are fully settled
- that Coding Plan and Token Plan are interchangeable
- that missing or unavailable validation means the provider definitely rejected the credential
- that a blank-looking secret field after success means the credential was discarded
- that a long-running validation check is acceptable simply because the final state eventually updates

## Related Files

- `docs/minimax-subscription-credential-ux.md`
- `docs/minimax-account-model.md`
- `docs/fixtures/minimax/reset-window-capture-inputs.md`
