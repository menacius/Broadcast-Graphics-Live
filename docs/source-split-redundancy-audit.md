# Source Split and Redundancy Audit

## Completed

- Split cache support classes out of `src/cache/cache-manager.cpp`:
  - `cache-frame-key.cpp`
  - `cache-state-tracker.cpp`
  - `ram-frame-cache.cpp`
  - `render-queue-manager.cpp`
- Split inline canvas text editing out of `src/canvas/canvas-preview.cpp` into `canvas-preview-inline-text.cpp`.
- Removed duplicate includes in `title-editor-internal.h` and `title-editor.h`.

## Remaining Large Sources

- `src/obs/title-source.cpp` remains the largest file. Best next split: separate CPU layer rendering helpers, stackable pixel effects, GPU scene-mask rendering, and OBS source callbacks behind an internal source-renderer boundary.
- `src/editor/properties-panel.cpp` is dominated by UI construction. Best next split: move repeated control factories and grouped section builders into dedicated `properties-panel-*.cpp` files.
- `src/editor/title-dock.cpp` mixes dock UI, live cue workflow, persistence settings, and cache orchestration. Best next split: isolate live cue row/view logic first.
- `src/editor/title-editor.cpp` still owns broad editor composition and command wiring. Best next split: move import/export/actions wiring into focused files.
- `src/editor/title-editor-internal.h` is still oversized and acts as a catch-all include. Best next cleanup: replace it with narrower internal headers per extracted editor component.

## Redundancy Findings

- The cache manager held several independent classes in one translation unit even though their class boundaries already existed in `cache-manager.h`.
- Canvas inline text editing was a self-contained functional cluster inside the main canvas file and could be split without changing the class API.
- `title-editor-internal.h` included `title-editor.h` twice.
- `title-editor.h` included `QSpinBox` twice.
- Several remaining files rely on umbrella internal includes, which increases rebuild scope and hides true dependencies.

## Verification

- `cmake --build build --config RelWithDebInfo --target broadcast-graphics-live` passes after the split.
- `ctest --test-dir build -C RelWithDebInfo --output-on-failure` finds no tests because this build was configured with `OBS_BGS_BUILD_TESTS=OFF`.
