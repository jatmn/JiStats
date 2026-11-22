# Portable quick start

JiStats runs on Windows 11 from a portable folder. If you are building from source, see [Build and packaging](../README.md#build-and-packaging).

## Launch

Use one desktop binary, either `JiStats.exe` or the optional packed `JiStats-upx.exe`. The executable contains the SQLite baseline and can initialize it from any working directory.

```powershell
.\JiStats.exe
# Or launch in the background with the tray icon.
.\JiStats.exe --background
```

JiStats stores the database in `%LOCALAPPDATA%\JiStats\jistats.db`. Moving the executable does not move that data. **Start with Windows** can launch JiStats at sign-in with the same embedded baseline. See [supported database versions](reference/storage.md#migration-behavior) before upgrading a development build.

## Enable providers

Open **File → Settings**. All integrations begin disabled. The [provider matrix](capability-matrix.md) explains setup and available data for OpenAI / Codex, MiniMax, Claude Code, and Kimi Code.

## View and export history

Open the dedicated workspace from the dashboard's **Analysis** tab to choose a provider, account, and date range. **File → Export HTML Report…** uses that scope. Set up recurring export in Settings.

For older ChatGPT activity, extract an official export and select its `conversations.json` with **File → Import OpenAI Export…**. Keep the companion files from the export alongside it. See the [OpenAI import guide](providers/openai.md#import-chatgpt-history).

For redistribution, keep the [license and notice files](../README.md#license) with the bundle, including the matching UPX notices if distributing the packed executable.
