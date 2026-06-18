# Draw tool hover point snapping fix

The draw-tool snap dot and helper lines were not appearing during hover because
`snap_canvas_point()` reused the bounds-based snap resolver with a zero-size
`QRectF`. In Qt, a zero-size rect is invalid, so the resolver cleared snap
feedback and returned the raw point.

`CanvasPreview::snap_canvas_point()` now resolves point snapping directly per
axis. This keeps the hover snap dot, helper lines, and mouse-press start point
using the same snapped coordinate.

The snap helper colors remain split between canvas snap helpers and object snap
helpers in Preferences > Appearance > Canvas.
