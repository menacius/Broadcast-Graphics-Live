# Development Version 014 — Cue first row when active

This delivery adds a per-instance OBS source option named **Cue first row when active**.

When enabled, the source activation callback:

1. Resolves the currently bound title.
2. Normalizes the exposed live-text columns and rows.
3. Applies row zero to the exposed layers.
4. Clears stale pending-uncue and persistence-transition state.
5. Advances the cue revision so the normal cache-gated playback state machine starts from the intro.
6. Synchronizes scene-mask source activation after the first row is active.

The setting defaults to disabled and is persisted by OBS with the source instance. Existing sources therefore keep their previous behavior.

If playlist restart-on-activation is also enabled, the first row is cued immediately and a forward playlist continues with row two.
