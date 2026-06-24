# GPU Live/Prerender Parity — Phase 5

This phase makes the live/source GPU render graph the reference for cached and prerendered output.

- The renderer ABI is now part of every cache content hash. Renderer/effect changes invalidate stale RAM and disk prerenders automatically.
- Titles containing bounds-expanding or blur-based effects bypass CPU dirty-tile replacement and are rendered as a complete GPU frame.
- Glow, drop shadow, long shadow, inner glow, inner shadow, blur, motion blur, bloom, emboss, and blur-based general transitions therefore cannot be clipped at dirty-tile edges or retain stale halo pixels.
- Titles without those effects retain dirty-tile cache optimization.
