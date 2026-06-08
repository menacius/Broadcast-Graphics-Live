# Stability and performance audit notes

This audit focused on the real-time OBS source path and the editor preview/export rendering path. The highest-risk issues found were in Cairo/Qt surface ownership, redundant effect-layer code paths, and renderer access to mutable title data.

## Fixed in this audit

- **Cairo resource lifetime is now centralized.** The renderer previously created and destroyed Cairo surfaces/contexts manually in several hot paths. The updated source renderer uses small RAII wrappers for Cairo surfaces and contexts, so early returns from failed surface creation no longer leak resources or leave invalid handles in use.
- **Surface/context creation is checked before painting.** Text, image, cached-effect, masked-layer, full-frame, and preview-export render paths now validate Cairo surfaces and contexts before drawing into them. Failed off-screen allocations return safely instead of dereferencing invalid Cairo objects.
- **Stackable effect filtering is no longer duplicated.** The old duplicated `switch` blocks for removing drop shadow, glow, inner shadow, and color-adjustment effects were replaced by a single helper that uses the existing stack-surface predicate. This keeps cached and uncached effect-rendering paths consistent.
- **OBS source callbacks are defensive against null callback data.** Destroy, update, size, tick, and render callbacks now return safely when OBS invokes them with missing private data or settings.
- **Rendering uses a deep snapshot of layers.** Before the OBS source renders a dirty frame, it deep-copies the title layers so long render passes do not iterate the same layer vector that the editor/live-data path may be changing.

## Remaining architectural risks

- **The title model is still mutable through shared pointers.** `TitleDataStore` protects its title vector, but callers receive `std::shared_ptr<Title>` and can mutate nested fields/layers without holding a model lock. The renderer snapshot narrows the race window, but a complete fix should introduce store-owned read/write transactions or immutable copy-on-write title snapshots.
- **Live editing persists too often.** Many editor property changes immediately call `notify_change()` and `save()`. This is behavior-preserving, but it can create unnecessary disk I/O and OBS-source invalidations during drags. A future refactor should debounce disk persistence while keeping runtime preview revisions immediate.
- **Whole-canvas intermediate surfaces are still used for masks and pixel effects.** This preserves behavior, but large canvases with masks/glows can allocate full-frame temporary buffers. Future work should crop intermediate surfaces to expanded layer bounds and upload only dirty regions where OBS APIs permit.
- **Image and shadow caches use clear-all eviction.** The current caches prevent unbounded growth, but clearing the entire cache can cause spikes. A small LRU cache keyed by file metadata/effect parameters would smooth frame time under asset-heavy projects.
