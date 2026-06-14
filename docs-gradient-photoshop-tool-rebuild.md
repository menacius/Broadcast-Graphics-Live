# Gradient editor and tool rebuild

- Reworked the gradient popup so the visible workflow is focused on the Photoshop-style gradient editor instead of the previous mixed color/swatches/pattern picker.
- Moved presets above the editable gradient strip, kept gradient type and stop controls, and hid geometry controls that are now intended to be edited on canvas.
- Added a sidebar Gradient Tool. When active, dragging on the selected layer applies a gradient and updates the selected layer's gradient vector directly on the canvas.
- Added canvas support for gradient handles while the Gradient Tool is active, reusing the existing start/end, center, radial radius, and focal handle system.
- Changed gradient center/focal positions to support coordinates outside the 0..1 object bounds, allowing the gradient vector to extend beyond the object.
- Expanded gradient scale persistence/rendering limits so large gradients can extend outside layer bounds while still being safely clamped.
