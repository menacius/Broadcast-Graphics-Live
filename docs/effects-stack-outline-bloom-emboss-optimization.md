# Effects stack, Outline, Bloom and Emboss

- Outline is now a true stackable post-render effect, independent from the layer Appearance stroke. Multiple Outline instances can be layered and reordered.
- Effect Up/Down actions now move the backing `LayerEffect` object and preserve the selected item/settings.
- Added Bloom with threshold, radius, intensity, tint, opacity, blur algorithm and blend mode controls.
- Added Emboss with depth, relief height, light angle, softness, opacity and blend mode controls.
- Added serialization, localization, animated-property integration, effect bounds expansion and CPU rendering for the new effects.
- Reused per-pass alpha extraction and invalidation, and avoided unnecessary surface work for disabled/zero-strength effects.
