# Inline Text Style Presets

Style presets now support inline application while the on-canvas text editor is active.

- Text style presets can be applied to the current text selection or, when the cursor is collapsed, to the typing format for newly inserted text.
- Gradient style presets can be applied to the current text selection without changing the whole layer.
- Saving a preset while editing text captures the selected/cursor character style instead of only the layer-level mirror values.
- If no inline text edit is active, the existing layer-level preset behavior is preserved.

The implementation routes preset payloads through the rich-text character format pipeline so inline formatting, thumbnails, import/export, categories and search continue to use the same preset library.
