# Layer List FX, Parenting, and Header Icon Fix

## Changes

- Added an `FX` square indicator in each layer-list row when the layer has an active/enabled effect stack.
- Added a matching FX header column so the layer list header remains aligned with the row controls.
- Replaced the text/emoji lock header with the project's `layer-lock.svg` icon for consistent OBS-style iconography.
- Updated parent assignment so changing a layer's parent preserves the layer's current world/canvas position by converting it into the new parent's local coordinate space.
- Added a simple parenting cycle guard so a layer cannot be parented to itself or one of its descendants.

## Notes

The relative-position parenting change is applied when selecting a parent from the layer list Parent dropdown. The child keeps its visible canvas position and stores the appropriate local offset relative to the selected parent, matching After Effects-style parenting expectations.
