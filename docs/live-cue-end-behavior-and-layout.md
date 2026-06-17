# Live cue end behavior and row layout refinements

- Live text row width fitting now only expands text columns to the dock width the first time a title is loaded in the dock. Later refreshes keep the user's interactive column sizing/reordering intact.
- Added a title-level `cue_end_behavior` property:
  - `Show last frame`: when a live text cue reaches the end of the title timeline, the last frame remains visible and the row stays active until the user clicks the cue again.
  - `Show nothing`: when a live text cue reaches the end, the row is deactivated and the OBS source outputs nothing.
- Added the cue end behavior dropdown to the title/graphic properties panel.
- Removed the OBS source-level Loop and Speed controls; playback behavior is now controlled by the title/graphic settings.
