# Unified GPU Pipeline Re-audit

Date: 2026-06-23

## Scope

This audit covers the editor native OBS display, title GPU render sessions,
full and partial prerender presentation, OBS Preview/Program source rendering,
scene-mask composition, GPU resource lifetime and canvas overlay caching.

## Correctness fixes

### Isolated final-frame presentation shader

Cached frames, partial-cache bases and stable final publication previously reused
the layer-copy shader. That shader retains per-layer wipe and image-crop
uniforms. A wipe or rounded image clip from the last rendered layer could
therefore be applied to the complete title frame and make Preview/Program or the
editor output partially or fully transparent.

Final/cache presentation now uses a dedicated blit effect with only `image` and
`ViewProj`. Layer transitions and image clipping remain exclusively in the
layer-copy shader.

### Atomic partial-cache publication

A cached-prefix submission previously updated the dynamic layer range and then,
in a second lock acquisition, attached the cached base. The display thread could
render the intermediate range-only state. A transaction guard now preserves the
last complete published texture until both operations are complete.

### Consistent GPU lock order

Explicit readback used `session mutex -> OBS graphics`, while display callbacks
and session destruction use `OBS graphics -> session mutex`. The inverse order
could deadlock a cache worker against Preview/Program. All GPU/readback lifetime
paths now use graphics-first ordering.

### Serialized preview-quality mutation

Preview quality and editor-draft state were mutated without the session mutex,
while the OBS display callback read them and traversed the same layer resources.
The mutation is now serialized with draw/update operations.

### Scene-mask alpha composition

The scene-mask pass selected a premultiplied-alpha blend function but did not
explicitly enable blending. When the inherited OBS state had blending disabled,
the transparent full-canvas scene-mask quad could overwrite the title source.
The pass now explicitly enables `ONE / INVSRCALPHA` blending.

### Cached-only visible startup

On a first cache miss or rejected payload, an OBS source had no published texture
to hold and returned permanently transparent. Cached-only playback now renders
one live bootstrap poster when no frame has ever been published. Once a valid
frame exists, later cache misses continue to hold it until the exact payload is
ready.

### Failure-domain isolation

The experimental primitive-shape shader was disabled by the renderer contract
but still mandatory at compositor startup. It is now lazy and optional.
Layer-copy, blend and mask shaders are also lazy, so a valid full prerender frame
can be presented using only the minimal frame-blit path. A backend-specific
failure in an unused live shader can no longer suppress cache presentation.

### Canvas tooltip and overlay invalidation

Drag tooltips, snap labels, guides and cursors are baked into the GPU overlay
cache. Mouse release cleared interaction state but often requested a repaint
without invalidating that cached overlay. All relevant release paths now
invalidate and repaint the overlay, and `clear_snap_feedback()` enforces the
same contract centrally.

### Actionable diagnostics

GPU session errors can now be copied safely through
`title_gpu_render_session_last_error()`. Source draw failures log the title,
consecutive failure count and concrete compositor error. Editor canvas draw
logging also includes the session error.

## Duplicate/conflict audit

The cache-critical shared contracts from the prior audit remain centralized:
cache-frame placement, time-to-frame ownership, immutable title snapshots and
live-text cue ordering.

The exact normalized clone audit still reports 13 pre-existing groups. The
largest are parallel text/vector raster helpers in the editor and OBS renderer.
Those should not be merged without pixel-regression fixtures for text shaping,
SVG/bitmap placement, gradients, corner geometry, effects and masks. Smaller
remaining duplicates include `LongPressToolButton`, the built-in open-color
palette builder and preset parsing helpers.

The GPU texture pool for immutable cache frames is still dormant because cache
submissions deliberately use `image_key = 0`; this avoids stale QImage identity
reuse but leaves a performance optimization for a future explicit immutable
frame-ID contract. It is not used for correctness in this change.

## Verification

Passed:

- `tools/audit_gpu_pipeline.py`: 15 GPU contract checks.
- `tools/audit_cache_contract.py`: 16 cache/structure checks.
- `tools/audit_code_clones.py --minimum-lines 20`: completed, 13 existing clone groups documented.
- Existing standalone cache-time, sparse-payload, live-cue, title-snapshot,
  animation and transition tests.
- Delimiter balance for both modified implementation files.
- Diff whitespace check.

A full plugin configure/build cannot run in this container because the libobs
development package is absent (`libobsConfig.cmake` / `libobs-config.cmake`). A
Windows Release build and runtime tests in OBS are therefore still required.

## Runtime acceptance cases

1. Play a fully cached animated title in editor Preview, OBS Preview and Program.
2. Repeat with a title containing an active wipe transition and a cropped,
   rounded image layer; the whole title must not inherit that layer's uniforms.
3. Test a title with a scene-mask layer; the title must remain visible outside
   the mask and the masked scene must alpha-composite above it.
4. Enable cached-only playback, clear the cache and cue a title. One visible
   bootstrap frame must appear; subsequent missing frames must hold the last
   valid frame until cache completion.
5. Test a partially cacheable clock/ticker title while prerender completes.
   No transparent intermediate range-only frame should be published.
6. Move, resize and rotate an object, then release the mouse. Position/size/
   rotation tooltips and snap labels must disappear immediately.
