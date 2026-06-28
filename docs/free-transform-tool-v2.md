# Free Transform Tool — Development Version 026

- Distorted selections use the transformed quadrilateral for handle drawing and hit testing.
- Free Transform mode retains scaling and rotation after a layer has been distorted.
- Perspective/Free Distort support snapping; Ctrl temporarily bypasses snapping, Shift constrains the drag axis, and Alt applies an opposite-corner symmetric edit.
- Drag completion uses the existing layer geometry transaction, preserving undo/redo behavior.
- Transform mode, keyframe toggle, and reset are shown in the dynamic editor toolbar.
- TL/TR/BR/BL offsets are animatable `AnimatedVec2Property` channels with backward-compatible legacy values.
- Canvas and OBS GPU rendering evaluate the same transform channels at local layer time.
