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

Position keyframes separate **temporal interpolation** (how progress advances over time) from **spatial interpolation** (the geometric motion path). Spatial modes are Linear, Auto Bezier, Continuous Bezier, and Manual Bezier. Incoming and outgoing handles are stored in layer-local coordinates, so parent, group, and nested-composition transforms are applied after path evaluation rather than baked into or destructively altering the curve.

Enable **Graph Editor** from the timeline footer to edit temporal interpolation directly. **Value Graph** displays the final animated property value; **Speed Graph** displays the derivative used by the final animation. Each keyframe supports independent incoming/outgoing influence and speed with Linear, Hold, Auto Bezier, Continuous Bezier, and Manual Bezier modes. Drag square velocity handles to shape the curve, Alt-drag to break only that temporal pair, use marquee or Shift selection for relative multi-edit, and open **Keyframe Velocity…** for exact numbers. Easy Ease, Easy Ease In, and Easy Ease Out are available from both the graph and ordinary keyframe context menus. Fit Graphs and Fit Selection frame both the value range and keyframe time range; Ctrl-wheel zooms time, Alt-wheel zooms values, Shift-wheel pans horizontally, and middle-drag pans freely.

When a layer with animated Position is selected with the Selection tool, the canvas shows its final transformed motion path, keyframe vertices, the selected vertex's incoming/outgoing handles, the current evaluated position, and a direction arrow. Drag a diamond vertex to edit its Position keyframe, drag a round handle to edit the tangent, hold **Shift** for 45-degree angle steps, or **Alt-drag** a handle to break only that tangent pair. Double-click a path segment to insert a keyframe on the existing curve. Right-click a vertex or path for Linear, Auto Bezier, Continuous Bezier, Manual Bezier, Rove Across Time, Break Tangents, and Join Tangents. Motion vertices snap to guides and other keyframe positions; holding **Ctrl** temporarily disables snapping. Locked layers and inline text-edit mode keep the editable handles hidden.

## Assets and libraries

A saved title can become a reusable Asset Layer. Assets may be synchronized to the parent playhead or independent when their animation mode supports it. Animated assets use a stable bounds envelope covering the full authored motion rather than resizing the selection box every frame.

The Libraries dock contains assets, style presets, gradients, effects, and transitions. Editing an asset opens its source composition after offering to save or discard unsaved work in the current title.

## Undo, persistence, and selection

Editor operations should enter the shared undo history rather than modifying the model silently. Selection-only changes must not trigger expensive rerenders. Saved titles persist authored content; editor-only panel order, panel collapse state, and presentation preferences persist separately as UI settings.
