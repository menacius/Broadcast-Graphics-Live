#!/usr/bin/env bash
set -Eeuo pipefail

show_help() {
  cat <<'HELP'
Build Broadcast Graphics Live against the OBS Linux baseline on WSL2.

Required arguments:
  --source PATH             Windows project mounted inside WSL
  --workspace PATH          Persistent Linux workspace/build root
  --output-dir PATH         Windows output directory mounted inside WSL

Optional arguments:
  --build-type TYPE         CMake build type (default: Release)
  --package-name NAME       Distribution package name
  --platform NAME           Package platform component
  --archive-format FORMAT   zip, tar.gz, or both (default: both)
  --expected-version VER    Required Ubuntu VERSION_ID (default: 24.04)
  --max-glibc VER          Reject binaries requiring a newer GLIBC symbol (default: 2.39)
  --build-tests             Build and run CTest tests
  --skip-deps               Do not install/update APT dependencies
  --clean                   Delete the Linux CMake build tree before building
  --install-dir PATH        Also install the staged plugin into this WSL OBS plugin root
  --help                    Show this help
HELP
}

SOURCE_MOUNT=""
WORKSPACE=""
OUTPUT_DIR=""
BUILD_TYPE="Release"
PACKAGE_NAME="Broadcast_Graphics_Live"
PLATFORM=""
ARCHIVE_FORMAT="both"
EXPECTED_VERSION="24.04"
MAX_GLIBC_VERSION="2.39"
BUILD_TESTS=0
INSTALL_DEPS=1
CLEAN=0
INSTALL_DIR=""

while (($#)); do
  case "$1" in
    --source) SOURCE_MOUNT=${2:?Missing value for --source}; shift 2 ;;
    --workspace) WORKSPACE=${2:?Missing value for --workspace}; shift 2 ;;
    --output-dir) OUTPUT_DIR=${2:?Missing value for --output-dir}; shift 2 ;;
    --build-type) BUILD_TYPE=${2:?Missing value for --build-type}; shift 2 ;;
    --package-name) PACKAGE_NAME=${2:?Missing value for --package-name}; shift 2 ;;
    --platform) PLATFORM=${2:?Missing value for --platform}; shift 2 ;;
    --archive-format) ARCHIVE_FORMAT=${2:?Missing value for --archive-format}; shift 2 ;;
    --expected-version) EXPECTED_VERSION=${2:?Missing value for --expected-version}; shift 2 ;;
    --max-glibc) MAX_GLIBC_VERSION=${2:?Missing value for --max-glibc}; shift 2 ;;
    --build-tests) BUILD_TESTS=1; shift ;;
    --skip-deps) INSTALL_DEPS=0; shift ;;
    --clean) CLEAN=1; shift ;;
    --install-dir) INSTALL_DIR=${2:?Missing value for --install-dir}; shift 2 ;;
    --help|-h) show_help; exit 0 ;;
    *) printf 'Unknown argument: %s\n' "$1" >&2; show_help >&2; exit 2 ;;
  esac
done

[[ -n "$SOURCE_MOUNT" ]] || { echo "--source is required" >&2; exit 2; }
[[ -n "$WORKSPACE" ]] || { echo "--workspace is required" >&2; exit 2; }
[[ -n "$OUTPUT_DIR" ]] || { echo "--output-dir is required" >&2; exit 2; }
[[ -f "$SOURCE_MOUNT/CMakeLists.txt" ]] || { echo "CMakeLists.txt not found under $SOURCE_MOUNT" >&2; exit 2; }
[[ "$ARCHIVE_FORMAT" =~ ^(zip|tar\.gz|both)$ ]] || { echo "Invalid archive format: $ARCHIVE_FORMAT" >&2; exit 2; }

if [[ ! -r /etc/os-release ]]; then
  echo "Cannot identify the Linux distribution." >&2
  exit 3
fi
# shellcheck disable=SC1091
source /etc/os-release
if [[ "${ID:-}" != "ubuntu" || "${VERSION_ID:-}" != "$EXPECTED_VERSION" ]]; then
  echo "This build requires Ubuntu $EXPECTED_VERSION; detected ${PRETTY_NAME:-unknown}." >&2
  exit 3
fi

if [[ $(id -u) -ne 0 && $INSTALL_DEPS -eq 1 ]]; then
  echo "Dependency installation requires root. The PowerShell launcher should invoke this helper as root." >&2
  exit 4
fi

if [[ $INSTALL_DEPS -eq 1 ]]; then
  echo "=== Installing Ubuntu build dependencies ==="
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends software-properties-common ca-certificates
  add-apt-repository -y universe >/dev/null
  apt-get update
  apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config rsync git \
    zip unzip tar gzip file patchelf binutils \
    libobs-dev \
    qt6-base-dev qt6-base-private-dev qt6-svg-dev \
    libcairo2-dev libpango1.0-dev liblz4-dev nlohmann-json3-dev
fi

for command_name in cmake ninja pkg-config rsync zip tar gzip; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "Required command is unavailable: $command_name" >&2
    exit 5
  }
done

arch=$(uname -m)
if [[ -z "$PLATFORM" ]]; then
  case "$arch" in
    x86_64|amd64) PLATFORM="linux-obs-ubuntu-${EXPECTED_VERSION}-x64" ;;
    aarch64|arm64) PLATFORM="linux-obs-ubuntu-${EXPECTED_VERSION}-arm64" ;;
    *) PLATFORM="linux-obs-ubuntu-${EXPECTED_VERSION}-${arch}" ;;
  esac
fi

SOURCE_DIR="$WORKSPACE/source"
BUILD_DIR="$WORKSPACE/build"
STAGE_DIR="$WORKSPACE/stage"
ARTIFACT_DIR="$WORKSPACE/artifacts"
mkdir -p "$SOURCE_DIR" "$OUTPUT_DIR" "$ARTIFACT_DIR"

if [[ $CLEAN -eq 1 ]]; then
  echo "=== Cleaning the persistent WSL build tree ==="
  rm -rf "$BUILD_DIR" "$STAGE_DIR" "$ARTIFACT_DIR"
  mkdir -p "$ARTIFACT_DIR"
fi

echo "=== Synchronizing changed source files into WSL ext4 ==="
# Deliberately do not preserve source mtimes. --checksum makes content the
# authority, so re-extracting an unchanged ZIP on Windows does not trigger a
# Linux rebuild. Existing identical files and their mtimes remain untouched.
rsync -r --checksum --delete --links --perms \
  --exclude '/.git/' \
  --exclude '/.vs/' \
  --exclude '/.bgl-update/' \
  --exclude '/build/' \
  --exclude '/build-*/' \
  --exclude '/cmake-build-*/' \
  --exclude '/*.zip' \
  --exclude '/*.tar.gz' \
  "$SOURCE_MOUNT/" "$SOURCE_DIR/"

cmake_project_version=$(sed -nE 's/^[[:space:]]*project\([^)]*VERSION[[:space:]]+([0-9]+(\.[0-9]+)*)[^)]*\).*/\1/p' "$SOURCE_DIR/CMakeLists.txt" | head -n1)
prerelease=$(sed -nE 's/^[[:space:]]*set\([[:space:]]*OBS_BGS_PRERELEASE[[:space:]]+"([^"]*)"[[:space:]]*\).*/\1/p' "$SOURCE_DIR/CMakeLists.txt" | head -n1)
dev_version=$(sed -nE 's/^[[:space:]]*set\([[:space:]]*OBS_BGS_DEVELOPMENT_VERSION[[:space:]]+"([^"]*)"[[:space:]]*\).*/\1/p' "$SOURCE_DIR/CMakeLists.txt" | head -n1)

[[ -n "$cmake_project_version" ]] || { echo "Could not read project version from CMakeLists.txt" >&2; exit 6; }
[[ -n "$dev_version" ]] || { echo "Could not read development version from CMakeLists.txt" >&2; exit 6; }

sanitize_component() {
  local value=$1
  value=${value// /_}
  value=$(printf '%s' "$value" | sed -E 's#[\\/:*?"<>|]+#_#g; s/^[_\.]+//; s/[_\.]+$//')
  [[ -n "$value" ]] || return 1
  printf '%s' "$value"
}

version_component="v${cmake_project_version}"
if [[ -n "$prerelease" ]]; then
  version_component+="-${prerelease}"
fi
development_component="development-version-${dev_version}"
package_stem="$(sanitize_component "$PACKAGE_NAME")_$(sanitize_component "$version_component")_$(sanitize_component "$development_component")_$(sanitize_component "$PLATFORM")"

tests_value=OFF
if [[ $BUILD_TESTS -eq 1 ]]; then
  tests_value=ON
fi

echo "=== Configuring OBS-compatible Linux build on Ubuntu $EXPECTED_VERSION ==="
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DOBS_BGS_BUILD_TESTS="$tests_value"

echo "=== Building with Ninja ==="
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

if [[ $BUILD_TESTS -eq 1 ]]; then
  echo "=== Running CTest ==="
  ctest --test-dir "$BUILD_DIR" --output-on-failure
fi

echo "=== Staging OBS plugin package ==="
rm -rf "$STAGE_DIR"
cmake --install "$BUILD_DIR" --prefix "$STAGE_DIR"
PLUGIN_ROOT="$STAGE_DIR/broadcast-graphics-live"
PLUGIN_BINARY=$(find "$PLUGIN_ROOT/bin" -maxdepth 3 -type f -name 'broadcast-graphics-live.so' -print -quit 2>/dev/null || true)
if [[ -z "$PLUGIN_BINARY" ]]; then
  echo "Staged plugin binary was not found below $PLUGIN_ROOT/bin" >&2
  exit 7
fi

# OBS installed from a distribution resolves its libraries from the normal system
# paths. The official Flatpak exposes OBS libraries below /app/lib, which is not
# guaranteed to be searched for manually installed plugins. Add those paths as a
# fallback RUNPATH without bundling or duplicating libobs/libobs-frontend-api.
echo "=== Adding OBS native and Flatpak runtime library search paths ==="
existing_rpath=$(patchelf --print-rpath "$PLUGIN_BINARY" 2>/dev/null || true)
obs_runtime_rpath='/app/lib:/app/lib64:/app/lib/x86_64-linux-gnu'
if [[ -n "$existing_rpath" ]]; then
  combined_rpath="$existing_rpath:$obs_runtime_rpath"
else
  combined_rpath="$obs_runtime_rpath"
fi
# Preserve order while removing duplicate entries.
combined_rpath=$(printf '%s' "$combined_rpath" | awk -v RS=: '!seen[$0]++ { if (out != "") out=out ":"; out=out $0 } END { print out }')
patchelf --set-rpath "$combined_rpath" "$PLUGIN_BINARY"
echo "Plugin RUNPATH: $(patchelf --print-rpath "$PLUGIN_BINARY")"

if readelf -d "$PLUGIN_BINARY" | grep -q 'libobs-frontend-api.so.0'; then
  echo "Frontend API dependency detected: libobs-frontend-api.so.0"
fi

version_gt() {
  dpkg --compare-versions "$1" gt "$2"
}

echo "=== Auditing Linux ABI compatibility ==="
highest_glibc=$(readelf --version-info "$PLUGIN_BINARY" 2>/dev/null \
  | grep -oE 'GLIBC_[0-9]+\.[0-9]+' \
  | sed 's/^GLIBC_//' \
  | sort -V \
  | tail -n1 || true)

if [[ -n "$highest_glibc" ]]; then
  echo "Highest required GLIBC symbol: GLIBC_$highest_glibc"
  if version_gt "$highest_glibc" "$MAX_GLIBC_VERSION"; then
    echo "The plugin requires GLIBC_$highest_glibc, newer than the configured OBS baseline GLIBC_$MAX_GLIBC_VERSION." >&2
    echo "Build aborted so an incompatible Linux package is not published." >&2
    exit 8
  fi
else
  echo "No versioned GLIBC requirement was detected in the plugin binary."
fi

if ! ldd "$PLUGIN_BINARY" | tee "$ARTIFACT_DIR/ldd-broadcast-graphics-live.txt" | grep -q 'not found'; then
  echo "All directly linked shared libraries resolve in the build environment."
else
  echo "One or more shared libraries could not be resolved:" >&2
  grep 'not found' "$ARTIFACT_DIR/ldd-broadcast-graphics-live.txt" >&2 || true
  exit 9
fi

{
  echo "Build distribution: ${PRETTY_NAME:-Ubuntu $EXPECTED_VERSION}"
  echo "Build glibc: $(getconf GNU_LIBC_VERSION 2>/dev/null || ldd --version | head -n1)"
  echo "Maximum allowed GLIBC: GLIBC_$MAX_GLIBC_VERSION"
  echo "Highest required GLIBC: ${highest_glibc:+GLIBC_$highest_glibc}"
  echo "Compiler: $(c++ --version | head -n1)"
  echo "Qt: $(pkg-config --modversion Qt6Core 2>/dev/null || true)"
  echo "libobs: $(pkg-config --modversion libobs 2>/dev/null || true)"
  echo "Plugin RUNPATH: $(patchelf --print-rpath "$PLUGIN_BINARY" 2>/dev/null || true)"
  echo "Dynamic dependencies:"
  readelf -d "$PLUGIN_BINARY" 2>/dev/null | grep 'Shared library:' | sed -E 's/.*\[([^]]+)\].*/  \1/' || true
} > "$PLUGIN_ROOT/BUILD-COMPATIBILITY.txt"

if [[ -n "$INSTALL_DIR" ]]; then
  echo "=== Installing staged plugin into WSL OBS directory ==="
  mkdir -p "$INSTALL_DIR"
  rm -rf "$INSTALL_DIR/broadcast-graphics-live"
  cp -a "$PLUGIN_ROOT" "$INSTALL_DIR/"
fi

rm -f "$ARTIFACT_DIR/${package_stem}.zip" "$ARTIFACT_DIR/${package_stem}.tar.gz"

if [[ "$ARCHIVE_FORMAT" == "zip" || "$ARCHIVE_FORMAT" == "both" ]]; then
  echo "=== Creating ZIP package ==="
  (cd "$STAGE_DIR" && zip -q -r "$ARTIFACT_DIR/${package_stem}.zip" broadcast-graphics-live)
  cp -f "$ARTIFACT_DIR/${package_stem}.zip" "$OUTPUT_DIR/"
  sha256sum "$OUTPUT_DIR/${package_stem}.zip" > "$OUTPUT_DIR/${package_stem}.zip.sha256"
  echo "BGL_PACKAGE_ZIP=$OUTPUT_DIR/${package_stem}.zip"
fi

if [[ "$ARCHIVE_FORMAT" == "tar.gz" || "$ARCHIVE_FORMAT" == "both" ]]; then
  echo "=== Creating gzip-compressed tar package ==="
  tar -C "$STAGE_DIR" -czf "$ARTIFACT_DIR/${package_stem}.tar.gz" broadcast-graphics-live
  cp -f "$ARTIFACT_DIR/${package_stem}.tar.gz" "$OUTPUT_DIR/"
  sha256sum "$OUTPUT_DIR/${package_stem}.tar.gz" > "$OUTPUT_DIR/${package_stem}.tar.gz.sha256"
  echo "BGL_PACKAGE_TARGZ=$OUTPUT_DIR/${package_stem}.tar.gz"
fi

echo "=== OBS-compatible Linux WSL2 build completed successfully ==="
echo "WSL_BUILD_DIR=$BUILD_DIR"
echo "STAGED_PLUGIN=$PLUGIN_ROOT"
