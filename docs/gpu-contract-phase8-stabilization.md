# GPU Contract Phase 8 — Stabilization

Phase 8 stabilizes the shared editor/live/prerender GPU contract.

## Changes

- Adaptive Rendering now remains active for the complete canvas manipulation. The 140 ms refinement timer no longer returns a drag to full resolution while the pointer is still down.
- Fixed percentage modes remain at their selected render scale; Auto returns to full quality only after interaction ends.
- Auto starts with a reduced draft scale when no prior full-quality timing sample exists.
- Background Color geometry is passed to the shader in logical layer-raster coordinates instead of texture pixels.
- Background fill, stroke and independent corner radii are now resolution-independent across full, 75%, 50%, 37.5% and 25% rendering.
- The renderer cache ABI was advanced so stale prerender/RAM/disk frames from the previous geometry contract are invalidated.

## Contract

Canvas, live source and prerender use the same logical effect geometry. Adaptive resolution changes texture and target resolution only; it must not change layer bounds, effect offsets, padding, stroke width or corner radii.
