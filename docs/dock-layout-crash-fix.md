# Dock layout transition crash fix

The editor now suppresses monitor-rate canvas/UI repaints while Qt is moving,
reparenting, resizing, or rebuilding dock widgets. A short single-shot settle
timer resumes adaptive GUI refresh only after the QMainWindow dock layout has
stabilized, then requests one final canvas update.

This prevents the high-refresh editor timer from traversing or repainting child
widgets while Qt's internal dock layout items are in a transitional state.
Playback timing remains driven exclusively by the project frame rate.
