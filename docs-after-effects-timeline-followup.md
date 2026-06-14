# After Effects Timeline Follow-up Fix

This update refines the AE-style layer controls introduced in the previous build.

## Layer stack UI

- Increased the visible width of the layer `Mode`, `Parent`, and `Mask` drop-down controls.
- Updated the layer list header so the columns line up with the wider controls.
- Added a dedicated `Mask` header label for the track matte / mask column.

## Parent / child movement

- Corrected canvas movement behavior for parented layers when a parent and its child are selected together.
- During move operations, children with a selected parent are no longer moved directly by the mouse delta.
- The child now follows the parent's actual transform delta through the parenting chain, matching After Effects behavior and preventing double movement.
- The same parent-aware movement rule is applied to keyboard nudging.
