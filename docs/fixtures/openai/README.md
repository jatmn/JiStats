# OpenAI Fixture Inventory

These fixtures support the current subscription-first OpenAI baseline while keeping the optional API-platform path separate.

For JiStats v1.0, OpenAI is intentionally split into:

- subscription-backed ChatGPT / Codex tracking
- optional OpenAI API-platform import work that depends on separate billing and higher-scope credentials

## Captured Artifacts

| Path | Type | What It Supports | Notes |
| --- | --- | --- | --- |
| [Subscription surface notes](subscription-surface-notes.md) | Evidence note | Inventory of official OpenAI subscription-visible surfaces and the no-scraping conclusion | Records account/export/Codex usage surfaces and the current subscription-first baseline |
| `docs/fixtures/openai/codex-rate-limits-shape.sample.json` | Public doc-derived sample | Reference shape for Codex-linked rate-limit parsing | Not a live user capture |
| `docs/fixtures/openai/codex-rollout-rate-limits.redacted.jsonl` | Synthetic local rollout example | Real-shape parser coverage for Codex-local `token_count` / `rate_limits` ingestion | Conversation and session content removed; preserves only the fields needed for parser/runtime validation |
| [Codex consumer artifact redaction note](codex-consumer-artifact-redaction-note.md) | Redaction contract | Commit-safe rules for any future Codex consumer artifact archive | Defines what a safe artifact may preserve, redact, and claim |
| `docs/fixtures/openai/chatgpt-export-redacted/` | Redacted extracted-export fixture directory | Historical ChatGPT export import parser/storage coverage | Includes `conversations.json` plus the extracted-export marker siblings used for provenance checks |
| `docs/fixtures/openai/chatgpt-export-derived-fixture-note.md` | Provenance/redaction note | Commit-safe rules for the extracted-export fixture package | Keeps historical-only labeling and redaction boundaries explicit |
| [Subscription augmentation fixture guide](subscription-augmentation-fixture-guide.md) | Coverage checklist | Fixture/test expectations for Codex-local parsing and ChatGPT export import | Keeps parser and reporting expectations pinned to the current local-first OpenAI path |
| [OpenAI API-platform](api-platform/README.md) | Validation instructions | Optional API-platform authz/readiness | The live probe returned missing usage scope |
| `docs/fixtures/openai/sample_openai_fixture.json` | Synthetic parser scaffold | `OpenAIAdapter::parseFixtureFile()` coverage and generic usage-event mapping | Test scaffold only; not evidence of a validated live OpenAI endpoint |

## What This Inventory Confirms

- JiStats has a real Codex-local parsing path backed by a synthetic rollout example.
- The preferred OpenAI subscription augmentation path is still local rollout parsing first, not scraping or SQLite-first coupling.
- The repo has a first redacted extracted-export fixture package for historical ChatGPT import coverage.
- Export-import inputs are intentionally scoped to extracted official export contents, not arbitrary browser captures or raw `.zip` guessing.
- The API-platform instructions summarize the earlier authorization result. Raw captures stay local.
- The current API-platform blocker is concrete: the tested key reached both organization endpoints but lacked `api.usage.read`.
- The repo has durable notes that keep `openai_subscription_local`, `openai_subscription_codex_local`, `openai_subscription_export_import`, and `openai_api_import` separate.

## What This Inventory Does Not Confirm

- Any general ChatGPT consumer quota-remaining API
- Any authoritative cross-ChatGPT subscription quota total for consumer users
- Any successful OpenAI API-platform freshness/lag capture yet
- Any raw live Codex consumer dashboard artifact committed to the repo
- `.zip` archive handling for ChatGPT export import

## Related References

- [Provider support](../../capability-matrix.md)
- [OpenAI and Codex reference](../../providers/openai.md)
- [API-platform validation](../../providers/openai-api-platform.md)
- Parser coverage: `tests/openai_fixture_tests.cpp`
