# Distribute selected layers

The editor toolbar provides a distribution-mode dropdown followed by **Distribute Horizontally** and **Distribute Vertically**.

- **Distribute to Bounds** (default) keeps the outer visual bounds fixed and creates equal gaps between layer bounds.
- **Distribute to Anchors** keeps the first and last layer anchors fixed and spaces all intermediate anchors evenly.
- Distribution requires at least three selected, unlocked layers.
- Multi-selection uses the editor's shared operation-selection path; it no longer depends on the single-selection `sel_layer_id_`.
- Position changes are written to the animated position property at the current playhead.
