# Unique RAM Payload Accounting

This step extends temporal visual-state deduplication so aliased timeline frames no longer consume or report a full frame allocation each.

## Changes

- Replaced per-frame `QImage` ownership/accounting in `RamFrameCache` with a key-to-payload map.
- A payload is identified by the backing store returned by `QImage::cacheKey()`.
- Multiple timeline keys can reference one immutable, implicitly shared `QImage` payload.
- RAM usage is charged once per unique backing store rather than once per frame key.
- Reference counts release the pixel allocation only after the final alias is removed.
- LRU eviction continues until a unique payload is actually released; evicting a single alias does not falsely reduce the byte counter.
- Canonical frames restored from disk are promoted back to RAM together with the requesting alias, preserving sharing after eviction.

## Scope

This step optimizes RAM residency and accounting. Timeline states, seeking, progress indicators, invalidation and disk frame files remain frame-addressable. Disk payload deduplication and cropped/sparse frame storage are intentionally left for later steps.
