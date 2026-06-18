# Direct Cropped Texture and Editor Preview Rendering

This update completes the fourth sparse-cache optimization stage.

## OBS playback

Cached alpha-cropped frames are uploaded directly as cropped `GS_BGRA` textures. The OBS source keeps the full logical title canvas size, while the cropped texture is rendered at its stored canvas X/Y offset. The previous temporary full-canvas reconstruction and zero-fill copy are removed.

The texture allocation now follows payload dimensions rather than canvas dimensions. A layout change recreates the texture only when the cropped payload size or offset changes. Uncached rendering still uses a normal full-canvas texture.

## Editor preview

The canvas preview reads the sparse frame metadata before converting the image to a pixmap. It stores the crop offset separately and draws the pixmap at that position using its own dimensions, instead of scaling it across the complete title canvas.

Color sampling also translates canvas coordinates into cropped-payload coordinates. Pixels outside the sparse payload are treated as transparent.

## Result

- No stretched cached previews in the editor.
- No full 1920×1080 staging buffer for sparse cached OBS playback.
- GPU uploads transfer only the visible payload.
- The OBS source and editor preserve the original full canvas coordinate system.
