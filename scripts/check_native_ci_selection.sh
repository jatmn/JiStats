#!/usr/bin/env bash
set -euo pipefail
source "$(dirname "$0")/native_ci_input.sh"

# These paths exercise separate production routes. They remain build inputs
# even if a future selector accidentally broadens the documentation exemption.
for file in CMakeLists.txt tests/CMakeLists.txt src/db/StorageService.cpp \
    tests/test_app_controller.cpp docs/fixtures/README.md \
    docs/fixtures/kimi/synthetic_local_runtime/config.toml \
    .github/workflows/ci.yml scripts/native_ci_input.sh; do
    if ! is_native_ci_input "$file"; then
        printf 'Native CI selection hid required input %s\n' "$file" >&2
        exit 1
    fi
done
