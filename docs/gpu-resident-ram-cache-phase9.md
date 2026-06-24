# Phase 9 — GPU-resident RAM-cache presentation

The cache worker still publishes `QImage` payloads because SSD persistence and
LZ4 compression require CPU-addressable bytes. Interactive/live presentation no
longer treats those payloads as disposable uploads.

Each `TitleGpuRenderSession` now owns an LRU-like GPU texture pool keyed by the
implicitly-shared `QImage::cacheKey()`. When playback revisits a RAM-cached
frame, the compositor reuses the existing `gs_texture_t` directly. This removes
repeated CPU-to-GPU uploads during loops, ping-pong playback and cue replay.

Contract:

- Cache worker: GPU graph -> one final readback -> RAM/SSD payload.
- First presentation of a payload: one GPU upload.
- Subsequent presentation in the same render session: texture reuse.
- Live/editor effects and composition remain GPU-resident.
- Staging/readback remains restricted to cache generation, export, screenshots
  and thumbnails.
- The presentation pool has a byte budget and evicts least-recently-used
  textures while protecting the currently submitted final frame and cached
  prefix.

This phase intentionally does not remove the CPU-addressable SSD transport
payload. A later phase can introduce shared cross-session GPU handles and an
asynchronous readback queue for disk-only persistence.
