# Shape Scene Mask Foreground Activation

Shape scene masks now keep their configured target OBS scenes active while the Broadcast Graphics Live source is active in Program. This makes scenes rendered through a mask behave like foreground scene content instead of passive off-screen renders.

## Behavior

- When the title source is activated, every unique OBS scene selected by an active shape scene mask receives an OBS active reference.
- While active, media sources inside those scenes are allowed to start playback and behave as active Program content.
- When the title source is deactivated, the active references are released so the masked scenes return to normal OBS lifecycle behavior.
- If source settings or title data change while the title source is active, the active scene list is rebuilt safely, avoiding duplicates when multiple mask layers use the same scene.
- Rendering still hides the title source temporarily when rendering a target scene, preventing recursive self-rendering.
