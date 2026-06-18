# OBS Graphics Studio Pro

OBS Graphics Studio Pro is a native C++/Qt graphics plugin for OBS Studio. It adds a dockable graphics manager, an After Effects-inspired editor, a layered canvas, timeline animation, live text cueing, and native OBS source playback so broadcast graphics can be created and played directly inside OBS without browser sources or external titling software.

The project is currently an active **alpha**. Core title creation, editing, serialization, OBS source playback, template import/export, and many editor workflows are implemented, while the codebase is still being modularized and hardened for production use.

---

## Current Capabilities

### OBS integration

- Native OBS input source: **OBS Graphics Studio Pro Title**.
- Dockable **OBS Graphics Studio Pro** panel for title/template management.
- One-click add-to-scene workflow for the selected title.
- Scene-collection/profile persistence through `titles.json`.
- Scene-mask support for mapping OBS scenes into animated graphics masks.
- Runtime live-text cueing with foreground/background persistence options.

### Editor and canvas

- Non-modal Qt editor with canvas preview, layers, properties, tools, and timeline.
- Text, clock, ticker, image, solid rectangle, and vector shape layers.
- Shape primitives: rectangle, rounded rectangle, ellipse, triangle, star, polygon, diamond, and line.
- Direct canvas interaction for selection, movement, resize handles, origin/anchor editing, and multi-select transforms.
- Photoshop-style rulers, draggable guides, safe guides, snapping, zoom, and pan.
- Layer visibility, locking, duplication, parent/child transforms, and track-matte style masks.

### Text and typography

- Rich text model with structured inline formatting and HTML fallback compatibility.
- On-canvas text editing for text-like layers.
- Font family/style/size, bold, italic, underline, strikethrough, kerning/tracking, character scale, baseline shift, leading, paragraph spacing, indents, alignment, and overflow controls.
- Clock layers with configurable time formats.
- Ticker layers with horizontal scrolling, vertical line-by-line, and vertical smooth modes.
- Exposed text columns for live data/cue workflows.

### Styling, gradients, effects, and masks

- Solid and gradient fills for supported layer/background/stroke paths.
- Gradient modes: linear, radial, angle, reflected, and diamond.
- Intermediate gradient stops with stored opacity/position data.
- Per-side text/background padding and per-corner radii/corner types.
- Outlines/strokes for text and shape-oriented layers.
- Drop shadows and long shadows with multiple blur modes.
- Stackable layer effects including background color, outline, drop shadow, long shadow, brightness/contrast, saturation, color overlay, glow, inner glow, inner shadow, blur, and motion blur.
- Blend modes including normal, multiply, additive, screen, overlay, and color.
- Alpha, inverted alpha, luma, and inverted luma masks.

### Timeline and animation

- Timeline with in/out ranges, playhead, transport controls, layer rows, switches/modes, parenting, and mask indicators.
- Keyframed properties for transform, opacity, text styling, color channels, background, shadow, shape/box size, and origin-related controls.
- Easing modes: linear, ease in, ease out, ease in/out, cubic Bezier, and hold.
- Title playback modes: play once, loop in/out, and pause at position; loop type supports restart and ping-pong.

### Templates and persistence

- Built-in starter templates for lower thirds, centered titles, ticker/strap graphics, and clocks.
- Template import/export through `.ogspt` files with metadata and preview screenshots.
- Manual title preview screenshot capture for the dock/template list.
- Editor preferences for colors, guides, and default styles.

---

## Architecture

```text
OBS-Graphics-Studio-Pro/
├── CMakeLists.txt
├── build-windows.ps1
├── data/
│   ├── icons/        # SVG icons used by the editor/dock
│   └── locale/       # OBS/Qt localization strings
├── docs/             # Architecture notes and feature/fix notes
├── tests/            # Lightweight C++ model tests
└── src/
    ├── core/         # Title data, serialization, preferences, localization
    ├── text/         # Rich-text document/model helpers
    ├── layers/       # Layer model and layer stack UI
    ├── effects/      # Effect model and effects panel
    ├── timeline/     # Keyframes, easing, timeline widget
    ├── canvas/       # Canvas preview, tools, guides, snapping, inline editing
    ├── editor/       # Dock, title editor, properties, hotkeys, assets
    ├── rendering/    # GPU/filter pipeline helpers
    ├── obs/          # OBS module entry point and source renderer
    └── performance/  # Performance/stability planning area
```

See [`docs/module-architecture.md`](docs/module-architecture.md) for the current module ownership map, dependency direction, and migration plan.

### Component Map

| Component | Integration | Purpose |
|---|---|---|
| `TitleSource` | OBS `INPUT` source | Renders a selected title into OBS and exposes scene-mask/live-cue source settings. |
| `TitleDock` | `obs_frontend_add_dock()` | Manages titles, templates, live text rows, screenshots, import/export, and add-to-scene actions. |
| `TitleEditor` | Qt non-modal editor | Hosts the canvas, layer stack, timeline, tools, preferences, and properties workflow. |
| `CanvasPreview` | Qt canvas widget | Handles preview rendering, selection, transforms, rulers/guides, snapping, masks, and inline text editing. |
| `PropertiesPanel` | Qt inspector | Edits transform, style, typography, image, shape, effects, masks, and live-text properties. |
| `TitleDataStore` | Singleton data store | Owns all titles and serializes them to the active OBS configuration path. |

---

## Dependencies

| Library | Purpose |
|---|---|
| **OBS Studio** (`libobs` + `obs-frontend-api`) | Plugin API, rendering, frontend dock integration. |
| **Qt 5.15+ or Qt 6** | Editor, dock, canvas, icons, and widgets. |
| **Cairo** | CPU-side 2D compositing path used by source rendering. |
| **Pango + PangoCairo** | Font layout and text rendering support. |
| **nlohmann/json** | JSON serialization; fetched automatically by CMake if not already available. |

---

## Build Instructions

### Linux (Ubuntu 22.04+)

```bash
sudo apt install \
  cmake ninja-build pkg-config \
  libobs-dev obs-frontend-api-dev \
  qtbase5-dev libqt5widgets5 libqt5svg5-dev \
  libcairo2-dev libpango1.0-dev

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build

cmake --install build --prefix ~/.config/obs-studio/plugins
```

The build also stages a copyable OBS plugin layout at `build/obs-graphics-studio-pro`.

### macOS

```bash
brew install cmake cairo pango pkg-config qt

cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DOBS_SOURCE_DIR=/path/to/obs-studio

cmake --build build
```

### Windows (Visual Studio / vcpkg)

Install dependencies with vcpkg, point CMake at an OBS SDK/plugin-deps package or OBS Studio install tree, then build:

```bat
vcpkg install cairo pango[fontconfig] qt6-base

set OBS_SDK_DIR=C:\path\to\plugin-deps-or-obs-studio
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  -DOBS_SDK_DIR=%OBS_SDK_DIR%

cmake --build build --config Release
```

Or use the helper script:

```powershell
.\build-windows.ps1 -ObsSdkDir C:\path\to\plugin-deps-or-obs-studio
.\build-windows.ps1 -ObsSdkDir C:\path\to\plugin-deps-or-obs-studio -Configuration RelWithDebInfo -BuildTests
```

The helper validates the modular source-tree paths, configures CMake, builds the selected configuration, optionally runs lightweight tests, and installs/stages the plugin layout.

Expected Windows OBS plugin layout:

```text
C:\ProgramData\obs-studio\plugins\obs-graphics-studio-pro\
├── bin\64bit\obs-graphics-studio-pro.dll
├── bin\64bit\cairo.dll
├── bin\64bit\pango-1.0.dll
├── bin\64bit\pangocairo-1.0.dll
├── bin\64bit\Qt6Core.dll / Qt5Core.dll
├── bin\64bit\Qt6Gui.dll / Qt5Gui.dll
├── bin\64bit\Qt6Widgets.dll / Qt5Widgets.dll
└── data\locale\en-US.ini
```

Use `-InstallRoot` for a portable OBS/custom plugin root. If OBS reports that the plugin failed to load on Windows, verify that the required dependency DLLs are beside `obs-graphics-studio-pro.dll`.

---

## Basic Workflow

1. Open the **OBS Graphics Studio Pro** dock in OBS.
2. Create a blank title, choose a starter template, or import an `.ogspt` template.
3. Edit layers, text, styling, masks, effects, and keyframes in the editor.
4. Capture/update a preview screenshot if desired.
5. Use the dock live-text table for exposed text layers and cue transitions when building data-driven graphics.
6. Click **Add to Scene** / **Scene** to add the selected title as a native OBS source.

---

## Data Format

Titles are saved in the OBS profile config directory:

```text
%APPDATA%\obs-studio\plugin_config\obs-graphics-studio-pro\titles.json
~/.config/obs-studio/plugin_config/obs-graphics-studio-pro/titles.json
~/Library/Application Support/obs-studio/plugin_config/obs-graphics-studio-pro/titles.json
```

A title contains metadata, timeline/playback settings, canvas size, editor defaults, live-text rows, preview screenshot data, and a bottom-to-top layer list. Layers store transform/timing state, rich text, shape/image/ticker/clock-specific fields, masks, effects, gradients, keyframes, and rendering style properties.

### Easing values

| Value | Easing |
|---:|---|
| 0 | Linear |
| 1 | Ease In |
| 2 | Ease Out |
| 3 | Ease In/Out |
| 4 | Cubic Bezier |
| 5 | Hold / jump cut |

---

## Developer Notes

### Adding a new layer type

1. Add the enum value and model fields in `src/layers/layer-model.h`.
2. Add JSON serialization/deserialization in `src/core/title-data.cpp`.
3. Add source rendering in `src/obs/title-source.cpp`.
4. Add editor preview/canvas behavior in `src/canvas/canvas-preview.cpp` and editor wiring in `src/editor/title-editor.cpp`.
5. Add controls in `src/editor/properties-panel.cpp` / `.h`.
6. Add localization strings in `data/locale/en-US.ini`.
7. Add or update focused tests where model behavior is affected.

### Adding animated property support

1. Add an `AnimatedProperty` field to the relevant model.
2. Persist it in title JSON.
3. Evaluate it in the OBS renderer and canvas preview at local layer time.
4. Expose keyframe controls in the properties panel/timeline where appropriate.

---

## Roadmap / Active Work

- Continue stabilizing alpha editor workflows and source rendering parity.
- Complete modular ownership migration described in `docs/module-architecture.md`.
- Expand automated tests around serialization, animation, rich text, masks, and effects.
- Improve GPU rendering/filter integration and performance for complex compositions.
- Refine template/library workflows and live-data integrations.
- Continue hardening undo/redo, multi-select editing, effect-stack ordering, and advanced keyframe controls.

### Sparse cached playback: direct crop drawing
Cached alpha-bounded payloads are now uploaded and drawn directly at their canvas offsets in OBS, and the editor preview uses the same sparse metadata instead of stretching cropped images over the full canvas.
