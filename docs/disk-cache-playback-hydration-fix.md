# Disk cache playback hydration fix

Restored sessions can correctly report live-cue variants as disk-resident while
none of their sparse payloads are in RAM yet. Starting playback in that state
previously queued one realtime frame at a time, so the source held or mixed
textures until enough frames had gradually been promoted.

The cue path now distinguishes **cache validity** from **playback readiness**.
Disk-resident frames remain blue/ready in the UI, but clicking or hotkeying a cue
queues the complete reachable steady and persistence-transition state for
high-priority disk-to-RAM hydration. Playback starts only after every required
frame is resident. Hydration reuses the existing LZ4/sparse payloads and never
forces a rerender. Dock cues use the existing armed-cue callback, while hotkeys
retry automatically after hydration completes.
