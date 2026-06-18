# Canvas Ruler Indicator and Unclipped View Fix

## Summary

This update refines the editor canvas interaction layer without changing the final OBS source output.

## Changes

- Ruler mouse indicators now update their tracked mouse position immediately in the mouse-move path and repaint the ruler strips directly, avoiding the small visual delay that came from waiting for the normal queued widget repaint.
- Added a persistent View menu toggle: **Clip Objects to Canvas**.
- When **Clip Objects to Canvas** is enabled, the editor keeps the existing canvas-clipped preview behavior.
- When disabled, the editor uses a dedicated editor-only render region that can extend outside the formal title canvas, so off-canvas objects remain visible and editable.
- The unclipped editor render keeps the area outside the canvas transparent and still draws the canvas checkerboard/border separately.
- OBS output and cache rendering remain clipped to the title canvas; the unclipped path is editor-only.
