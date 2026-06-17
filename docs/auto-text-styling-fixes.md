# Auto Text Styling fixes

This build fixes the Auto Text Styling feature so rules are applied through the rich-text model in both editor preview and OBS/source rendering paths.

## Changes

- Added cached default-style format/mask to `RichTextDocument`, matching the cached format already used by per-rule auto styles.
- Serialized/deserialized the cached default style so title files remain portable even when a referenced preset is unavailable at render time.
- Updated auto-style rendering to fall back to cached preset data when `StylePresetLibrary` cannot resolve a preset ID.
- Cleared legacy rich-text HTML when auto-style settings change, preventing stale HTML from hiding structured auto styles.
- Forced inline text editor document refresh when auto styling is enabled, because rule changes can alter formatting without changing plain text.
- Improved custom-character marker matching to work with UTF-8 characters instead of byte-only matching.

## Validation

- `src/text/title-rich-text.cpp` was compiled in standalone mode with `g++ -std=c++17 -DOBS_GSP_RICH_TEXT_STANDALONE_TEST`.
- Full OBS/CMake build was not available in this environment because `libobs` is not installed here.
