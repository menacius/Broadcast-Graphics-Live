# Broadcast Graphics Live

<p align="center">
  <img width="520" alt="Broadcast Graphics Live" src="data/icons/broadcast-graphics-live-logo.svg" />
</p>

**Broadcast Graphics Live** is a native C++/Qt graphics plugin for OBS Studio. It combines a dockable title manager, a layered motion-graphics editor, timeline animation, live text and image cueing, reusable templates, GPU-assisted rendering, and native OBS source playback without requiring browser sources or a separate titling application.

**Current build:** `v0.8.8-alpha` · `Development Version 144`

<p align="center"><strong>Developed by: omniatv</strong></p>
<p align="center">
  <a href="https://omniatv.com">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="data/icons/omniainvert.svg" />
      <img width="230" alt="OmniaTV" src="data/icons/omnianormal.svg" />
    </picture>
  </a>
</p>

## What’s new in v0.8.8-alpha

`v0.8.8-alpha` consolidates the editor, animation, text, and live-data work completed since `v0.8.7-alpha` Development Version 107. The release adds a fully asynchronous external-data workflow, table-driven Live Text Cues, spatial and temporal Bezier authoring, a Value/Speed Graph Editor, learned Auto Styling, the unified Text Animator runtime, improved transition editing, synchronized layer/timeline keyframe controls, and a broad theme-aware editor UI refresh.

- **External data and live production:** asynchronous JSON, CSV, HTTP, WebSocket, text-file, and manual providers; automatic field discovery; formatting pipelines; table-to-cue mapping; managed table cells; reliable cue switching; and privacy-safe diagnostics.
- **Advanced animation:** spatial Bezier paths, on-canvas motion handles, Value and Speed Graphs, manual temporal velocity handles, Easy Ease commands, modifier-aware graph dragging, and synchronized keyframe sections and controls across the layer list and timeline.
- **Smarter text workflows:** structural and invisible-character recognition, learned regex rules, smart rule generalization, reusable style rules, and a unified Text Animator system for text, ticker, clock, and legacy text-transition presets.
- **Transition and rendering reliability:** larger strip handles, full-area transition replacement with preserved duration, deterministic text-transition cleanup, corrected glyph envelopes, shared blur behavior, Unicode-aware unit segmentation, and safer cache/runtime migration.
- **Editor and dock refinements:** collapsible Titles and Graphics dock, improved timeline ruler and cache bands, safer layer-list sizing, theme-aware icons, updated Graph Editor controls, matte-role cleanup, and full-width font family/style selectors in the Character panel.

## Highlights

- Redesigned Auto Styling panel with Quick Setup, clearer rule priority, contextual fields, and collapsible advanced conflict controls.
- Reusable Auto Styling rule sets can be saved and loaded as `.gsp-auto-style.json` files, with Replace or Append import modes.

- Native OBS dock and source integration for titles, graphics, live text cues, playlists, and scene insertion.
- Layered editor with text, shapes, images, clocks, tickers, assets, groups, parenting, mattes, masks, blend modes, and adjustment-style compositing.
- After Effects-style timeline with Value/Speed Graph Editor, manual temporal velocity handles, Easy Ease commands, manual spatial Bezier motion paths, transform tools, rulers, guides, snapping, panel reordering, and persistent inspector layouts.
- Panel-based effect stack with drag-and-drop ordering, per-effect enable switches, presets, animated parameters, and GPU-backed effects.
- Rich text, inline editing, text styles, auto-styling rules, visual external-data bindings, formatter pipelines, table-to-cue row mapping, live cue bindings, and asynchronous JSON, CSV, HTTP, WebSocket, text-file, and manual providers.
- RAM and disk frame caching, prerender queues, dirty-region invalidation, live-cue cache reuse, and project-rate playback.
- Reusable title templates, animated asset layers, style libraries, effect presets, transition presets, and extension manifests.
- Theme-aware Qt UI that follows the active OBS palette while preserving explicit per-control colors.

## Requirements

- OBS Studio with a compatible Qt 6 plugin SDK including Qt Network and Qt WebSockets.
- CMake 3.16 or newer.
- A C++17 compiler supported by the target OBS build.
- Platform dependencies described in [INSTALL.txt](INSTALL.txt) and [docs/ARCHITECTURE_AND_BUILD.md](docs/ARCHITECTURE_AND_BUILD.md).


### Automatic Qt WebSockets bootstrap

When the selected OBS Qt6 SDK does not ship `Qt6WebSockets`, CMake now downloads the official `qt/qtwebsockets` source at the exact `v<Qt6_VERSION>` tag and builds it against that same SDK. The dependency is cached under the build tree (`_deps`) and is not downloaded or rebuilt on every build. Set `OBS_BGS_BOOTSTRAP_QT_WEBSOCKETS=OFF` to require a preinstalled module, or override `OBS_BGS_QT_WEBSOCKETS_GIT_TAG` only when deliberately testing a matching Qt tag. A complete matching Qt development SDK, including `Qt6BuildInternals`, is required for the standalone module build.

### External-data diagnostics

Open **Broadcast Graphics Live Preferences → Logging**, enable logging, select **Debug** or **Trace**, and keep the **External data** category enabled. The session log records provider and table-mapping flow without writing authentication tokens or raw external values; value summaries use type/size/emptiness plus a fingerprint so repeated and changed values can still be compared safely.
The same category also records Live Text Cue row-switch decisions. This includes pending-row transitions for Loop/Pause playback and the final source-side application of the resolved mapped value.

## Build and install

### Windows

```powershell
powershell -ExecutionPolicy Bypass -File .\build-windows.ps1
```

The script configures, builds, stages the OBS plugin layout, and creates a distributable ZIP. Use `update-and-build.ps1` for incremental update/build/package workflows.

### Linux

Use `build-ubuntu-wsl.ps1` from Windows/WSL or configure the project directly against the same Qt/libobs stack used by the target OBS package.

### macOS

Configure CMake against an OBS development bundle and a matching Qt toolchain, then build the `broadcast-graphics-live` target.

## Installed layout

```text
obs-plugins/64bit/broadcast-graphics-live.dll   # Windows
obs-plugins/broadcast-graphics-live.so          # Linux
obs-plugins/broadcast-graphics-live.plugin      # macOS bundle form

data/obs-plugins/broadcast-graphics-live/
  locale/
  icons/
  effect-transitions/
```

## Documentation

The consolidated documentation starts at [docs/README.md](docs/README.md):

- [User guide](docs/USER_GUIDE.md)
- [Editor workflow](docs/EDITOR_WORKFLOW.md)
- [Text and live data](docs/TEXT_AND_LIVE_DATA.md)
- [Text Animators](docs/TEXT_ANIMATORS.md)
- [Rendering and cache](docs/RENDERING_AND_CACHE.md)
- [Effects and extensions](docs/EFFECTS_AND_EXTENSIONS.md)
- [Architecture and build](docs/ARCHITECTURE_AND_BUILD.md)
- [Changelog](docs/CHANGELOG.md)

## Tests and audits

Contract tests live in `tests/`. Source and packaging audits live in `tools/`. Most audits are source-only and can run without launching OBS; rendering and integration validation still require a matching OBS/libobs SDK and runtime.

## Versioning

Public releases use semantic versions such as `v0.8.8-alpha`. Development packages append the development revision and platform, for example:

```text
Broadcast_Graphics_Live_v0.8.8-alpha_development-version-144_windows-x64.zip
```

## License

Broadcast Graphics Live is distributed under the terms in [LICENSE](LICENSE). Third-party libraries and optional extension packages retain their own compatible licenses. No external application-wide Qt theme package is bundled.



### Development version 144 — v0.8.8-alpha and Character panel cleanup

- Updated the public release identity to `v0.8.8-alpha` across CMake, runtime metadata, dependency manifests, documentation, audits, installation examples, and package naming.
- Removed the redundant Font and Style labels from the Character section and expanded both selectors across the full inspector width.
- Added a consolidated README summary of the major editor, animation, text, transition, and external-data changes completed since `v0.8.7-alpha` Development Version 107.

### Development version 136 — Complete unified BGL text transitions

- Restored the existing BGL text-transition workflow and timeline handles while making editable managed Text Animators the only runtime implementation.
- Fade, Slide, Scale, Blur, Wipe, and Blur Slide preserve entrance/exit timing, character/word/sentence units, stagger, reverse order, direction, duration, easing, and layer-local playback.
- Transition Editor previews, editor output, OBS source output, cache/prerender evaluation, color-font/emoji fallback, and long-text fallback all consume the shared shaped-layout evaluator.
- Added common word/sentence transform origins, directional shaped-unit wipe clipping, contracting multi-sample blur, live duration/trim synchronization, binding-safe manual editing, and Development Version 134/135 recovery.
- Removed the former descriptor-driven text-transition runtime renderers; legacy fields remain only for authoring metadata and automatic project migration.

### Development version 134 — Unified Text Animator Core and Legacy Preset Migration

- Added the renderer-neutral Text Animator stack for text, ticker, and clock layers, with multiple ordered animators, generic properties, Range/Procedural/Text-based/Wiggly selectors, deterministic composition, and shared `AnimatedProperty` keyframes.
- Migrated all six existing text-transition preset identifiers (`text.fade`, `text.slide-in`, `text.scale`, `text.blur`, `text.wipe`, and `text.blur-slide-in`) to concrete editable Text Animator structures on apply/load; legacy text runtime transitions are removed after conversion.
- Added shaped-cluster unit maps for graphemes, characters excluding spaces, words, lines, paragraphs, rich-text runs, and whole-layer selection, including Greek, combining marks, emoji ZWJ, and RTL test coverage.
- Integrated animator tracks into the existing timeline/Graph Editor property adapter and added a Text Animators Properties section for stack, property, selector, keyframe, and dynamic-text behaviour editing.
- Added deterministic content-change remapping for Live Text Cues, clocks, and external-data updates, plus animator-aware cache signatures, dynamic-frame detection, and visual padding.
- Extended the batched GPU glyph compositor with per-cluster transform, reveal/opacity, colour, stroke-width, tracking, baseline, skew, size scaling, and SDF-softness data without creating one draw call per character.
- Added standalone `.obgtextanim` import/export and schema validation for editable custom animator presets.
- Added core, timeline-contract, preset round-trip, migration, Unicode, determinism, and 1,200-cluster/10-animator stress tests, plus canonical Text Animator documentation.

### Development version 132 — Collapsible Titles and Graphics Dock

- Added a persistent collapse/expand caret to the Titles and Graphics header.
- Collapsed mode keeps a compact status rail with the dock icon, selected-title active state, cue state, and aggregate cache/prerender progress while Live Text Cues remains fully available.
- Preserves the title list model, selection, cue/playback state, prerender work, splitter size, expanded width, and last dock area across sessions.
- Adapts caret direction and placement for left-docked, right-docked, and floating layouts without resizing the OBS dock during collapse.

### Development version 131 — Auto Styling Rule Save Preset Fix

- Saving a learned regex rule no longer converts it into a marker-range rule.
- The selected per-rule preset remains bound to the selected rule after Save Rule.
- Inferred regex patterns and capture groups are preserved, so the rule continues matching and does not fall back visually to the base preset.

### Development version 130 — Auto Styling Rule State Synchronization
- Fixed per-rule preset display in the Rule Editor.
- Learned rules explicitly show “Embedded / learned style”.
- Missing preset references are visible instead of silently showing another rule's preset.
- Preset changes apply immediately to only the selected rule.
- Rule selection is preserved during property refreshes.
- Added signal blocking while loading every rule field to prevent cross-rule state leakage.
