# Phase 15 runtime visibility recovery

The first GPU-only renderer removal was rolled back because runtime coverage was
not complete: text and vector layer rasters could fail before entering the
shared compositor, leaving only decoded image textures visible in both live and
cached output. This recovery restores the last known working Phase 14 artwork
pipeline, including the proven GPU SDF text path, GPU primitive path, effects,
masks and compatibility fallback for unsupported artwork.

The rollback is intentional and follows the Phase 15 gate: legacy artwork paths
must be removed only after text, image, shape, masks and effects have passed
real OBS runtime parity tests. The dynamic cache preference remains 16 MB to 50%
of installed physical RAM. Cache ABI v24 invalidates blank frames produced by
the failed Phase 15 renderer generation.
