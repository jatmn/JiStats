# MiniMax Reset-Window Evidence Log

> **Status**: Decision-ready for the current v1.0 runtime baseline on accepted `5h`-family parity; future direct Token Plan rollover capture remains optional confidence-refresh work
> **Purpose**: Provide an evidence-bound record of the MiniMax reset-window conclusion and a reusable log shape for any future confidence-refresh captures

## Observed Behavior

- Coding Plan showed a full pre/post rollover for the five-hour model family. The countdown and usage reset with the interval. Longer interval rows remained anchored.
- Token Plan showed the same five-hour family in the accepted comparison between pre-boundary and post-boundary captures.
- Twenty-four-hour and weekly counters coexist with the shorter interval. Keep them separately labeled and model-aware.
- The current runtime behavior follows the five-hour finding. Repeat direct pre/post capture if new evidence disagrees.

Use the [capture inputs](reset-window-capture-inputs.md) and keep account-specific payloads and exact timestamps in local storage.

## Compact Record Template

```text
capture_utc:
surface:
credential_profile:
endpoint_family:
expected_boundary_utc:
relative_time:
http_status:
base_status_code:
base_status_msg:
model_name:
start_time_ms:
end_time_ms:
remains_time_ms:
current_interval_total_count:
current_interval_usage_count:
current_weekly_total_count:
current_weekly_usage_count:
artifact_reference:
notes:
```

## Comparison Questions

For each pre/post sequence, record:

- whether `start_time` stays fixed or jumps
- whether `end_time` stays fixed or jumps
- whether `current_interval_usage_count` resets or continues
- whether `current_interval_total_count` changes unexpectedly
- whether `remains_time` rolls smoothly or jumps to a new window
- whether weekly counters move independently from interval counters

## Exit Condition

The reset-window question is considered settled for the current v1.0 runtime behavior when either:

- both Coding Plan and Token Plan have direct full pre/post sequences, or
- one sibling surface has a direct full pre/post sequence and the other satisfies the accepted archived-pre/live-post comparison path defined in `docs/fixtures/minimax/reset-window-capture-inputs.md`

The validation met that condition:

- Coding Plan has the direct full pre/post rollover proof
- Token Plan satisfies the accepted archived-pre/live-post comparison path
- The [MiniMax provider reference](../../providers/minimax.md) and [provider matrix](../../capability-matrix.md) reflect the observed semantics

## Future Confidence Questions

Remaining questions for a follow-up capture are:

- whether Token Plan shares the same `5h` rollover as Coding Plan across a full pre/post boundary sequence
- whether the `24h` rows are fixed-boundary or rolling
- whether Coding Plan and Token Plan reset at the same wall-clock boundaries
- how quickly the remains payload reflects a reset immediately after the boundary

## Related Files

- `docs/fixtures/minimax/reset-window-capture-inputs.md`
- [MiniMax provider reference](../../providers/minimax.md)
- `docs/capability-matrix.md`
