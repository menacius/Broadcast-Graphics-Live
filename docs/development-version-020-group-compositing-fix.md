# Development Version 020 — Group Compositing Fix

- Groups are now rendered to a full-canvas intermediate texture containing their child layers.
- The group effect stack is applied to the composited child result.
- Groups can be used as Alpha, Inverted Alpha, Luma, or Inverted Luma track mattes.
- Group masks, mask-respecting effect order, opacity, and blend modes now follow the normal layer pipeline.
- Child layers owned by a group are no longer composited a second time in the root layer pass.
- Nested groups are composited recursively.
