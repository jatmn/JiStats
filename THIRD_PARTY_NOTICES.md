# Third-Party Notices

JiStats includes third-party components that remain licensed under their own terms.

## SQLite

- Version: 3.54.1, compiled into the executables unless `JISTATS_USE_SYSTEM_SQLITE` is enabled
- Upstream: [SQLite copyright and public-domain dedication](https://www.sqlite.org/copyright.html)

## spdlog

- Version: 1.17.0, compiled into the executables
- Upstream: https://github.com/gabime/spdlog
- License: MIT

```text
The MIT License (MIT)

Copyright (c) 2016 - present, Gabi Melman and spdlog contributors.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

## fmt (bundled inside spdlog)

- Version: 12.1.0, compiled into the executables through spdlog
- Upstream: https://github.com/fmtlib/fmt
- License: MIT with an exception that allows redistributing embedded object code without the notice. See the [fmt license](https://github.com/fmtlib/fmt/blob/12.1.0/LICENSE).

## Catch2

- Version: 3.16.1, used only by the `JiStatsTests` executable and not included in release binaries
- Upstream: https://github.com/catchorg/Catch2
- License: [Boost Software License 1.0](https://github.com/catchorg/Catch2/blob/v3.16.1/LICENSE.txt)

## MinGW-w64 and winpthreads

MinGW builds on Linux and MSYS2 statically link the MinGW-w64 runtime and winpthreads into the Windows executables. For each release bundle, copy both complete notice files from the MinGW-w64 source release used to build it. Preserve them alongside the executable, including when packing it with UPX.

| Source path in the matching MinGW-w64 release | Path in the bundle |
| --- | --- |
| `COPYING.MinGW-w64-runtime/COPYING.MinGW-w64-runtime.txt` | `licenses/mingw-w64/COPYING.MinGW-w64-runtime.txt` |
| `mingw-w64-libraries/winpthreads/COPYING` | `licenses/winpthreads/COPYING` |

The [MinGW-w64 releases](https://github.com/mingw-w64/mingw-w64/tags) provide both files. If the distribution supplies patched runtime packages, use the corresponding package sources. For the statically linked GCC libraries, include `COPYING3` and `COPYING.RUNTIME` from the matching GCC source in `licenses/gcc/`. See the [GCC runtime license](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html).

## UPX (optional release packing tool)

UPX is not stored in this repository or linked into JiStats. The optional `-upx` release variants are packed with an external UPX executable.

- Upstream project: https://github.com/upx/upx
- Upstream license: GPL v2 or later, with the UPX special exception for compressed executables
- Distribution note: if you ship UPX-packed JiStats binaries, use an official unmodified UPX release and ship the matching upstream `LICENSE` and `COPYING` texts with that distribution.

Distributors should ship this file together with the root `LICENSE` and the toolchain and UPX license texts listed above.
