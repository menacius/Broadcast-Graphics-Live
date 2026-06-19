# Editor General Performance Optimization

This change removes the largest UI-thread stalls in complex editor sessions and during title saves.

## Canvas rendering

- Added adaptive render coalescing to `CanvasPreview`.
- Expensive full-title renders are no longer executed for every queued pointer/paint event.
- The canvas keeps at most one delayed render pending, preventing event-queue buildup when many layers or effects are present.
- Preview pacing adapts between approximately 60 and 20 FPS based on the measured cost of the previous frame, preserving high refresh rates for light titles while keeping complex titles responsive.
- The last completed frame remains visible between renders, so the editor does not blank or block while interaction continues.

## Save path

- Saving now reuses the canvas's current rendered frame for the title-list preview instead of rendering the whole title again on the UI thread.
- Added asynchronous title-store persistence with an immutable deep snapshot.
- JSON serialization and disk writes now run outside the editor/OBS UI thread.
- Rapid consecutive saves are coalesced by generation; stale background saves are discarded before writing.
- Atomic temporary-file replacement is retained.

## Expected result

Complex titles should feel substantially less sluggish during canvas manipulation, and saving should no longer produce a large synchronous CPU/I/O spike that can interfere with OBS frame delivery.
