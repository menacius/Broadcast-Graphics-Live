# Phase 14 — GPU prerender and disk-cache pipeline

## Audited state before Phase 14

| Requirement | Previous state |
|---|---|
| Same graph for live/editor/prerender | Mostly present: final cache rendering already entered the unified GPU compositor. |
| GPU-resident RAM frames | Missing as the cache contract; completed frames were primarily retained as `QImage` payloads and uploaded again by consumers. |
| Asynchronous staging | Missing: one staging surface was copied and mapped immediately. |
| Double/triple-buffered readback | Missing. |
| Compression off the render worker | Missing: LZ4 and file I/O ran in the cache render worker. |
| No intermediate readback | Present in the main full-frame path, but the dirty-tile fallback still performed a final full-frame readback and CPU-side image work. |
| Dirty-region invalidation | Partial: tile bookkeeping existed, but the old update path rendered/read back a complete frame and merged with CPU image operations. |
| Effect-aware expanded bounds | Partial and symmetric; directional shadows and several halo effects were not represented accurately enough. |
| GPU tile work | Missing: tile decisions were made after a full CPU image existed. |

## Phase 14 contract

The prerender worker submits the same `TitleGpuRenderSession` graph used by the OBS source and editor preview. A completed graph result is copied into a stable process-wide GPU render target before any staging operation.

```text
Unified GPU render graph
    ├── GPU RAM cache: stable GS texture/render target
    ├── live/editor: direct texture submission and presentation
    └── SSD cache: final-frame staging ring -> one CPU map -> writer queue -> LZ4/file I/O
```

### GPU RAM tier

`CacheFrameKey::toString()` is the immutable GPU cache token. Source and editor consumers ask for this token first and submit the resident texture directly to their render session. New prerender output is not inserted into the legacy `QImage` RAM cache. That cache remains only as a bounded compatibility hydration buffer for frames loaded from SSD.

### Final asynchronous readback

Each GPU render session owns three independent staging slots. The entire prerender submission is wrapped in the `FinalFrameOnly` readback contract, disabling legacy compatibility helpers that would otherwise map an intermediate GPU surface. Submission renders the frame, stores the full result in GPU RAM, optionally extracts a safe dirty region on the GPU, and calls `gs_stage_texture`. Mapping is deferred until older submissions have overlapped later GPU frames or the queue must drain. No layer, mask, effect, or intermediate surface is mapped.

OBS graphics does not expose a portable explicit fence in this plugin contract, so `gs_stagesurface_map` remains the synchronization boundary. Triple buffering moves that boundary away from the producing frame and prevents the worker from mapping immediately after every staging copy.

### SSD writer

The mapped final payload is transferred to `DiskFrameCache` through a dedicated writer queue. LZ4 compression, temporary-file writing, atomic replacement, manifest updates, and byte accounting no longer execute in the GPU render loop. Writer generations are checked after the disk-cache lock is acquired, so a dequeued write cannot recreate data after cache clearing or a directory switch.

### Dirty regions and effects

Invalidation retains 256×256 tile bookkeeping. Layer bounds now cover the full animated position/scale/size/font envelope and expand independently on each side for animated backgrounds, outlines, directional drop/long shadows, glow, bloom, blur, motion blur, and emboss. Animated rotation, parented graphs, track mattes, scene masks, and halo/blur effects conservatively select full-frame staging because a local rectangle cannot safely describe all dependent pixels. Old and new layer bounds are united. Because edits create a new content-addressed key, the patch path resolves the predecessor through the title’s last published visual hash rather than incorrectly looking only for the new key. A dirty union below 60% of the canvas is extracted into a region-sized GPU target before staging; larger or fragmented changes use one full-frame staging copy.

Blur/halo effects and blur transitions conservatively force a full final-frame readback so tile edges cannot clip effect kernels. CPU `QPainter` compositing and the retired per-tile/full-image merge functions are absent from the cache manager; the only CPU patch operation is row-copying the already-read final dirty rectangle into the previous SSD payload.

### Compatibility boundaries

The immediate `title_gpu_render_session_readback()` API remains for non-prerender compatibility callers. Phase 14 worker code does not call it. SSD-loaded frames can still use the older `QImage` submission path when a GPU token has been evicted; live/editor output otherwise uses direct GPU texture presentation.
