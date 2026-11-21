# AGENTS.md

This file provides guidance to code agents working with this repository.

## Project State

- JiStats is in early implementation mode, not planning-only.
- Read the root `AGENTS.md` first for product boundaries and working agreement.
- Treat `docs/plan.md` as the roadmap/architecture source of truth.
- Treat `docs/progress.md` as the live execution-state and handoff source of truth.

## Build And Test

Use the repo root as the working directory.

Example local Windows flow:

```bash
cmake -B build -G "Visual Studio 18 2026" -DJISTATS_BUILD_TESTS=ON
cmake --build build --config Debug --target ALL_BUILD
ctest --test-dir build -C Debug --output-on-failure
```

Useful CMake options:

- `JISTATS_BUILD_TESTS`
- `JISTATS_BUILD_EXAMPLES`
- `JISTATS_USE_SYSTEM_SQLITE`
- `JISTATS_USE_SYSTEM_JSON`

The current CMake config targets C++17 and writes binaries under `build/bin/`.

## Product Boundary

- OpenAI / Codex is currently subscription-first; do not code as if OpenAI API-platform billing is automatically available.
- MiniMax is currently subscription-first for v1.0, with PAYG validation kept as follow-on work.
- Quota mirroring is a central feature, so code should preserve provenance and confidence rather than hiding uncertainty.

## Conventions

- Follow the existing `JiStats::` namespace.
- Keep `include/` and `src/` layout aligned.
- Use PascalCase for public C++ headers/sources and snake_case only for clearly internal helpers.
- Keep public interfaces in `.hpp` files and implementation in `.cpp` files.
- Follow existing PIMPL usage for public/runtime-facing classes when touching those areas.
- Respect the current Windows build defines such as `WIN32_LEAN_AND_MEAN` and `NOMINMAX` where Windows headers are involved.

## DB And Migration Rules

- Route DB writes through `StorageService`.
- Keep migrations as numbered SQL files in `db_migrations/`.
- Assume tests run from the repo root; the test target sets `WORKING_DIRECTORY` to `CMAKE_SOURCE_DIR`.

## Logging Rules

- Use the logger macros and `LogCategory` values already established in the repo.
- Never log API keys, subscription secrets, bearer tokens, or raw request/response bodies.
- Use `sanitize::mask(value)` when a sensitive value must be referenced in logs.
- Do not log `UsageResponse::rawResponse` or `QuotaResponse::rawResponse` directly.
- The app log lives under `%LOCALAPPDATA%/JiStats/logs/jistats.log`.

## UI Thread Rule

- No blocking network or DB work on the UI thread.
- UI surfaces should consume derived view models, never raw provider or DB objects.

## Handoff Checklist

- [ ] Update `docs/progress.md` if task state changed
- [ ] Run tests appropriate to the change, or note why not
- [ ] Cover logging and error paths where relevant
- [ ] Avoid introducing UI-thread blocking
- [ ] Update any docs that changed meaningfully with the code
