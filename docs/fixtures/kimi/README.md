# Kimi Fixture Pack

This folder contains a synthetic Kimi local-runtime fixture pack for `P7.8`.

It follows the Phase 7 boundary in:

- [Kimi Code provider reference](../../providers/claude-kimi.md#kimi-code)
- [Local redaction contract](../../providers/local-redaction.md#kimi-appendix)

What is intentionally included:

- `config.toml` with provider/model defaults only
- `kimi.json` with synthetic working-directory linkage
- `sessions/*/state.json`
- `sessions/*/tasks/*/runtime.json`
- `sessions/*/notifications/*/{event,delivery}.json`

What is intentionally excluded:

- `context*.jsonl`
- `wire.jsonl`
- `credentials/`
- `logs/`
- `plans/`
- transcript content
- raw credential material
- raw local paths in any persisted parser output

Redaction/provenance note:

- all values are synthetic
- working directories are fake
- timestamps and runtime states are shaped to exercise the bounded parser only
- the parser is expected to hash or normalize local path scope before surfacing it
