# Prerender Presentation and Code Audit

**Date:** 2026-06-23
**Scope:** editor preview, OBS source presentation, RAM/disk frame cache, live-text cue cache structure, duplicate/conflicting contracts.

## Evidence from the supplied diagnostics

The latest diagnostic logs showed that cache generation and lookup were generally succeeding, but the two consumers did not present the cache in the same way:

- The OBS source logged completed cache payload submission (`consumer=source action=submit-final`) and GPU draws with non-zero submitted/uploaded/published serials.
- The editor logged many cache hits, including sparse `1x1` transparent frames and cropped lower-third frames, while its GPU draw path repeatedly remained at `submittedSerial=0 ... useSubmitted=0`.
- In `obs-graphics-studio-pro(13)(2).log`, there were 227 editor cache-hit results and 1,292 GPU draws with no submitted cache texture. In `obs-graphics-studio-pro(14)(2).log`, there were 150 editor cache-hit results and 448 such draws.

This means the editor was using `requestFrame()` as an availability gate but then rendering the live GPU graph again instead of presenting the returned prerender payload. A valid cache hit therefore did not guarantee visual parity with the source.

## Confirmed root causes and fixes

### 1. Editor and source used different presentation contracts

`CanvasPreview::render_to_frame()` now submits the actual cache image to the same GPU render-session APIs used by the source:

- `title_gpu_render_session_submit_final_frame()` for fully cacheable titles.
- `title_gpu_render_session_submit_cached_prefix()` for partially cacheable titles.

Cached payloads are submitted in full-canvas coordinate space with adaptive preview scaling disabled for that presentation. Direct canvas interaction still uses the live adaptive GPU path.

### 2. Static OBS sources could miss the completed prerender forever

A static source could render a live fallback while its frame was still being generated, become clean, and never perform another cache lookup after `frameReady` was emitted. Each source now has a bounded, thread-safe frame-ready wake state. Completion of the exact current frame marks the source dirty so it adopts the prerender without waiting for an unrelated edit or cue event.

The connection is disconnected during source destruction and captures only a weak shared state, avoiding use-after-free across cache/source threads.

### 3. Invalid sparse crops could be stretched as full frames

Sparse placement metadata now has one shared contract in `src/cache/cache-frame-payload.h`. A payload is accepted only when:

- all four placement values are valid;
- the metadata canvas matches the requested title canvas;
- the crop lies fully inside that canvas; or
- a metadata-free image is already exactly full-canvas size.

A damaged, stale, or metadata-free crop is rejected instead of being stretched. Rejected payloads are removed from RAM and disk, marked stale, and queued for realtime regeneration. In cached-only mode, editor/source retain the previous valid texture while repair is pending.

### 4. Lookup and invalidation disagreed at frame boundaries

Cache lookup used floor-based half-open intervals while invalidation used `round()`. Near half-frame boundaries, the invalidated key could differ from the key later requested by playback. All lookup and invalidation paths now use `cache_frame_index_for_time()` from `src/cache/cache-time.h`.

Frame `n` owns `[n/fps, (n+1)/fps)`.

Editor/source completion wakeups additionally use `CacheManager::frameIndexForTitleTime()`. This preserves the cache rule that a title without timeline changes always owns frame `0`, even when its runtime playhead is paused at a non-zero time.

### 5. Cache identity contained non-pixel editor state

The visual cache hash no longer includes:

- animated-property display names;
- caret/typing format that affects only future input;
- auto-style rule display labels.

Rule IDs remain included because exclusions can alter rendered output. The renderer cache ABI was advanced to `gpu-renderer-v12-strict-cache-placement`, so frames generated under the previous payload/hash contract are not reused.

## Duplicate/conflict audit

### Consolidated in this change

- Sparse-frame metadata keys and validation: one definition.
- Time-to-frame ownership: one helper.
- Deep immutable title snapshots: one implementation in `src/core/title-snapshot.h`.
- Exposed live-text layer ordering and row remapping: one implementation in `src/core/live-text-cue-utils.h`, shared by the dock, hotkeys, source, and cache-related runtime behavior.
- Removed an adjacent duplicate `output_visible` assignment in source update state.
- Verified that every plugin compilation unit is listed once in the CMake source groups.
- Verified no exact duplicate source/header files, duplicate includes, adjacent duplicate assignments, duplicate localization keys, or merge-conflict markers.

### Remaining duplicate families found

`tools/audit_code_clones.py --minimum-lines 20` reports 13 exact normalized clone groups. The audit still identifies existing clone/refactor debt outside the cache contract:

1. **High risk:** parallel text/vector helper implementations in `title-editor-internal.h` and `title-source.cpp`. Exact normalized clone groups range from roughly 20 to 234 meaningful lines. These should be moved into a renderer-neutral module, but only together with pixel-regression fixtures for text layout, gradients, corners, ticker paths, backgrounds, and effects. Refactoring them without those fixtures would risk changing current rendering semantics.
2. **Low risk:** `LongPressToolButton` is duplicated in the dock and editor.
3. **Low risk:** the built-in open-color palette builder is duplicated in the properties panel and editor.
4. **Low/medium risk:** smaller shared parsing/utility clones remain between effect/transition preset catalogs and rich-text/editor helpers.

These remaining clones are reported as warnings rather than silently modified in this cache fix. The cache-critical and live-cue-critical duplicates were consolidated.

## Verification performed

Passed:

- `cache_time_contract_test`
- `cache_frame_payload_test`
- `live_text_cue_utils_test`
- `title_snapshot_test`
- existing `animation_model_test`
- existing `transition_model_test`
- `tools/audit_cache_contract.py`: 16 cache/structure checks passed, with three documented non-blocking clone warnings
- `tools/audit_code_clones.py --minimum-lines 20`: 13 remaining exact normalized clone groups reported for follow-up
- `git diff --check`

Known pre-existing baseline failure:

- `rich_text_model_test` fails at `tests/rich_text_model_test.cpp:40` because `rich_text_document_replace_text()` currently clears editor-runtime transactions instead of recording the single replacement transaction expected by the test. The identical assertion failure occurs on the untouched baseline commit, so it is not a regression from this prerender/audit change. It remains an existing rich-text contract/test mismatch and was intentionally not mixed into the cache fix.

A full plugin configure/build could not be completed in this Linux container because the OBS development package is unavailable: CMake stops at `find_package(libobs)` because neither `libobsConfig.cmake` nor `libobs-config.cmake` is installed. Therefore the final Windows/OBS build still needs to be run in the normal project build environment.

## Runtime acceptance cases

1. Open a cached static title in the editor before and after its first frame finishes rendering; the editor must switch to the completed prerender automatically.
2. Display the same title in the editor and an OBS source at the same frame; both must show the same crop, position, transparency, and pixels.
3. Test a fully transparent frame (`1x1` sparse payload), a lower-third crop, and a full-canvas frame.
4. Enable “Play after rendering”; both consumers must retain the previous valid texture on a miss rather than render a different fallback frame.
5. Corrupt or retain an old cache entry; the payload must be rejected, evicted, and regenerated rather than stretched.
6. Reorder exposed live-text columns and trigger cues from both dock and hotkeys; values must follow stable layer IDs.
7. Test cache completion while an OBS source is static/paused; the source must log `action=frame-ready-wakeup` and submit the completed frame.
