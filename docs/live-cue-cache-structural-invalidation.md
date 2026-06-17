# Live Cue Cache Structural Invalidation

Live cue frame keys are now based only on render-visible state. The editable cue-row collection and numeric row positions are excluded from the frame content hash because adding, deleting, or reordering rows does not by itself change pixels in an already-applied cue snapshot.

When the cue structure changes, the cache manager rebuilds each row's required transition-key set. Existing content-addressed frames are reused, and only genuinely new persistence transition variants are queued. Frame-key generation hashes each variant once rather than once per frame, reducing synchronous UI work as cue lists grow.

The manager also tracks live-cue keys loaded or rendered during the session. Keys removed from every active cue requirement set are cancelled and evicted from RAM. Disk frames are retained for undo and future reuse. An in-flight render whose key becomes obsolete is discarded instead of being inserted back into RAM.
