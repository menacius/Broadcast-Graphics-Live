# Effect Stack Mask Toggle

Adds a per-layer Effects dock toggle that controls whether a layer's stackable effects are evaluated before or after its track matte / mask.

## Behavior

- The Effects dock button bar now includes a mask icon toggle.
- When the toggle is enabled, the button is highlighted and the layer is first composited through its mask, then the stackable effect stack is applied to the masked result.
- When the toggle is disabled, the layer keeps the existing behavior: effects are applied to the source layer first, then the result is masked.
- The setting is stored per layer as `effect_stack_respects_masks` and serialized in title JSON.

## Rendering Notes

The CPU/Cairo render path now branches inside masked layer rendering:

1. Render the base layer without stackable pixel effects.
2. Apply alpha or inverted-alpha track matte to the base layer surface.
3. Apply the layer's stackable effect stack to the masked surface.
4. Composite the final result into the title frame.

This allows effects such as glow, blur, color overlay, shadows, and color adjustments to respect the visible masked shape when the new toggle is enabled.
