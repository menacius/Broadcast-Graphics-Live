# GPU-only contract — Phase 2

This phase removes synchronous interactive frame fallback and disables the legacy Cairo pixel-effect implementation.

## Changes

- `CacheManager::requestFrame()` no longer renders a replacement frame on cache miss, cache-disabled mode, interactive bypass, or non-cacheable titles.
- Cache misses are queued and returned as unavailable; editor/live presentation remains owned by `TitleGpuRenderSession`.
- Explicit screenshot/thumbnail capture uses `render_title_to_image()` directly, keeping final readback limited to an intentional capture operation.
- The Cairo surface effect stack is disabled. It can only produce base raster coverage; all stackable effects are applied by the unified GPU compositor.
- Mask/effect ordering now always treats GPU effects as active, preventing the legacy masked CPU effect branch.

## Remaining CPU work

Text and vector base coverage still originate from Cairo/Qt raster generation when their content changes. They remain cached and GPU-resident between content changes, but replacing those generators requires the next renderer phase (GPU glyph atlas and GPU vector tessellation).
