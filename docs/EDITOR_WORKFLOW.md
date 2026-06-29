# Editor workflow

## Window and panel model

The editor uses dockable Qt panels with a shared compact visual contract. Collapsible panels have a drag handle, title, optional header controls, an overflow action button where applicable, and a right-aligned caret. Sibling panels can be reordered by drag-and-drop, and their collapse/order state persists through `QSettings`.

Properties and Effects use the same margins, body insets, header sizing, dividers, switch geometry, and theme-derived colors. Explicit user colors and effect colors are not overridden by the OBS palette.

## Canvas and tools

The canvas supports selection, direct selection, shape drawing, pen/path editing, text creation, image placement, eyedropper sampling, gradient editing, ruler guides, snapping, safe areas, and zoom controls. Transform behavior is shared across single layers and groups, including center resize, constrained resize, rotated bounds, animated origins, and free-transform corners.

External drag-and-drop and clipboard input can create text and image layers. Canvas context menus expose the same layer actions available in the layer list, including grouping, parenting, matte assignment, asset editing, duplication, and deletion.

## Layer hierarchy

Grouping and parenting are separate systems.

- **Group:** composites child layers into one result. Group transforms and effects apply after children are composited. Collapsed groups are manipulated as a single object; expanded groups allow direct child selection.
- **Parenting:** applies inherited transform motion without changing layer ownership or list hierarchy.
- **Snapping:** moving a parent or group does not snap against its own descendants.
- **Ordering:** children remain inside their group ordering scope. Ungrouping and reparenting preserve visual placement.

## Mattes, masks, and visibility

A layer or group can act as a matte target or matte source. Alpha, Luma, inverted, and Clipping Matte modes are supported. Clipping uses shape coverage rather than source opacity. Visibility controls distinguish ordinary artwork visibility from active-mask participation.

Effects can respect the matte result through the Effects Settings toolbar. Canvas and OBS source compositing share the same intended matte/effect ordering.

## Timeline and animation

The timeline is frame-based and project-rate aware. It supports:

- property keyframes and easing;
- multi-keyframe selection and movement;
- group and child strips;
- layer ordering by drag-and-drop;
- playhead scrubbing and transport controls;
- keyframe navigation;
- negative values and keyframeable scale/size/origin properties;
- monitor-rate editor presentation with project-rate playback.

## Assets and libraries

A saved title can become a reusable Asset Layer. Assets may be synchronized to the parent playhead or independent when their animation mode supports it. Animated assets use a stable bounds envelope covering the full authored motion rather than resizing the selection box every frame.

The Libraries dock contains assets, style presets, gradients, effects, and transitions. Editing an asset opens its source composition after offering to save or discard unsaved work in the current title.

## Undo, persistence, and selection

Editor operations should enter the shared undo history rather than modifying the model silently. Selection-only changes must not trigger expensive rerenders. Saved titles persist authored content; editor-only panel order, panel collapse state, and presentation preferences persist separately as UI settings.
