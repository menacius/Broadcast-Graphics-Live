# Playback controls and static checkerboard canvas background

This update fixes two editor-side behaviours:

- **Play after rendering** now gates timeline advancement on the availability of the exact cached frame that would be displayed next. When the frame is not ready yet, the playhead holds on the current frame while the cache queues/renders the missing frame. This prevents the playhead from running ahead while the canvas keeps showing an older cached image.
- **Play every frame** continues to advance by one OBS frame per timer tick and is not affected by skip-frame or speed controls. Combined with Play after rendering, it advances one cached frame at a time only when that frame is available.

The canvas transparency background was also changed from per-cell drawing on every paint to a cached 2x2 checkerboard tile rendered with `drawTiledPixmap()`. The tile is anchored to the editor viewport, not the canvas origin, so panning behaves like a static Photoshop-style transparency backdrop and avoids repainting thousands of individual squares while dragging the canvas.

These changes are editor-only and do not affect OBS source output or exported title rendering.
