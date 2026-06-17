# Title Cache Optimization

This pass makes the title cache more selective and more consistent with the render pipeline.

## Changes

- Static titles are now cache-normalized to frame `0`. A title with no animated properties, no time-bounded layers and no dynamic clock/ticker layers stores a single reusable frame instead of writing identical frames for the whole timeline.
- The cache content hash now tracks rendered visual state instead of metadata-only changes. Renaming a title no longer forces a cache rebuild, while animated properties, gradient stops and stackable effect settings/keyframes are included so stale disk frames are not reused after real visual changes.
- Cache invalidation now records dirty tiles per title/frame. Layer invalidation estimates the layer's current affected bounds and unions them with the last rendered bounds so moved/resized layers invalidate both the old and new tile regions.
- Stale-frame rebuilds reuse the previous cached frame and replace only dirty tiles after rendering a fresh frame. This keeps the invalidation model tile-aware and prepares the pipeline for future renderer-side tile-only drawing.
- Redundant invalidations are skipped when the title's visual hash has not changed. Live cue caches are still invalidated from whole-title invalidation paths because cue data can change without changing the base title pixels.

## Notes

The current Cairo renderer still produces a full fresh frame before merging dirty tiles. The cache manager now has the dirty-region/tile bookkeeping needed by the render pipeline; the next deeper optimization is to expose a clipped/tile render entry point in `render_title_to_image()` so Cairo only rasterizes dirty tiles instead of drawing the full canvas first.
