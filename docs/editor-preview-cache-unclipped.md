# Editor Preview Cache for Unclipped Canvas View

This change keeps the final OBS/output cache and the editor-only unclipped preview cache as separate systems.

## Behavior

- The OBS source/output cache continues to render only the canvas-sized frame.
- When `View -> Clip Objects to Canvas` is disabled, `CanvasPreview` renders the visible editor viewport as a larger editor-only region.
- That unclipped editor viewport is now cached in RAM, keyed by title id, playhead time, render region and an editor preview cache epoch.
- The cache is intentionally small and RAM-only because the editor viewport changes frequently with pan and zoom.

## Invalidation

The editor preview cache is cleared when:

- the active title changes,
- the canvas clipping toggle changes,
- the editor preview is explicitly refreshed,
- the rendered frame is cleared,
- canvas geometry/gradient drags commit changes.

During active layer manipulation, inline text editing and dynamic text titles, the editor bypasses this preview cache and renders the live model immediately.

## Output Safety

This cache is used only by `CanvasPreview`. It does not change `title-source.cpp`, `CacheManager`, the OBS source output, masks, bounds, or scene compositing. The final render remains clipped to the title canvas.
