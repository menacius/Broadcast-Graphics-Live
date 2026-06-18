# Cached playback render-lag fix

Cached OBS playback is now a strict realtime-safe path:

- cache hits upload straight-alpha BGRA bytes directly to the dynamic OBS texture;
- premultiplied-to-straight conversion is performed once by the background cache worker;
- the OBS tick no longer deep-copies the complete title graph for every cached frame;
- cache misses hold the last valid texture and queue background work instead of invoking the synchronous Cairo renderer;
- automatic external-asset hash verification is deferred while playback is active;
- disk frame format version 4 stores the OBS-ready straight-alpha payload, invalidating older premultiplied cache files safely.

This removes the per-frame full-image conversion, duplicate 8 MB CPU copy, scalar unpremultiply pass, and synchronous fallback render that could exceed OBS's frame budget even when a title was nominally cached.
