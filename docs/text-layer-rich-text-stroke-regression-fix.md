# Text Layer Rich-Text Stroke Regression Fix

## Problem
Normal Text and Clock layers use the canonical rich-text/QTextDocument rendering path. The text outline callback returned immediately for that path, so layer stroke settings were visible in the UI and serialized correctly but no stroke was drawn. The offscreen text surface and clipping rectangle also did not reserve space for the outline, which could crop strokes near text-box edges.

## Fix
- Render rich-text outlines through `QTextCharFormat::setTextOutline()` using the same shaped QTextDocument glyphs as the fill.
- Render outline-only and fill passes separately so the existing front/behind ordering remains supported.
- Preserve solid and gradient stroke brushes, width, join style, opacity and antialiasing.
- Add outline-aware offscreen padding and expand the text clip rectangle to prevent edge clipping.
- Leave ticker/path-based text rendering unchanged while sharing the same evaluated stroke properties.
