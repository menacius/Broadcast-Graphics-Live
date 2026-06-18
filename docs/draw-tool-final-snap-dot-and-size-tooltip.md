# Draw Tool Final Snap Dot and Size Tooltip

This update extends the Shape/Text draw-tool snapping feedback.

## Behavior

- Before drawing starts, hover snapping still shows the snapped start-point circle and snap helper lines.
- While drawing, the original start snap circle remains visible.
- When the current/final draw point snaps to a canvas or object helper, a second snap circle is drawn at the final snapped point.
- While drawing a shape/text box, the editor now shows the same size tooltip format used during object scaling.

## Notes

- `Ctrl` continues to bypass snapping.
- The feature is editor-only and does not affect OBS/source rendering.
- The final snap circle uses the same Canvas/Object snap helper color role as the active snap line.
