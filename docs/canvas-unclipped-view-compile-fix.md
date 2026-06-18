# Canvas Unclipped View Compile Fix

Fixed a Windows build failure caused by editor-only unclipped-canvas background logic leaking into the OBS source texture render path. The main source renderer now keeps the normal full-canvas background fill, while transparent outside-canvas handling remains isolated to the editor region render helper used by the unclipped preview mode.
