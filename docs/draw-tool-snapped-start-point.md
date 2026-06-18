# Draw Tool Snapped Start Point

Shape and text draw tools now snap their initial anchor point before a new layer is created.

When a draw tool is active and snapping is enabled, hovering over a valid snap target resolves the mouse position through the existing canvas snap system. The editor displays a small Adobe-style circular indicator at the resolved snapped start point. On mouse press, that snapped point is used as `shape_draw_start_canvas_`, so the new layer begins exactly on guides, object edges/centers, canvas bounds, grid targets, or spacing targets.

During the drag that defines the new layer size, the circular indicator remains tied to the snapped start point. Opposite-edge snapping can still show snap guide lines, but the dot no longer jumps to the current drag edge; this keeps the visual language clear: circle = start anchor, guide lines = live size/edge snapping.

Holding Ctrl keeps the existing behavior of bypassing snapping for the start point.
