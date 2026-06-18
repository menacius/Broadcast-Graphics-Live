# Ruler Indicator Line Latency Fix

This change reverts to the canvas hover/ruler/border implementation baseline and updates only the ruler mouse indicators.

## Changes

- Ruler mouse indicators are now simple guide lines instead of triangular markers with numeric coordinate labels.
- Mouse move handling now forces an immediate repaint of only the lightweight ruler strips.
- The forced repaint keeps the indicators visually locked to the cursor and avoids the small delay caused by deferred widget updates.
- The canvas hover bounding boxes and canvas border behavior remain unchanged.

## Notes

This is editor UI only. It does not affect title rendering, OBS output, cache rendering, or exported graphics.
