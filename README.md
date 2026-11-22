# 📊 JiStats

![JiStats logo](assets/logo.png)

JiStats is a local-first Windows 11 desktop app for monitoring AI provider quota, recent activity, and usage history. It supports OpenAI / Codex, MiniMax, Claude Code, and Kimi Code. Enable the integrations you use in Settings.

- [Quick start](docs/quick-start.md) covers getting and launching the portable app.
- [Provider support](docs/capability-matrix.md) explains the quota and history each integration can show.
- [Documentation](docs/README.md) links to architecture, validation, and project planning.
- [Release checklist](docs/release-checklist.md) tracks v1.0 signoff.

## Build and packaging

The build requires CMake 3.20 or newer.

CMake downloads pinned dependencies during configuration and verifies their SHA-256 hashes. For an offline configure, reuse a populated build directory or point `FETCHCONTENT_SOURCE_DIR_SQLITE`, `FETCHCONTENT_SOURCE_DIR_SPDLOG`, and, for test builds, `FETCHCONTENT_SOURCE_DIR_CATCH2` at local copies of the pinned sources. The pins are in [CMakeLists.txt](CMakeLists.txt).

### Building on Linux

To run the portable Catch2 tests directly on Debian or Ubuntu, install the native C++ toolchain, CMake and Ninja. This exercises the same sources used by the Windows app, including quota mirroring, observation persistence and bundled SQLite migrations.

```bash
sudo apt install build-essential cmake ninja-build
cmake -S . -B build-linux -G Ninja -DJISTATS_BUILD_TESTS=ON
cmake --build build-linux
ctest --test-dir build-linux --output-on-failure
```

To build the Windows app on Linux, use a separate build directory with MinGW-w64 and ImageMagick.

```bash
sudo apt install cmake ninja-build g++-mingw-w64-x86-64-posix imagemagick
cmake -S . -B build-mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
```

`build-mingw/bin/` then holds `JiStats.exe`. Add `-DJISTATS_BUILD_TESTS=ON` for `JiStatsTests.exe`. The executables link the MinGW runtime statically. ImageMagick generates the app icon when cross-building.

Cross-builds use bundled SQLite by default. With bundled or statically linked SQLite, you can copy the executables to Windows without extra DLLs. To use `-DJISTATS_USE_SYSTEM_SQLITE=ON`, install SQLite built for Windows x64 into the MinGW target prefix or set `CMAKE_FIND_ROOT_PATH` to the Windows dependency prefix. If the build links a shared SQLite package, copy that package's SQLite DLL (typically `sqlite3.dll`) alongside the executables. Configure in a fresh build directory if CMake previously cached Linux SQLite paths.

For a portable release, take `JiStats.exe` from `build-mingw/bin/` (it embeds the SQLite baseline) and include the [redistribution notices](#license), including the [toolchain notices for MinGW builds](THIRD_PARTY_NOTICES.md#mingw-w64-and-winpthreads). The test executable is a developer tool. Follow the [release checklist](docs/release-checklist.md) to smoke-test the assembled bundle on Windows.

For changes that require a build, GitHub Actions runs the portable tests and MinGW-w64 cross-build on Linux, then runs the full suite on Windows. See the [verification guide](docs/validation/testing.md#continuous-integration) for build triggers and manual dispatch.

### Building on Windows

From an [MSYS2 UCRT64 shell](https://www.msys2.org/docs/environments/), install the Windows-native GCC, CMake and Ninja packages and build from the repository root.

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
cmake -S . -B build-ucrt -G Ninja -DCMAKE_BUILD_TYPE=Release -DJISTATS_BUILD_TESTS=ON
cmake --build build-ucrt
ctest --test-dir build-ucrt --output-on-failure
```

Release outputs are in `build-ucrt/bin/`.

For an existing Visual Studio build tree, run `pwsh -File .\scripts\build_release.ps1 -BuildDir build`. Replace `build` with your configured directory. The helper stops running JiStats binaries to release file locks and builds the Release configuration. The Visual Studio generator puts executables under that directory's `bin/Release/`.

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
