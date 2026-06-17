# Auto Text Styling rich text sync fix

This revision fixes the Auto Text Styling update path by keeping automatic styles as a render-time overlay instead of writing them back into the manual rich-text ranges.

## Main fixes

- The inline QTextEdit is now populated from the canonical/manual rich-text document only.
- Auto style rules and default auto styles are resolved during canvas/source rendering, not persisted as manual formatting.
- The canvas preview bypasses the frame cache while a text layer is being edited, so the transparent inline editor always sits above a freshly rendered text preview.
- The frame cache content hash now includes rich-text content, manual ranges, auto-style settings, cached preset formats, and core typography fields. This prevents stale cached frames when styling changes but plain text does not.

## Result

Changing Auto Text Styling settings now updates the visible text box/canvas preview without requiring text edits, cache toggles, or closing/reopening the editor.
