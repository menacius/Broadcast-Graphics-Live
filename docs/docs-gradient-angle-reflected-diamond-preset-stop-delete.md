# Gradient angle/reflected/diamond and stop behavior fixes

- Added Angle, Reflected, and Diamond gradient type options to the gradient editor and property/effects gradient type controls.
- Preset selection now replaces the entire current gradient state: start/end colors, start/end positions, opacity values, and intermediate stops are reset so no stale stop data remains from the previous gradient.
- Gradient ramp preview remains a fixed left-to-right linear preview, matching Photoshop-style gradient editing behavior, regardless of the selected object gradient type.
- Double-clicking an existing stop deletes it until only two stops remain. Double-clicking empty ramp space still adds a new stop.
- Rendering paths now preserve the new gradient type values through serialization and UI refresh.
