# 📊 JiStats

![JiStats logo](assets/logo.png)

JiStats is a local-first Windows 11 desktop app for monitoring AI provider quota, recent activity, and usage history. It supports OpenAI / Codex, MiniMax, Claude Code, and Kimi Code. Enable the integrations you use in Settings.

- [Quick start](docs/quick-start.md) covers the portable app.
- [Provider support](docs/capability-matrix.md) explains the quota and history each integration can show.
- [Documentation](docs/README.md) links to architecture, validation, and project planning.
- [Release checklist](docs/release-checklist.md) tracks v1.0 signoff.

## Build and packaging

### Building on Linux

To run the portable Catch2 tests directly on Debian or Ubuntu, install the native C++ toolchain, CMake and Ninja. This exercises the same sources used by the Windows app, including quota mirroring, observation persistence and bundled SQLite migrations.

```bash
sudo apt install build-essential cmake ninja-build
cmake -S . -B build-linux -G Ninja -DJISTATS_BUILD_TESTS=ON
cmake --build build-linux
ctest --test-dir build-linux --output-on-failure
```

To build the Windows app and CLI on Linux, use a separate build directory with MinGW-w64 and ImageMagick.

```bash
sudo apt install cmake ninja-build g++-mingw-w64-x86-64-posix imagemagick
cmake -S . -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
```

`build-mingw/bin/` then holds `JiStats.exe` and `JiStatsMiniMaxCli.exe`. Add `-DJISTATS_BUILD_TESTS=ON` for `JiStatsTests.exe`. The executables link the MinGW runtime statically. ImageMagick replaces the Windows-only PowerShell step that builds the app icon. The bundled `tools/upx/upx.exe` only runs on Windows, so packed `-upx` variants need a `upx` on `PATH`.

Cross-builds use bundled SQLite by default. With bundled or statically linked SQLite, you can copy the executables to Windows without extra DLLs. To use `-DJISTATS_USE_SYSTEM_SQLITE=ON`, install SQLite built for Windows x64 into the MinGW target prefix or set `CMAKE_FIND_ROOT_PATH` to the Windows dependency prefix. If the build links a shared SQLite package, copy that package's SQLite DLL (typically `sqlite3.dll`) alongside the executables. Configure in a fresh build directory if CMake previously cached Linux SQLite paths.

On code pull requests, GitHub Actions runs the portable tests on Linux and the full cross-built test executable on Windows. Contributors can push from Linux without a local Windows build. See the [verification guide](docs/validation/testing.md).

### Building on Windows

For the reference MSVC Release build, run these commands from the repository root on Windows. The Visual Studio C++ Build Tools and Windows SDK are sufficient for command-line builds.

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -DJISTATS_BUILD_TESTS=ON
pwsh -File .\scripts\build_release.ps1
```

The helper stops running JiStats, MiniMax CLI, and test binaries before building to release file locks. Use `-Target JiStats` to build only the desktop app. Release outputs are in `build/bin/Release/`.

- `JiStats.exe` is the desktop app. `JiStatsMiniMaxCli.exe` and `JiStatsTests.exe` are developer tools.
- With `JISTATS_ENABLE_UPX=ON`, Release builds also produce `JiStats-upx.exe` using the bundled UPX 5.1.1. Use `-DJISTATS_ENABLE_UPX=OFF` at configure time to omit packed variants.
- For a portable bundle, include the license files described below. Follow the [release checklist](docs/release-checklist.md) for package smoke testing.

## 📦 Third-Party Components

JiStats keeps its default dependency sources in the checkout. To select an installed SQLite, configure with `-DJISTATS_USE_SYSTEM_SQLITE=ON`.

| Component | Upstream | Location | Purpose | License |
| --- | --- | --- | --- | --- |
| SQLite amalgamation | https://www.sqlite.org/ | `third_party/sqlite/` | bundled local storage engine | SQLite upstream terms |
| Catch2 amalgamation | https://github.com/catchorg/Catch2 | `third_party/catch2/` | test framework used by `JiStatsTests.exe` | Catch2 upstream terms |
| spdlog | https://github.com/gabime/spdlog | `third_party/spdlog/include/` | header-only logging | MIT |
| bundled `fmt` inside spdlog | https://github.com/fmtlib/fmt | `third_party/spdlog/include/spdlog/fmt/bundled/` | formatting support used through spdlog | MIT |
| UPX | https://github.com/upx/upx | `tools/upx/` | optional release-size reduction tool for `-upx` variants | GPL v2 or later with the UPX special exception |

Notes:

- `tools/upx/` is a build/distribution tool path, not a runtime dependency of the app.
- See [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md) for the current notice file and redistribution notes.

## License

JiStats' first-party code is source-available under Apache-2.0 with Commons Clause.

Business and internal use are allowed, but selling JiStats itself requires separate permission.

This repository is not distributed under a single license for every file. Bundled third-party components keep their own licenses and notices.

For redistribution or packaging, keep the root `LICENSE`, `THIRD_PARTY_NOTICES.md`, and the upstream third-party license texts together. If shipping UPX-packed binaries, also include the matching upstream UPX `LICENSE` and `COPYING` texts for the exact official UPX release used. The current bundled copies are `tools/upx/LICENSE` and `tools/upx/COPYING` for UPX `5.1.1`.

See `LICENSE` and `THIRD_PARTY_NOTICES.md` for the current license disclosures.
