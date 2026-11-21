# MiniMax Fixtures

These examples show the MiniMax subscription response format. Raw provider captures stay in local storage.

| File | Purpose |
| --- | --- |
| [Coding Plan example](coding-plan-quota.json) | Synthetic remains payload with model-level interval and weekly counters |
| [Token Plan example](token-plan-balance.json) | Synthetic remains payload for the sibling subscription endpoint |
| [API validation summary](api-validation-2026-04-17.md) | Endpoint and response-shape findings |
| [Reset-window capture inputs](reset-window-capture-inputs.md) | Method for checking both subscription surfaces |
| [Reset-window findings](reset-window-evidence-log.md) | Observed reset behavior and a template for recording later checks |
| [PAYG probe](payg/README.md) | How to capture dedicated-key per-request usage evidence locally |

The subscription examples preserve mixed interval families and companion weekly counters. Per-request usage on the dedicated PAYG path has been observed, and aggregate PAYG usage history remains unvalidated.
