# Development Version 016 — Grouped child resize and canvas layer actions

- Corrects resize coordinate conversion for layers nested in scaled or rotated groups.
- Uses the complete parent/world transform for resize handle hit geometry and snapping.
- Converts canvas-space position compensation back into the child layer's parent-local space.
- Adds a unified canvas context menu for visibility, locking, duplicate/cut/copy/paste/delete, grouping, transforms, and layer ordering.
- Adds Flip Horizontal/Vertical, Rotate ±90°, Bring to Front, Bring Forward, Send Backward, and Send to Back.
- Layer-order operations keep selected group descendants together and preserve their internal order.
