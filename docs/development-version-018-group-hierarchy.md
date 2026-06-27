# Development Version 018 — Group hierarchy interaction fixes

- Collapsed groups hide all descendants in both Layers and Timeline.
- Timeline and Layers use one shared visible hierarchy model.
- Layer ordering is sibling-scoped, so child layers remain inside their group and group subtrees move atomically.
- Double-click drills into grouped artwork without starting inline text editing; collapsed ancestors expand automatically.
- Selecting a child shows its immediate group outline as context without handles.
- Group timeline strips display the number of contained non-group objects.
- Add to Group accepts mixed group/non-group selections when at least one selected layer can move into the target.
- Move/resize snapping ignores the selected child layer's group ancestors while preserving canvas, guide, grid, sibling, and other-object snapping.
