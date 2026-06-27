# Development Version 005 — Layer-mask GPU composition fix

## Root cause

The GPU mask shader and the retained matte copy pass draw full-canvas sprites, but those passes did not establish an identity model matrix. Depending on the OBS/editor presentation state or the transform used by the previously rendered layer, the fullscreen quad could inherit translation, scale, or rotation. The mask textures were valid, but the composition quad could be displaced or clipped, making layer masks appear completely non-functional.

## Fix

- `apply_gpu_mask()` now wraps the full-canvas shader draw in `gs_matrix_push()`, `gs_matrix_identity()`, and `gs_matrix_pop()`.
- `copy_full_canvas_gpu_texture()` applies the same transform-neutral contract while publishing retained matte textures.
- The fix covers editor presentation, live OBS rendering, cached frames, nested track mattes, and scene-mask matte dependencies because they share these GPU stages.
- Delivery identity was advanced to **Development Version 005**.

## Validation

Static contract tests verify that both full-canvas mask stages reset and restore the graphics matrix. A complete plugin build still requires the OBS/libobs SDK on the build host.
