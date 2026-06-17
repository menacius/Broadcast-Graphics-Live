# Auto Text Styling

Auto Text Styling is stored per rich text layer and is evaluated before final text layout/rendering.

Each rule now has two independent boundaries:

- `start_condition` + `start_offset` + optional `start_custom_chars`
- `end_condition` + `end_offset` + optional `end_custom_chars`

Supported boundary markers:

- `text_start`
- `text_end`
- `character_index`
- `space`
- `line_break`
- `newline`
- `paragraph_start`
- `paragraph_end`
- `custom_char`

Rules are applied in list order. Later rules override earlier rules where ranges overlap. Manual inline formatting is applied last and therefore overrides automatic styling.

Legacy rules using `condition_type = start_to_char` still load and render as before.
