# Editor prerender priority focus

When a Title Editor window has an active title, CacheManager enters editor prerender focus mode.

- Urgent realtime and live-cue requests remain eligible and are never blocked.
- Normal background jobs for other titles remain queued but are not consumed while the editor is open.
- Normal jobs belonging to the title currently open in the editor are consumed before unrelated background work.
- Closing the editor clears the focus and immediately resumes the preserved background queue.
- Switching the editor to another title updates the focused title without deleting queued work.

This keeps editor timeline prerendering responsive without discarding or rebuilding existing live-cue and background cache jobs.
