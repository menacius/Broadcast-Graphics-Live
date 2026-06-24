# OBS Source Startup First-Frame Recovery

OBS title sources now remain dirty until `video_render` successfully draws their first GPU frame.

## Fixed failure mode

During OBS startup, source creation, title-store restoration, RAM/disk cache restoration, and graphics-resource initialization can overlap. Previously `video_tick` could prepare an empty or temporarily invalid GPU session and mark the source clean before `video_render` proved that a drawable texture existed. The source then remained transparent until an unrelated cue or runtime revision forced another update.

## Contract

- Startup and source rebinding force one full store/session reconciliation.
- Existing current or pending cues are explicitly synchronized at startup.
- Source activation requires a fresh successful draw.
- A missing title during store restoration keeps the source pending instead of clean.
- `video_tick` only prepares the render graph.
- Only a successful `video_render` clears the first-frame pending and dirty states.
- Failed draws automatically retry and invalidate stale store/cache revision assumptions after repeated failures.
