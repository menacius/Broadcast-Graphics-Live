# Rich Text Clean Architecture Rebuild

This build replaces the previous patch-based rich text synchronization with a single document-oriented flow:

- `RichTextDocument` is the persistent source of truth for text content, character style ranges, paragraph defaults, selection, and collapsed-cursor typing format.
- Layer scalar text fields are legacy mirrors only. They are refreshed from the active rich text document for compatibility with existing rendering and serialization paths, but the Properties panel no longer treats them as the primary source while rich text editing is active.
- Collapsed cursor state now has its own persistent `typing_format`, so changing Fill/Size/etc. with no selection updates the current insertion style instead of overwriting the layer-wide default or falling back to stale cached colors.
- Selection summaries now follow Illustrator/Photoshop-style behavior: a collapsed cursor shows the style under the caret/current typing style; a selected range reports mixed values only for properties that truly differ inside the selection; non-edit mode applies property changes to the whole text content.
- Inline editor changes update the rich text model even when only selection/cursor format changes occur and the plain text itself is unchanged.
- Text box autosize is driven from the live QTextDocument layout during edit mode, with sane minimum dimensions to prevent the box from collapsing below usable size.

This intentionally avoids the previous incremental rich text fixes and rebuilds the edit/properties/sync loop around one authoritative rich text model.
