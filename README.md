# 📊 JiStats

![JiStats logo](assets/logo.png)

JiStats is a local-first Windows 11 desktop app for monitoring AI provider quota, recent activity, and usage history. It supports OpenAI / Codex, MiniMax, Claude Code, Kimi Code, and subscription quota for Grok and Cursor. Enable the integrations you use in Settings.

- [Quick start](docs/quick-start.md) covers getting and launching the portable app.
- [Build from source](#build-and-packaging) gives the commands to build the desktop app.
- [Provider support](docs/capability-matrix.md) explains the quota and history each integration can show.
- [Documentation](docs/README.md) links to architecture, validation, and project planning.
- [Release checklist](docs/release-checklist.md) tracks v1.0 signoff.

## Build and packaging

The build requires CMake 3.20 or newer.

CMake downloads pinned dependencies during configuration and verifies their SHA-256 hashes. For an offline configure, reuse a populated build directory or point `FETCHCONTENT_SOURCE_DIR_SQLITE`, `FETCHCONTENT_SOURCE_DIR_SPDLOG`, and, for test builds, `FETCHCONTENT_SOURCE_DIR_CATCH2` at local copies of the pinned sources. The pins are in [ThirdPartySources.cmake](cmake/ThirdPartySources.cmake).

### Building on Windows

If JiStats is running from the build directory, choose **File → Exit** in the dashboard or right-click the JiStats tray icon and choose **Exit**. Wait for JiStats to exit before rebuilding so the linker can replace the executable.

Open an [MSYS2 UCRT64 shell](https://www.msys2.org/docs/environments/) and run these commands from the repository root.

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
cmake -S . -B build-ucrt -G Ninja -DCMAKE_BUILD_TYPE=Release -DJISTATS_BUILD_TESTS=OFF
cmake --build build-ucrt --target JiStats
```

These commands build the desktop app without compiling or running the tests. Once the build finishes, open `build-ucrt/bin/JiStats.exe` to launch JiStats. The [quick start](docs/quick-start.md#launch) covers first launch and setup.

For an existing Visual Studio build tree, run `cmake --build build --config Release --target JiStats` and open `build/bin/Release/JiStats.exe`. Replace `build` with your configured directory.

The PowerShell [Release build wrapper](scripts/build_release.ps1) reports running executables from the selected build directory and targets. Close any reported process normally before rebuilding its executable. The wrapper runs CMake and returns its exit code.

### Building on Linux

Use MinGW-w64 and ImageMagick to build the Windows desktop app from Debian or Ubuntu.

```bash
sudo apt install cmake ninja-build g++-mingw-w64-x86-64-posix imagemagick
cmake -S . -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release -DJISTATS_BUILD_TESTS=OFF
cmake --build build-mingw --target JiStats
```

Copy `build-mingw/bin/JiStats.exe` to Windows and open it there. The executable links the MinGW runtime statically. ImageMagick generates the app icon when cross-building.

Cross-builds use bundled SQLite by default. With bundled or statically linked SQLite, you can copy the executables to Windows without extra DLLs. To use `-DJISTATS_USE_SYSTEM_SQLITE=ON`, install SQLite built for Windows x64 into the MinGW target prefix or set `CMAKE_FIND_ROOT_PATH` to the Windows dependency prefix. If the build links a shared SQLite package, copy that package's SQLite DLL (typically `sqlite3.dll`) alongside the executables. Configure in a fresh build directory if CMake previously cached Linux SQLite paths.

### Contributor tests and release checks

For local test commands and CI coverage, see the [verification guide](docs/validation/testing.md). The Windows tests can open desktop windows as they exercise the UI.

For a standalone portable release, use `build-mingw/bin/JiStats.exe` from the [MinGW build with bundled SQLite](#building-on-linux). Include the [redistribution notices](#license) and [MinGW toolchain notices](THIRD_PARTY_NOTICES.md#mingw-w64-and-winpthreads). Follow the [release checklist](docs/release-checklist.md) to smoke-test the assembled bundle on Windows.

### Optional UPX compression

[UPX](https://upx.github.io/) compresses the Windows executable so the standalone download is smaller and the file takes up less disk space. The packed app decompresses itself at launch, which adds startup work. Both variants run on Windows without a separate UPX installation.

CMake looks for UPX by default when targeting Windows. To produce `JiStats-upx.exe` alongside `JiStats.exe` in a Release build, put an [official UPX release](https://github.com/upx/upx/releases) for your build host on `PATH` or configure with `-DJISTATS_UPX_EXECUTABLE=/path/to/upx`. Use a Linux UPX executable when cross-building from Linux. If CMake cannot find UPX, the Release build produces the unpacked executable. Set `-DJISTATS_ENABLE_UPX=OFF` to skip detection and packing.

When you reuse a build directory, an older `JiStats-upx.exe` can remain after you disable packing. Package `JiStats.exe` for the current unpacked release.

For packed releases, follow the [redistribution instructions](#license).

## License

JiStats' first-party code is source-available under Apache-2.0 with Commons Clause.

Business and internal use are allowed, but selling JiStats itself requires separate permission.

This repository is not distributed under a single license for every file. Bundled third-party components keep their own licenses and notices.

For redistribution or packaging, keep the root `LICENSE` and `THIRD_PARTY_NOTICES.md` together. If shipping UPX-packed binaries, also include the upstream UPX `LICENSE` and `COPYING` texts from the exact official UPX release used.

See `LICENSE` and `THIRD_PARTY_NOTICES.md` for the current license disclosures.
