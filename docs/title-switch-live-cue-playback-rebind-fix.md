# Title-switch live cue playback rebind fix

## Problem

When the OBS source was initially bound to one title and the dock was switched to a second title, a cue could be issued before the asynchronous OBS source settings update completed. `source_update()` then copied the second title's current `cue_revision` into `seen_cue_revision`, incorrectly treating the already-issued cue as consumed. The cached frame lookup succeeded for frame zero, but the playback state machine remained stopped.

## Fix

- Detect an actual title ID change in `source_update()`.
- If the newly bound title already contains an active or pending cue, arm a one-shot `force_cue_state_sync` flag.
- Process that state on the next `source_video_tick()` even when the revision numbers already match.
- Preserve normal stopped behavior when switching to a title with no active cue.
- Add a diagnostic log entry for title-switch cue rebinding.
