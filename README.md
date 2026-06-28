# Broadcast Graphics Live

<p align="center">
  <img width="520" alt="Broadcast Graphics Live" src="data/icons/broadcast-graphics-live-logo.svg" />
</p>

**Broadcast Graphics Live** is a native C++/Qt graphics plugin for OBS Studio. It combines a dockable title manager, a layered motion-graphics editor, timeline animation, live text and image cueing, reusable templates, GPU-assisted rendering, and native OBS source playback—without browser sources or separate titling software.

**Current build:** `v0.8.5-alpha` · `Development Version 057`

> **Alpha software:** This build is intended for development and testing. Keep backups of important projects and templates. File formats, UI behavior, performance characteristics, and internal APIs may still change before the stable release.

## Development Version 057 — Refined Group Editing, Independent Parenting, and Matte Controls

Group and parenting workflows are now clearly separated: transform parenting no longer creates or modifies Groups, while parent/unparent operations preserve the layer's world-space appearance. Newly created Groups start collapsed, collapsed or keyframe-only Groups behave as single selectable containers, and fully expanded Groups expose their children for direct canvas and Effects-panel editing. Child effect stacks continue to render correctly inside Groups, including mask-aware ordering and affect-behind compositing. The update also prevents parents and Groups from snapping to their own descendants, adds three-state track-matte visibility, introduces three-state Group timeline expansion, aligns hierarchy-aware layer-list columns, and standardizes Parent and Matte selectors with numbered layer names.

## Main features

### Native OBS workflow

- Dockable **Titles and Graphics** manager.
- Native OBS source playback with cue, uncue, persistence, playlist, and activation controls.
- Add titles directly to Preview or Program scenes.
- Live title and cue-state feedback inside the Dock.
- Scene-mask support with balanced OBS active/showing lifecycle handling.

### Layered editor

- Text, Clock, Ticker, Image, Shape, Color Solid, Adjustment, Scene Mask, and Group layers.
- Canvas selection, marquee selection, rulers, guides, snapping, alignment, duplication, clipboard operations, and layer context menus.
- Move, resize, rotate, flip, corner controls, and animatable Free Transform.
- Layer colors reflected in rows, timeline strips, bounding boxes, handles, and related editor chrome.
- Nested Group containers with independent child ordering, collapse/expand states, bounds, transforms, and Effects stacks.
- Collapsed and keyframes-only Groups manipulate as complete containers; fully expanded Groups expose their children.

### Parenting, mattes, and compositing

- Independent transform parenting that does not alter Group membership or layer order.
- World-transform-preserving parent and unparent operations.
- Parent and Matte selectors shown as `<layer number>. <layer name>`.
- Alpha/Luma and Normal/Inverted track-matte modes.
- Three matte visibility states:
  - Hidden and inactive
  - Active as matte only
  - Visible and active as matte
- Parents and Groups do not snap to their own transform children or contained layers.
- Group and single-layer effects share the same destination-aware compositing model.
- Affect-behind effects, adjustment layers, masks, scene masks, blend modes, and expanded effect bounds are supported.

### Text and typography

- Rich text with multiple inline styles in one layer.
- Multiple fills, gradients, strokes, stroke order, and stroke alignment.
- Paragraph alignment, justification, tracking, baseline, horizontal/vertical scale, and paragraph spacing.
- On-canvas text editing with shared caret and selection geometry.
- Auto Text Styling rules and reusable Text Style presets.
- Clock and Ticker layers, including horizontal and vertical ticker layouts.
- Keyframeable ticker **Completion** for deterministic custom playback and prerendering.

### Effects and transitions

- Stackable, reorderable, duplicable, and individually enabled effects.
- Interactive FX master toggle in the layer list.
- Built-in color, blur, shadow, glow, glare, outline, distortion, and compositing effects.
- Group effects operate on the composited child result.
- Effect bounds preserve pixels outside the original artwork bounds.
- Extensible native effect architecture with stable effect identities, package discovery, ABI migration, validation, and lifecycle cleanup.
- Layer and text transitions with easing, per-character/word/sentence processing, and optimized cropped intermediate surfaces.

### Timeline and animation

- Keyframes for transform, appearance, text, effects, masks, and other supported properties.
- Linear, Hold, Ease In, Ease Out, Ease In/Out, and Cubic Bezier interpolation.
- Multi-keyframe selection and editing.
- Work area, in/out points, looping, ping-pong, restart, pause, and end-of-cue behavior.
- Three-state Group caret:
  - Closed
  - Group keyframes only
  - Group keyframes and child layers
- Layer hierarchy, ordering, Group strips, and child counts reflected in the timeline.

### Live text and image cues

- Expose Text, Ticker, Clock, and Image values to the OBS Dock.
- Multi-row cue table with configurable exposed columns.
- Cue, uncue, transition, foreground/background persistence, and unchanged-value persistence.
- Import, append, export, reorder, clear-all, and playlist workflows.
- Optional single-value columns and empty-value suppression.
- Per-row and aggregate cache/progress feedback.

### Templates and presets

- Built-in starter templates and metadata-driven template collections.
- Import/export through `.obgt` title-template files.
- Embedded image assets and preview images.
- Searchable Text Style and Gradient Style libraries.
- Categories, collections, thumbnails, and inline preset application.

## Caching and rendering

Broadcast Graphics Live includes a background frame-cache system designed to keep complex graphics responsive during editing and playback.

- GPU-resident RAM frames with tiled texture reuse and transparent-tile omission.
- RAM allocation configurable from **16 MB** up to **50% of installed physical memory**.
- LZ4-compressed, content-addressed disk tiles and atomic manifest updates.
- Dirty-region invalidation with full-frame fallback for unsafe transform, matte, blur, scene-mask, or parenting cases.
- Priority scheduling for editor, timeline, title, and live-cue rendering.
- Persistent cache reuse when title state and renderer ABI remain compatible.
- GPU text glyph atlases and retained layer targets for supported text paths.
- Bounded transition, blur, image, and texture caches.
- Clock and Ticker layers remain live; the renderer may cache a safe static prefix below the first dynamic output.

Cacheability is reported as **Cacheable**, **Partially cacheable**, or **Non-cacheable**.

## Current status

The project is approaching feature completion but is not yet beta-quality. Remaining work includes UI consistency, broader compatibility testing, performance validation, final feature completion, and systematic bug hunting across the editor, Dock, cache, and OBS playback paths.

The current renderer combines GPU-native paths with compatibility fallbacks for artwork cases that have not yet reached verified parity. Complex rich text, masks, effects, motion blur, large images, high resolutions, and long timelines can still require substantial CPU, RAM, GPU, and disk resources.

## Basic workflow

1. Open **Broadcast Graphics Live** from OBS.
2. Create or import a title.
3. Build the design with layers, Groups, effects, parenting, mattes, and animation.
4. Expose editable Text, Clock, Ticker, or Image values to the Dock when needed.
5. Save the title and add it to an OBS scene.
6. Cue a title or live-data row, then control playback from the Dock or OBS source controls.
7. Export reusable designs as templates.

## Requirements

- CMake 3.16 or newer
- C++17 compiler
- OBS Studio development files containing `libobs`, `obs-frontend-api`, and OBS headers
- Qt 5.15 or Qt 6 with Core, Widgets, and SVG
- Cairo, Pango, PangoCairo, and LZ4
- `pkg-config` where available

CMake also resolves or pins `nlohmann/json`, Qt-Color-Widgets, and LZ4 when a suitable system target is unavailable. A clean configuration may therefore require network access.

## Building

Clone the repository:

```bash
git clone https://github.com/menacius/Broadcast-Graphics-Live.git
cd Broadcast-Graphics-Live
```

### Windows

The recommended setup uses Visual Studio 2022, CMake, vcpkg, and an OBS SDK/plugin-deps tree.

```powershell
vcpkg install cairo pango[fontconfig] qt6-base qt6-svg lz4 --triplet x64-windows

$env:VCPKG_ROOT = "C:\vcpkg"
$env:OBS_SDK_DIR = "C:\path\to\obs-plugin-deps-or-obs-studio-sdk"

.\build-windows.ps1 -ObsSdkDir $env:OBS_SDK_DIR -Configuration RelWithDebInfo -BuildTests
```

The helper uses a manifest installation directory outside the project build path. Override it with `-VcpkgInstalledDir` or `OBS_BGS_VCPKG_INSTALLED_DIR`; the selected path must not contain whitespace.

Manual configuration:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DVCPKG_INSTALLED_DIR="$env:VCPKG_ROOT/manifest-installed/broadcast-graphics-live" `
  -DOBS_SDK_DIR="$env:OBS_SDK_DIR" `
  -DOBS_BGS_BUILD_TESTS=ON

cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Linux

Example Debian/Ubuntu dependencies:

```bash
sudo apt install \
  cmake ninja-build pkg-config libobs-dev \
  qt6-base-dev qt6-svg-dev \
  libcairo2-dev libpango1.0-dev liblz4-dev
```

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBS_BGS_BUILD_TESTS=ON

cmake --build build
ctest --test-dir build --output-on-failure
```

Use `-DOBS_SDK_DIR=/path/to/obs-sdk-or-plugin-deps` when OBS CMake packages are not installed in a standard prefix.

### macOS

```bash
brew install cmake ninja pkg-config qt cairo pango lz4

cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DOBS_SDK_DIR=/path/to/obs-sdk-or-build-tree

cmake --build build
```

The final bundle/install layout depends on the OBS build and packaging workflow used by the developer.

## Plugin layout

CMake stages a directly copyable plugin directory at:

```text
build/broadcast-graphics-live/
├── bin/64bit/
│   └── broadcast-graphics-live.dll
└── data/
    ├── effects/
    ├── effect-transitions/
    ├── icons/
    └── locale/
```

The Windows helper copies required runtime DLLs into the plugin binary directory.

## Project structure

```text
src/
├── cache/       Background prerender and frame-cache system
├── canvas/      Editor canvas, interaction, transforms, and inline text
├── core/        Title data, serialization, rendering contracts, and build info
├── editor/      Main editor, Dock integration, properties, templates, and presets
├── effects/     Built-in effects and extension support
├── layers/      Layer model and hierarchy UI
├── obs/         OBS source and plugin integration
├── text/        Text shaping, layout, styling, and GPU text support
└── timeline/    Timeline, keyframes, animation, and playback controls
```

## Tests

Enable structural and contract tests with:

```bash
cmake -B build -DOBS_BGS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The test suite covers selected serialization, hierarchy, parenting, matte, Group, cache, extension, canvas, compositing, and build-identity contracts. Full runtime validation inside OBS is still required before release builds.

## Versioning

- **`v0.8.x-alpha`** — active feature development and UI integration.
- **`v0.9.0-beta.n`** — feature-complete testing, polish, compatibility, and performance work.
- **`v0.9.0-rc.n`** — release candidates under feature freeze.
- **`v1.0.0`** — first stable compatibility baseline.

The public and development versions are defined in the root `CMakeLists.txt` and exposed through `src/core/build-info.h` to the About dialog and diagnostic logs.

## License and third-party components

The project license is provided in [`LICENSE`](LICENSE). Binary redistributors must also comply with the licenses of OBS Studio, Qt, Cairo, Pango, LZ4, nlohmann/json, Qt-Color-Widgets, and any other bundled runtime dependencies or assets.

## Contributing

Bug reports and focused pull requests are welcome. Include the OBS version, operating system, GPU, exact reproduction steps, relevant logs, and a minimal title/template whenever possible.
