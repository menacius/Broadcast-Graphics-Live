# Cache/Prerender Background Worker and Realtime Fallback

This pass replaces the UI-timer cache renderer with an actual background worker and makes the proven uncached renderer the correctness fallback for missing cached frames.

## Architecture

- Prerender jobs are immutable deep snapshots. Layer objects are not shared with the editor while the worker is rendering.
- One cancellable background worker owns cache rendering and disk hydration. The Qt/OBS UI thread no longer executes `render_title_to_image()` from a timer callback.
- Every job carries a global cache epoch and a per-title generation. Cache toggles, clears, title deletion, save cancellation and invalidation can cancel dequeued/in-flight work without allowing obsolete frames to be committed.
- The final generation check and RAM/SSD publication share one publication gate. Clear, invalidate, row refresh and title removal therefore cannot interleave between validation and commit, and live-cue keys are revalidated while their state map is locked.
- Cancelled jobs relinquish `Queued`/`Rendering` ownership explicitly. A stale worker generation cannot leave a title or live-cue row permanently stuck in a transient state. Partial cache residency without an active job is reported as incomplete rather than falsely reported as queued.
- The queue tracks active job identities, so a slow frame requested at OBS frame rate is not enqueued repeatedly while the same generation is already rendering.
- Realtime jobs and live-cue jobs remain eligible while normal prerender is paused. Normal full-timeline work stays paused.

## Realtime source behavior

- `obs_source_video_tick` and the editor preview perform RAM-only cache lookup. Disk payload reads and writes are performed by the worker.
- A normal cache miss renders through the existing uncached source path until the exact cached frame becomes available. `Cached frames only` preserves the explicit hold behavior.
- Live-cue misses always use the exact uncached runtime snapshot, including Background Persistence, so a missing transition frame cannot display an unrelated cached frame. The realtime cache key is built from that same snapshot rather than re-applying a cue row and accidentally discarding an in-progress transition.
- Identical cached `QImage` instances are not converted, unpremultiplied and uploaded to the OBS texture on every tick.
- The visual content hash is reused across timeline ticks, recalculated immediately on title/source revision changes, and verified at a low frequency for externally replaced image assets instead of on every frame.

## RAM and disk cache

- RAM, disk, queue, playback settings and live-cue state maps are synchronized for concurrent worker/source/editor access.
- Repeated disk membership checks use a manifest-backed in-memory index; realtime source/editor paths do not probe the filesystem per frame.
- Disk frames use the existing versioned BGLF container. The build prefers an installed LZ4 package and otherwise fetches the pinned official LZ4 1.10.0 source automatically. BGRA payloads are compressed only when the result is smaller; raw version-2 and legacy version-1 frames remain readable.
- Frame dimensions and payload sizes are validated before allocation/decompression.

## Invalidation and identity

- The visual hash now includes all renderer-relevant layer, rich-text, transform, gradient, effect, background, image-file and animated-property data.
- Image cache identity includes file size and modification time whenever the visual identity is recalculated, so a replaced file does not reuse an older content key after the title/source revision refreshes.
- Static titles normalize to frame zero; animated titles retain frame-addressed cache keys.
- Queue ranges and full-timeline prerenders calculate the visual hash once and share one immutable title snapshot across their jobs.

## Validation notes

- Source formatting and structural checks pass (`git diff --check`, balanced delimiters, declaration/definition consistency and stale worker-API scans).
- The animation model regression test passes.
- The LZ4 block roundtrip was exercised with an 8,294,400-byte 1920×1080 BGRA test buffer.
- Full plugin compilation requires the OBS/libobs SDK and Qt development package. They are not installed in the validation container, so CMake stops at `find_package(libobs)` before compiling project sources.
