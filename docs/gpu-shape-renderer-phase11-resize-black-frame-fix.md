# Phase 11 resize black-frame fix

The Phase 11 primitive raster upload previously ran before the render session entered its isolated GPU state. In the editor display callback, the active matrix already contained the artwork-rectangle translation. A primitive redraw triggered by shape scaling therefore inherited that translation while rendering into its small per-layer texrender, which could place the whole analytic shape outside the target and publish an empty texture. The primitive-sized projection and blend state could also leak back into the final canvas draw.

The render session now enters `ScopedGpuCompositorState` before any pending raster upload, and the auxiliary mask path follows the same ordering. This guarantees identity matrix, isolated viewport/projection, and restored blend state for every primitive replacement.

The transactional fallback now accepts the last published frame when it belongs to the same non-empty title ID, even when the model revision has advanced for the current interactive edit. Cross-title/project retention remains blocked because lifecycle invalidation clears the published pointer and identity before a new source generation is used.
