# Atomic cache clear and exact prerender publication

This change separates logical cache invalidation from slow filesystem cleanup.
RAM/disk generations are invalidated under the publication gate, while the disk
cache directory is detached with a directory rename. Playback and the prerender
worker no longer wait for per-file deletion. Disk reads also require membership
in the active in-memory generation, so obsolete files can never be hydrated after
a clear even if physical reclamation is deferred.

Shutdown now stops the prerender worker before the single cache-clear operation.
The frontend exit callback no longer performs a duplicate clear while sources and
workers are active.

Perceptual/adaptive canonical-frame reuse is disabled for published prerender
frames. Exact evaluated-state reuse remains, but quantized values can no longer
replace frames at keyframe or transition boundaries. This preserves frame-level
correctness in both the editor and OBS source output while prerender is running.
