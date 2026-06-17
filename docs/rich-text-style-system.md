# Rich Text Style System Refactor

This build moves text styling to a single canonical model:

- `Layer::rich_text` is the only source of truth for text content and styling.
- `RichTextDocument::plain_text` owns the text.
- `RichTextDocument::default_format` owns the base character style.
- `RichTextDocument::ranges` owns manual inline character styles.
- `RichTextDocument::auto_style_rules` owns automatic style rules.
- `rich_text_document_with_auto_styles()` materializes the effective view used by the editor and renderer.
- `Layer::rich_text_html` is no longer serialized, loaded, rendered, or used as an editing fallback.

## Priority model

1. Layer/rich text default style.
2. Auto default style preset.
3. Auto style rules, in rule order.
4. Manual inline ranges.
5. Typing format for collapsed cursor insertion.

Manual inline ranges are applied last, matching Illustrator/After Effects style precedence.

## Synchronization rules

Every text-like layer is normalized through `rich_text_document_ensure_canonical()` before rendering, editing, loading, or panel display. This keeps `text_content` as a mirror only and prevents properties panel, canvas, inline editor, and OBS source rendering from diverging.

## Removed legacy behavior

The previous HTML round-trip path was removed from active use. HTML is not used as an intermediate representation because it caused stale spans, auto styles becoming manual ranges, and different UI surfaces showing different text states.
