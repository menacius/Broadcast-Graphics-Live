# Development Version 003 — Project Audit and Cleanup

## Scope

Static audit and targeted cleanup of the complete Broadcast Graphics Live source tree, with emphasis on effect/extension identity, ABI lifecycle, GPU resource ownership, cache growth, import validation, persistence, shutdown ordering and delivery-version consistency.

This pass intentionally avoids deleting compatibility code merely because it is labelled legacy. Compatibility branches are retained when they are still required to open older title files or preserve the last verified artwork path.

## Corrected issues

### Canonical built-in effect metadata

Built-in effect IDs, names, categories and shader paths previously existed in more than one table. The extension catalog now derives built-ins directly from `TitleEffectRegistry::definitions()`, giving the renderer, Add Effect menu, diagnostics and serialization one source of truth.

### Stable-ID shader cache isolation

Third-party effects use a legacy numeric adapter internally. The compiled-effect cache previously looked up built-ins by that numeric type, so an extension could be returned accidentally for the built-in Background Color effect. Compiled shaders are now identified by canonical stable ID.

### Native extension lifecycle

Loaded native libraries are now explicitly unloaded during catalog reload and plugin shutdown. The catalog is initialized once before title data is loaded, allowing extension state migrations to run while avoiding incidental catalog mutation during rendering/editor use.

### ABI v2/v3 completion

The host now:

- reads ABI v3 canvas-handle descriptors from the v3 descriptor array;
- validates the v2 descriptor size before reading appended fields;
- retains and invokes native state-validation and migration callbacks;
- stores an extension schema version with every effect instance;
- migrates valid JSON state to the installed schema version;
- preserves older state when no migration is available;
- releases plugin-returned strings through the ABI callback when provided.

### Extension package hardening

Extension discovery now rejects symbolic-link traversal, excessive recursion, oversized JSON files, invalid IDs, escaping shader/index/asset paths and unreasonable catalog/effect counts. Manifest and asset paths are canonicalized and must remain inside the extension package.

### GPU resource lifetime

The compatibility GPU effect registry and extension image textures no longer rely on process-static destruction. They are explicitly released while the OBS graphics context is active. Failed `gs_image_file` initialization is also cleaned up correctly.

### Bounded image caches

Layer-image, intrinsic-size and extension-texture caches now use bounded incremental LRU eviction instead of unbounded growth or clear-all spikes. Extension textures are invalidated when the source file timestamp or size changes.

### Safer embedded assets

Embedded image restoration now uses `QSaveFile` with atomic commit and no direct-write fallback, avoiding partially written assets and improving Unicode path handling on Windows.

### Import diagnostics

Import checks now use stable extension IDs, distinguish installed third-party effects from built-ins, and recognise embedded shader fallbacks. This removes false missing-effect warnings and reports genuinely unavailable extension shaders.

### Animation initialization cleanup

`AnimatedProperty` and `AnimatedVec2Property` now have explicit constructors, removing repeated partial-aggregate initialization warnings while preserving existing two- and three-argument initialization syntax.

### Delivery identity

The previous delivery identifier was removed. A manually assigned `Development Version` is now the delivery identity and comes from one CMake definition, with a header fallback for standalone checks. Development Version 003 is shown in the editor, dock, About dialog, logs and package name.

## Validation performed

- All extension manifests, indexes and preset JSON files parsed successfully.
- The public plugin ABI header passed C11 and C++17 syntax checks.
- Twelve source-contract tests passed, covering adjustment layers, transitions, procedural effects, Lens Flare, masks, tiled RAM/disk cache, Phase 14 prerender, Phase 15 visibility recovery and this audit contract.
- Core model tests passed for rich text, animation, transitions, cache time, ticker runtime, system memory, live-text cue utilities and deep title snapshots.
- `git diff --check` passed.
- CMake reached OBS dependency discovery; full configuration/build could not continue because the audit environment does not contain the OBS `libobs` development package.

## Remaining architectural risks

These are not presented as fixed without runtime evidence:

- `title-source.cpp` and several editor translation units remain very large and should be split only with staged runtime parity tests.
- Some editor rendering work remains synchronous and can still block the GUI for expensive titles.
- GPU/cache correctness and shutdown behavior still require real OBS stress testing with AddressSanitizer or equivalent instrumentation.
- The per-thread GPU readback-session pool is released at plugin shutdown but can retain sessions for worker threads that terminate earlier; replacing it requires a safe worker-lifecycle contract rather than an unsafe thread-local destructor inside a unloadable module.
- Live native-extension hot reload is intentionally not exposed yet. Reloading executable modules while effects are rendering requires generation-owned descriptors and callback leases.

No claim is made that static review proves the absence of every leak. This pass removes the concrete ownership and growth issues found and documents the remaining cases that require instrumented OBS runtime testing.
