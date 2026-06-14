# Gradient Stop Drag/Creation Stability Fix

## Changes

- Fixed intermediate gradient stop dragging so existing stops remain selected and draggable instead of being deselected during mouse movement.
- Added a stable drag target index for gradient stops, so the stop captured on mouse press remains the active drag target for the full drag operation.
- Updated overlapping stop hit-testing to prefer the currently selected stop, preventing newly-created or stacked stops from being hijacked by lower-index start/end stops.
- Prevented the gradient preview from being rebuilt from serialized layer data during live stop drags. Preview-originated changes now persist to the layer without reloading the preview widget mid-drag.
- Reduced re-entrant UI updates when adding, duplicating, deleting, or moving stops. Stop creation now selects the new stop and lets the Stops panel edit it instead of immediately opening a color popup during the creation event.

## Expected behavior

- A third, fourth, or later intermediate stop can be selected and dragged normally.
- Dragging an existing stop does not create another stop.
- Adding a fourth stop no longer enters the old re-entrant add/popup/update path that could crash OBS.
- The Stops panel continues to show the selected stop properties and remains synchronized with the preview.
