# Exact prerender playback fix

- Frame lookup now uses interval ownership (`floor`) instead of advancing half a frame early with `round`.
- Animated prerender frames no longer share canonical payloads across frame keys.
- Realtime playback immediately hydrates a completed `CachedDisk` frame when its RAM payload was evicted.
- Renderer cache ABI was advanced so older frame mappings are invalidated.
