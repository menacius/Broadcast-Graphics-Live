# Phase 12C — Persistent GPU Text Renderer

Phase 12C replaces the normal Text/Clock final-pixel adapter with a GPU-native
consumer of the immutable Phase 12B layout. The canonical rich-text document
and Qt shaping backend remain unchanged; only the final glyph materialization
and composition path changes.

## Data flow

1. `RichTextDocument` is evaluated at the layer time.
2. `cached_text_layout()` returns immutable glyph, cluster, run, line, and font
   fingerprint data.
3. `text_layout_paint_runs()` resolves fill, gradient, inline stroke, underline,
   and strikethrough without invalidating the shaped layout. Immutable cursor
   boundaries stored on each cluster split ligatures into exact paint slices,
   so one shaped cluster can retain multiple gradient and stroke styles.
4. `bgs::gpu_text::Renderer` reconstructs the exact shaped font face and adds
   only missing glyphs to a bounded session-local atlas.
5. Each grayscale glyph mask is converted to an R8 signed-distance field.
6. Adjacent compatible glyph/decorations are batched without reordering the
   original glyph stream, even when inline styles or atlas pages alternate.
   Composition uses three global phases: strokes behind the fill, all fills and
   decorations, then strokes in front. The explicit Behind/Front choice is
   honored for outer, mid, and inner alignment; an inner stroke placed behind
   an opaque fill is therefore intentionally hidden.
7. The inactive per-layer BGRA target is rendered and published only after a
   successful draw. The previous target stays valid during replacement.
8. The resulting texture enters the existing unified compositor for layer
   transforms, parent transforms, masks, effects, blend modes, temporal motion
   blur, Preview/Program presentation, and final cache readback.

## Resource and lifecycle contract

- Atlas pages are 2048 × 2048 R8 dynamic textures, capped at eight pages per
  render session.
- Atlas identity includes the exact face fingerprint, pixel size, glyph ID, and
  shaping variant required by synthetic bold/italic and H/V scale.
- Paint-only changes do not rebuild shaping. Cluster cursor geometry is reused
  to clip a ligature or complex-script cluster into multiple inline materials.
- Every paint batch carries its own fill and stroke material, allowing multiple
  independent solid/gradient and stroke styles in one text box while keeping
  gradient coordinates in the existing text-box space.
- Each text layer owns two texrender targets. Rendering always happens into the
  inactive target; publication is transactional.
- All OBS GPU resources are created, updated, and destroyed while the graphics
  context is held.
- Switching a layer between GPU text, primitive, direct image, or compatibility
  raster explicitly releases resources owned by the previous adapter.
- Session destruction releases layer targets before resetting atlas pages and
  the text shader.

## Phase 12C2 editor integration

- The object-level text stroke is injected as the canonical document fallback
  at render time. Sparse `RichTextCharStroke` ranges continue to override it,
  so layer-wide order changes and mixed inline stroke styles use the same GPU
  paint plan.
- Opening the Gradient tab no longer commits the inline text adapter. The
  selected UTF-8 range remains active while the gradient tool is temporarily
  enabled.
- The gradient panel is a non-modal tool window and remains open during canvas
  interaction.
- Canvas gradient handles read and write the selected range's `RichTextFill`.
  They fall back to layer-level gradient fields only when no inline text range
  is being edited.
- Switching back to the Text tool restores the hidden inline editor and its
  exact selection instead of starting a new edit session.

## Compatibility boundaries

The exact legacy raster adapter remains active for:

- color-font glyph tables (`COLR`, `CBDT`, `sbix`, or `SVG `);
- Ticker layers;
- active text-unit transitions that still isolate character, word, or sentence
  surfaces;
- inline strokes larger than the configured SDF spread;
- a font face that cannot be reconstructed with the Phase 12B fingerprint.

These cases fall back per layer. They do not disable the GPU text backend for
other compatible layers. A shader compilation failure disables the backend for
that render session and safely retries through the compatibility path on the
next model update.

## Validation

- `tools/test_gpu_text_sdf.py` compiles the actual C++ distance-field source with
  warnings-as-errors and tests sign, padding, invalid inputs, and deterministic
  output.
- `tools/audit_gpu_text_phase12c.py` verifies CMake wiring, atlas format and
  limits, double-buffered publication, source integration, lifecycle release,
  global stroke ordering, multi-paint cluster slicing, cache ABI invalidation,
  editor selection preservation, non-modal canvas gradient interaction,
  compatibility gates, and absence of full text-raster APIs inside the new
  renderer.
