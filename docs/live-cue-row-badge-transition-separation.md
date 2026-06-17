# Live Cue Row Badge / Transition Progress Separation

Per-row cache badges now report only the steady prerender state of that row.
Background-persistence transition pairs remain part of the title-level aggregate
cache status and progress, but no longer force unrelated row badges to display
`Queued (100%)` when a single row is refreshed.
