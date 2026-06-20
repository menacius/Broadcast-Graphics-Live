# Live cue emergency cache, playback gate, and playlist focus fix

## Fixed failures

- Live cue jobs cancelled while a row editor had focus are explicitly restored when editing ends. This closes the case where an asynchronous structure refresh accepted the edited row fingerprint while rendering was paused and therefore failed to queue the row on resume.
- Manual live cue cache rebuilds now override stale/edit-focus pause state instead of silently returning without work.
- Cue playback is gated by actual frame residency in RAM or SSD:
  - loop mode requires frames from the start through `loop_end`;
  - pause mode requires frames from the start through `pause_time`;
  - play-once mode requires the complete title;
  - row-to-row transitions require their complete outgoing/incoming transition state.
- The OBS source repeats the same cache gate before consuming a cue revision, preventing hotkeys, title rebinding, or another runtime path from starting an incompletely cached cue.
- Runtime revisions produced by an active playlist no longer rebuild the live cue table. Only cue/cache status decorations are refreshed, so cell editors and keyboard focus remain intact.

## Queue behavior

Resuming a focused row restores its steady state and every transition involving that row. Existing resident frames are reused, while missing or cancelled frames are queued normally. Manual rebuild remains a forced regeneration with its existing priority group.
