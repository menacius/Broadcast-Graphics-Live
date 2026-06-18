# Canvas helper color ADL compile fix

This build fix resolves an MSVC ambiguity in `canvas-preview.cpp` after adding configurable Canvas helper overlay colors.

The file had a local anonymous-namespace helper named `canvas_helper_color(...)`, while `TitlePreferences` also exposes `TitlePreferences::canvas_helper_color(...)`. Calls using `TitlePreferences::CanvasHelperColorRole` could trigger argument-dependent lookup and make MSVC consider both functions viable.

The local helper is now named `editor_canvas_helper_color(...)` and forwards explicitly to `TitlePreferences::canvas_helper_color(...)`. This keeps the editor overlay color lookup centralized in preferences while avoiding the ambiguous overload set.
