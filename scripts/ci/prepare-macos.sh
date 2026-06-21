#!/usr/bin/env bash
set -euo pipefail

TEMPLATE_DIR=".ci/obs-plugintemplate"
BUILD_HELPER="${TEMPLATE_DIR}/cmake/common/buildspec_common.cmake"
SWIFT_HOOK="${TEMPLATE_DIR}/cmake/enable-swift.cmake"

cp buildspec.json "${TEMPLATE_DIR}/buildspec.json"

cat > "${SWIFT_HOOK}" <<'EOF'
if(APPLE)
  enable_language(Swift)
endif()
EOF

python3 - "${BUILD_HELPER}" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old = 'set(_cmake_extra "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")'
new = '''set(
      _cmake_extra
      "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
      "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/enable-swift.cmake"
    )'''
if old not in text:
    raise SystemExit("Could not locate the macOS nested CMake argument block")
path.write_text(text.replace(old, new, 1), encoding="utf-8")
PY

cmake --preset macos -S "${TEMPLATE_DIR}"

DEPS_ROOT="${PWD}/${TEMPLATE_DIR}/.deps"
PREFIX_PATH="${DEPS_ROOT}"
for directory in "${DEPS_ROOT}"/obs-deps-*; do
  if [[ -d "${directory}" ]]; then
    PREFIX_PATH="${PREFIX_PATH};${directory}"
  fi
done

printf '%s\n' "OBS_GSP_DEPS_ROOT=${DEPS_ROOT}" >> "${GITHUB_ENV}"
printf '%s\n' "OBS_GSP_PREFIX_PATH=${PREFIX_PATH}" >> "${GITHUB_ENV}"
