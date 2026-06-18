# Draw Tool Hover Snap Feedback and Split Snap Colors

This change makes draw tools preview snapping before the mouse press:

- When the Shape or Text draw tool is active and snapping is enabled, moving the mouse over a snap target now updates the snap resolver immediately.
- The editor paints the snap helper line(s) during hover, not only during drag.
- A small Adobe-style snapped circle is painted at the snapped start point while hovering.
- Pressing the mouse starts drawing from the same snapped point that was already shown on hover.
- Holding Ctrl still bypasses snapping.

Snap helper colors are now split into two Appearance > Canvas preferences:

- Canvas Snap Lines: default green. Used for canvas bounds, canvas center, guides, safe guides, grid and other canvas-space helpers.
- Object Snap Lines: default red. Used for object edge, object center and spacing helpers.

The change is editor-only and does not affect final OBS/source rendering.
