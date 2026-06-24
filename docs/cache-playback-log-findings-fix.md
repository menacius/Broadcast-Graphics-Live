# Cache playback log findings fix

The diagnostics revealed two independent failures:

1. A prerender job could publish a transient 1x1 transparent sparse payload as a valid frame. Suspicious empty payloads are now verified using a fresh GPU render session before cache publication.
2. The Phase 9 GPU presentation pool used `QImage::cacheKey()` as a persistent texture identity. That value only identifies Qt implicit-sharing data and is not a durable cache-frame ID. GPU cached-frame pooling is disabled for final/prefix submissions until an explicit immutable frame key is passed to the render session.
