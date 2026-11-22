# Third-Party Notices

JiStats includes third-party components that remain licensed under their own terms.

## SQLite

- Component: `third_party/sqlite/`
- Upstream: [SQLite copyright and public-domain dedication](https://www.sqlite.org/copyright.html)

## Catch2

- Component: `third_party/catch2/`
- Upstream license: Boost Software License 1.0
- Copyright: Catch2 Authors
- Local license file: `third_party/catch2/LICENSE.txt`

## spdlog

- Component: `third_party/spdlog/`
- Upstream license: MIT
- Copyright: Gabi Melman
- Local license file: `third_party/spdlog/LICENSE`

## fmt (bundled inside spdlog)

- Component: `third_party/spdlog/include/spdlog/fmt/bundled/`
- Upstream license: MIT
- Copyright: Victor Zverovich
- Local license file: `third_party/spdlog/include/spdlog/fmt/bundled/fmt.license.rst`

## MinGW-w64 and winpthreads

MinGW builds on Linux and MSYS2 statically link the MinGW-w64 runtime and winpthreads into the Windows executables. For each release bundle, copy both complete notice files from the MinGW-w64 source release used to build it. Preserve them alongside the executable, including when packing it with UPX.

| Source path in the matching MinGW-w64 release | Path in the bundle |
| --- | --- |
| `COPYING.MinGW-w64-runtime/COPYING.MinGW-w64-runtime.txt` | `licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt` |
| `mingw-w64-libraries/winpthreads/COPYING` | `licenses/winpthreads/COPYING` |

The [MinGW-w64 releases](https://github.com/mingw-w64/mingw-w64/tags) provide both files. If the distribution supplies patched runtime packages, use the corresponding package sources. For the statically linked GCC libraries, include `COPYING3` and `COPYING.RUNTIME` from the matching GCC source in `licenses/gcc/`. See the [GCC runtime license](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html).

## UPX (optional release packing tool)

- Component: bundled build/distribution tool under `tools/upx/`
- Upstream project: `https://github.com/upx/upx`
- Bundled official version: `5.1.1`
- Upstream license: GPL v2 or later, with the UPX special exception for compressed executables
- Local license files: `tools/upx/LICENSE` and `tools/upx/COPYING`
- Corresponding source for the bundled executable: [UPX 5.1.1 source archive](tools/upx/upx-5.1.1-src.tar.xz)
- Distribution note: if you ship UPX-packed JiStats binaries, use an official unmodified UPX release and ship the matching upstream `LICENSE` and `COPYING` texts with that distribution.

Distributors should ship this file together with the root `LICENSE` and the upstream third-party license texts above.
