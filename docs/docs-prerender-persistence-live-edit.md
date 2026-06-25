# Prerender Dock persistence and Live Edit persistence override

## Changes

- Renamed the Prerender Dock option `Play cached frames only` to `Play after rendering` while keeping the existing playback behavior (`cached_frames_only`) intact.
- Persisted the Prerender Dock playback controls through `QSettings` under the existing `BroadcastGraphicsLive/Dock` scope:
  - start mode
  - playback mode
  - skip frames
  - speed percent
  - Play after rendering
  - Play every frame
- Added a per-layer Live Edit checkbox: `Ignore Persistence`.
- `Ignore Persistence` is serialized with the layer as `ignore_persistence`.
- When `Expose To Dock` is enabled, `Ignore Persistence` is forced off, disabled, and shown grayed out.
- During Background Persistence cue holds, non-exposed layers with `Ignore Persistence` keep evaluating at the live/current timeline time instead of the held persistence time.

## Behavior

Background Persistence still freezes ordinary non-exposed layers during cue transitions. Layers marked `Ignore Persistence` are excluded from that hold and can continue animating normally. Exposed dock text remains controlled by cue/text persistence and cannot opt out through this checkbox.
