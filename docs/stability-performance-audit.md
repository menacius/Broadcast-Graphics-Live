# Stability and performance audit notes

This audit focused on the real-time OBS source path and the editor preview/export rendering path. The highest-risk issues found were in Cairo/Qt surface ownership, redundant effect-layer code paths, and renderer access to mutable title data.

## Fixed in this audit

- **Cairo resource lifetime is now centralized.** The renderer previously created and destroyed Cairo surfaces/contexts manually in several hot paths. The updated source renderer uses small RAII wrappers for Cairo surfaces and contexts, so early returns from failed surface creation no longer leak resources or leave invalid handles in use.
- **Surface/context creation is checked before painting.** Text, image, cached-effect, masked-layer, full-frame, and preview-export render paths now validate Cairo surfaces and contexts before drawing into them. Failed off-screen allocations return safely instead of dereferencing invalid Cairo objects.
- **Stackable effect filtering is no longer duplicated.** The old duplicated `switch` blocks for removing drop shadow, glow, inner shadow, and color-adjustment effects were replaced by a single helper that uses the existing stack-surface predicate. This keeps cached and uncached effect-rendering paths consistent.
- **OBS source callbacks are defensive against null callback data.** Destroy, update, size, tick, and render callbacks now return safely when OBS invokes them with missing private data or settings.
- **Rendering uses store-owned deep snapshots.** Before the OBS source renders a dirty frame, it now asks `TitleDataStore` for a deep title/layer snapshot instead of copying through a shared mutable pointer in the render callback. OBS size callbacks also read dimensions through a guarded store helper.
- **Save/export paths serialize deep copies and skip null layers.** Persistence now snapshots titles under the store lock before JSON serialization, template export serializes the same kind of detached copy, and serialization ignores null layer entries instead of dereferencing them.
- **Cue cleanup is centralized.** Repeated live-cue reset blocks now call one helper that only emits runtime revisions when cue state actually changed, avoiding duplicated state transitions and redundant dirty notifications.
- **Preview image rendering clamps canvas dimensions.** `render_title_to_image` now uses the same maximum source dimension guard as the OBS texture path, avoiding accidental huge image allocations from malformed or stale title dimensions.

## Remaining architectural risks

- **The title model is still mutable through shared pointers.** `TitleDataStore` now offers safer snapshot/dimension helpers for rendering and persistence, but many editor and live-cue callers still receive `std::shared_ptr<Title>` and mutate nested fields/layers without store-owned transactions. A complete fix should introduce read/write transactions or immutable copy-on-write title snapshots, then migrate editor commands to those APIs.
- **Live editing persists too often.** Many editor property changes immediately call `notify_change()` and `save()`. This is behavior-preserving, but it can create unnecessary disk I/O and OBS-source invalidations during drags. A future refactor should debounce disk persistence while keeping runtime preview revisions immediate.
- **Whole-canvas intermediate surfaces are still used for masks and pixel effects.** This preserves behavior, but large canvases with masks/glows can allocate full-frame temporary buffers. Future work should crop intermediate surfaces to expanded layer bounds and upload only dirty regions where OBS APIs permit.
- **Image and shadow caches use clear-all eviction.** The current caches prevent unbounded growth, but clearing the entire cache can cause spikes. A small LRU cache keyed by file metadata/effect parameters would smooth frame time under asset-heavy projects.
