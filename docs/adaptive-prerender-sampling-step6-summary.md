# OBS Graphics Studio Pro — Step 6

Implemented conservative adaptive prerender sampling on top of the existing exact temporal deduplication, sparse payloads, direct cropped drawing, and dirty-region renderer.

## Main changes

- Added a perceptual visual-state hash with conservative quantization.
- Reuses canonical rendered payloads for sub-visible frame-to-frame changes.
- Exact state reuse has priority over adaptive reuse.
- Disabled adaptive reuse for realtime requests and forced/stale renders.
- Preserved independent timeline frame states, seeking, progress, and invalidation.
- Added trace logging for adaptive reuse decisions.
