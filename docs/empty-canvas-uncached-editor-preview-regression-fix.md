# Empty canvas and uncached editor preview regression fix

- A title with no layers now clears only the retained artwork pixmap; the canvas checkerboard, rulers, guides, safe areas, and editor overlays remain visible.
- The editor-facing `requestFrame()` path now renders directly from the live title model while interactive cache bypass is active, even when the cached-frames-only preference is enabled.
- Cache-disabled editor sessions also keep a fully functional uncached realtime preview.
- OBS playback remains unaffected because it uses the separate non-blocking `requestFrameRealtime()` path.
