# Editor GUI keyframe, spin-box and performance fixes

- Replaced the passive animated-property rows in the layer list with functional keyframe toggles and editable scalar/vector value controls connected to the shared animation model.
- Removed spin-box arrow buttons throughout the editor and enabled hover-wheel value changes without stealing persistent focus.
- Added default-value snapping to label-drag numeric editing by recording each control's initial/default value and snapping within a small step-relative threshold.
- Reduced unnecessary editor/OBS contention by disabling animated dock transitions and avoiding continuous 30 Hz canvas invalidation; dynamic clock/ticker previews now refresh only while the editor is visible at a lower UI rate.
