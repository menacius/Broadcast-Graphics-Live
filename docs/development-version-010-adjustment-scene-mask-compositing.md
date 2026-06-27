# Development Version 010 — Adjustment and scene-mask compositing

## Adjustment layers

Adjustment layers use the accumulated lower-layer artwork as their input. Their transformed layer coverage, opacity and optional track matte define the affected region. The editor transparency checkerboard is added after artwork rendering and is never processed by adjustment effects.

## Scene masks

A scene-mask layer remains a normal visible artwork layer in the editor, so its fill and effects are preserved. The editor draws only an identification outline above it.

In Preview/Program, the selected OBS scene is matted and inserted at the scene-mask layer index. After insertion, only layers above that index are recomposited. This preserves ordering among ordinary layers, adjustment layers and multiple scene masks.
