# MiniMax

MiniMax is opt-in and mirrors two subscription surfaces. In **File → Settings → MiniMax**, select the surface that matches the key and save or validate the credential.

| Surface | Validation and quota endpoint |
| --- | --- |
| Coding Plan | `GET /v1/api/openplatform/coding_plan/remains` |
| Token Plan | `GET /v1/token_plan/remains` |

[MiniMaxAdapter](../../src/providers/minimax/MiniMaxAdapter.cpp) uses the returned subscription fields and window boundaries for quota. Use observed reset evidence when interpreting windows. The [reset-window record](../fixtures/minimax/reset-window-evidence-log.md) distinguishes a full Coding Plan rollover capture from the accepted Token Plan comparison. Keep a profile bound to its selected surface. The displayed count uses the shared `MiniMax-M*` five-hour interval. Mixed payloads can also contain daily or weekly counters and quotas for individual tools. The count and reset date come from the selected row regardless of its position. For Token Plan responses with a percentage-only `general` allowance, JiStats keeps the reported interval and reset. Request counts remain unknown.

## Credential behavior to preserve

- Save secrets in Windows Credential Manager. SQLite keeps profile metadata, validation state, and the secret reference.
- Keep Settings and unrelated app controls responsive while validation runs. Show progress and then the result for the selected surface.
- Reopening Settings after save or validation should show the stored profile and a masked secret field. Revalidation should use the stored credential when the user has entered no replacement.
- Keep missing, saved, validating, validated, failed, and temporarily unavailable states distinct. Wrong-surface and rejected-key results should stay distinguishable from network failures and endpoint unavailability.
- Subscription validation requires a numeric `base_resp.status_code` of zero in the remains response. A nonzero code fails validation even when the HTTP request succeeds. A missing or malformed code leaves validation unavailable.
- Verify adding, replacing, and revalidating a stored credential, including both successful and failed validation and unavailable responses. The [adapter tests](../../tests/test_minimax_adapter.cpp) and [controller tests](../../tests/test_app_controller.cpp) cover the provider and settings paths. Settings currently has no action for removing a stored credential.

## PAYG

A dedicated PAYG key yielded per-request token usage on the text API in the recorded probe. Separate aggregate history and in-app PAYG import/polling remain deferred. Use the [MiniMax fixture inventory](../fixtures/minimax/README.md) for the payload samples and capture procedures.

## Building the MiniMax CLI

For standalone readiness, capture, and diagnostics commands, add `-DJISTATS_BUILD_MINIMAX_CLI=ON` when configuring and include `JiStatsMiniMaxCli` in the `--target` list when building. Follow the [Linux cross-build instructions](../../README.md#building-on-linux) or the [Windows build instructions](../../README.md#building-on-windows).

Keep native capture and diagnostics output outside any JiStats source checkout. Both the desktop capture command and the standalone CLI reject paths inside a source tree, including when the executable was built elsewhere.

Capture and diagnostic exports check that the JSON file closes successfully before reporting a completed write. A write error identifies the output path.

The option defaults to `OFF`. Enabling it builds `JiStatsMiniMaxCli.exe` alongside the desktop app, and `cmake --install` installs it to the install prefix's `bin/` directory. The desktop MiniMax integration is available in either configuration. `JISTATS_BUILD_TESTS` controls the tests independently.
