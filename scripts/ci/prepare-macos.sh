#!/usr/bin/env bash
set -euo pipefail

TEMPLATE_DIR=".ci/obs-plugintemplate"
cp buildspec.json "${TEMPLATE_DIR}/buildspec.json"
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
