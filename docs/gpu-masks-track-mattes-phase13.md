# Phase 13 — GPU Masks and Track Mattes

Phase 13 moves the complete mask-compositing contract into the unified OBS GPU render graph.

## Implemented contract

- Alpha, inverted alpha, luma and inverted luma track mattes are evaluated by `kGpuMaskEffect` from premultiplied GPU textures.
- Matte sources are rendered through the same layer graph as visible artwork, preserving image/text/vector GPU resources, stack effects, temporal motion blur and parent transforms.
- Mask sources can themselves reference another track matte. Dependency chains are prepared recursively and cyclic graphs fail safely instead of recursing indefinitely.
- `effect_stack_respects_masks` retains both orders: effects-before-mask and effects-after-mask.
- Transformed/effected mattes are retained in a bounded, double-buffered per-session GPU texture cache. Alpha/luma/inverted modes share the same cached source matte.
- Scene-mask layers consume the same cached GPU matte graph, including parented transforms and nested matte dependencies.
- Cache readback remains only at the final frame boundary. No CPU alpha-mask image, `cairo_mask_surface`, QPainter composition or Cairo track-matte compositor remains.

## Editor startup correction

Opening a title now primes its immutable GPU model immediately. With caching disabled, the first native display callback therefore receives prepared GPU-text glyph batches instead of showing an empty text layer until a canvas edit occurs.
