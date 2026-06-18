# Editor prerender hard-preemption fix

When a title editor is open, its normal frame-cache jobs now take precedence over background live-text-cue cache population. The previous scheduler treated every live-cue job as urgent, so a large cue prerender queue continued draining before editor work despite editor focus being active.

Changes:

- Added an explicit `urgent` flag to render jobs instead of deriving urgency from `live_cue`.
- Only realtime/on-air playback hydration and explicitly urgent cue requests may preempt editor work.
- Background live-cue prerender jobs remain queued while editor frames are available.
- Editor-focused dequeue ignores live-cue variants, including variants belonging to the same title ID.
- Opening a title queues the complete editor timeline immediately after restoring disk states and prioritizing the visible frame.
- Closing or switching the editor automatically releases the focus and allows suspended background work to continue.

The currently executing worker job is allowed to finish safely; preemption takes effect before the next frame job, avoiding partial cache publication or corrupt state.
