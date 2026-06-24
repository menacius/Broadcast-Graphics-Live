# GPU Image Pipeline — Phase 10

Phase 10 keeps decoded PNG/JPEG and bucketed SVG source textures resident in the unified GPU session and moves image-box clipping and rounded-corner coverage into the mandatory GPU copy shader.

## Contract

- Bitmap files are decoded once at source resolution and reused as persistent layer textures.
- SVG files use aspect-preserving raster buckets; move, rotate and interactive box changes reuse the current GPU texture.
- Fit, Fill, Stretch and anchor changes update logical geometry instead of resampling bitmap pixels.
- `crop outside box` is evaluated in logical image coordinates by the GPU shader.
- Four independent image corner radii are evaluated by the same shader and remain stable across adaptive-resolution modes.
- CPU/Cairo image-box clipping is retained only for decorated image layers that still require the legacy base-raster path (background, outline or surface-expanding effects).
- The renderer cache ABI is bumped so stale prerender payloads cannot retain the previous image geometry.

## Remaining image work

Image outlines and surface-expanding background/effect padding still need a dedicated expanded GPU local target before every decorated image layer can bypass Cairo base-raster generation.
