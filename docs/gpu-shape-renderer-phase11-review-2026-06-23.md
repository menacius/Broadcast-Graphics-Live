# OBS Graphics Studio Pro — Phase 11 GPU Shape Renderer Review

Date: 2026-06-23

## Scope

This pass revisits the disabled Phase 11 analytic GPU/SDF renderer in the uploaded `OBS_GSP_source_retention_fixed(1).zip` baseline. The goal is to reactivate the safe primitive subset without reintroducing the historical full-black resize frames or the shutdown deadlock.

## Confirmed issues in the disabled implementation

1. **The renderer was still hard-disabled.** The complete primitive path existed, but layer selection forced `gpu_primitive = false`.
2. **Interactive local-size edits could be skipped.** A transform-only update with changed box dimensions could pass the first geometry test and then be discarded by the generic unchanged-model early return.
3. **Shape and render-surface geometry were conflated.** Stroke and antialias coverage had no independent padding, so outer/centered strokes could be clipped at the primitive texture boundary.
4. **Polygon geometry did not match the legacy circumradius contract.** The previous radial expression passed through vertices but not through the straight edge segments.
5. **CPU-to-GPU promotion leaked the previous owned texture.** On the first successful primitive render, `entry.texture` was replaced by a borrowed texrender texture without releasing the old CPU-uploaded texture.
6. **A new primitive generation could retain an old effected result.** The per-layer effect-cache identity was not explicitly invalidated after successful primitive publication.
7. **Primitive shader failure had no stable escape route.** A backend compilation failure could keep retrying the optional path instead of routing future updates to the proven CPU base-raster generator.
8. **Old prerender frames shared the previous renderer identity.** The cache ABI did not distinguish the reactivated shape publication contract.

## Implemented changes

### Safe Phase 11 selection

The hard-disable is replaced by an explicit eligibility helper. The GPU-native subset is:

- rectangle and rounded rectangle with round corner semantics;
- ellipse;
- fill-only triangle, diamond and regular polygon;
- centered solid-stroke line.

The exact Cairo base-raster path remains active for:

- arbitrary Pen/Bezier paths;
- stars, pending exact concave-star SDF parity;
- gradient fills or gradient strokes;
- non-round corner bevels;
- rounded polygon corners;
- outlined polygonal primitives, pending join-style parity;
- outer/inner line alignment;
- primitive shader compilation failure.

### Transactional texture lifetime

- Each primitive layer retains two texrender targets.
- Redraw always uses the inactive target.
- The active target changes only after the target has rendered and returned a valid texture.
- An incomplete required raster replacement preserves the last complete title frame.
- The completed title is copied into independent double-buffered presentation storage before Preview/Program or the editor samples it.
- CPU-to-primitive promotion destroys the replaced owned texture only after successful GPU publication.

### Correct local geometry

- The GPU surface now has independent `surfaceSize`, `shapeSize` and `shapeOffset` uniforms.
- Stroke/antialias padding is calculated from alignment and width.
- Layer origin and `layer_box_rect` account for the padded surface.
- Independent rounded-rectangle corner radii stay in shape coordinates.
- Polygon evaluation uses the same circumradius vertex/edge contract as the legacy primitive path and preserves non-square aspect ratios.
- Stroke alignment and fill-front/behind ordering are represented explicitly.

### Failure isolation and cache compatibility

- Primitive shader creation remains lazy and optional.
- Compilation failure marks the primitive backend unavailable, clears the affected key and causes the next update to use the stable CPU base raster.
- Successful primitive publication clears the layer effect-cache key.
- Cache ABI advanced from `gpu-renderer-v12-strict-cache-placement` to `gpu-renderer-v13-phase11-shape-publication`.

## Files changed

- `src/obs/title-source.cpp`
- `src/cache/cache-manager.cpp`
- `tools/audit_gpu_pipeline.py`
- `tools/audit_cache_contract.py`
- `tools/audit_gpu_shape_phase11.py` (new)
- `tools/test_gpu_shape_math.py` (new)
- `docs/gpu-shape-renderer-phase11.md`
- `docs/gpu-shape-renderer-emergency-stability-rollback.md`
- `docs/gpu-shape-renderer-phase11-review-2026-06-23.md` (new)

## Verification performed

- Phase 11 structural audit: **12/12 passed**
- Phase 11 numerical polygon/aspect/padding test: **passed**
- Unified GPU pipeline audit: **15/15 passed**
- Source presentation lifecycle audit: **16/16 passed**
- Prerender/cache contract audit: **16/16 passed**
- Five standalone C++ model/cache tests: **passed**
- Python audit syntax checks: **passed**
- C++ delimiter balance: **passed**
- `git diff --check`: **passed**
- Patch dry-run against the exact uploaded baseline: **passed**
- ZIP integrity test: **passed**

The existing clone audit still reports the same 13 normalized clone groups. No new clone family was introduced by this change.

## Build limitation

A complete plugin configuration/build could not run in this container because the libobs development package/configuration is absent (`libobsConfig.cmake` / `libobs-config.cmake`). CMake reaches the required `find_package(libobs)` call and stops there. The change therefore still requires a normal Windows + OBS SDK build and runtime acceptance pass before it should be considered release-ready.

## Required runtime acceptance

1. Create and repeatedly resize each GPU-eligible primitive in the editor.
2. Resize while Preview and Program are both drawing the source.
3. Test outer/center/inner strokes on rectangles and ellipses, and centered lines at thin and very large widths.
4. Toggle stroke front/behind fill and opacity.
5. Test non-square triangle, diamond and polygon boxes.
6. Confirm stars, gradients, paths, rounded polygons and outlined polygons remain visually identical through fallback.
7. Add effects, masks, parenting, transitions and temporal motion blur to eligible shapes.
8. Switch scene collections and projects while a shape title is visible.
9. Close the editor and OBS during an active resize.
10. Confirm no full-black frame, stale retained frame, resource leak or shutdown deadlock.
