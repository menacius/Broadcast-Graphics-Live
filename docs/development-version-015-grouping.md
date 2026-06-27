# Development Version 015 — Group container port

## Correctness changes

- Added a persisted `LayerType::Group` container instead of representing a group by parenting every selected layer to the last selected artwork layer.
- A group has transform, visibility, opacity and timing state, but no artwork raster of its own. Child text, image and vector rasters remain independent and are composited with the accumulated parent transform.
- Group creation preserves the current canvas result and selects the new group as the only active selection.
- Nested grouping keeps the existing group and its descendants intact.
- Ungrouping reparents only direct children and preserves their current world transforms.
- Copy, duplicate, paste and delete expand a selected group to its descendants so the hierarchy cannot be left half-copied or orphaned.

## Editor interaction

- Group rows are hierarchical and indented in the Layers panel.
- The group caret collapses or expands descendants without affecting rendering.
- The context menu includes Group Layers, Ungroup, Add to Group and Remove from Group.
- Canvas clicks and marquee selection resolve grouped artwork to the outer group container, while selecting a child explicitly in the Layers panel still permits editing or moving that child.
- A group bounding box is calculated live from all visible active descendants, including descendants of nested groups, so child movement immediately changes the group bounds.
- Group resize manipulates the group transform rather than writing synthetic artwork dimensions.

## Grouped text regression

The compositor explicitly skips raster allocation for Group layers. This prevents an empty group texture or clip rectangle from replacing the child text result. Text layers inside groups use the same GPU text or compatibility raster path as ungrouped text and inherit only the parent transform/visibility/opacity chain.

## Validation

- Added `group_container_contract_test` covering model persistence, editor commands, hierarchy UI, dynamic canvas bounds and the non-rendering group compositor contract.
- Standalone contract test passed.
- Existing development audit contract passed.
- CMake parsing/compiler detection completed and stopped only at the unavailable `libobs` SDK dependency.
- Quoted include resolution, locale uniqueness, delimiter balance and whitespace checks passed.

## Port base

This implementation was three-way merged from the Development Version 004→005 grouping change set onto Development Version 014, preserving the later source-activation and project changes.
