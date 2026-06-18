# Clock Layer Empty Rich-Text Render Fix

Clock layers were routed through the normal rich-text document path. Unlike text layers, a clock does not persist its current display value in `rich_text.plain_text`; its value is generated dynamically from `clock_format`. As a result, the renderer canonicalized an empty rich-text document and drew nothing, while ticker layers continued to work because they already used the evaluated display text path.

The renderer now treats both Clock and Ticker as dynamic text layers and fills the `QTextDocument` from `display_text_for_style(layer)`. Regular Text layers continue to use canonical rich-text content and ranges.
