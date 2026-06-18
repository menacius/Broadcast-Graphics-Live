# Tile-Based Dirty-Region Rendering

This update converts cache invalidation from full-canvas rerendering into real regional rasterization.

## Behavior

- Layer edits calculate the union of the previous and current conservative layer bounds.
- The affected rectangle is expanded for stroke, background padding, shadows, glow, blur, and other effect extents.
- The rectangle is divided into 256×256 cache tiles.
- When a stale cached frame already has a valid previous payload, the worker renders only those tiles through `render_title_region_to_image()`.
- Cairo keeps absolute title coordinates but clips and translates the destination to a tile-sized image.
- Dirty tile pixels replace the corresponding areas of the previous frame using source composition; unchanged pixels are reused.
- The merged result then continues through alpha cropping, temporal aliases, unique RAM payload accounting, LZ4 disk storage, and direct cropped GPU drawing.

## Safety fallbacks

- Missing previous frames perform a normal full render.
- Full-title or full-range invalidations may mark the whole canvas dirty.
- Non-normal blend modes retain full-canvas intermediate rendering and crop the requested result, preserving exact blend coordinates.
- Empty/invalid regional results fall back to the established full renderer.

## Diagnostics

Trace logging reports the tile count and the number/percentage of canvas pixels rasterized for every regional cache rebuild.

This stage primarily accelerates rerendering after localized edits. Initial unique animated frames still use the full renderer; temporal visual-state deduplication already avoids rendering unchanged timeline states.
