# Project-FPS transport and monitor-refresh GUI separation

The editor now uses two independent timing domains:

- `play_timer_` advances the playhead strictly at the configured OBS/project frame rate.
- `gui_refresh_timer_` follows the refresh rate of the monitor containing the editor and is used only for interactive canvas/UI presentation.

Moving the editor between 60 Hz, 120 Hz, 144 Hz, or 240 Hz displays no longer changes playback speed or creates additional title frames. High-refresh updates are limited to active pointer drags and inline text editing to avoid permanent idle repaint load.
