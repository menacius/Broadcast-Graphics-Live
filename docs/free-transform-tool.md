# Free Transform Tool

The editor now includes an Illustrator-style Free Transform tool (`E`) directly after Direct Selection in the tools sidebar.

The tool provides three modes from its hold/drop-down menu:

- **Free Transform** — move, rotate, and resize with the existing eight-handle transform box. Shift constrains rotation and proportional resize, Alt uses the established center/object-scaling behavior, and Ctrl temporarily bypasses snapping.
- **Perspective Distort** — dragging a corner adjusts its paired corner to preserve a perspective-style side relationship.
- **Free Distort** — each corner can be moved independently.

Distortion is stored as normalized per-corner quad offsets in every layer, serialized in title files, included in render/cache hashes, displayed by the editor selection overlay, and rendered by the shared OBS/editor GPU compositor through a subdivided texture mesh. Existing projects remain unchanged because the default offsets are zero.


## Development Version 029: Projective text-quality correction

The compositor now maps the source rectangle to the destination quad with a homography. This preserves straight text baselines and avoids the curved or segmented appearance caused by bilinear offset interpolation. The GPU mesh is also adaptively subdivided based on source texture resolution, reducing affine interpolation artifacts during strong perspective compression.
