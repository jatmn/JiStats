# OpenAI subscription surface evidence

The April 2026 proof path identified documented consumer account and export surfaces, a Codex Usage Dashboard, and Codex app-server rate-limit fields. The [public shape fixture](codex-rate-limits-shape.sample.json) records the Codex fields. The [synthetic rollout](codex-rollout-rate-limits.redacted.jsonl) exercises the local representation. These fixtures do not contain live account responses.

No general consumer quota API was validated in that work. Keep automated collection on approved local files and user-initiated export paths. Avoid website scraping, undocumented consumer endpoints, and intercepted auth traffic.

Recursive rollout discovery was selected for quota. The examined Codex SQLite thread metadata did not supply rate-limit fields. JiStats now uses that SQLite source separately for sanitized model attribution. See the [OpenAI provider reference](../../providers/openai.md) for the current paths.

A redacted Codex Usage Dashboard capture remains optional. Follow [consumer artifact redaction](codex-consumer-artifact-redaction-note.md) if adding one. The [original evidence and references](https://github.com/jatmn/JiStats-public-prep/blob/fd934179646f130fe346a20fb1ca7d82162902f6/docs/fixtures/openai/subscription-surface-notes.md) preserve the research record.
