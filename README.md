# Broadcast Graphics Live

<img width="1919" height="1040" alt="Screenshot 2026-06-25 025251" src="https://github.com/user-attachments/assets/70126f07-1a3d-4d56-ae29-a680e7dd52d0" />

**Broadcast Graphics Live** is a native C++/Qt graphics plugin for OBS Studio. It combines a dockable title manager, a layered motion-graphics editor, timeline animation, live text and image cueing, template workflows, and native OBS source playback—without relying on browser sources or separate titling software.

**Current development version: `v0.8.3-alpha`**

> [!WARNING]
> `v0.8.3-alpha` is an advanced development build, not a production-stable release. The main authoring, serialization, playback, live-cue, caching, vector-editing, and template workflows are implemented, but several planned features, UI refinement, compatibility testing, and systematic bug hunting remain before beta. File formats, UI behavior, and internal APIs may still change. Keep backups of important title libraries and templates.

Broadcast Graphics Live is an independent third-party project and is not affiliated with or endorsed by the OBS Project.

## Development approach

This project is the result of **vibe coding**. **[Antonios Dimopoulos](https://web.facebook.com/menacius)** defines the product vision, broadcast workflows, desired behavior, interface decisions, architecture goals, and acceptance criteria, while AI-assisted development tools help translate those requirements into C++/Qt implementation. He knows considerably more about what he wants to build than he does about C++.

That development model makes validation especially important. This alpha should be treated as experimental: code changes need diff review, structural and build checks, focused automated tests, and real OBS runtime testing before they can be considered reliable for production use.

## What changed in `v0.8.3-alpha`

Compared with the current GitHub `main` snapshot, this development archive advances the unified GPU pipeline through the Phase 12D–15 work:

- The project is fully renamed to **Broadcast Graphics Live**, including application branding, plugin/package identifiers, settings scopes, source IDs, namespaces, build variables, documentation, and the `.obgt` title-template extension.
- Broadcast Graphics title templates use `.obgt`; effect presets use `.obgeffect`; text transitions use `.obgtranst`; and general transitions use `.obgtransg`.
- The first-launch editor workspace now matches the broadcast layout reference: compact tools on the far left, title properties/color libraries/effects on the left, layer properties on the right, and a full-width bottom timeline.
- Cache, diagnostic file logging, and editor Adaptive Resolution are disabled by default; each remains available as an explicit user option.
- The About dialog now displays the supplied Broadcast Graphics Live logo. Its `#dddddd` artwork color is resolved at paint time from the active OBS theme icon color, while the SVG asset itself remains unchanged on disk.
- The editor text canvas uses the same immutable layout and GPU SDF text artwork path as the OBS source, including GPU-derived caret/selection geometry and corrected outer/mid/inner text strokes with independent Behind/Front ordering.
- Alpha, inverted-alpha, luma, and inverted-luma track mattes moved into the shared GPU graph, including nested matte dependencies, scene masks, effects-before/after-mask ordering, and bounded retained matte textures.
- Prerendered RAM frames are reconstructed directly from shared, content-addressed 128×128 GPU tile textures. SSD output uses final-frame-only triple-buffered staging, a separate writer queue, and content-addressed LZ4-compressed 256×256 tiles shared between frames.
- RAM allocation is dynamically clamped from 16 MB up to 50% of installed physical memory, according to the user preference.
- Phase 15 runtime visibility recovery restored the last known working Phase 14 artwork path after premature legacy removal caused text, vectors, masks, and effects to disappear. Cache ABI v24 invalidates blank frames from that failed renderer generation.
- The standalone Line primitive was removed from the shape-tool list; line artwork remains available through open Pen paths.

---

## Highlights

### Native OBS integration

- Native OBS input source: **Broadcast Graphics Live Title**.
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
- Direct canvas selection, movement, resizing, rotation, anchor/origin editing, and multi-selection.
- Pen Tool for open and closed straight or cubic Bézier paths.
- Direct Selection Tool for anchor points, Bézier direction handles, marquee point selection, live-corner editing, and compound-path contours.
- Context-sensitive toolbar controls for transforms, selected path points, and multi-shape operations.
- Vector boolean operations for two or more selected shape layers: **Unite**, **Subtract Front**, **Intersect**, and **Exclude**.
- Boolean results remain editable as Path layers, preserve compound contours and holes, retain parametric rounded shapes when possible, and otherwise convert curved boundaries to cubic Bézier segments. Gradient mapping is preserved in canvas space when the result bounds change.
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
- Linear, radial, and conical/angle gradients, with pad, reflected, and repeating spread modes. Legacy reflected and diamond preset data is normalized on load.
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

The editor also includes an **Effects & Presets** dock with an After Effects-style virtual folder tree and instant search. All `.obgeffect`, `.obgtranst`, and `.obgtransg` files live together in one physical library directory; each file declares its visible category path in its own slash-separated `category` metadata. The browser builds nested virtual folders from those paths and omits empty branches, so presets can be reorganized without moving files on disk.

Effects can be applied by drag-and-drop onto a timeline layer strip, directly onto a canvas layer, or into the Effects stack for the selected layer. A common effect factory keeps button-based and drag-and-drop creation consistent.

### Transitions

- Independent **In** and **Out** transition slots on every compatible layer.
- Premiere-style transition overlays at the beginning and end of timeline strips.
- Drag-and-drop from **Effects & Presets** directly onto either strip edge.
- Timeline resizing controls the transition duration.
- Double-click opens a transition editor with duration, easing, direction, offset, scale, blur radius, wipe softness, text-unit, stagger, and reverse-order controls where applicable.
- Animated previews use an **A → B** demonstration for general transitions and **Abc De** for text transitions.
- Transition overlays and empty In/Out targets are selectable timeline items with Copy, Cut, Paste, Delete, context-menu, and keyboard support.
- Pasting or dropping onto an occupied slot replaces the existing transition while preserving the copied preset's complete configuration.
- Text transitions can animate by grapheme/character, word, or sentence while preserving text shaping, kerning, tracking, ligatures, outlines, shadows, and overlapping glyph bounds.
- Text blur transitions animate the actual blur radius down to zero rather than compositing a sharp glyph over a blurred halo.
- General transition primitives include dissolve/fade, opacity and blur, scale change, directional slide, blur slide, wipe with feathering, and zoom blur.
- Text transition primitives include fade, slide, blur slide, blur, scale, and wipe, with configurable grouping and direction instead of duplicated directional/unit presets.
- `.obgtranst` is reserved for text transitions and `.obgtransg` for general transitions; category metadata is validated against the file type.

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

<img width="1920" height="1044" alt="Screenshot 2026-06-25 025718" src="https://github.com/user-attachments/assets/9f409f1f-aafe-419f-a4a7-c735a80cac1d" />

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
- Import and export through `.obgt` title-template files.
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

Broadcast Graphics Live includes a background frame-cache system intended to keep complex titles and live cues responsive during playback.

### Cache architecture

- GPU-resident RAM frames reference shared, content-addressed 128×128 OBS tile textures; transparent tiles are omitted and live/editor consumers reconstruct frames directly on the GPU without a routine CPU image round-trip.
- The RAM preference is clamped dynamically between **16 MB** and **50% of installed physical memory**.
- The disk tier stores small `.ogsf` frame manifests that reference independently LZ4-compressed, SHA-256-addressed 256×256 `.ogst` BGRA tiles.
- Fully transparent tiles are omitted, while identical non-empty tiles can be shared across frames and titles in the active cache directory.
- Final SSD readback uses a three-slot staging ring. Only the completed frame or a safe final dirty region is mapped; layers, masks, effects, and other intermediate surfaces are not read back.
- LZ4 compression, temporary-file writes, atomic replacement, manifest updates, and byte accounting run on a dedicated writer queue rather than the GPU render loop.
- Dirty-region invalidation expands animated bounds for effects and falls back to full-frame staging for rotations, parented graphs, track mattes, scene masks, blur/halo effects, and other cases where a local rectangle is unsafe.
- A bounded compatibility `QImage` hydration path remains for SSD-loaded frames whose GPU texture has been evicted.
- Background render scheduling covers title, timeline, editor, and live-cue priorities, including work-area and full-timeline prerendering.
- Per-frame states track queued, rendering, GPU-RAM-resident, disk-resident, stale, and disabled content.
- Content hashes, renderer ABI versions, state tracking, invalidation, payload sharing, and startup index validation prevent incompatible or orphaned cache data from being reused.
- Cache data can persist between editor sessions when the title state and renderer ABI remain valid.
- Phase 12C text layers retain bounded R8 SDF glyph-atlas pages and their last valid layer target, so normal frame presentation does not rerun full `QTextDocument`/`QPainter` text rasterization.
- Transition rendering avoids work entirely for layers without an active transition.
- Per-character/word/sentence transition surfaces are cropped to their actual visual bounds instead of allocating full-layer images per unit.
- Blur variants and other transition caches are bounded by entry count and memory size to prevent unbounded growth during live cueing or prerendering.
- Preset scanning, MIME payload handling, preview refresh, and filesystem watching include validation and workload limits to avoid UI stalls and malformed-input regressions.

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

- The current release line is **`v0.8.3-alpha`**. The application is approaching feature completion, but it is not yet beta-quality or recommended as the only copy of production-critical graphics.
- Remaining pre-beta work includes the final planned features, UI consistency and visual polish, broader workflow validation, performance verification, and focused bug hunting across editor, dock, cache, and OBS playback paths.
- Supported Text and Clock layers use the Phase 12C/12D GPU text backend: immutable shaped glyph data feeds bounded R8 SDF atlas pages, GPU glyph quads, multiple per-range fills/gradients/strokes, globally correct Behind/Front stroke composition, persistent double-buffered layer textures, and shared editor caret/selection geometry.
- The Phase 13 GPU mask graph handles alpha/luma variants, nested track-matte dependencies, scene masks, effect ordering, parent transforms, and bounded retained matte textures without CPU mask compositing.
- The Phase 14 cache path keeps prerendered RAM frames GPU-resident and performs SSD readback only at the final-frame boundary through triple-buffered staging and a tiled content-addressed disk format.
- Phase 15 intentionally retains the last known working Phase 14 artwork pipeline. A first attempt to remove all legacy artwork paths was rolled back after real runtime output showed only image layers; complete removal remains gated on verified parity for text, images, vectors, masks, and effects in editor, OBS live output, and cached playback.
- Cairo/Pango, Qt raster adapters, and `render_layer_unmasked()` compatibility branches therefore remain for unsupported or not-yet-migrated artwork cases, including color-font glyphs, Ticker output, active per-character/word/sentence text transitions, and runtime fallback recovery.
- The unified GPU compositor handles transforms, masks, effects, blending, temporal motion blur, preview/program presentation, GPU RAM frames, and final disk-cache readback, but the migration of every source adapter to a fully GPU-native representation is not complete.
- Partial dynamic caching currently supports one safe static prefix below the first dynamic output.
- Complex rich text, masks, effects, motion blur, large images, and high-resolution timelines can still require significant CPU, RAM, GPU, and disk resources.
- Template and title schemas may evolve before a stable release.
- Automated coverage currently focuses on selected model behavior rather than the complete OBS/editor integration surface.
- The Effects & Presets and transition implementation has undergone a baseline-comparison audit for ownership, bounded caching, per-frame work, serialization, drag-and-drop conflicts, clipboard behavior, and UI refresh paths; complete in-OBS regression testing is still required for release builds.

---

## Versioning and release maturity

The project follows Semantic Versioning for public development builds:

- **`v0.8.x-alpha`** — completion of the remaining planned feature set, editor integration work, and active UI changes.
- **`v0.9.0-beta.n`** — feature-complete testing phase focused on UI polish, compatibility, performance, and bug fixing.
- **`v0.9.0-rc.n`** — release-candidate builds under feature freeze, with only release-blocking fixes accepted.
- **`v1.0.0`** — first stable release with a documented compatibility and migration baseline.

The version displayed by the About dialog and plugin logs is generated from the root `CMakeLists.txt`. Update the numeric `project(... VERSION ...)` value and the `OBS_BGS_PRERELEASE` channel together when preparing a release.

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
git clone https://github.com/menacius/Broadcast-Graphics-Live.git
cd Broadcast-Graphics-Live
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

The helper stores manifest-mode packages under
`$env:VCPKG_ROOT\manifest-installed\broadcast-graphics-live` instead of
`build\vcpkg_installed`. This deliberately keeps Autotools/MSYS dependencies
such as `libiconv` out of project paths containing spaces. Override it with
`-VcpkgInstalledDir C:\vcpkg-installed\obs-bgs` or the
`OBS_BGS_VCPKG_INSTALLED_DIR` environment variable when needed; the selected
path must not contain whitespace.

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
  -DVCPKG_INSTALLED_DIR="$env:VCPKG_ROOT/manifest-installed/broadcast-graphics-live" `
  -DOBS_SDK_DIR="$env:OBS_SDK_DIR" `
  -DOBS_BGS_BUILD_TESTS=ON

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
  -DOBS_BGS_BUILD_TESTS=ON

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
build/broadcast-graphics-live/
├── bin/
│   └── 64bit/
│       ├── broadcast-graphics-live.dll
│       └── dependency DLLs on Windows
└── data/
    ├── effect-transitions/
    ├── icons/
    └── locale/
```

Typical Windows installation:

```text
C:\ProgramData\obs-studio\plugins\broadcast-graphics-live\
├── bin\64bit\
│   ├── broadcast-graphics-live.dll
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

1. Open the **Broadcast Graphics Live** dock in OBS Studio.
2. Create a blank title, select a starter template, or import an `.obgt` template.
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
broadcast-graphics-live/
└── scene-collection-titles/
    └── <sanitized-scene-collection-name>-<hash>.json
```

Typical plugin configuration roots are:

```text
Windows:
%APPDATA%\obs-studio\plugin_config\broadcast-graphics-live\

Linux:
~/.config/obs-studio/plugin_config/broadcast-graphics-live/

macOS:
~/Library/Application Support/obs-studio/plugin_config/broadcast-graphics-live/
```

The exact OBS configuration root can vary with portable installations and custom builds.

Writes use an atomic replacement path so an interrupted save does not intentionally overwrite the last valid title file with a partial document.

### Template files

`.obgt` exports use the format identifier:

```text
broadcast-graphics-live-title-template
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
Broadcast-Graphics-Live/
├── CMakeLists.txt
├── LICENSE
├── README.md
├── build-windows.ps1
├── data/
│   ├── effect-transitions/ # Effect/transition presets and OBS shader assets
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
| Root `CMakeLists.txt` | Canonical project and prerelease version used by About and plugin diagnostics |

See [`docs/module-architecture.md`](docs/module-architecture.md) for the current ownership and dependency map.

---

## Tests

Enable the lightweight validation targets with:

```bash
-DOBS_BGS_BUILD_TESTS=ON
```

Current CTest targets include model, cache, renderer, mask, and phase-contract coverage such as:

- `rich_text_model_test`
- `text_layout_contract_test`
- `animation_model_test`
- `transition_model_test`
- `cache_time_contract_test`
- `system_memory_contract_test`
- `cache_frame_payload_test`
- `cache_tile_payload_test`
- `disk_cache_tiling_contract_test`
- `live_text_cue_utils_test`
- `title_snapshot_test`
- `gpu_rounded_image_clip_test`
- `gpu_mask_contract_test`
- `gpu_ram_cache_tiling_contract_test`
- `gpu_prerender_phase14_contract_test`
- `phase15_visibility_recovery_contract_test`

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

- <https://github.com/menacius/Broadcast-Graphics-Live/issues>

---

## Project license

The repository includes the **GNU General Public License, version 3** as its project license. Unless an individual file or asset states different terms, project-authored source code is distributed under **GPL-3.0-only**.

See [`LICENSE`](LICENSE) for the complete license text.

The software is provided without warranty, as described by the GPL.

---

## Third-party software and assets

Broadcast Graphics Live uses, links to, fetches, or redistributes components owned by other projects. Those components remain under their own licenses.

| Component | Use in Broadcast Graphics Live | Upstream license / notice |
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

Broadcast Graphics Live is built on the work of the OBS Project, The Qt Company and Qt contributors, the Cairo and GNOME/Pango communities, the HarfBuzz project, Niels Lohmann and contributors, Mattia Basaglia and contributors, Yann Collet and LZ4 contributors, and Fonticons, Inc.

Their projects make native, cross-platform broadcast graphics inside OBS possible.
