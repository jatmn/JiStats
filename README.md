# 📊 JiStats

![JiStats logo](assets/logo.png)

JiStats is a local-first Windows 11 desktop app for monitoring AI provider quota, recent activity, and usage history. It supports OpenAI / Codex, MiniMax, Claude Code, and Kimi Code. Enable the integrations you use in Settings.

- [Quick start](docs/quick-start.md) covers getting and launching the portable app.
- [Provider support](docs/capability-matrix.md) explains the quota and history each integration can show.
- [Documentation](docs/README.md) links to architecture, validation, and project planning.
- [Release checklist](docs/release-checklist.md) tracks v1.0 signoff.

## Build and packaging

The build requires CMake 3.20 or newer.

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

`build-mingw/bin/` then holds `JiStats.exe`. Add `-DJISTATS_BUILD_TESTS=ON` for `JiStatsTests.exe`. The executables link the MinGW runtime statically. ImageMagick generates the app icon when cross-building. To produce `JiStats-upx.exe`, put a Linux `upx` on `PATH` or pass `-DJISTATS_UPX_EXECUTABLE=/path/to/upx` at configure time.

Cross-builds use bundled SQLite by default. With bundled or statically linked SQLite, you can copy the executables to Windows without extra DLLs. To use `-DJISTATS_USE_SYSTEM_SQLITE=ON`, install SQLite built for Windows x64 into the MinGW target prefix or set `CMAKE_FIND_ROOT_PATH` to the Windows dependency prefix. If the build links a shared SQLite package, copy that package's SQLite DLL (typically `sqlite3.dll`) alongside the executables. Configure in a fresh build directory if CMake previously cached Linux SQLite paths.

For a portable release, take `JiStats.exe` from `build-mingw/bin/` (it embeds the SQLite baseline) and include the [redistribution notices](#license), including the [toolchain notices for MinGW builds](THIRD_PARTY_NOTICES.md#mingw-w64-and-winpthreads). The CLI and test executable are developer tools. Follow the [release checklist](docs/release-checklist.md) to smoke-test the assembled bundle on Windows.

For changes that require a build, GitHub Actions runs the portable tests and MinGW-w64 cross-build on Linux, then runs the full suite on Windows. See the [verification guide](docs/validation/testing.md#continuous-integration) for build triggers and manual dispatch.

### Building on Windows

From an [MSYS2 UCRT64 shell](https://www.msys2.org/docs/environments/), install the Windows-native GCC, CMake and Ninja packages and build from the repository root.

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
cmake -S . -B build-ucrt -G Ninja -DCMAKE_BUILD_TYPE=Release -DJISTATS_BUILD_TESTS=ON
cmake --build build-ucrt
ctest --test-dir build-ucrt --output-on-failure
```

Release outputs are in `build-ucrt/bin/`. To also produce the optional packed `JiStats-upx.exe`, put an official [UPX release](https://github.com/upx/upx/releases) on `PATH` or pass `-DJISTATS_UPX_EXECUTABLE=C:/path/to/upx.exe` at configure time. Configure with `-DJISTATS_ENABLE_UPX=OFF` to skip it.

For an existing Visual Studio build tree, run `pwsh -File .\scripts\build_release.ps1 -BuildDir build`. Replace `build` with your configured directory. The helper stops running JiStats binaries to release file locks and builds the Release configuration. The Visual Studio generator puts executables under that directory's `bin/Release/`.

### Building the MiniMax CLI

For standalone MiniMax readiness, capture, and diagnostics commands, add `-DJISTATS_BUILD_MINIMAX_CLI=ON` to either Windows-target CMake configure command above. The option defaults to `OFF`. When enabled, the build produces `JiStatsMiniMaxCli.exe` alongside the app, and `cmake --install` installs it to the install prefix's `bin/` directory. The desktop MiniMax integration is available in either configuration, and `JISTATS_BUILD_TESTS` controls the tests independently.

## 📦 Third-Party Components

Third-party sources are not stored in this repository. The first CMake configure downloads the pinned upstream releases below and verifies each archive's SHA-256 hash; the versions and hashes are declared in [CMakeLists.txt](CMakeLists.txt). Downloads are cached in the build directory's `_deps/`, so later builds in that directory work offline. To build without network access, pass `-DFETCHCONTENT_SOURCE_DIR_<NAME>=/path/to/extracted/release` for `SQLITE`, `SPDLOG` and `CATCH2`. To use an installed SQLite instead of the pinned amalgamation, configure with `-DJISTATS_USE_SYSTEM_SQLITE=ON`.

| Component | Version | Upstream | Purpose | License |
| --- | --- | --- | --- | --- |
| SQLite amalgamation | 3.53.0 | https://www.sqlite.org/ | local storage engine, compiled into the executables | Public domain |
| spdlog | 1.15.1 | https://github.com/gabime/spdlog | header-only logging, compiled into the executables | MIT |
| `fmt` bundled inside spdlog | 11.1.3 | https://github.com/fmtlib/fmt | formatting support used through spdlog | MIT with an object-code exception |
| Catch2 amalgamation | 3.7.1 | https://github.com/catchorg/Catch2 | test framework, only downloaded with `-DJISTATS_BUILD_TESTS=ON` | BSL-1.0 |

UPX is an optional external build tool for the `-upx` release variants. It is not stored in the repository or required by the app. See [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md) for the notices that ship with release binaries.

## License

JiStats' first-party code is source-available under Apache-2.0 with Commons Clause.

Business and internal use are allowed, but selling JiStats itself requires separate permission.

This repository is not distributed under a single license for every file. Bundled third-party components keep their own licenses and notices.

For redistribution or packaging, keep the root `LICENSE` and `THIRD_PARTY_NOTICES.md` together. If shipping UPX-packed binaries, also include the upstream UPX `LICENSE` and `COPYING` texts from the exact official UPX release used.

See `LICENSE` and `THIRD_PARTY_NOTICES.md` for the current license disclosures.
