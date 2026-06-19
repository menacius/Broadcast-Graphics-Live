# Effect cache invalidation and canvas history shortcuts

- Effects Panel edits now explicitly force a visual update before entering the title modification pipeline. This prevents effect enable/disable, reordering, and parameter changes from being mistaken for selection-only UI notifications and ensures the prerender/frame cache is invalidated.
- Undo and redo are routed at the editor event-filter level so focused canvas and viewport widgets cannot consume the shortcuts.
- Undo uses `Ctrl+Z`.
- Redo uses only `Ctrl+Shift+Z`; `Ctrl+Y` is intentionally not registered.
- Text-entry widgets keep their native document-level undo/redo behavior.
