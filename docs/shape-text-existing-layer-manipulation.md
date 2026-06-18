# Shape/Text Existing Layer Manipulation

This update keeps the Shape and Text draw tools usable for existing selected layers:

- When the Shape tool is active and the current selection is a shape/solid-rect layer, transform handles, origin, rotation and corner-radius handles remain interactive.
- When the Text tool is active and the current selection is a text layer, transform handles, origin and rotation remain interactive.
- A click/drag on an existing selected layer's handles starts manipulation instead of creating a new layer.
- A click away from the selected layer keeps the previous draw-tool behavior and starts a new Shape/Text layer.
- Hovering a shape layer now shows its corner-radius handles together with the Illustrator-style hover outline, so the user can see the available corner controls before selecting/manipulating it.

This is editor-only canvas interaction behavior and does not affect OBS/source rendering.
