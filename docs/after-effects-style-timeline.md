# After Effects-style Timeline Controls

This build extends the timeline/layer stack so layer relationships are edited in a way that is closer to After Effects.

## Layer switches and modes

Each layer row now exposes:

- Visibility and lock switches.
- Animated property expansion.
- A layer blending mode menu (`Normal`, `Multiply`, `Additive`, `Screen`, `Overlay`, `Color`).
- A parent pick-whip indicator and parent layer menu.
- A Track Matte / Mask menu with alpha and inverted-alpha options.

The timeline strip also displays active relationship badges so masks, parent links and non-normal modes are visible without opening properties.

## Masks / Track Mattes

The existing mask source and mask mode are now presented as AE-style Track Matte controls. A layer can use another layer as an alpha or inverted-alpha matte. The underlying render path continues to apply the selected matte in both the editor and OBS source output.

## Parenting

Parent selection is surfaced directly in the timeline row, next to a pick-whip icon. Parenting continues to use the existing transform chain, so child layers inherit the parent layer transform in editor preview and OBS output.

## Modes

Layer blend modes are now stored per layer, serialized with the title, and applied during output rendering by compositing each non-normal layer through an intermediate Cairo surface before blending it into the frame.
