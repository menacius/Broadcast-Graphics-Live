# Live cue manual refresh and cache lifecycle fix

- Manual row refresh rebuilds only that row's steady cache state.
- Transition-pair states are not removed or requeued by the row refresh button.
- Disabling cache stops the worker, clears queued jobs, and invalidates the active job before publishing the disabled state.
- Deferred render callbacks return immediately while cache is disabled.
- Saving a title cancels pending work for that title before the stored title is copied or replaced.
- Existing cached frames remain available and normal selective invalidation decides what must be rebuilt after save.
