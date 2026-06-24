# Cache playback presentation diagnostics

Detailed cache playback logging now follows cached frames beyond the cache manager.

New stages:

- `gpu-submit-final`: a cached payload entered a GPU render session.
- `gpu-upload-final`: the pending payload became a GPU texture.
- `gpu-publish-final`: the rendered texture was copied to a stable presentation target.
- `gpu-draw`: the session actually submitted a texture to OBS/Qt display output.
- `consumer=editor-canvas stage=draw`: the editor canvas draw callback succeeded or failed.

Each final-frame submission has a monotonically increasing serial. Comparing submitted,
uploaded and published serials reveals dropped, overwritten or stale frames.
