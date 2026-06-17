# Rich Text Style System Refactor

This update makes `RichTextDocument` the canonical text styling source of truth.

Key points:

- The inline editor now displays the effective appearance of rich text, including automatic character styles.
- Auto-generated style ranges are tagged in the `QTextDocument`, so committing text edits no longer converts auto styles into persistent manual inline ranges.
- Manual inline edits remain higher priority than auto styles and are stored as canonical `RichTextRange` entries.
- The properties panel now reads the effective character style under the cursor/selection, matching Illustrator/After Effects behavior.
- UTF-8 byte offsets are converted to Qt text positions when applying ranges to `QTextDocument`, fixing style range placement for Greek and other non-ASCII text.
- Auto style `character_index` conditions now count UTF-8 codepoints instead of raw bytes.
- OBS/source rendering uses the same byte-offset-to-QText-position conversion as the editor.

Legacy HTML remains only as an import fallback. Structured rich text is preferred for editing, properties synchronization, and rendering.
