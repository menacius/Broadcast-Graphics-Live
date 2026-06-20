# Clock and ticker partial frame caching

Titles containing a Clock or Ticker layer are no longer automatically excluded from the frame cache.

## Render policy

The cache policy classifies Clock and Ticker layers as runtime-dynamic. Dynamic state is propagated to layers whose rendered output depends on:

- a dynamic parent transform or opacity;
- a dynamic track matte / mask source.

The renderer then identifies the first dynamic output in bottom-to-top layer order. The largest z-order-safe static prefix below that point is prerendered and stored in the normal RAM/SSD frame cache. The first dynamic layer and every layer above it remain live.

This preserves the original compositing order for foreground artwork, masks, parenting and non-normal blend modes. A flattened static frame is never drawn above a live layer that should cover it.

## Playback

- The background worker renders only the cacheable prefix.
- Sparse cached payloads are expanded to the logical title canvas before composition.
- The editor-facing cache API composites the current live suffix over the cached prefix.
- The OBS source performs the same composition for normal title playback and live-text cue variants.
- Clock refresh remains event-driven at one-second intervals when appropriate.
- Ticker movement remains live and marks the source dirty every video tick.

## Cache states

`TitleCacheability::PartiallyCacheable` is now active:

- `Cacheable`: no runtime-dynamic layer exists; cache the complete frame.
- `PartiallyCacheable`: a safe static prefix exists below the first dynamic output.
- `NonCacheable`: the first rendered output is dynamic, so the current single-prefix implementation cannot provide a useful cached underlay.

## Current boundary

This implementation deliberately uses one cached prefix rather than multiple cached islands. Therefore, static layers above the first Clock/Ticker are rendered as part of the live suffix. This is the safest integration with the current single-texture Cairo renderer and already removes the expensive background/lower-third render cost in the common layout where clocks and tickers sit near the top of the stack.

A future multi-texture render graph can extend this into multiple cacheable islands without changing the cacheability analysis introduced here.
