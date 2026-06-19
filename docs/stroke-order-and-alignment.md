# Stroke order and alignment

Layer appearance strokes now keep draw order and path alignment as independent settings.

- Order: Behind Fill (default) or In Front of Fill.
- Alignment: Outer (default), Mid/centered, or Inner.
- Outer and Inner use a double-width centered stroke that is isolated to the outside or inside of the object path, preserving the requested visible width.
- Text strokes use an alpha mask so rich-text shaping, inline styles, gradients, and ligatures remain intact.
- Shape strokes use Cairo grouping/clipping for equivalent output in the editor and OBS render path.
- New and legacy files without these properties default to Behind Fill + Outer Stroke.
