# Gradient Intermediate Stops

This update adds support for real intermediate gradient stops in the gradient editor.

## Behavior

- The legacy start and end stops remain backward-compatible and continue to serialize through the existing fields.
- Additional stops are stored as `gradient_stops`, `stroke_gradient_stops`, and `background_gradient_stops` arrays.
- The gradient preview can display and drag any number of stops.
- Double-clicking the gradient ramp adds a new intermediate stop at that position.
- The Add Stop button creates a new intermediate stop at the center of the ramp.
- Duplicate creates a new intermediate stop based on the currently selected stop.
- Delete removes the currently selected intermediate stop; start/end stops are protected.
- Rendering applies all stops in the editor preview and OBS output paths, including Cairo fallback patterns.

## Compatibility

Existing projects with only start/end gradient fields load unchanged. New projects save intermediate stops in separate arrays, so the old start/end fields remain intact.
