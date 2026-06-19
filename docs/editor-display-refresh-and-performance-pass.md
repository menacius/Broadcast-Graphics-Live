# Editor display refresh and performance pass

This pass makes editor playback and interaction follow the refresh rate of the monitor containing the editor window while keeping title time quantized to the OBS frame rate.

## Changes

- Uses a precise playback timer paced from the active screen refresh rate, with a safe 24–240 Hz range and 60 Hz fallback.
- Re-evaluates pacing when the editor is shown or moved to another monitor.
- Skips duplicate playhead updates when multiple display refresh ticks map to the same OBS frame.
- Throttles cache reprioritization to avoid queue churn during playback.
- Limits expensive Properties/Effects panel reconstruction to approximately 30 Hz while the canvas and timeline remain display-paced.
- Avoids a synchronous canvas `repaint()` during inline text refresh and queues the affected region instead.
- Adds a no-op guard to `CanvasPreview::set_playhead()` to prevent duplicate renders and text-editor repositioning.

The result is smoother high-refresh-rate presentation without multiplying renderer, cache, or panel work at 120/144/240 Hz.
