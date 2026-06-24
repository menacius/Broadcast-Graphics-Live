# Phase 12A — Rich Text Core Consolidation

Phase 12A establishes the canonical input contract for the GPU text renderer. It does not yet replace the temporary `QTextDocument`/`QImage` rendering path; that removal belongs to the shaping and glyph-renderer stages that follow.

## Canonical ownership

- `Layer::rich_text` is the source of truth for text-like layers (`Text`, `Ticker`, and `Clock`).
- `RichTextDocument::plain_text` owns the text content.
- `default_format` and `default_paragraph_format` own document defaults.
- `ranges` contains manual character overrides.
- `blocks` contains paragraph overrides.
- `auto_style_rules` contains automatic styling rules.
- `typing_format` contains collapsed-cursor insertion overrides.
- Scalar `Layer` text properties are one-way compatibility mirrors. They are no longer imported back into an existing canonical document.
- The obsolete `Layer::rich_text_html` mirror and all active HTML fallback paths have been removed.

## Version 2 format contract

`RichTextDocument::version` is now `2`.

Character ranges, paragraph blocks, and typing state are sparse overrides:

- `RichTextRange::mask` identifies the character properties explicitly authored by the range.
- `RichTextBlock::mask` identifies the paragraph properties explicitly authored by the block.
- `typing_format_mask` identifies collapsed-cursor insertion overrides.
- A masked property remains explicit even when its value temporarily equals the document default.
- Unmasked properties inherit current document defaults, including evaluated animated defaults in the renderer.

Version 1 files are migrated by inferring masks from fields that differ from their document defaults. Version 2 files persist masks directly.

## Index and Unicode contract

The canonical model uses UTF-8 byte offsets, but every selection, range, block, edit boundary, and transaction boundary is normalized to a complete UTF-8 codepoint boundary.

Qt adapters convert explicitly between:

- canonical UTF-8 byte offsets; and
- Qt UTF-16 cursor positions.

This prevents Greek, emoji, surrogate-pair, and other multibyte text from producing split ranges or invalid transaction strings. Grapheme-cluster and Unicode word segmentation remain responsibilities of the Phase 12B shaping/layout layer.

## Formatting precedence

The effective visual style is resolved in this order:

1. Document defaults.
2. Auto default style preset.
3. Auto-style rules in rule order.
4. Manual sparse character overrides.
5. Sparse typing overrides for future inserted text only.

Manual ranges remain separate from auto-generated spans in the inline Qt adapter. Nonvisual metadata preserves the manual masks when the temporary `QTextDocument` is converted back into the canonical model.

## Paragraph model

Paragraph blocks are now loaded from JSON, normalized as real paragraph spans, preserved across text edits, applied to the editor adapter, and applied to the source renderer. Paragraph properties inherit document defaults unless their block mask explicitly overrides them.

## Dynamic text layers

`Text`, `Ticker`, and `Clock` now pass through the same canonical rich-text and auto-style pipeline. Dynamic display text is replaced through the same Unicode-safe edit contract, using the document's sparse typing mask for inserted content.

## Cache identity

The visual cache hash includes:

- canonical document defaults;
- character and paragraph masks;
- only the values selected by those masks;
- automatic-style masks and masked values.

Caret position, selection, typing state, transaction history, and unmasked snapshot values do not alter visual cache identity.

## Runtime transactions

Edit transactions are bounded runtime state and use Unicode-safe boundaries. They are intentionally not written to title files. Older serialized transaction arrays are ignored on load.

## Temporary compatibility renderer

The current source and inline-editor adapters still use Qt text objects. Phase 12A makes their input deterministic, but does not claim GPU text rendering. The following remain for Phase 12B/12C:

- immutable shaped glyph/cluster/line output;
- HarfBuzz feature selection and script/language shaping;
- grapheme and Unicode word segmentation;
- persistent glyph atlas textures;
- glyph instance buffers and GPU quads;
- shader fill, gradient, stroke, shadow, clipping, and text effects;
- GPU per-character/word/line transitions;
- removal of frame-time `QTextDocument::drawContents`, `QImage`, `QPainter`, and Cairo text rasterization.
