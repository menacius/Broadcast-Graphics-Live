# Motion blur consistency for bitmap, alpha bitmap and SVG layers

Motion blur now uses the same temporal sampling path in the editor, prerender/cache output and the live OBS source.

## Fixes

- Bitmap, transparent bitmap and SVG layers are rendered at every shutter sample instead of reusing one frozen cached raster.
- Animated transform, size, opacity, effects and alpha are therefore evaluated at the correct sample time.
- Non-Normal blend modes no longer neutralize the animated transform before motion-blur sampling; these layers use the full-canvas blend path.
- Zero-opacity or zero-shutter motion blur falls back to the same base rendering path in both editor and OBS.
- File image decoding remains cached, so temporal sampling does not repeatedly read bitmap or SVG files from disk.
