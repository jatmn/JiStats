# Third-Party Notices

JiStats includes third-party components that remain licensed under their own terms.

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

## UPX (optional release packing tool)

- Component: bundled build/distribution tool under `tools/upx/`
- Upstream project: `https://github.com/upx/upx`
- Bundled official version: `5.1.1`
- Upstream license: GPL v2 or later, with the UPX special exception for compressed executables
- Local license files: `tools/upx/LICENSE` and `tools/upx/COPYING`
- Distribution note: if you ship UPX-packed JiStats binaries, use an official unmodified UPX release and ship the matching upstream `LICENSE` and `COPYING` texts with that distribution.

Distributors should ship this file together with the root `LICENSE` and the upstream third-party license texts above.
