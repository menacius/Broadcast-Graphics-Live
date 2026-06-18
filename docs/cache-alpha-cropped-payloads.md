# Alpha-cropped cache payloads

The frame cache now stores only the non-transparent alpha bounds of each rendered frame while preserving the original canvas dimensions and crop origin as payload metadata.

## Behavior

- RAM accounting uses the cropped pixel payload rather than the full title canvas.
- SSD cache format version 4 stores canvas width/height, crop X/Y, cropped width/height, and the LZ4-compressed cropped BGRA payload.
- Fully transparent frames use a single 1x1 transparent payload.
- Temporal aliases continue to share the same immutable cropped `QImage` backing store.
- Dirty-tile merges temporarily reconstruct the previous full canvas only when an invalidated frame must be merged.
- The current OBS playback stage reconstructs the full upload surface for compatibility. Direct cropped GPU texture drawing is intentionally left for the next optimization step.
- Older disk-cache versions are ignored by the manifest index and regenerated automatically.
