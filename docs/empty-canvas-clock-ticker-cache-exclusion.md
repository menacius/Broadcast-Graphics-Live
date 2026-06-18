# Empty canvas and dynamic layer cache exclusion

- Clearing the final editor layer now clears the retained cached pixmap immediately, so an empty title displays an empty canvas rather than the last cached frame.
- Titles containing a Clock or Ticker layer are fully non-cacheable.
- Dynamic titles are excluded from editor prerender, live-cue prerender, RAM/disk hydration, session restore state publication, progress reporting, and playback preparation.
- Existing RAM and disk cache entries are removed when a title becomes dynamic, preventing stale frames from being reused after adding a Clock or Ticker layer.
