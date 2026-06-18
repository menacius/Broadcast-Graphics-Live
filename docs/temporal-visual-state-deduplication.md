# Temporal Visual-State Deduplication

The prerender worker now computes a lightweight evaluated visual-state hash for every timeline sample before invoking the full renderer. The hash combines the immutable content identity with resolved visibility and all animated layer/effect values consumed by the renderer.

When a previous frame in the same title/cue variant has the same evaluated state, the worker reuses its cached image instead of calling `render_title_to_image()` again. Timeline frame keys and cache states remain independent, so seeking, invalidation, live-cue progress and playback semantics are unchanged.

This first optimization targets hold/static intervals and repeated animation states. It intentionally keeps the existing per-frame RAM/disk accounting and storage format; unique-payload accounting and persistent alias records belong to the next optimization stage.
