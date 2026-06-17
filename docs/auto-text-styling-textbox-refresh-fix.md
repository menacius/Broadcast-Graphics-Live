# Auto Text Styling Text Box Refresh Fix

Fixes a runtime issue where changing Auto Styling settings could fail to update the active text box/inline editor.

Changes:

- Auto styling changes now explicitly invalidate and normalize the rich text model.
- Default style and rule preset formats are re-cached whenever auto styling settings change.
- Stale rich text HTML is cleared after every auto styling change.
- Inline text editing now preserves Auto Styling metadata when committing QTextEdit contents back into the layer model.
- This prevents the active text box from silently dropping `auto_style_enabled`, default preset, or rule definitions while the user is editing text.

This is intended to make Default Style and From/To rule changes repaint immediately in the canvas text box and survive inline text edits.
