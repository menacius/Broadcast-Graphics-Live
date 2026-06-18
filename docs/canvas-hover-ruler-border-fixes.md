# Canvas Hover, Ruler Cursor Indicators, and Canvas Border Fixes

Implemented editor-only canvas polish changes:

- Added Illustrator-style hover outlines for visible, unlocked canvas layers under the mouse cursor. The hover box is a thin translucent bounding box and does not change the actual title rendering.
- Added live ruler cursor indicators. When rulers are visible and the mouse is over the canvas, the top and left rulers show the current canvas X/Y position with small markers and pixel readouts.
- Fixed transparency checkerboard painting so checker cells are clipped to the canvas rectangle and no longer bleed beyond the right or bottom canvas edge.
- Added a 1 px canvas boundary overlay drawn above editor objects.
- Added a persistent View menu toggle, **Canvas Border**, for enabling or disabling the canvas boundary overlay.
