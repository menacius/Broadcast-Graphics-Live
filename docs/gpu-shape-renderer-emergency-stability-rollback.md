# GPU Shape Renderer Emergency Stability Rollback — Historical Note

The original Phase 11 primitive path was disabled after two backend-critical regressions:

- full black canvas frames during interactive shape resize;
- an OBS shutdown deadlock between the display callback and GPU session destruction.

The shutdown issue was corrected by standardizing the lifetime lock order as OBS graphics lock first, then the session mutex, with the destruction gate raised before waiting.

The shape path has now been reactivated with a transactional lifetime contract:

- each layer renders into the inactive member of a double-buffered primitive-target pair;
- the active primitive generation changes only after a successful render;
- the compositor refuses to publish a frame with an incomplete required replacement;
- the finished title is copied into independent presentation storage;
- unsupported semantics and primitive-shader failures route to the stable Cairo base-raster generator.

See `gpu-shape-renderer-phase11.md` for the current contract and runtime acceptance cases.
