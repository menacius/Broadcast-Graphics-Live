# Empty canvas and dynamic layer cache handling

- Clearing the final editor layer clears the retained cached pixmap immediately, so an empty title displays an empty canvas rather than the last cached frame.
- Clock and Ticker layers remain runtime-dynamic and are never baked into cached pixels.
- A title containing Clock or Ticker can now be `PartiallyCacheable`: the largest z-order-safe static prefix below the first dynamic output is stored in RAM/SSD and the remaining layers are rendered live.
- Dynamic dependency propagation covers parent transforms/opacity and track mattes.
- Titles whose first rendered output is dynamic remain non-cacheable under the current single-prefix implementation.
- Existing full-frame cache artifacts are invalidated through the normal content/generation lifecycle when the title structure changes.

See `clock-ticker-partial-frame-cache.md` for the current architecture and limitations.
