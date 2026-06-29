# Architecture and build

## Source ownership

Broadcast Graphics Live keeps public source paths stable while large implementation units are divided into ordered ownership-oriented `.inc` modules. The facade translation units preserve private symbol ordering and anonymous namespaces while making navigation manageable.

Major facades include:

| Facade | Ownership |
| --- | --- |
| `src/obs/title-source.cpp` | OBS source lifecycle, playback, cache presentation, masks, groups, effects, and registration. |
| `src/editor/title-editor.cpp` | Window/session lifecycle, commands, panels, document editing, playback, templates, and signals. |
| `src/canvas/canvas-preview.cpp` | Geometry, selection, snapping, tools, input, overlays, and preview rendering. |
| `src/editor/properties-panel.cpp` | Inspector construction, text/shape/image properties, gradients, and synchronization. |
| `src/editor/title-dock.cpp` | Title management, import/export, cue rows, playlists, cache status, and dock actions. |
| `src/cache/cache-manager.cpp` | Cache identity, storage, invalidation, queueing, workers, and publication. |

The machine-readable module inventory lives at `tools/modular-source-map.json` and is validated by `tools/audit_modularity_performance.py`.

## Dependency direction

```text
Editor / Dock / Canvas / Timeline
                │
                ▼
       Layer and title models
          │             │
          ▼             ▼
 Text / Effects     Cache contracts
          \             /
           ▼           ▼
        Rendering and OBS integration
```

Core serialization must not depend on Qt widgets. OBS source code must not depend on editor widgets. Shared algorithms belong in core, text, effects, layers, rendering, or cache modules rather than being copied between editor and source paths.

## Configure and build

The project requires a matching OBS/libobs development package and Qt toolchain. CMake configuration fails intentionally when `libobsConfig.cmake` cannot be found.

### Windows

```powershell
powershell -ExecutionPolicy Bypass -File .\build-windows.ps1
```

The script discovers configured SDK paths, builds the plugin, stages runtime dependencies, copies locale/icon/effect data, and creates a versioned ZIP.

### Linux/WSL

```powershell
powershell -ExecutionPolicy Bypass -File .\build-ubuntu-wsl.ps1
```

Build against the same ABI, glibc baseline, Qt version, and packaging environment used by the target OBS distribution. Avoid producing a binary against a newer libc than the target OBS runtime.

### Incremental update/build/package

```powershell
powershell -ExecutionPolicy Bypass -File .\update-and-build.ps1
```

Configuration values can be stored in the adjacent JSON config files instead of hardcoding local paths in scripts.

## Data and packaging

CMake stages locale files, all SVG icons, and effect/transition data into the OBS plugin data directory. Windows dependency DLLs are copied beside the plugin binary. Build trees, staged packages, archives, IDE state, caches, and generated artifacts are excluded by `.gitignore`.

## Tests and audits

- `tests/` contains C++ source-contract tests.
- `tools/` contains Python audits for architecture, build/version consistency, rendering contracts, and specific regressions.
- `cmake/ci/` documents CI-oriented configuration.

When a canonical document path changes, update audits to read the new document instead of retaining a duplicate historical note.

## Contribution rules

- Add code to the owning module rather than the facade.
- Keep UI-only state out of title serialization.
- Keep project data out of global `QSettings`.
- Preserve editor/source rendering parity.
- Add a focused regression contract for compile fixes and model-order changes.
- Update `docs/CHANGELOG.md` and the relevant thematic document rather than adding a new one-off markdown file.
