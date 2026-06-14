# Clock Layer Properties Fix

This patch makes Clock layers participate in the same rich-text/default-format synchronization path as Text and Ticker layers.

Fixed issues:
- Clock layers now persist and render changes from the Character section, including Font, Font Style, size, faux style/type controls, tracking, scale and baseline.
- Clock layers now persist and render Paragraph settings, including horizontal/vertical alignment, indents, line spacing, space before/after and hyphenation.
- The properties panel now reads Clock formatting from the rich text summary just like Text/Ticker layers, so the controls reflect the active Clock layer state correctly.
- Paragraph space before/after mirror properties are synchronized together with the other paragraph mirror values.
