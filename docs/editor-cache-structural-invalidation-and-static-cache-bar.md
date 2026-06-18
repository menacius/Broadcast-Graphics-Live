# Editor cache structural invalidation and static cache bar

This update fixes a stale editor preview edge case with caching enabled and improves the timeline cache bar.

## Editor canvas invalidation

Some structural edits can return the title to a previously rendered visual state. For example:

1. open an already cached title,
2. draw a temporary shape,
3. delete that shape.

The title's final content can match the last remembered cache baseline, so the selection-only guard in `TitleEditor::on_title_modified()` could incorrectly skip the editor refresh. That left the canvas showing the previous interactive pixmap until another repaint/cache event happened.

Structural layer mutations now mark the next title modification as a forced visual update. This bypasses the selection-only guard, enables the existing interactive cache bypass, and refreshes the canvas from the live model immediately. Normal selection-only property notifications still remain ignored.

Affected structural paths include layer add, external image/text add, canvas-created layer finalize/cancel, and layer deletion.

## Timeline cache bar static frame states

The timeline cache bar now differentiates static/reused frames from dynamic frames:

- Dynamic RAM frames: bright green.
- Static/reused RAM frames: dark green.
- Dynamic disk frames: light blue.
- Static/reused disk frames: blue.

For titles without timeline changes, frame 0's cache state is displayed across the visible timeline as static cache coverage. For animated titles, adjacent adaptive visual-state hashes are compared to mark visually unchanged spans.
