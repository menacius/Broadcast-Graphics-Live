# GPU Shape Black Flicker Fix

The analytical GPU shape renderer now uses two local primitive render targets and two final presentation targets.

- Shape creation and interactive scaling render into an inactive primitive target.
- The previous shape texture remains valid until the replacement draw succeeds.
- Completed compositor frames are copied into an inactive presentation target and swapped atomically.
- A failed intermediate render keeps the last valid frame instead of exposing a cleared render target.

This prevents transient black frames during rapid shape creation, resizing, adaptive-resolution changes, and final-quality refinement.
