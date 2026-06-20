# Exposed Image Cue Intrinsic Size Fix

## Problem

Changing an exposed image through a live cue updated only the image path. The layer kept the previous asset's intrinsic width, height, and image-size values, causing incorrect dimensions and aspect ratio in dock playback, hotkeys, uncached rendering, and cached/prerender variants.

## Changes

- Added a shared image cue update helper for bitmap and SVG assets.
- Reads and caches each replacement asset's intrinsic dimensions.
- Updates `image_width`, `image_height`, and the image-size model whenever the exposed image changes.
- Rebases animated image-size keyframes to the replacement asset's dimensions instead of discarding the animation.
- Uses the same update path for dock cues, cue hotkeys, source playback, and cache variants.
