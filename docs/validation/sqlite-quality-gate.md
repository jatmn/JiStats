# SQLite-Backed Storage Quality Gate

## Intent

JiStats storage and migration tests are written to use real SQLite when it is available.

The [shared test helper](../../tests/sqlite_test_support.hpp) marks SQLite-dependent cases as skipped when SQLite support is absent. Confirm the relevant cases ran against an opened database when validating storage changes.

## Minimum Gate

Before closing a storage or schema checkpoint that touches migrations, `SchemaManager`, `StorageService`, or SQLite-backed persistence behavior, run at least one real SQLite-enabled build/test path.

A valid gate requires all of these:

- tests are built with `JISTATS_BUILD_TESTS=ON`
- SQLite is actually enabled through either the bundled SQLite target defining `JISTATS_HAS_SQLITE` or `JISTATS_USE_SYSTEM_SQLITE=ON` with system SQLite
- the selected tests execute against a real opened SQLite database instead of taking the `SQLite not available` early-return path

## Minimum Required Test Coverage

Always run these in at least one SQLite-enabled path:

- `tests/test_schema_manager.cpp`
  - `SchemaManager migrates storage baseline tables and quota metadata when SQLite is available`
  - `StorageService preserves database settings on reopen without rewriting it`
  - `StorageService rejects databases older than the baseline`
  - `StorageService upgrades legacy Claude references without reading the source cache`
- `tests/test_storage_mirror_state.cpp`
  - `StorageService upserts and queries sync state when SQLite is available`
  - `StorageService upserts and queries mirror rules when SQLite is available`
- `tests/test_storage_activity_events.cpp`
  - `StorageService persists and queries activity events when SQLite is available`

Run these as well when the touched change reaches into the same behavior:

- `tests/test_observation_persistence_wiring.cpp`
- `tests/test_app_observation_persistence.cpp`
- `tests/test_openai_export_import.cpp`
- `tests/test_app_controller.cpp` for settings or credential persistence changes

## Evidence To Record

Capture enough evidence that the next engineer can tell the run used real SQLite:

- the configure/build path used
- the flag proving SQLite support was enabled
- the exact test command or `ctest` filter used
- whether the run passed or failed
- any relevant failure text if the gate did not pass

Good evidence examples:

- a configure command showing `-DJISTATS_USE_SYSTEM_SQLITE=ON`
- a compile command containing `-DJISTATS_HAS_SQLITE=1` for a bundled SQLite build
- `ctest` output for the targeted storage/schema tests

Bad evidence examples:

- `ctest` passed, without proving SQLite was enabled
- a default build where storage tests quietly took the `SQLite not available` branch

## Linux validation

The [portable test build](testing.md#linux-commands) enables bundled SQLite and runs the required schema, storage and rollup cases directly on Linux. The suite also covers both historical version 12 layouts and overlapping opens during repair. For build-affecting PRs, CI runs the remaining controller and provider integration cases on Windows using the cross-built executable.

## Windows validation

Follow the [Windows verification commands](testing.md#windows-commands) locally, or dispatch [CI](../../.github/workflows/ci.yml) to build on Linux and run the full test executable on Windows. Both use bundled SQLite. An initialized Windows developer shell can also run [the SQLite quality gate script](../../scripts/run_sqlite_quality_gate.ps1), which configures a bundled-SQLite test build and runs CTest. Record the commands and outcome in the handoff.

## Exit Rule

Do not mark a storage/schema checkpoint complete if the only available evidence is a non-SQLite run.

If SQLite cannot be enabled in the current environment, record that explicitly as a quality gap or blocker instead of treating the storage path as fully verified.
