# Image motion blur sampling and live cue Enter behavior

- Bitmap, bitmap-with-alpha and SVG motion blur now uses adaptive temporal sampling based on the layer's pixel travel across the shutter interval. This prevents low-sample horizontal ghost copies on sharp image edges.
- Temporal samples use shutter-bin midpoints and discard duplicate clamped times, preventing exposure endpoints from being over-weighted.
- Live text cue editors now commit with Enter and move focus to the next editable cue cell.
- Shift+Enter is the only Enter combination that inserts a newline inside a live text cue cell.
