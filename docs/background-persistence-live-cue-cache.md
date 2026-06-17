# Background Persistence live-cue cache fix

This update makes live-cue prerendering match the uncached playback path for
stateful cue transitions.

## Changes

- Live-cue cache requests now address the full frame range by playback time,
  instead of only using the pause-frame key.
- The OBS source render path now prefers `requestLiveCueFrame(title, row, time)`
  for active cue rows before falling back to normal title cache frames.
- When a cue is clicked from an already active row, the cache queues both:
  - the current row range, used for the uncached outro/hold phase;
  - the requested row range, used for the intro/active phase.
- Background Persistence transition flags are preserved when the live source is
  already inside a persistence transition, and are not forced on for stable
  active rows.
- Live cue readiness and progress now include prerequisite transition ranges, so
  UI progress does not show a cue as ready while required persistence frames are
  still missing.

## Expected behavior

With cache enabled, a cue that needs Background Persistence should be fully
prerendered before activation. When the cue is fired, playback should consume
ready cached frames and visually match the uncached source behavior.
