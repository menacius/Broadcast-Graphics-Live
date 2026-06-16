# Live Cue Cache Queue/Worker Fix

This update fixes the live text cue cache queue so rows no longer remain permanently in `Queued` when the worker drains the queue.

## Changes

- `RenderQueueManager::takeNext()` and `takeNextLiveCue()` now release the queue mutex before emitting `queueChanged()`.
  - This prevents direct Qt signal handlers from re-entering cache/queue status reads while the queue mutex is still held.
  - The previous behavior could deadlock the cache worker immediately after a job was removed from the queue, leaving the UI stuck on `Queued`.
- Live cue cache jobs continue to be drained even while prerender is paused or the editor is in interactive bypass mode.
- Manual live cue cache rebuilds now always reset the row cache and requeue the urgent job instead of becoming a no-op when the previous state was stuck in `Rendering`.
- Failed/null renders are now reported as `Stale`/error instead of leaving the row forever in `Rendering`.
- Successful worker renders now emit diagnostics updates after writing RAM/disk cache entries.

## Expected behavior

- Clicking the cue cache button should immediately requeue that row with urgent priority.
- The icon should progress from `Queued` to `Caching` and then to `Ready (RAM)` or `Cache error or stale` if rendering fails.
- Pausing timeline prerender should not block live cue cache jobs.
