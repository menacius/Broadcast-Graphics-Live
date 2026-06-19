# Full Code Performance, Reliability and Security Audit

## Scope

Static review of the complete OBS Graphics Studio Pro source tree (53 C/C++ files, approximately 48,700 lines), with emphasis on editor rendering, cache/prerender, persistence, threading, file handling, ownership and shutdown behavior.

A full binary build was not possible in the audit environment because the OBS `libobsConfig.cmake` package is unavailable. The code was nevertheless checked structurally and CMake configuration was run up to dependency resolution.

## Fixed in this audit

### 1. Unbounded detached save threads — High

`TitleDataStore::save_async()` created a new detached `std::thread` for every save. Rapid editor changes could therefore cause repeated deep title/layer copies, simultaneous JSON serialization, excessive thread creation, shutdown races and background access after plugin teardown.

Remediation:

- Replaced detached-per-save threads with one lazy, long-lived save worker.
- Pending save requests are coalesced; a newer pending snapshot replaces the older pending snapshot.
- Added deterministic worker shutdown and `join()` in `TitleDataStore` destruction.
- Serialized synchronous and asynchronous writes with a dedicated I/O mutex.
- Consolidated duplicate synchronous/asynchronous JSON write logic into one atomic-write helper.
- Kept the previous file until the temporary file has been fully written.

### 2. Recursive template traversal through symbolic links — Medium

Recursive category enumeration accepted directory symlinks. A cyclic link could cause repeated traversal, UI stalls or stack exhaustion; links could also expose paths outside the intended template library.

Remediation:

- Added `QDir::NoSymLinks` to all recursive template-category directory scans.

## High-priority findings requiring architectural work

### 3. Editor rendering remains synchronous on the GUI thread — High performance risk

`CanvasPreview::render_to_pixmap()` performs title rasterization and `QPixmap::fromImage()` in the paint/update path. Adaptive scaling reduces work but does not remove the fundamental blocking behavior. Any expensive layer/effect still blocks input dispatch.

Recommended remediation:

- Maintain immutable editor render snapshots.
- Render `QImage` frames on a dedicated editor worker.
- Publish only the newest completed generation to the GUI thread.
- Never create or mutate `QPixmap` outside the GUI thread.
- Present transformed selected-layer proxies immediately during drag/nudge, then refine asynchronously.

### 4. Cache worker combines rendering, conversion, alpha scanning, compression and disk I/O — High performance risk

The single cache worker performs full rendering, format conversion, `alpha_bounds()` scanning, sparse cropping, LZ4 encoding and disk writes serially. This creates long CPU/I/O bursts and makes throttling coarse.

Recommended remediation:

- Separate render, encode and disk-I/O stages with bounded queues.
- Use low-priority I/O workers and explicit OBS load/backpressure signals.
- Skip alpha-bound scanning when renderer dirty bounds are already known.
- Batch manifest updates and disk flushes.
- Cap speculative live-cue work by CPU-time budget, not only sleeps.

### 5. Full-frame alpha scan on every newly rendered cache frame — High performance risk

`make_sparse_frame()` calls `alpha_bounds()`, which scans the full image pixel-by-pixel. At 1920×1080 this is over two million alpha tests per frame before compression.

Recommended remediation:

- Propagate conservative content/dirty bounds from the renderer.
- Scan only changed tiles or the union of layer bounds.
- Fall back to full scan only when bounds are unknown.

### 6. RAM cache LRU updates are O(n) — Medium/High performance risk

`RamFrameCache::get()` and `put()` call `QVector::removeAll()` for every cache hit/update, and eviction removes from the front. Large caches therefore spend increasing time moving/searching elements while holding the cache mutex.

Recommended remediation:

- Replace the vector with `std::list<CacheFrameKey>` plus a key-to-list-iterator hash.
- Make hit promotion and removal O(1).

### 7. Very large translation units and duplicated rendering responsibilities — Medium maintainability/performance risk

Several files exceed 4,000–5,900 lines. Rendering/state logic is distributed between `title-source.cpp`, `canvas-preview.cpp`, `cache-manager.cpp`, and editor code. This encourages duplicated branches and makes it difficult to guarantee that editor, cache and OBS rendering use identical semantics.

Recommended remediation:

- Extract a shared immutable `RenderRequest` and renderer service.
- Split effect evaluation, layer traversal, cache publication and OBS texture upload into separate units.
- Remove editor-specific branches from the OBS source renderer and vice versa.

### 8. Broad recursive mutex use — Medium concurrency risk

The cache subsystem uses multiple recursive mutexes alongside Qt mutexes and standard mutexes. Several publication paths hold more than one lock. Recursive locks hide re-entry and increase deadlock/priority-inversion risk.

Recommended remediation:

- Document and enforce a global lock order.
- Replace recursive mutexes with non-recursive locks after separating callbacks/publication.
- Never emit Qt signals or perform disk/render work while holding shared state locks.

### 9. Editor quality cache uses broad clear-on-limit behavior — Medium performance risk

The private editor quality cache clears all entries when it reaches its threshold. This creates periodic latency spikes and destroys useful locality.

Recommended remediation:

- Use a small byte-bounded LRU.
- Include title revision/content hash in the cache key.
- Evict incrementally rather than clearing the entire cache.

### 10. JSON persistence maximums are generous — Medium resource-exhaustion risk

The loader permits very large JSON and embedded image payloads. Although counts and string lengths are bounded, a crafted import can still cause significant memory/CPU use.

Recommended remediation:

- Use a lower default import limit for individual templates.
- Validate base64 decoded size before allocation.
- Reject extreme dimensions/effect sample counts before renderer allocation.
- Add time/complexity limits for imported rich-text and keyframe data.

## Additional observations

- Most Qt widgets use QObject parent ownership correctly; no obvious systematic QWidget leak was found.
- `TitleSourceData` has a matching destroy path, but all OBS graphics resources should continue to be reviewed under graphics-context rules.
- User-selected cache directories should remain dedicated cache roots; clearing a shared directory based only on extension is undesirable.
- Logging in per-frame/cache-hot paths should remain disabled below warning level in release builds.
- Repeated `QImage::convertToFormat`, sparse expansion and copy operations should be measured with representative 1080p/4K titles.

## Recommended validation

1. Build with AddressSanitizer and UndefinedBehaviorSanitizer in a standalone OBS test profile.
2. Run ThreadSanitizer on editor/cache stress tests where supported.
3. Record ETW/WPA or Instruments traces during drag, key nudging, Save and live-cue prerender.
4. Add automated tests for rapid save/close, scene-collection switch during save, malformed imports, cache clear during render and plugin shutdown with pending jobs.
5. Add frame-time budgets and counters for editor render, cache render, alpha scan, compression, disk write and OBS texture upload.
