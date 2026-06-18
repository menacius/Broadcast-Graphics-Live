# Layer Selection No-Rerender Fix

Layer selection is a UI-only operation and must not be treated as a render-affecting title modification.

The editor now guards programmatic refreshes of the Properties and Effects panels while a layer selection is being loaded. Any `property_changed` signals emitted as a side effect of populating controls are ignored during that guarded interval. Genuine user edits remain unchanged and still mark the title dirty, invalidate the affected cache, and update the canvas.

This prevents a plain layer click from enabling interactive cache bypass, scheduling invalidation, or queuing a fresh prerender sequence.
