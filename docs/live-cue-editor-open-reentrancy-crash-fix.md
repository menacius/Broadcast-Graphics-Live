# Live cue editor-open reentrancy crash fix

## Problem

Opening the editor for a newly created title while cache/prerender was enabled could crash OBS without a crash message. The live-cue structure refresh queued the first frame and synchronously emitted cache state signals. UI refresh code could then request another structure refresh before the original refresh had committed its structure bookkeeping. The nested call still saw the title as uninitialized, pruned the just-queued frame, queued it again, and recursively repeated the cycle until OBS terminated.

## Fix

`CacheManager::refreshLiveCueStructure()` now uses a per-title reentrancy guard. Nested refresh attempts for the same title are ignored until the active reconciliation finishes, while refreshes for unrelated titles remain allowed. The guard is removed automatically on every function exit.

This prevents the `prune -> queue -> signal -> refresh` recursion, preserves the newly queued prerender job, and allows the editor to open normally with cache enabled.
