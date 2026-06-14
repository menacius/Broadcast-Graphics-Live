# Gradient Stop Selection, Preview, and Drag Fix

- Reworked the Photoshop-style Gradient tab Stops section so it displays the properties of the currently selected stop only.
- Disabled the Stop color, location, opacity, and delete controls when no stop is selected.
- Kept start/end serialization controls as hidden backing fields while routing visible stop editing through the selected stop model.
- Kept the gradient ramp preview always linear, regardless of the selected final gradient type; on-canvas/object rendering still uses the selected type.
- Fixed stop hit-testing so dragging an existing stop selects and moves that stop instead of creating or prioritizing a newly overlapping intermediate stop.
- Added selected-stop delete handling for intermediate stops and guarded duplicate/delete actions when no stop is selected.
