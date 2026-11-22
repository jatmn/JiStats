# OpenAI API-platform validation

This optional path requires separate API-platform billing and organization credentials. Keep organization usage and cost provenance under `openai_api_import`. ChatGPT and Codex subscription evidence has its own source labels.

## Probe

The [capture script](../../scripts/capture_openai_api_platform_evidence.ps1) queries organization completions usage and costs using a recent time range. Use the [capture instructions](../fixtures/openai/api-platform/README.md) for credential variables and output paths. Use costs for financial reconciliation and usage for model/request/token activity.

The first live probe on 2026-04-19 reached both endpoints and returned HTTP 403 with `Missing scopes: api.usage.read`. For a successful probe, use an organization credential with the required usage scope, role, and organization context. The [tracker](../progress.md#blocked-backlog) records this dependency as A0.2d.

## Freshness

Once authorized, compare returned bucket timestamps and totals against known recent API activity over repeated polls. Record the delay before usage and costs become visible. Record usage and cost behavior separately. Keep raw captures, organization/project identifiers, and credentials outside the checkout. Use a redacted summary or synthetic fixture when contributing evidence.

See the [organization usage and costs reference](https://platform.openai.com/docs/api-reference/usage/costs) and the [dated provider validation record](../validation/provider-evidence-2026-04-21.md).
