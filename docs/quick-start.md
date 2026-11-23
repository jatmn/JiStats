# Portable quick start

JiStats runs on Windows 11 from a portable folder. If you need the executable, check [GitHub Releases](https://github.com/jatmn/JiStats/releases) for a published bundle or follow [Build and packaging](../README.md#build-and-packaging) to build it from source.

## Launch

> [!IMPORTANT]
> To collect Codex usage by model, go to **File → Settings → OpenAI** and select **Enable Codex model attribution**. This separate setting starts off. JiStats reads the local Codex state database and saves a baseline when you enable it. See the [Codex SQLite reference](providers/openai.md#codex-sqlite-privacy-boundary).

Use one desktop binary, either `JiStats.exe` or the optional packed `JiStats-upx.exe`. The executable contains the SQLite baseline and can initialize it from any working directory.

```powershell
.\JiStats.exe
# Or launch in the background with the tray icon.
.\JiStats.exe --background
```

JiStats stores the database in `%LOCALAPPDATA%\JiStats\jistats.db`. Moving the executable does not move that data. **Start with Windows** can launch JiStats at sign-in with the same embedded baseline. Existing version 12 databases reopen in place. See [supported database versions](reference/storage.md#migration-behavior) for how JiStats handles older files.

If JiStats warns that it rejected a development database, exit JiStats. Move the database named in the warning and any `-wal`, `-shm`, or `-journal` files to a backup folder. Also move any legacy `jistats.db` and companion files beside the executable or in its working directory, since JiStats can copy them into the default location. Restart JiStats to create a fresh database.

## Enable providers

Choose **Settings** in the dashboard. All provider tracking toggles start off. **Overview** shows remaining quota, reset times, and freshness for enabled subscriptions. See the [provider matrix](capability-matrix.md) for setup.

## View and export history

Choose **History** in the dashboard to open Analysis and select a provider, account, and date range. **File → Export HTML Report…** uses that scope. Set up recurring export in Settings.

For older ChatGPT activity, extract an official export and select its `conversations.json` with **File → Import OpenAI Export…**. Keep the companion files from the export alongside it. See the [OpenAI import guide](providers/openai.md#import-chatgpt-history).

For redistribution, keep the [license and notice files](../README.md#license) with the bundle, including the matching UPX notices if distributing the packed executable.
