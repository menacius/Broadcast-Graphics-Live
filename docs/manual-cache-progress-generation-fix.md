# Manual cache progress generation fix

Manual live-cue cache rebuilds now track progress for the current rebuild generation instead of deriving it from frames that were already resident in RAM or on disk.

This prevents second and subsequent manual refreshes from remaining at 100% while alternating between Rendering and CachedRam. The row badge now advances through the four progress SVG buckets on every manual rebuild and reaches the final RAM/disk state only after all jobs in that generation complete.

The title dock also keeps a per-button visual signature, avoiding redundant icon recreation when duplicate state-owner notifications resolve to the same SVG and tint.
