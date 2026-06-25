# Transactional text/prerender recovery and session logging

## Render and cache contract

The compositor now publishes a frame only when every required layer raster is ready. This applies to the first frame of a new render session as well as later replacements. A same-title previously published frame may remain visible during an interactive replacement, but neither asynchronous nor synchronous cache readback may consume that fallback: readback requires a clean frame whose published model revision exactly matches the submitted model.

Prerender jobs retry bounded transient failures at the submit, staging-resolution, and sparse-payload stages. After the retry budget is exhausted the frame remains stale/not cached instead of being reported as complete. Renderer cache ABI `gpu-renderer-v25-transactional-text-prerender` invalidates older payloads that may contain startup-transparent frames.

The editor also queues one model recovery snapshot after a failed GPU draw. This lets a permanent GPU-text backend failure rebuild through the compatibility raster path and lets transient first-frame atlas failures retry without requiring a user edit.

## Logging contract

Logging is global-enable plus per-category. Categories cover module/source lifecycle, title persistence, dock/editor/canvas, text/GPU/effects/masks, cache queue/playback/RAM/disk/prerender, live cues/playlists/timeline/animation/transitions, import/export/assets, preferences, and performance.

Each OBS module session has one stable file named `broadcast-graphics-live_YYYY-MM-DD_HH-mm-ss-zzz.log`. Changing the log directory relocates the current session file; it does not merge multiple OBS sessions into one file. Cache-playback and performance categories default off because they are high-volume.
