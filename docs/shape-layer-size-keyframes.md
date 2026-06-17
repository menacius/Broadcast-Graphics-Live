# Shape layer size keyframes

Shape and solid-rectangle layers expose their Width/Height geometry through the shared `Layer::size` animated Vector2 property.

The Transform Size keyframe control now reflects the actual `size` animation state, including inactive, keyframed-at-current-time, and animated-at-another-time states. Width and Height remain a single Vector2 timeline property, preserving aspect-ratio locking and the existing canvas/property-panel synchronization.
