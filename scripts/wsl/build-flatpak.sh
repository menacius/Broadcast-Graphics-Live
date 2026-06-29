#!/usr/bin/env bash
set -Eeuo pipefail

show_help() {
  cat <<'HELP'
Build Broadcast Graphics Live as an OBS Studio Flatpak-compatible plugin.

The build runs through flatpak-builder as an extension of
com.obsproject.Studio, using the KDE Qt SDK selected below. This prevents the
host Ubuntu/WSL glibc from leaking into broadcast-graphics-live.so.

Required arguments:
  --source PATH             Windows project mounted inside WSL
  --workspace PATH          Persistent Linux workspace/build root
  --output-dir PATH         Windows output directory mounted inside WSL

Optional arguments:
  --build-type TYPE         CMake build type (default: Release)
  --package-name NAME       Distribution package name
  --platform NAME           Package platform component
  --archive-format FORMAT   zip, tar.gz, or both (default: both)
  --obs-branch BRANCH       OBS Flatpak branch (default: stable)
  --kde-sdk-version VER     org.kde.Sdk branch (default: 6.8)
  --build-tests             Build and run CTest tests
  --skip-deps               Do not install/update host Flatpak tools/runtimes
  --clean                   Delete Flatpak build state before building
  --install-dir PATH        Also copy the unpacked plugin to this directory
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
OBS_BRANCH="stable"
KDE_SDK_VERSION="6.8"
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
    --obs-branch) OBS_BRANCH=${2:?Missing value for --obs-branch}; shift 2 ;;
    --kde-sdk-version) KDE_SDK_VERSION=${2:?Missing value for --kde-sdk-version}; shift 2 ;;
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

if [[ $(id -u) -ne 0 && $INSTALL_DEPS -eq 1 ]]; then
  echo "Dependency installation requires root." >&2
  exit 4
fi

if [[ $INSTALL_DEPS -eq 1 ]]; then
  echo "=== Installing host Flatpak build tools ==="
  export DEBIAN_FRONTEND=noninteractive
  apt-get update
  apt-get install -y --no-install-recommends \
    flatpak flatpak-builder rsync zip unzip tar gzip ca-certificates

  flatpak remote-add --system --if-not-exists flathub \
    https://dl.flathub.org/repo/flathub.flatpakrepo

  echo "=== Installing OBS and matching Flatpak SDK ==="
  flatpak install --system -y flathub "com.obsproject.Studio//$OBS_BRANCH"
  flatpak install --system -y flathub "org.kde.Sdk//$KDE_SDK_VERSION"
fi

for command_name in flatpak flatpak-builder rsync zip tar gzip; do
  command -v "$command_name" >/dev/null 2>&1 || {
    echo "Required command is unavailable: $command_name" >&2
    exit 5
  }
done

flatpak info --system "com.obsproject.Studio//$OBS_BRANCH" >/dev/null 2>&1 || {
  echo "OBS Flatpak com.obsproject.Studio//$OBS_BRANCH is not installed." >&2
  exit 5
}
flatpak info --system "org.kde.Sdk//$KDE_SDK_VERSION" >/dev/null 2>&1 || {
  echo "Flatpak SDK org.kde.Sdk//$KDE_SDK_VERSION is not installed." >&2
  exit 5
}

arch=$(flatpak --default-arch)
if [[ -z "$PLATFORM" ]]; then
  PLATFORM="flatpak-${arch}"
fi

SOURCE_DIR="$WORKSPACE/source"
BUILDER_DIR="$WORKSPACE/flatpak-builder"
STATE_DIR="$WORKSPACE/.flatpak-builder"
REPO_DIR="$WORKSPACE/repo"
STAGE_DIR="$WORKSPACE/stage"
ARTIFACT_DIR="$WORKSPACE/artifacts"
MANIFEST_PATH="$WORKSPACE/com.obsproject.Studio.Plugin.BroadcastGraphicsLive.yml"
mkdir -p "$SOURCE_DIR" "$OUTPUT_DIR" "$ARTIFACT_DIR"

if [[ $CLEAN -eq 1 ]]; then
  echo "=== Cleaning Flatpak build state ==="
  rm -rf "$BUILDER_DIR" "$STATE_DIR" "$REPO_DIR" "$STAGE_DIR" "$ARTIFACT_DIR" "$SOURCE_DIR"
  mkdir -p "$SOURCE_DIR" "$ARTIFACT_DIR"
fi

echo "=== Synchronizing changed source files into WSL ext4 ==="
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
[[ -z "$prerelease" ]] || version_component+="-${prerelease}"
development_component="development-version-${dev_version}"
package_stem="$(sanitize_component "$PACKAGE_NAME")_$(sanitize_component "$version_component")_$(sanitize_component "$development_component")_$(sanitize_component "$PLATFORM")"

tests_value=OFF
[[ $BUILD_TESTS -eq 0 ]] || tests_value=ON

cat > "$MANIFEST_PATH" <<EOF_MANIFEST
id: com.obsproject.Studio.Plugin.BroadcastGraphicsLive
branch: ${OBS_BRANCH}
runtime: com.obsproject.Studio
runtime-version: ${OBS_BRANCH}
sdk: org.kde.Sdk//${KDE_SDK_VERSION}
build-extension: true
separate-locales: false
appstream-compose: false
build-options:
  prefix: /app/plugins/broadcast-graphics-live
  env:
    CMAKE_BUILD_PARALLEL_LEVEL: "$(nproc)"
modules:
  - name: broadcast-graphics-live
    buildsystem: cmake-ninja
    builddir: true
    config-opts:
      - -DCMAKE_BUILD_TYPE=${BUILD_TYPE}
      - -DCMAKE_INSTALL_LIBDIR=lib
      - -DOBS_BGS_BUILD_TESTS=${tests_value}
    sources:
      - type: dir
        path: source
EOF_MANIFEST

# flatpak-builder resolves local sources relative to the manifest directory.
if [[ $BUILD_TESTS -eq 1 ]]; then
  cat >> "$MANIFEST_PATH" <<'EOF_MANIFEST'
    post-install:
      - ctest --test-dir . --output-on-failure
EOF_MANIFEST
fi

echo "=== Building against OBS Flatpak $OBS_BRANCH with org.kde.Sdk $KDE_SDK_VERSION ==="
mkdir -p "$STATE_DIR"

builder_args=(
  --force-clean
  --state-dir="$STATE_DIR"
  --system
  --repo="$REPO_DIR"
  --default-branch="$OBS_BRANCH"
  "$BUILDER_DIR"
  "$MANIFEST_PATH"
)
flatpak-builder "${builder_args[@]}"

echo "=== Extracting plugin from Flatpak extension build ==="
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR"

plugin_root=$(find "$BUILDER_DIR/files" -type d -path '*/broadcast-graphics-live' -print -quit 2>/dev/null || true)
if [[ -z "$plugin_root" ]]; then
  plugin_binary=$(find "$BUILDER_DIR/files" -type f -name 'broadcast-graphics-live.so' -print -quit 2>/dev/null || true)
  [[ -n "$plugin_binary" ]] || {
    echo "The Flatpak build completed, but broadcast-graphics-live.so was not found." >&2
    find "$BUILDER_DIR/files" -maxdepth 6 -type f | sort >&2 || true
    exit 7
  }
  plugin_root=$(dirname "$(dirname "$(dirname "$plugin_binary")")")
fi
cp -a "$plugin_root" "$STAGE_DIR/broadcast-graphics-live"

PLUGIN_BINARY=$(find "$STAGE_DIR/broadcast-graphics-live" -type f -name 'broadcast-graphics-live.so' -print -quit)
[[ -n "$PLUGIN_BINARY" ]] || { echo "Staged Flatpak plugin binary was not found." >&2; exit 7; }

# Verify that no host /usr libraries leaked into the plugin's version needs.
echo "=== Verifying Flatpak ABI ==="
required_glibc=$(readelf --version-info "$PLUGIN_BINARY" 2>/dev/null | grep -o 'GLIBC_[0-9][0-9.]*' | sort -V | tail -n1 || true)
echo "Highest required glibc symbol: ${required_glibc:-none reported}"
flatpak run --command=sh "org.kde.Sdk//$KDE_SDK_VERSION" -lc \
  'printf "SDK "; ldd --version | head -n1' || true

if [[ -n "$INSTALL_DIR" ]]; then
  echo "=== Installing staged plugin into requested OBS plugin directory ==="
  mkdir -p "$INSTALL_DIR"
  rm -rf "$INSTALL_DIR/broadcast-graphics-live"
  cp -a "$STAGE_DIR/broadcast-graphics-live" "$INSTALL_DIR/"
fi

rm -f "$ARTIFACT_DIR/${package_stem}.zip" "$ARTIFACT_DIR/${package_stem}.tar.gz"
if [[ "$ARCHIVE_FORMAT" == "zip" || "$ARCHIVE_FORMAT" == "both" ]]; then
  (cd "$STAGE_DIR" && zip -q -r "$ARTIFACT_DIR/${package_stem}.zip" broadcast-graphics-live)
  cp -f "$ARTIFACT_DIR/${package_stem}.zip" "$OUTPUT_DIR/"
  sha256sum "$OUTPUT_DIR/${package_stem}.zip" > "$OUTPUT_DIR/${package_stem}.zip.sha256"
  echo "BGL_PACKAGE_ZIP=$OUTPUT_DIR/${package_stem}.zip"
fi
if [[ "$ARCHIVE_FORMAT" == "tar.gz" || "$ARCHIVE_FORMAT" == "both" ]]; then
  tar -C "$STAGE_DIR" -czf "$ARTIFACT_DIR/${package_stem}.tar.gz" broadcast-graphics-live
  cp -f "$ARTIFACT_DIR/${package_stem}.tar.gz" "$OUTPUT_DIR/"
  sha256sum "$OUTPUT_DIR/${package_stem}.tar.gz" > "$OUTPUT_DIR/${package_stem}.tar.gz.sha256"
  echo "BGL_PACKAGE_TARGZ=$OUTPUT_DIR/${package_stem}.tar.gz"
fi

echo "=== Flatpak-compatible BGL build completed successfully ==="
echo "FLATPAK_BUILD_DIR=$BUILDER_DIR"
echo "STAGED_PLUGIN=$STAGE_DIR/broadcast-graphics-live"
