# Text and live data

## Rich-text model

Text layers use a run-based rich-text model as the single source of truth. Manual formatting, auto styling, the inline editor, property panels, serialization, and rendering read and write the same runs rather than converting between an independent HTML model and legacy overrides.

Character properties include font, size, fill, stroke, horizontal and vertical scale, tracking, baseline, capitalization, and style. Paragraph properties include alignment, justification, indentation, spacing before/after, vertical alignment, wrapping, and line breaks.

## Inline editing

Clicking a text layer can enter on-canvas editing. Cursor movement and character insertion preserve the active run style and caret position. Escape or switching tools leaves text-edit mode. Multi-style selections display mixed/blank property values when no single value is authoritative.

## Auto text styling

Auto-style rules apply formatting to ranges selected by conditions such as text start/end, paragraph start/end, whitespace, newline, custom characters, word counts, and character counts. Rules can combine conditions, exclude another rule, include or omit stop characters, and avoid applying when a required stop condition is absent.

Live text cues apply auto styling when values are committed, so dock-driven text uses the same formatting logic as editor-authored text.

## Clock and ticker layers

Clock and ticker content is generated at runtime but uses the same text layout and property model. Tickers support wrapping, vertical/horizontal modes, paragraph formatting, custom completion, and independent playback where applicable. Clock and non-cacheable ticker modes remain real-time and are excluded from ordinary frame prerendering.

## Exposed properties and cues

Text and image properties can be exposed to the Titles and Graphics dock. Cue rows store user-facing values and are applied as snapshots. The currently active cue keeps its applied snapshot even when the row is edited; updated values become active on the next cue.

Cue states include inactive, queued, active, and ending/outro. Runtime counters can show elapsed or remaining time according to playback mode. Uncue continues from the current frame and reaches the authored end before applying the configured end behavior.

## External data

External data sources can update exposed fields through the dock import/append/refresh workflow. Keep external-data mappings separate from authored title structure so refreshes change values without rebuilding unrelated layers or invalidating unaffected cached frames.
