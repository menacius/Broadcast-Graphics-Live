# OBS Graphics Studio Pro

<img width="1920" height="1044" alt="Screenshot 2026-06-21 012849" src="https://github.com/user-attachments/assets/3e01f3f6-2b03-400c-bf14-5a1c44cc6e29" />

**OBS Graphics Studio Pro** is a native C++/Qt graphics plugin for OBS Studio. It combines a dockable title manager, a layered motion-graphics editor, timeline animation, live text and image cueing, template workflows, and native OBS source playback—without relying on browser sources or separate titling software.

> [!WARNING]
> The project is under active **alpha development**. Core authoring, serialization, playback, live-cue, and caching workflows are implemented, but file formats, UI behavior, and internal APIs may still change. Keep backups of important title libraries and templates.

OBS Graphics Studio Pro is an independent third-party project and is not affiliated with or endorsed by the OBS Project.

---

## Highlights

### Native OBS integration

- Native OBS input source: **OBS Graphics Studio Pro Title**.
- Dockable title and template manager inside OBS Studio.
- Add the selected title directly to the active scene.
- Scene-collection-specific title libraries.
- Native rendering and playback through `libobs`.
- Scene-mask support for using OBS scenes as animated mask inputs.
- Persistent plugin preferences and title metadata.

### Layered editor and canvas

- Non-modal Qt editor with canvas, tools, layers, properties, effects, styles, and timeline panels.
- Layer types:
  - Text
  - Clock
  - Ticker
  - Image
  - Solid rectangle
  - Vector shape
- Shape primitives:
  - Rectangle
  - Rounded rectangle
  - Ellipse
  - Triangle
  - Star
  - Polygon
  - Diamond
  - Line
- Direct canvas selection, movement, resizing, rotation, anchor/origin editing, and multi-selection.
- Photoshop-style rulers, draggable guides, safe guides, snapping, zoom, and pan.
- Layer visibility, locking, duplication, ordering, parent/child transforms, blend modes, and track-matte-style masks.
- Alpha, inverted alpha, luma, and inverted luma masks.

### Typography and rich text

- Structured rich-text document model with inline formatting.
- Direct on-canvas text editing.
- Font family, style, size, weight, italic, underline, strikethrough, kerning, tracking, leading, baseline shift, character scaling, paragraph spacing, indentation, alignment, and overflow controls.
- Inline text styles, mixed formatting, gradients, and preset application.
- Clock layers with configurable time formats.
- Ticker layers with:
  - Horizontal scrolling
  - Vertical line-by-line movement
  - Vertical smooth scrolling
- Rule-based automatic text styling for reusable live graphics.

### Images, fills, gradients, and strokes

- Image layers with independent image size and image-box size.
- Image fit, fill, stretch, long-side, and short-side layout modes.
- Internal image anchoring, optional clipping/cropping, aspect-ratio controls, and scalable filtering.
- Bilinear, bicubic, Lanczos, and area image filters.
- Solid and gradient fills.
- Linear, radial, angle, reflected, and diamond gradients.
- Editable gradient stops, opacity, position, angle, center, focal point, and scale.
- Outer, centered, and inner stroke alignment.
- Per-side padding and independent corner radii/types where supported.
- Placeholder rendering for image boxes without an assigned asset.

### Effects and compositing

Stackable effects currently represented by the editor and renderer include:

- Background Color
- Outline
- Drop Shadow
- Long Shadow
- Brightness & Contrast
- Saturation
- Color Overlay
- Glow
- Inner Glow
- Inner Shadow
- Blur
- Motion Blur
- Bloom
- Emboss

Supported layer/effect blend modes include Normal, Multiply, Additive, Screen, Overlay, and Color.

### Timeline and animation

- Layer-based timeline with in/out ranges, playhead, transport controls, switches, parenting, masks, and cache state display.
- Keyframeable transform, opacity, typography, color, shape, image, background, shadow, and effect properties.
- Scalar and two-dimensional animated properties.
- Easing modes:
  - Linear
  - Ease In
  - Ease Out
  - Ease In/Out
  - Cubic Bezier
  - Hold
- Playback modes:
  - Play once
  - Loop between in/out points
  - Pause at a defined position
- Restart and ping-pong loop behavior.
- End-of-cue options:
  - Show last frame
  - Show nothing
  - Show first frame

### Live text and image cues

- Expose text, ticker, and image layers to the OBS dock.
- Multi-row cue table with reorderable exposed columns.
- Text and image values can be changed without editing the title design.
- Cue, uncue, row-to-row transition, foreground/background persistence, and unchanged-text persistence workflows.
- Optional **Do not show if empty** behavior.
- Optional shared **Single value** columns.
- Per-title playlist controls, including loop, reverse order, hold duration, restart on source activation, and stop on source deactivation.
- Import, append, and export workflows for cue data.
- Per-row and aggregate cache/progress feedback.

### Templates and style presets

- Built-in starter templates.
- Import and export through `.ogspt` title-template files.
- Template metadata:
  - Title
  - Description
  - Creator
  - Creation date
  - Preview image
- Embedded image assets in exported templates.
- Manual preview screenshot capture.
- Searchable text-style and gradient-style libraries.
- Preset categories, thumbnails, import/export, and inline rich-text application.

---

## Caching and prerendering

OBS Graphics Studio Pro includes a background frame-cache system intended to keep complex titles and live cues responsive during playback.

### Cache architecture

- Raw image payloads in the RAM cache.
- Optional disk cache with LZ4-compressed frame data.
- Configurable RAM limit and disk-cache location.
- Background render queue with title, timeline, editor, and live-cue priorities.
- Work-area and full-timeline prerendering.
- Live-cue frame preparation before playback.
- Per-frame states for queued, rendering, RAM-resident, disk-resident, stale, and disabled content.
- Content hashes, frame-state tracking, invalidation, payload sharing, and sparse alpha-bounded cached frames.
- Cache data can persist between editor sessions when the underlying title state remains valid.

### Clock and ticker titles

Clock and Ticker layers remain live and are never baked into cached pixels.

When possible, the renderer caches the largest z-order-safe static prefix below the first runtime-dynamic output. The Clock/Ticker layer and all layers above that boundary continue rendering live. Dynamic dependencies through parenting and track mattes are included in the cacheability analysis.

Current cacheability states are:

- **Cacheable** — the complete title can be cached.
- **Partially cacheable** — a safe static prefix can be cached while the dynamic suffix remains live.
- **Non-cacheable** — the first rendered output is dynamic, so the current single-prefix strategy cannot provide a useful cached underlay.

The current implementation uses one cached prefix rather than multiple independent cache islands. See [`docs/clock-ticker-partial-frame-cache.md`](docs/clock-ticker-partial-frame-cache.md) for details.

---

## Current status and limitations

- The project is an alpha and is not yet recommended as the only copy of production-critical graphics.
- The primary cross-platform composition and text-rendering path uses Cairo, Pango, and PangoCairo.
- GPU effect-pipeline infrastructure exists, but the migration of all rendering and effects to a fully GPU-native path is not complete.
- Partial dynamic caching currently supports one safe static prefix below the first dynamic output.
- Complex rich text, masks, effects, motion blur, large images, and high-resolution timelines can still require significant CPU, RAM, GPU, and disk resources.
- Template and title schemas may evolve before a stable release.
- Automated coverage currently focuses on selected model behavior rather than the complete OBS/editor integration surface.

---

## Requirements

### Build requirements

- CMake 3.16 or newer
- C++17 compiler
- OBS Studio development files containing:
  - `libobs`
  - `obs-frontend-api`
  - OBS headers
- Qt 5.15 or Qt 6 with:
  - Core
  - Widgets
  - SVG
- Cairo
- Pango
- PangoCairo
- `pkg-config` where available

### Dependencies resolved by CMake

The build configuration pins or resolves the following libraries:

- `nlohmann/json` 3.11.3
- Qt-Color-Widgets at commit `8491078434b24cba295b5e41cc0d2a94c7049a5b`
- LZ4 1.10.0 source at commit `ebb370ca83af193212df4dcbadcc5d87bc0de2f0` when a system/vcpkg LZ4 target is unavailable

A clean configuration may therefore require network access unless the dependency sources are already present in the CMake dependency cache or supplied by a dependency provider.

---

## Building

Clone the repository:

```bash
git clone https://github.com/menacius/OBS-Graphics-Studio-Pro.git
cd OBS-Graphics-Studio-Pro
```

### Windows

The recommended Windows workflow uses Visual Studio 2022, CMake, vcpkg, and an OBS SDK/plugin-deps tree.

Install the main vcpkg dependencies:

```powershell
vcpkg install cairo pango[fontconfig] qt6-base qt6-svg lz4 --triplet x64-windows
```

Set the required paths:

```powershell
$env:VCPKG_ROOT = "C:\vcpkg"
$env:OBS_SDK_DIR = "C:\path\to\obs-plugin-deps-or-obs-studio-sdk"
```

Build and install with the helper script:

```powershell
.\build-windows.ps1 -ObsSdkDir $env:OBS_SDK_DIR
```

Build with validation tests:

```powershell
.\build-windows.ps1 `
  -ObsSdkDir $env:OBS_SDK_DIR `
  -Configuration RelWithDebInfo `
  -BuildTests
```

Build without copying the result into OBS:

```powershell
.\build-windows.ps1 `
  -ObsSdkDir $env:OBS_SDK_DIR `
  -SkipInstall
```

Use `-InstallRoot` to target a portable or custom OBS plugins directory.

Manual configuration:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DOBS_SDK_DIR="$env:OBS_SDK_DIR" `
  -DOBS_GSP_BUILD_TESTS=ON

cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

### Linux

Package names for OBS development files differ between distributions. Install the OBS SDK/development headers, Qt, Cairo, Pango, CMake, Ninja, and `pkg-config`.

Example Debian/Ubuntu-oriented dependency set:

```bash
sudo apt install \
  cmake ninja-build pkg-config \
  libobs-dev \
  qt6-base-dev qt6-svg-dev \
  libcairo2-dev libpango1.0-dev liblz4-dev
```

Configure and build:

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBS_GSP_BUILD_TESTS=ON

cmake --build build
ctest --test-dir build --output-on-failure
```

When the OBS CMake packages are not installed in a standard prefix, point CMake at an OBS SDK or build tree:

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOBS_SDK_DIR=/path/to/obs-sdk-or-plugin-deps
```

Install into the user plugin root:

```bash
cmake --install build --prefix "$HOME/.config/obs-studio/plugins"
```

### macOS

Install the general dependencies:

```bash
brew install cmake ninja pkg-config qt cairo pango lz4
```

Configure against an OBS source, SDK, or compatible build tree:

```bash
cmake -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DOBS_SDK_DIR=/path/to/obs-sdk-or-build-tree

cmake --build build
```

The exact macOS bundle/install arrangement depends on the OBS build tree and packaging workflow used by the developer.

---

## Plugin layout

CMake stages a directly copyable plugin directory at:

```text
build/obs-graphics-studio-pro/
├── bin/
│   └── 64bit/
│       ├── obs-graphics-studio-pro.dll
│       └── dependency DLLs on Windows
└── data/
    ├── effects/
    ├── icons/
    └── locale/
```

Typical Windows installation:

```text
C:\ProgramData\obs-studio\plugins\obs-graphics-studio-pro\
├── bin\64bit\
│   ├── obs-graphics-studio-pro.dll
│   ├── Qt6Core.dll
│   ├── Qt6Gui.dll
│   ├── Qt6Widgets.dll
│   ├── Qt6Svg.dll
│   ├── cairo.dll
│   ├── pango-1.0.dll
│   ├── pangocairo-1.0.dll
│   └── other required runtime DLLs
└── data\
    ├── effects\
    ├── icons\
    └── locale\
```

The Windows build helper copies vcpkg runtime DLLs into the plugin binary directory. Binary redistributors must also include the applicable third-party license notices described below.

---

## Basic workflow

1. Open the **OBS Graphics Studio Pro** dock in OBS Studio.
2. Create a blank title, select a starter template, or import an `.ogspt` template.
3. Open the editor and build the composition with layers, text, images, shapes, masks, effects, and keyframes.
4. Expose text, ticker, or image layers to the dock when the design needs operator-controlled live values.
5. Add cue rows and optionally enable caching or playlist playback.
6. Capture or update the title preview image.
7. Add the selected title to the active OBS scene.
8. Cue, uncue, or automate rows from the dock while the native OBS source remains on air.

---

## Data and file formats

### Scene-collection title storage

Title libraries are saved per OBS scene collection below the plugin configuration directory:

```text
obs-graphics-studio-pro/
└── scene-collection-titles/
    └── <sanitized-scene-collection-name>-<hash>.json
```

Typical plugin configuration roots are:

```text
Windows:
%APPDATA%\obs-studio\plugin_config\obs-graphics-studio-pro\

Linux:
~/.config/obs-studio/plugin_config/obs-graphics-studio-pro/

macOS:
~/Library/Application Support/obs-studio/plugin_config/obs-graphics-studio-pro/
```

The exact OBS configuration root can vary with portable installations and custom builds.

Writes use an atomic replacement path so an interrupted save does not intentionally overwrite the last valid title file with a partial document.

### Template files

`.ogspt` exports use the format identifier:

```text
obs-graphics-studio-pro-title-template
```

The current export schema writes version `3` and can contain:

- Title and layer data
- Template metadata
- Preview PNG data
- Embedded image assets
- Live-cue structure
- Animation, masks, gradients, effects, and editor defaults

Imported templates receive new title/layer identifiers so they can coexist with the original design.

---

## Architecture

```text
OBS-Graphics-Studio-Pro/
├── CMakeLists.txt
├── LICENSE
├── README.md
├── build-windows.ps1
├── data/
│   ├── effects/          # OBS shader/effect assets
│   ├── icons/            # UI SVG assets
│   └── locale/           # OBS/Qt localization strings
├── docs/                 # Architecture and implementation notes
├── tests/                # Focused C++ model tests
└── src/
    ├── cache/            # RAM/disk cache, state tracking, queue, prerender UI
    ├── canvas/           # Canvas interaction, preview, snapping, inline text
    ├── core/             # Title storage, serialization, preferences, logging
    ├── editor/           # Dock, editor shell, properties, styles, tools
    ├── effects/          # Effect model and effect-stack UI
    ├── layers/           # Layer model, image helpers, layer stack
    ├── obs/              # OBS module entry point and native source
    ├── rendering/        # Effect registry and GPU pipeline foundations
    ├── text/             # Rich-text model and helpers
    └── timeline/         # Animation model and timeline widget
```

Key components:

| Component | Role |
|---|---|
| `TitleSource` | Native OBS input source and runtime title renderer |
| `TitleDock` | Title library, live-cue table, playlist, cache status, and scene actions |
| `TitleEditor` | Non-modal authoring environment |
| `CanvasPreview` | Canvas rendering, transforms, guides, snapping, masks, and inline editing |
| `PropertiesPanel` | Layer, typography, image, shape, mask, and effect controls |
| `TimelineWidget` | Layer timing, transport, parenting, keyframes, and cache visualization |
| `TitleDataStore` | Scene-collection-specific title ownership and atomic persistence |
| `CacheManager` | Frame caching, invalidation, background scheduling, and live-cue preparation |

See [`docs/module-architecture.md`](docs/module-architecture.md) for the current ownership and dependency map.

---

## Tests

Enable the lightweight validation targets with:

```bash
-DOBS_GSP_BUILD_TESTS=ON
```

Current CTest targets include:

- `rich_text_model_test`
- `animation_model_test`

Run them with:

```bash
ctest --test-dir build --output-on-failure
```

For multi-configuration generators such as Visual Studio:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

---

## Contributing

Bug reports, focused pull requests, build fixes, documentation improvements, and reproducible performance reports are welcome.

When changing the project:

- Keep UI strings in `data/locale/en-US.ini`.
- Put implementation notes in `docs/`.
- Update serialization for every persistent model change.
- Keep editor preview and OBS source behavior consistent.
- Add or update tests for model-level changes where practical.
- Preserve all upstream copyright and license notices.
- Do not add external assets, source files, or dependencies without recording their origin and license.

Use the repository issue tracker for defects and feature proposals:

- <https://github.com/menacius/OBS-Graphics-Studio-Pro/issues>

---

## Project license

The repository includes the **GNU General Public License, version 3** as its project license. Unless an individual file or asset states different terms, project-authored source code is distributed under **GPL-3.0-only**.

See [`LICENSE`](LICENSE) for the complete license text.

The software is provided without warranty, as described by the GPL.

---

## Third-party software and assets

OBS Graphics Studio Pro uses, links to, fetches, or redistributes components owned by other projects. Those components remain under their own licenses.

| Component | Use in OBS Graphics Studio Pro | Upstream license / notice |
|---|---|---|
| [OBS Studio](https://github.com/obsproject/obs-studio) | Runtime host, `libobs`, graphics API, and frontend dock API | GPL-2.0-or-later; see the upstream `COPYING` file |
| [Qt](https://www.qt.io/) Core, Widgets, and SVG | Dock, editor, widgets, SVG rendering, and application UI | Open-source Qt is generally available under LGPL-3.0 and/or GPL-3.0, with commercial licensing also available; verify the exact Qt package and module terms used for distribution |
| [Cairo](https://www.cairographics.org/) | 2D composition and raster rendering | LGPL-2.1 or MPL-1.1, at the recipient's option |
| [Pango](https://docs.gtk.org/Pango/) and [PangoCairo](https://docs.gtk.org/PangoCairo/) | Text layout and Cairo text rendering | LGPL-2.1-or-later |
| [GLib](https://docs.gtk.org/glib/) and [GObject](https://docs.gtk.org/gobject/) | Runtime dependencies used by the Pango stack and fallback CMake linking | LGPL-2.1-or-later |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) | Text shaping dependency used by the Pango stack | Old MIT-style license; see upstream `COPYING` |
| [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 | JSON serialization and template/title persistence | MIT; retain the upstream `LICENSE.MIT` and notices in `LICENSES/` for included third-party portions |
| [Qt-Color-Widgets](https://gitlab.com/mattbas/Qt-Color-Widgets) | Color wheel, selectors, palettes, and dialogs; fetched at commit `8491078434b24cba295b5e41cc0d2a94c7049a5b` and compiled as a static library | LGPL-3.0-or-later; copyright Mattia Basaglia and contributors |
| [LZ4](https://github.com/lz4/lz4) 1.10.0 | Disk frame-cache compression; fetched at commit `ebb370ca83af193212df4dcbadcc5d87bc0de2f0` when no suitable system target is available | BSD-2-Clause; copyright Yann Collet and contributors |
| [Font Awesome Free](https://fontawesome.com/) 6 | SVG artwork used by the editor and dock icons | SVG icons are licensed under CC BY 4.0; copyright Fonticons, Inc. |

Font Awesome attribution is also documented in [`data/icons/README.md`](data/icons/README.md), and individual imported SVGs include source/license comments where applicable.

### Redistribution notes

When distributing compiled builds:

1. Include this project's `LICENSE`.
2. Include the copyright notices and full license texts required by every bundled library and asset.
3. Preserve Font Awesome attribution for the included SVG artwork.
4. Preserve the Qt-Color-Widgets LGPL notice, particularly because the current CMake build compiles it statically into the plugin.
5. Preserve the nlohmann/json MIT notice and its upstream third-party notices.
6. Preserve the LZ4 BSD-2-Clause notice when the bundled/pinned implementation is used.
7. Include notices for Qt, Cairo, Pango, GLib, HarfBuzz, and any other runtime DLLs copied into a binary package.
8. Review the exact licenses of the versions produced by your package manager, OBS SDK, or Qt distribution; those packages may contain additional third-party components not enumerated here.

This section is an attribution summary, not a replacement for the complete upstream license texts or legal advice.

---

## Acknowledgements

OBS Graphics Studio Pro is built on the work of the OBS Project, The Qt Company and Qt contributors, the Cairo and GNOME/Pango communities, the HarfBuzz project, Niels Lohmann and contributors, Mattia Basaglia and contributors, Yann Collet and LZ4 contributors, and Fonticons, Inc.

Their projects make native, cross-platform broadcast graphics inside OBS possible.
