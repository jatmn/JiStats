# 📊 JiStats

![JiStats logo](assets/logo.png)

JiStats is a lightweight Windows 11 desktop app that mirrors AI provider quota state and usage across OpenAI / Codex, MiniMax, Claude Code, and Kimi Code.

## ✨ At A Glance

| Item | Value |
| --- | --- |
| Status | Active v1.0 implementation with a working Windows desktop baseline |
| Primary Goal | Mirror provider quota state and usage clearly |
| Initial Providers | OpenAI / Codex + MiniMax + Claude Code + Kimi Code |
| Platform | Windows 11 |
| Stack | Native `C++/C` |
| License Model | Mixed: JiStats first-party code + third-party licenses |
| Branding Assets | `assets/logo.png` for repo/docs branding, `assets/icon2.png` as the source artwork for the embedded Windows app icon |
| Release Packaging | Portable Release binaries from canonical `build/` outputs plus an optional side-by-side `JiStats-upx.exe` variant |

## 🧭 Current Status

JiStats is no longer planning-only. The current checkout has a working native Windows desktop baseline with:

- dashboard, tray, and taskbar surfaces
- bundled-SQLite local storage plus schema migrations
- provider-scoped Settings controls for OpenAI, MiniMax, Claude Code, and Kimi Code
- OpenAI / Codex subscription-local quota and activity ingestion
- MiniMax subscription credential storage, validation, and live capture helpers
- Claude Code local-first stats/settings ingestion
- Kimi Code local-first runtime/activity ingestion plus OAuth-backed `/usages` quota mirroring when usable credentials exist
- a lightweight `Analysis` launcher tab that opens a dedicated Analysis workspace window with persisted provider/account/date-range scope, daily history, breakdowns, trend views, and gap/provenance detail
- manual HTML export plus scheduled HTML export with persisted settings, last-run status, and shared analysis scope/date-window behavior
- offline notice retry/dismiss flows wired through the dashboard and tray
- a shipped `File -> Import OpenAI Export...` path for extracted ChatGPT exports
- a provider-tabbed Settings window with `General`, `OpenAI`, `MiniMax`, `Claude Code`, `Kimi Code`, and the deferred `Kilo Code` placeholder tab
- optional OpenAI API-platform readiness/evidence tooling with the first live authorization baseline archived

All four shipped providers are optional and disabled by default until the user enables them in Settings.

The current product path is still subscription-first:

- OpenAI / Codex local and export-import paths are the primary OpenAI v1.0 evidence surfaces
- MiniMax subscription remain tracking is in-product today
- the optional OpenAI API-platform advanced path is partially validated but still blocked on a credential with `api.usage.read` for freshness/lag capture

Current shipped provider posture:

- OpenAI / Codex: subscription-first support with local observation, Codex-local quota pressure, and optional export-import history enrichment
- MiniMax: subscription-backed support for Coding Plan and Token Plan paths
- Claude Code: bounded local-first support from approved stats/settings inputs, shown with honest non-provider-exact confidence labels
- Kimi Code: bounded local-first activity/history support from approved runtime/activity inputs, plus provider-exact quota mirroring through the validated `/usages` path when usable OAuth or refresh tokens exist

Current UI/runtime highlights:

- remaining-first quota summaries across dashboard and taskbar
- persisted activity history, rollups, retention, and recovery gap labeling
- startup recovery and catch-up behavior after downtime
- user-facing provider activation toggles applied before startup refresh work
- exported reports and in-app labels that avoid leaking raw provider-surface ids for Claude/Kimi local evidence

The repo is still in active implementation, and `docs/progress.md` remains the source of truth for what is finished, what is under review, and what is next.

The main planning documents are:

- `AGENTS.md`
- `docs/plan.md`
- `docs/progress.md`

The current workspace branding assets are now wired into the project:

- `assets/icon2.png` is the source artwork used to generate the embedded Windows app icon resource
- `assets/logo.png` is the repository/readme branding image only and is not packaged with the app

## 🚀 Quick Start

For the shipped portable Release-binary workflow, start with [docs/quick-start.md](docs/quick-start.md).

Short version:

- launch `JiStats.exe` for the normal desktop app, or `JiStats.exe --background` for tray-first startup
- if a size-reduced build is shipped, `JiStats-upx.exe` is the optional packed app variant; `JiStatsMiniMaxCli.exe` and `JiStatsTests.exe` are not needed for normal app use
- first run creates the local database under `%LOCALAPPDATA%\\JiStats\\jistats.db`, not next to the executable
- all four providers are opt-in and disabled by default until you enable them in `Settings`

## 🛠️ Build Notes

JiStats now uses a single canonical build tree:

- `build/`

Current repo-backed v1.0 distribution posture is portable Release binaries from `build/bin/Release/`. No installer, shortcut, or uninstaller assets are part of the shipped baseline today.

If you are using the built app rather than producing it, follow `docs/quick-start.md`. The notes below are for local source builds and packaging.

Default Windows IDE/toolchain baseline for this repo is Visual Studio 2026 plus MSVC. When you need an install path rather than the product name, use the major-version `18` root under `C:\Program Files (x86)\Microsoft Visual Studio\18\...`.

Preferred local Release build flow:

```powershell
pwsh -File .\scripts\build_release.ps1
```

That helper always stops any running `JiStats` / `JiStats-upx` / `JiStatsMiniMaxCli` / `JiStatsMiniMaxCli-upx` / `JiStatsTests` processes before invoking the Release build so a forgotten running binary does not leave the output locked and fail the link step. If needed, you can still target a specific Release output:

```powershell
pwsh -File .\scripts\build_release.ps1 -Target JiStats
```

Current Release outputs from that tree are native Windows executables:

- `JiStats.exe`
- `JiStatsMiniMaxCli.exe`
- `JiStatsTests.exe`

The repo also keeps an official UPX tool under `tools/upx/` for optional release-size reduction. Normal Release builds keep the standard binaries and also emit the side-by-side packed app variant:

- `JiStats-upx.exe`

The currently bundled official UPX tool is `tools/upx/upx.exe` version `5.1.1`.

UPX is treated as a packaging tool, not a runtime dependency. If needed, packed variants can still be disabled with `-DJISTATS_ENABLE_UPX=OFF`.

## 📦 Third-Party Components

JiStats currently uses or includes the following third-party components:

| Component | Upstream | Location | Purpose | License |
| --- | --- | --- | --- | --- |
| SQLite amalgamation | https://www.sqlite.org/ | `third_party/sqlite/` | bundled local storage engine | SQLite upstream terms |
| Catch2 amalgamation | https://github.com/catchorg/Catch2 | `third_party/catch2/` | test framework used by `JiStatsTests.exe` | Catch2 upstream terms |
| spdlog | https://github.com/gabime/spdlog | `third_party/spdlog/` | logging support | MIT |
| bundled `fmt` inside spdlog | https://github.com/fmtlib/fmt | `third_party/spdlog/include/spdlog/fmt/bundled/` | formatting support used through spdlog | MIT |
| UPX | https://github.com/upx/upx | `tools/upx/` | optional release-size reduction tool for `-upx` variants | GPL v2 or later with the UPX special exception |

Notes:

- `tools/upx/` is a build/distribution tool path, not a runtime dependency of the app.
- `third_party/imgui/` is still optional and is not treated as an active bundled dependency unless its upstream source and license texts are actually populated in the repo.
- See [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md) for the current notice file and redistribution notes.

## 🤖 Vibecoded By

This project was lovingly vibecoded by a small parade of AI agents:

- GPT-5.4
- GPT-5.3-Codex
- GPT-5.3-Codex-Spark
- GPT-5-nano
- DeepSeek v3.2
- GLM-5
- Kimi K2.6
- MiniMax M2.7
- Claude Sonnet 4.6
- Claude Opus 4.6
- Bonsai Ai (Beta)

## 🔐 License

JiStats' first-party code is source-available under Apache-2.0 with Commons Clause.

Business and internal use are allowed, but selling JiStats itself requires separate permission.

This repository is not distributed under a single license for every file. Bundled third-party components keep their own licenses and notices.

For redistribution or packaging, keep the root `LICENSE`, `THIRD_PARTY_NOTICES.md`, and the upstream third-party license texts together. If shipping UPX-packed binaries, also include the matching upstream UPX `LICENSE` and `COPYING` texts for the exact official UPX release used. The current bundled copies are `tools/upx/LICENSE` and `tools/upx/COPYING` for UPX `5.1.1`.

See `LICENSE` and `THIRD_PARTY_NOTICES.md` for the current license disclosures.
