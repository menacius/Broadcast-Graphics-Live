# Distribute individual bounds and toolbar shortcut fix

- `Distribute to Bounds` now derives an axis-aligned bound independently for each selected layer from its own animated size, origin, scale, rotation and position.
- Equal spacing is calculated only from the trailing bound of the first object, the leading bound of the last object and the extents of intermediate objects. The aggregate selection rectangle is not treated as a distribution bound.
- Tool shortcuts are handled by the editor-wide event filter so they work while focus is inside any editor dock or child widget.
- Shortcuts trigger the sidebar buttons' actual actions, keeping checked states, icons and the active canvas tool synchronized.
- Shortcut keys are ignored while editing text/numeric input fields.
- Dynamic Shape and Text tooltips retain their keyboard shortcut labels.
