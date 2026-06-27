# Development Version 011

Restores scene masks as non-artwork matte controls with the editor hatch, inserts each configured OBS scene at its true layer-stack position, and prevents repeated compositing of upper title layers. Adjustment processing uses transparent title artwork rather than the checkerboard; newly generated effect alpha is preserved outside the adjustment coverage so shadows, glow, bloom and blur extents are not clipped to the bounding box.
