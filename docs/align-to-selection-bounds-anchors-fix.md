# Align to Selection: Bounds and Anchors

- Fixed multi-selection alignment by using the editor's shared operation-selection path instead of `sel_layer_id_`.
- Added **Align to Selection (Bounds)** and **Align to Selection (Anchors)** targets.
- Bounds mode uses each layer's transformed, rotated axis-aligned bounds at the current playhead.
- Anchors mode aligns layer position/anchor points against the minimum, center, or maximum selected anchor coordinate.
- Both modes require at least two unlocked selected layers and preserve animated position/keyframe behavior.
