# Broadcast Graphics Live module architecture

This document defines the incremental module split for Broadcast Graphics Live. The
first pass moves the existing monolithic files into ownership-oriented source
folders while preserving the legacy include names and runtime behavior. New work
should land in the owning module below instead of expanding editor/source files
without a clear boundary.

## Dependency direction

Keep dependencies flowing downward through stable data contracts:

```text
Editor UI / Docking ─┐
Canvas & Tools ──────┼──> Layer System ─┐
Timeline & Animation ┘                  ├──> Core Architecture
Text Engine ────────────────────────────┘
Effects System ─────> Rendering Engine ───> OBS Integration
Performance & Stability audits observe all modules but should not own features.
```

Rules for future refactors:

- `core` owns project data, serialization formats, title metadata, undo/redo
  state, command contracts, and global state transitions.
- Feature modules may depend on `core` models, but `core` must not depend on UI,
  OBS, rendering, effects, timeline, or tool modules.
- `obs` owns plugin registration and source lifecycle. Editor-only code should
  call into `obs` through narrow interfaces, not by embedding OBS-specific
  rendering or property logic in panels.
- `editor`, `canvas`, and `timeline` own user interaction; they should request
  model changes through commands instead of mutating shared state ad hoc.
- `rendering`, `effects`, and `text` own drawing decisions and cache invalidation
  for their domains. Expensive work should be explicit and measurable.

## Current source ownership

| Module | Current files | Responsibility |
| --- | --- | --- |
| Core Architecture | `src/core/title-data.*`, `src/core/title-localization.h` | Title/project ownership, serialization helpers, localization helpers, and data-store state contracts. |
| Layer System | `src/layers/layer-model.h` | Layer type definitions, hierarchy metadata, transforms, masks, visibility/locking flags, and layer-owned defaults. |
| Effects System | `src/effects/layer-effects.h` | Stackable layer effect types, blend modes, and effect parameter contracts. |
| Timeline & Animation | `src/timeline/animation.*`, `src/timeline/timeline-widget.*` | Keyframes, easing types, animated property contracts, interpolation evaluation, timeline interaction, keyframe selection, and timeline painting. |
| Text Engine | `src/text/title-rich-text.*` | Rich-text model, mixed inline styles, plain-text conversion, and text serialization helpers. |
| OBS Integration | `src/obs/plugin-main.*`, `src/obs/title-source.*` | OBS module registration, source creation/destruction, source properties, preview/live output, and OBS-facing rendering entry points. |
| Canvas & Tools | `src/canvas/canvas-preview.*`, `src/editor/tools-sidebar.*` | Canvas preview rendering, direct manipulation, zoom/pan, snapping, inline text editing, drawing tools, and tool palette state. |
| Editor UI / Docking | `src/editor/title-dock.*`, `src/editor/title-editor.*`, `src/editor/title-hotkeys.*`, `src/editor/title-assets.h`, `src/editor/title-properties-panel.*`, `src/editor/properties-panel.*` | Qt dock integration, editor window orchestration, toolbars, property inspectors, hotkeys, icons, and layout UI. |
| Layer Stack UI | `src/layers/layer-stack-widget.*` | Layer list interaction, ordering, visibility/lock toggles, hierarchy/mask controls, and layer context actions. |
| Effects UI | `src/effects/effects-panel.*` | Effect stack editing, effect-specific inspector controls, and synchronization with layer effect contracts. |

`src/editor/title-editor.cpp` is now the editor shell only: window setup,
toolbar/menu wiring, persistence, title commands, and cross-panel coordination.
Do not add Text Layer, Canvas, Timeline, Layer Stack, or Effects feature work to
that file. Put it in the module listed above and expose only the narrow signal,
slot, or model contract the editor shell needs.

## Target modules for subsequent phases

The following directories are intentionally introduced now so future work has a
clear destination even before all code is extracted from legacy files:

- `src/rendering`: GPU/OBS render paths, Cairo fallback, textures, alpha,
  blending, filters, render caches, invalidation, and performance-critical draw
  code.
- `src/layers`: Text/shape/image/group/mask layer behavior, hierarchy,
  transforms, parenting, visibility, locking, and layer-specific keyframe hooks.
- `src/effects`: Stackable effects such as shadows, glows, color overlays,
  brightness/contrast, saturation, blend modes, effect serialization, and effect
  caches.
- `src/canvas`: Selection, transform handles, snapping, rulers, guides, direct
  manipulation, drawing tools, zoom/pan, and keyboard-routing for canvas tools.
- `src/timeline`: Playback, timecode, animation curves, keyframe selection,
  easing types, and layer animation state.
- `src/performance`: Profiling notes, cache audits, render-time spike analysis,
  memory usage findings, crash-risk reviews, and regression-test plans.

## Incremental migration phases

1. **Source ownership split (completed).** Move existing files into module
   folders and expose those folders in CMake include paths without behavior
   changes.
2. **Model contract extraction (in progress).** Move layer, effects, and timeline
   primitives behind module-owned headers/sources while keeping existing JSON and
   editor behavior stable.
3. **Core contracts.** Extract command/undo interfaces and project metadata from
   `title-data` into smaller core headers and add serialization regression tests.
4. **Rendering extraction.** Move Cairo/OBS drawing helpers out of
   `src/obs/title-source.cpp` and editor paint code into `src/rendering` with
   cache ownership documented at each boundary.
5. **Layer/effects extraction.** Move layer-specific behavior and effect stacks
   into `src/layers` and `src/effects`, keeping serialization in core-facing DTOs.
6. **Canvas/timeline extraction.** Move direct manipulation, shortcuts,
   keyframe UI, playback, and easing logic out of editor widgets into tool and
   timeline services.
7. **Stability pass.** Add regression tests and profiling checkpoints for render
   spikes, cache invalidation, memory ownership, undo/redo consistency, and OBS
   source lifecycle crashes.
