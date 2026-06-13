# Editor Fixes — 2D Vector Transform Keyframes

Implemented changes:

- Position is now treated as one 2D vector control for keyframing: X and Y are added, removed and deleted together from the Position keyframe button.
- Scale is now treated as one 2D vector control for keyframing: X and Y are added, removed and deleted together from the Scale keyframe button.
- Size is now treated as one 2D vector control for keyframing: Width and Height are added, removed and deleted together from the Size keyframe button.
- Origin/Anchor is now treated as one 2D vector control for keyframing: X and Y are added, removed and deleted together from the Origin keyframe button.
- The timeline keyframe list now exposes one lane for each vector transform group instead of duplicate X/Y or W/H lanes.
- JSON persistence now writes vector aliases for `position`, `scale`, `size` and `origin` while keeping the older scalar fields for backward compatibility.
- Loading remains backward compatible with older project files that only contain scalar fields.

Validation notes:

- Source tree was inspected for all transform/keyframe call sites touching position, scale, size and origin.
- CMake configure was attempted, but this sandbox does not include the OBS/libobs development package, so full native build verification cannot complete here. The failure is the expected missing `libobsConfig.cmake` / `libobs-config.cmake` dependency, not a source-code error from these changes.
