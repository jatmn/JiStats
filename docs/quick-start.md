# Portable Release Quick Start

> **Status**: Current release-facing quick-start guide for the shipped portable binary workflow
> **Purpose**: Explain how to use the portable Release build honestly without implying an installer or fully self-contained in-place storage

## What This Covers

This guide is for the current JiStats v1.0 distribution posture:

- no installer
- no shortcut or uninstaller workflow
- portable Windows executables
- local app data still stored under `%LOCALAPPDATA%`

If you are building from source inside this repo, use `pwsh -File .\scripts\build_release.ps1` first and then come back to this guide. The current repo-local Release output folder is `build/bin/Release/`.

## 1. Pick The App Binary

For normal app use, start one of these:

- `JiStats.exe` for the standard Release build
- `JiStats-upx.exe` only if you specifically want the smaller packed variant

Do not launch both at the same time as your primary app instance.

You do not need these for normal desktop use:

- `JiStatsMiniMaxCli.exe`
- `JiStatsTests.exe`

## 2. Put The Portable Files Where You Want Them

Because JiStats is currently shipped as a portable binary, you can keep the Release folder wherever you want to run it from.

Important limitation:

- "portable" here means no installer is required
- it does **not** mean JiStats keeps its database next to the executable

## 3. Launch JiStats

Normal desktop launch:

```powershell
.\JiStats.exe
```

Background / tray-first launch:

```powershell
.\JiStats.exe --background
```

If you are using the packed variant instead, substitute `JiStats-upx.exe` in the same commands.

## 4. Know What Happens On First Run

On a clean first run, JiStats creates its local app folder and SQLite database under:

```text
%LOCALAPPDATA%\JiStats\jistats.db
```

That first-run path is the current verified release baseline. Moving the executable later does not move the existing database automatically because the default storage location is `LOCALAPPDATA`.

## 5. Enable Only The Providers You Actually Use

All four shipped providers start disabled by default on a fresh install.

Open `Settings` and enable only the integrations you want:

- `OpenAI / Codex`: local subscription-first tracking, plus optional `File -> Import OpenAI Export...` for extracted ChatGPT export history
- `MiniMax`: subscription-backed Coding Plan / Token Plan support through the Settings credential flow
- `Claude Code`: bounded local-first activity/runtime support from approved local artifacts
- `Kimi Code`: bounded local-first activity/runtime support, plus provider-exact quota mirroring only when usable OAuth credentials exist in the provider-owned Kimi credential file

Honest current limits:

- there is no installer-based setup flow today
- Claude Code is still local-first rather than provider-exact quota parity
- Kimi quota stays empty when usable OAuth is not available; JiStats does not fabricate a local quota value there

## 6. Use The Main Release Features

After launch and provider enablement, the current shipped baseline includes:

- dashboard, tray, and taskbar quota/status surfaces
- the dedicated `Analysis` workspace launched from the dashboard
- `File -> Export HTML Report...`
- optional `Start with Windows`, which uses the app's `--background` startup path

## 7. If You Are Redistributing The Portable Build

Keep the release-facing license files with the app:

- `LICENSE`
- `THIRD_PARTY_NOTICES.md`

If you ship `JiStats-upx.exe`, also keep the matching bundled UPX license texts:

- `tools/upx/LICENSE`
- `tools/upx/COPYING`

## Related Docs

- `README.md`
- `docs/release-checklist.md`
- `docs/startup-behavior.md`
