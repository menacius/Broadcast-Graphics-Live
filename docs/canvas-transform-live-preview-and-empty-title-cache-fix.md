# Canvas transform live preview and empty-title cache fix

- All active canvas geometry gestures now render directly from the live title model while the gesture is in progress, matching the existing corner-radius behavior. This covers move, resize/scale, rotate, origin, gradient and related geometry manipulation paths.
- Deleting the last layer now performs a synchronous editor artwork clear and removes all RAM/disk cache entries for the title before delayed invalidation can reuse the previous frame.
- The canvas widget and editor chrome remain visible for empty titles; only the stale artwork frame is cleared.
