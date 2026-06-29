# Broadcast Graphics Live

<img width="3840" height="2088" alt="image" src="https://github.com/user-attachments/assets/c352f7d0-ba78-4964-94ea-a1b0d82c108e" />

**Broadcast Graphics Live** is a native C++/Qt graphics plugin for OBS Studio. It combines a dockable title manager, a layered motion-graphics editor, timeline animation, live text and image cueing, reusable templates, GPU-assisted rendering, and native OBS source playback without requiring browser sources or a separate titling application.

**Current build:** `v0.8.7-alpha` · `Development Version 105`

<p align="center"><strong>Developed by: omniatv</strong></p>
<p align="center">
  <a href="https://omniatv.com">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="data/icons/omniainvert.svg" />
      <img width="230" alt="OmniaTV" src="data/icons/omnianormal.svg" />
    </picture>
  </a>
</p>

## Highlights

- Native OBS dock and source integration for titles, graphics, live text cues, playlists, and scene insertion.
- Layered editor with text, shapes, images, clocks, tickers, assets, groups, parenting, mattes, masks, blend modes, and adjustment-style compositing.
- After Effects-style timeline, keyframes, easing, transform tools, rulers, guides, snapping, panel reordering, and persistent inspector layouts.
- Panel-based effect stack with drag-and-drop ordering, per-effect enable switches, presets, animated parameters, and GPU-backed effects.
- Rich text, inline editing, text styles, auto-styling rules, live-data fields, and independent clock/ticker playback.
- RAM and disk frame caching, prerender queues, dirty-region invalidation, live-cue cache reuse, and project-rate playback.
- Reusable title templates, animated asset layers, style libraries, effect presets, transition presets, and extension manifests.
- Theme-aware Qt UI that follows the active OBS palette while preserving explicit per-control colors.

## Requirements

- OBS Studio with a compatible Qt 6 plugin SDK.
- CMake 3.16 or newer.
- A C++17 compiler supported by the target OBS build.
- Platform dependencies described in [INSTALL.txt](INSTALL.txt) and [docs/ARCHITECTURE_AND_BUILD.md](docs/ARCHITECTURE_AND_BUILD.md).

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
- [Rendering and cache](docs/RENDERING_AND_CACHE.md)
- [Effects and extensions](docs/EFFECTS_AND_EXTENSIONS.md)
- [Architecture and build](docs/ARCHITECTURE_AND_BUILD.md)
- [Changelog](docs/CHANGELOG.md)

## Tests and audits

Contract tests live in `tests/`. Source and packaging audits live in `tools/`. Most audits are source-only and can run without launching OBS; rendering and integration validation still require a matching OBS/libobs SDK and runtime.

## Versioning

Public releases use semantic versions such as `v0.8.7-alpha`. Development packages append the development revision and platform, for example:

```text
Broadcast_Graphics_Live_v0.8.7-alpha_development-version-105_windows-x64.zip
```
<p align="center">
  <img width="520" alt="Broadcast Graphics Live" src="data/icons/broadcast-graphics-live-logo.svg" />
</p>

## License

Broadcast Graphics Live is distributed under the terms in [LICENSE](LICENSE). Third-party libraries and optional extension packages retain their own compatible licenses. No external application-wide Qt theme package is bundled.
