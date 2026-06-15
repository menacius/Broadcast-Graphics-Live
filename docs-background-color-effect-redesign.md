# Background Color Effect Redesign

Redesigned the Background Color effect so it uses shape-style controls instead of the older simple background rectangle UI.

Implemented:
- Fill type, fill color, gradient type/start/end/angle and opacity controls in the effect settings.
- Independent stroke color, stroke width and stroke opacity controls.
- Independent Left/Right/Top/Bottom padding controls, allowing negative values.
- Independent TL/TR/BL/BR corner radius controls.
- Corner type buttons matching the rectangle shape corner modes: Round, Straight/Bevel, Concave/Inverse and Cutout/Inset.
- Serialization/deserialization for the new background padding, corner and stroke fields.
- Rendering support for the new independent padding, per-corner radius, corner type and stroke settings in both text and image background rendering paths.
- Keyframe-capable animated properties for the new padding, corner, stroke width and stroke opacity values.
