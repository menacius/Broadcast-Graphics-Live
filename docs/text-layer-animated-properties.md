# Text Layer Animated Properties

This update makes the following text-layer controls keyframeable and animateable through the existing timeline/keyframe engine:

- Character Size
- H Scale
- V Scale
- Tracking
- Baseline
- Space Before Paragraph
- Space After Paragraph

Each property now has its own animated backing property, keyframe button, context-menu support for deleting all keyframes, JSON persistence, timeline exposure, and playhead evaluation in both editor preview and OBS source rendering. Legacy scalar values are still written and read for backward compatibility; animated property data is stored alongside them and safely falls back to the scalar value when no keyframes exist.

The Language control has been removed from the text layer properties UI. Existing project files that still contain saved language values continue to load safely, but the editor no longer exposes the option.
