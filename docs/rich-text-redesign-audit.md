# Rich text redesign audit

This audit guided the structured text model used by OBS Graphics Studio Pro.

## Mature editor patterns studied

- **Qt QTextDocument / QTextCursor / QTextCharFormat**: a document object owns structured content, cursors describe selections and editing intent, and character/block formats are applied through deterministic ranges rather than by mutating renderer-only state.
- **ProseMirror**: editor state combines document, selection, and transactions; every edit is represented as a transaction that maps subsequent selections and formatting through the change.
- **Lexical**: a versioned editor state contains nodes plus selection and is updated inside explicit update transactions, keeping rendering views synchronized from one authoritative state.
- **Slate**: high-level transforms become low-level operations against a node tree; selections/paths are remapped as operations are applied.
- **Quill Delta**: text and attributes are represented as compact operations that can describe both full documents and edits without depending on ambiguous HTML.
- **Canvas editors such as Fabric IText / Konva rich text patterns**: canvas renderers need the same measured model as the editing overlay; per-character styles and selection must be mapped through grapheme/text positions rather than maintained as an unrelated string.

## Architectural decisions

1. **One source of truth per text layer**: each layer owns `RichTextDocument`, which stores plain text, paragraph blocks, inline ranges, selection, and recent edit transactions.
2. **Ranges, not HTML, are authoritative**: legacy HTML remains only as backward-compatible import/render fallback; new canvas edits, Properties-panel edits, serialization, undo snapshots, and rendering synchronize through the structured model.
3. **Deterministic replace transactions**: property-panel text changes compute common prefix/suffix, remove deleted ranges, shift preserved ranges, and assign inserted text to the active/default format.
4. **Renderer synchronization**: canvas editing and OBS rendering build `QTextDocument` views from `RichTextDocument` when mixed inline formatting requires document layout, while plain/default text still uses the path renderer for outlines and existing effects.
5. **Validation focus**: tests target mixed style range remapping, gradient insertion formats, deletion clipping, and deterministic transactions, providing a base for future GUI/OBS pixel-comparison tests.
