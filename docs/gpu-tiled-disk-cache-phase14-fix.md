# Phase 14 tiled disk-cache correction

The original Phase 14 implementation tracked dirty 256×256 regions and could
stage a dirty union, but the SSD tier still serialized every completed frame as
one alpha-bounded rectangle. Widely separated elements therefore retained the
transparent rectangle between them, and identical static areas were compressed
again for every timeline frame.

The disk cache now uses two content-addressed payload levels:

- `.ogsf` is a small per-frame manifest containing canvas dimensions and tile
  references.
- `.ogst` is an independently LZ4-compressed, SHA-256-addressed BGRA tile.

Only tiles containing non-zero alpha are written. Identical tiles are shared by
all frames and titles in the active cache directory. Reference counts delete a
tile after the last frame stops using it. Startup index rebuilding validates
frame manifests, drops orphan frame files, and removes unreferenced tile blobs.

Disk frame format is version 5 and the renderer cache ABI is
`gpu-renderer-v22-phase14-tiled-disk-cache`, so older monolithic payloads are
invalidated rather than interpreted as tiled frames.

`clearFast()` still atomically detaches the active cache directory, but the
owned disk writer now reclaims the detached `.obsolete-*` generation. The
worker is joined at shutdown; no detached deletion thread can outlive OBS.
