# GPU Draw Preview and Glow Cache Fix

- Canvas shape, text, and image box drawing now invalidates the native GPU overlay texture on every geometry update, so the temporary object outline remains visible throughout the mouse drag without committing project changes or forcing a full artwork render.
- Releasing the mouse explicitly clears and refreshes the overlay before the final layer is created, preventing stale preview geometry.
- GPU pixel-effect output is cached per layer after the first render. Transform-only manipulation reuses the effected texture, so glow is not recomputed for every move, rotate, or interactive scale frame.
- Glow and bloom surface padding now matches the actual clamped shader footprint instead of allocating `effect_size * 3` transparent pixels on all sides. Inner glow and temporal motion blur no longer request unnecessary outer padding, and directional blur padding is aligned with its furthest shader tap.
- Effect caches are invalidated when the base raster changes and are destroyed with their owning layer/session GPU resources.
