# Unified GPU-only rendering pipeline

The editor canvas, OBS live source, partial-cache playback and final cache/prerender readback now use the same `TitleGpuRenderSession` frame graph.

## Contract

- Layer source rasters are transform-neutral resources. Text shaping, vector path rasterization and image decoding prepare those resources, but no CPU full-frame compositor is used.
- Position, parent transforms, rotation, scale, opacity, general transitions, masks, blend modes, stack effects and temporal motion blur are composed in OBS graphics render targets.
- Editor and live presentation keep the final texture GPU-resident.
- Cache/prerender/export are the only paths that map the completed frame to a staging surface.
- Cached full frames and partially cached prefixes are uploaded once and then remain GPU-resident for playback.
- The old user GPU switch and the CPU fallback pipeline have been removed.

## Feature parity

- Text, rich text and auto-styled live text: shared layer-raster input and GPU composition.
- Shapes, paths, gradients and strokes: shared layer-raster input and GPU composition.
- Bitmap and SVG image layers: persistent layer texture, GPU transforms and temporal accumulation.
- Parenting, masks, inverted alpha/luma mattes and scene-mask layers: GPU frame-graph passes.
- Stack effects and layer blend modes: GPU shader passes.
- Motion blur: weighted shutter samples over GPU transforms; invisible shutter intervals contribute transparency and are not renormalized.
- Background persistence and live-cue text persistence: per-layer render-time selection before GPU composition.
- Cache, prerender, partial-cache dynamic suffixes and dirty-frame regeneration: final-frame-only readback.
- Clock and ticker: dynamic source raster invalidation with the same GPU compositor and presentation path.

## Interactive performance rules

- Move and rotate edits update matrices without rebuilding layer rasters.
- Direct resize uses temporary GPU local-space scaling during drag and performs one exact raster rebuild after release.
- Per-layer render fingerprints prevent an edit to one layer from invalidating every other layer texture.
- Static OBS source ticks reuse the last frame graph result without repeated cache lookups or title snapshots.
- Editor overlays are invalidation-driven and are uploaded only when their geometry actually changes.
