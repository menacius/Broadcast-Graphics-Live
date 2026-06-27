# Development Version 008 — Adjustment coverage and editor compositing

- Adjustment layers now use their transformed box as an explicit GPU coverage region.
- Position, size, scale, rotation, parenting, opacity and transitions constrain the affected pixels.
- Track mattes are resolved through the same nested GPU mask graph and multiply the adjustment coverage.
- The effect stack is evaluated on the lower-layer composite, then mixed back only through the coverage texture.
- Editor transparency checkerboards are excluded from the artwork render graph: title layers and adjustments render on transparency first, then the checkerboard is composited underneath for presentation.
- OBS Preview/Program continues to evaluate destination-aware compositing against the actual scene background.
