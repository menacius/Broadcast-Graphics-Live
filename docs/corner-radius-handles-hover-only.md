# Corner Radius Handles Hover-Only Behavior

Corner radius handles are now shown only as hover feedback for shape/solid rectangle layers.

## Behavior

- Selecting a shape no longer shows corner radius handles by itself.
- Hovering a shape shows the corner radius handles.
- If the hovered shape is already selected, the selection transform box remains unchanged and only the corner radius handles are added as hover feedback.
- Non-selected hovered shapes still show the Illustrator-style hover outline plus the corner radius handles.
- Existing transform handles and shape/text layer manipulation behavior are unchanged.

This keeps corner radius editing discoverable without permanently cluttering the selected-layer transform UI.
