# Live Cue Persistence Startup and Cache UI Synchronization Fix

- Applies the saved Background Persistence and Text Persistence settings inside the cache manager before any live-cue variant or transition is generated. This prevents OBS startup from prerendering non-persistent variants before the dock finishes restoring its UI state.
- Persistence changes are saved first, then immediately invalidate/rebuild the affected live-cue requirement sets and refresh the row badges.
- RAM, disk, and full cache clears now publish an explicit global live-cue state notification. The title dock responds by rebuilding both title-level and row-level cache indicators immediately instead of waiting for a later worker or polling update.
