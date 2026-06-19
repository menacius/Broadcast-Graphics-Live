# Editor Adaptive Rendering

The canvas button bar now includes a persistent **Adaptive** toggle. This feature is intentionally scoped to `CanvasPreview`; OBS source output, cache generation, exports, and final title rendering remain full quality.

The editor measures the latest full-quality render cost. Titles that already render within the interactive budget stay at 100%. During continuous editing (move, resize, rotate, gradient/corner manipulation, and keyboard nudging), slower titles are rasterized directly by Cairo at 75%, 50%, 37.5%, or 25% resolution. This is render-time scaling rather than post-render image downsampling, so the destination raster and most effect work are reduced.

Input events are coalesced and the reduced preview is stretched to the title's logical canvas dimensions. After 140 ms without interaction, the editor performs a forced uncached full-quality refinement from the live model, preventing stale cache content from replacing the edited frame.

The setting is stored under `OBSGraphicsStudioPro/Dock/titleEditor/adaptiveRendering` and defaults to enabled.
