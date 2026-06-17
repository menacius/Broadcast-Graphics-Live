# Illustrator-style Style Presets Infrastructure

Implemented the first reusable style preset infrastructure for the editor Styles dock.

## Scope

- Added a persistent style preset library stored under the user's application data directory.
- Presets are JSON based and portable, so users can import/export style libraries.
- Presets support categories, search/filtering, icon-grid access, double-click apply, and generated thumbnails.
- Added built-in starter presets for text and gradient styles so the dock is useful on first launch.
- Completed the Text and Gradient tabs in the existing Styles dock with a shared Illustrator-like preset browser.

## Text style presets

Text presets capture the current selected text/clock/ticker layer typography:

- font family/style/size
- bold, italic, underline, strikethrough
- tracking, horizontal/vertical scale, baseline shift, leading
- text color
- horizontal/vertical alignment
- paragraph spacing and indents
- current gradient fill data when present

Applying a text preset updates the selected text-like layer and refreshes canvas, layer list, properties, timeline/cache invalidation, and live edit state.

## Gradient style presets

Gradient presets capture and apply the layer fill gradient:

- gradient type
- start/end colors, positions, opacities
- total opacity
- angle, center, scale, focal point
- intermediate stops

The generated thumbnail renders the actual gradient stop set for quick visual browsing.

## User workflow

The Styles dock now exposes:

- Search box
- Category filter
- Thumbnail grid
- Save current selection as preset
- Apply preset
- Delete preset
- Import preset JSON
- Export preset JSON

Pattern styles remain as the existing placeholder tab for a future pattern-style implementation.
