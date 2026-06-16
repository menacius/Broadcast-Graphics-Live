# Gradient Tabbed Editor Fix

- Reworked the gradient editor content so it remains inside the existing tabbed color/fill selector instead of opening as a separate Gradient Editor dialog.
- Removed the extra geometry/parameter section from the gradient UI; gradient placement/scale/angle remain editable through the on-canvas Gradient Tool handles.
- Rebuilt the gradient tab with a Photoshop-style layout: presets area, name/type/smoothness controls, opacity stops above the ramp, color stops below the ramp, midpoint indicators, and a compact Stops panel.
- Preserved the existing gradient data model, intermediate stops, serialization, rendering, and on-canvas gradient tool behavior.
