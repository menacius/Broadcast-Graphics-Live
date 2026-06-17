# Auto Text Styling

Implemented an automatic text styling pipeline for rich text layers.

## User workflow

Text layers now expose an **Auto Styling** section in the properties panel:

- Enable/disable automatic styling per text layer.
- Pick a default text style preset.
- Add conditional rules that apply a selected text style preset from the start of the text to a user-defined character count.
- Update, delete, and reorder rules with explicit priority. Later rules override earlier automatic rules, while manual inline formatting remains the highest-priority layer.

## Data model

The structured `RichTextDocument` now stores:

- `auto_style_enabled`
- `auto_default_style_preset_id`
- `auto_style_rules`

Rules reference stable preset IDs and also store a cached character format/mask snapshot. This keeps the file robust if a preset is later missing while still allowing live preset resolution when available.

## Rendering behavior

Both editor preview and OBS source rendering resolve automatic styles before final text layout. The effective format order is:

1. Rich text default format
2. Default style preset
3. Auto style rules in list order
4. Manual inline rich-text ranges

Manual inline styling therefore overrides auto styling.

## Serialization

Auto styling fields are serialized inside the existing `rich_text` JSON object and are restored with the title/template file.
