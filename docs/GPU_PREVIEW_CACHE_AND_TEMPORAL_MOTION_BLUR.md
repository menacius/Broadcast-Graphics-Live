# GPU Preview, Cache Readback, and Temporal Motion Blur

## Scope of this step

This change introduces the presentation/readback contract needed for the editor and cache paths to stop treating every rendered surface as a CPU image.

### Editor preview contract

- The canvas no longer converts every rendered frame to `QPixmap`.
- The rendered artwork is uploaded to a persistent OBS `gs_texture_t` and remains on the GPU until the frame changes.
- The canvas is presented through an OBS display/swap chain.
- Checkerboard/background and editor interaction chrome are separate textures. Zooming, panning, selecting, drawing guides, or moving handles updates presentation state without re-uploading the artwork texture.
- A CPU `QImage` is still retained where editor features require pixel access (eyedropper, export/screenshot, and compatibility fallback). It is no longer the display object.

### Cache worker readback contract

The cache render entry points run under a `FinalFrameOnly` GPU readback scope. Layer-local GPU surface passes are not allowed to stage intermediate surfaces back to the CPU while that scope is active. The worker therefore produces one completed cache frame instead of performing GPU upload/readback round-trips for each intermediate effect surface.

The RAM/disk cache still stores a CPU frame because disk compression and the existing cache format require CPU bytes. A future all-GPU cache tier can consume the same contract without changing editor presentation ownership again.

## Temporal motion blur method

The selected method is true shutter-time temporal accumulation for transform motion:

1. Render a transform-neutral local raster once.
2. Upload it once as a premultiplied BGRA texture.
3. Evaluate the layer and parent transforms at midpoint shutter samples.
4. Draw every sample into one GPU exposure target using normalized additive weights.
5. Blend the sharp current sample only for the unblurred portion of the effect amount.
6. Stage the completed exposure once only when a CPU compositor still needs it.

This is a better fit for the current 2D title model than a depth/velocity reconstruction filter because the renderer already owns exact per-layer animation curves and shutter times. A velocity-buffer/feature-aware reconstruction pass remains useful later for deforming content or a full-frame approximation mode.

## Image-layer artifact fixes

- Image layers are no longer excluded from the transform-raster reuse path.
- Accumulation is performed in premultiplied-alpha space with weights normalized across the exposure.
- A zero-motion exposure returns the original sharp layer exactly; static PNG/SVG alpha edges are never accumulated repeatedly.
- Adaptive sample density uses the maximum transformed corner displacement, so rotation and scale are covered in addition to position.
- The same local image raster is reused for rigid transform samples instead of repeatedly rasterizing and adding Cairo copies.

## Resource ownership

- Editor display textures belong to `CanvasPreview` and are released with the widget.
- Temporal motion-blur textures, stage surface, render target, and effect are reused across frames and released from `obs_module_unload()` while the OBS graphics context is valid.
- OBS display callbacks only read textures while holding the preview-state mutex.

## Validation checklist

1. Static PNG and SVG with motion blur enabled remain pixel-identical to the sharp image.
2. Position, scale, and rotation animation produce continuous blur without separated copies.
3. Motion-blur amount below 100% mixes sharp and temporal exposure without increasing total alpha.
4. Zoom/pan/selection changes do not upload a new artwork texture.
5. Cache rendering does not call intermediate GPU stage-surface readback paths.
6. Closing the editor and unloading the plugin release display and temporal GPU resources without a render-thread race.

## References

- NVIDIA GPU Gems 3, Chapter 27, “Motion Blur as a Post-Processing Effect”.
- Guertin, McGuire, and Nowrouzezahrai, “A Fast and Stable Feature-Aware Motion Blur Filter”, High-Performance Graphics 2014.
- OBS Studio Core Graphics API (`gs_texrender`, `gs_texture`, `gs_stagesurface`) and OBS native display implementation.
