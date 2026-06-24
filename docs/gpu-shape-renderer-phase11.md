# GPU Shape Renderer — Phase 11 Reactivation

Phase 11 provides an analytic GPU/SDF base raster for supported built-in shape layers. This reactivation keeps the primitive shader optional and preserves the exact legacy base-raster path whenever visual parity is not yet guaranteed.

## GPU-native primitives

- Rectangle and rounded rectangle
- Ellipse
- Triangle
- Diamond
- Regular polygon

Eligible shapes no longer create a Cairo/QImage base raster. The primitive is evaluated into the inactive target of a double-buffered, per-layer `gs_texrender_t` pair. Only a successful render becomes the active layer generation. Effects, masks, transitions and title composition remain in the existing GPU graph.

The completed title is copied into a separate double-buffered presentation target. Preview/Program and the editor therefore never sample a primitive target that is being resized or reset.

## Geometry and appearance contract

The analytic target now separates the padded surface from the actual shape box:

- stroke/antialias padding is part of the local GPU surface;
- shape size and offset are independent shader uniforms;
- non-square triangle, diamond and regular-polygon layers retain their box aspect ratio;
- rectangle and rounded-rectangle layers preserve all four live corner radii;
- outer, centered and inner stroke placement is represented analytically;
- front/behind fill ordering is preserved;

Interactive local-size changes explicitly invalidate the primitive raster even when the caller labels the edit as transform-only and the title model revision has not yet advanced.

## Exact compatibility fallback

The following remain on the Cairo base-raster generator:

- arbitrary Pen/Bezier paths;
- gradient fills and gradient strokes;
- non-round rectangle corner bevel modes;
- stars and rounded triangle, diamond or polygon corners until exact analytic corner parity is implemented;
- legacy Line shape layers, pending the planned dedicated line tool;
- outlined polygonal primitives until join-style parity is implemented;
- any optional primitive shader compile failure.

This fallback affects only reusable base coverage generation. Pixel effects and final composition stay on the GPU pipeline.

## Failure and lifetime behavior

- Primitive shader creation is lazy and cannot block cache-only frame presentation.
- A shader compilation failure marks the backend unavailable and clears the layer key, causing the next update to generate the stable CPU base raster rather than retrying a blank first frame indefinitely.
- Primitive redraw uses the inactive target and commits it only after `gs_texrender_get_texture()` succeeds.
- CPU-to-primitive promotion releases the replaced owned texture after successful GPU publication.
- A new primitive generation invalidates the layer effect cache.
- If any required replacement is incomplete, the compositor preserves the last complete frame.
- GPU session destruction continues to use graphics-lock → session-mutex order.

## Runtime acceptance cases

1. Repeatedly create and resize every supported primitive in the editor.
2. Resize with and without active effects, masks, parents and transitions.
3. Test independent rectangle corner radii during direct manipulation and numeric editing.
4. Test rounded triangle and diamond layers and confirm they match the exact editable-path renderer.
5. Toggle stroke front/behind fill and stroke opacity.
6. Test non-square triangle, diamond and polygon boxes, then confirm that stars remain visually exact through fallback.
7. Confirm that Line is absent from the shape toolbar and shape properties selector.
8. Switch an existing layer between gradient/path/rounded-corner fallback and eligible solid primitives.
9. Close the editor and OBS while resizing and while Preview/Program are actively drawing.
10. Confirm that no black full-canvas frame, stale frame, texture leak or shutdown deadlock occurs.

## Cache compatibility

The renderer cache ABI is advanced to `gpu-renderer-v14-phase11-corner-contract`, preventing old sharp-corner frames from being mixed with the corrected corner-radius contract.
