# Rich-text stroke alignment mask fix

Text stroke alignment now uses a dedicated opaque glyph alpha mask rather than the rich-text document's inline fill colours and opacity. This makes the doubled-width outline isolation deterministic:

- Outer removes the complete glyph interior from the stroke layer.
- Mid keeps the normal centred outline.
- Inner keeps only pixels inside the glyph silhouette.

Inner text strokes are composited after the fill even when the general stroke order is `Behind Fill`, because an entirely interior stroke would otherwise be fully obscured by an opaque text fill. Outer and Mid continue to respect the configured stroke order.
