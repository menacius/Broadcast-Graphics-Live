# Live cue row structure cache reuse

Live-cue frame keys now represent rendered output rather than the complete editable cue-list structure.

- Adding, deleting, or reordering a cue row no longer changes every existing row's cache key.
- Existing steady and persistence-transition variants remain reusable from RAM or disk.
- A newly added row is queued without destructively invalidating frame keys that may be shared with an identical existing row.
- Row-index state bookkeeping is reset only when the row's actual required-frame set changes.
- New transition combinations introduced by a new row are still prerendered and included in progress, while already-rendered combinations are reused.
