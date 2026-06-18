# Adaptive Prerender Sampling

The prerender worker now performs a second, conservative temporal reuse pass after exact visual-state deduplication.

Animated renderer inputs are quantized at sub-visible thresholds (for example 0.25 px for spatial values, 0.05 degrees for rotation, and approximately one 8-bit step for color channels). Frames that resolve to the same adaptive state reuse the first rendered payload instead of invoking the renderer again.

Safety rules:

- Exact temporal deduplication always runs first.
- Visibility and other discrete states remain exact.
- Forced/stale rerenders never use adaptive aliases.
- Realtime requests are excluded; only background prerender jobs participate.
- Timeline keys, cache states, seeking, and progress remain frame-accurate.
- The adaptive canonical map is generation-bound and cleared with cache invalidation.

This primarily benefits very slow movement, opacity fades, subtle effect animation, and long eased sections where adjacent frames differ by less than a perceptible pixel/color threshold.
