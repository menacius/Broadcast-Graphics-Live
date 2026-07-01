# Timeline & Animation

Owns timeline primitives in `animation.h` / `animation.cpp`, including legacy easing, explicit per-keyframe temporal velocity/influence, spatial Bezier paths, deterministic evaluation, keyframe selection, and graph editing.

`timeline-widget.cpp` provides the ordinary timeline, while `temporal-graph-editor.inc` implements the AE-style Value/Speed Graph Editor. Both views mutate `TimelinePropertyRef` and commit through the same title undo stack. Editor playback, cached/prerendered frames, and OBS output must always consume `AnimatedProperty::evaluate()` / `AnimatedVec2Property::evaluate()` rather than implementing separate curve math.
