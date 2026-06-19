# Editor interactive rendering and Live Text Cue throttling

- Fixed adaptive-quality modes now use the same live interaction path as Auto.
- Editor-local settled-frame cache reads/writes are bypassed while the model is changing, preventing stale nudge frames and per-input cache overhead.
- Reduced-quality interaction uses an editor-only draft pass that temporarily disables expensive blur, glow, shadow, bloom, emboss and motion-blur effects. A settled render restores the selected quality after the interaction pause.
- Interactive presentation cadence remains at a 60 Hz target and still coalesces duplicate render requests.
- Live Text Cue prerender jobs no longer occupy the highest normal queue band. Realtime/urgent hydration can still preempt normally.
- Speculative Live Text Cue prerender receives pre-render and post-render cooperative delays to reduce CPU, compression and disk-I/O bursts that can interfere with OBS realtime output.
