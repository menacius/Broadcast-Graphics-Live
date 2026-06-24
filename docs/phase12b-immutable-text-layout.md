# Phase 12B — Immutable Text Layout and CPU Shaping

## Purpose

Phase 12B separates text shaping and line layout from rasterization. An
evaluated `RichTextDocument` is converted into an immutable, renderer-neutral
snapshot containing glyph IDs, physical font identities, glyph positions,
Unicode clusters, rich-text runs and line geometry.

The snapshot contains no `QTextDocument`, `QImage`, `QPainter`, Cairo, Pango or
thread-bound Qt font objects. The Qt backend uses `QTextLayout` only while a
layout cache miss is being built. The resulting plain C++ data can be retained
and consumed by the future GPU glyph-atlas renderer.

## Main contract

`src/text/title-text-layout.h` defines:

- `TextLayoutRequest`: evaluated canonical document plus layout constraints;
- `TextLayoutKey`: separate content/shaping and geometry fingerprints;
- `TextLayoutGlyph`: atlas-ready glyph ID, position, advance and ink bounds;
- `TextLayoutCluster`: UTF-8 byte range, glyph range, bidi direction and cursor
  bounds;
- `TextLayoutRun`: physical font fingerprint, shaping properties and
  split-ligature clip bounds;
- `TextLayoutLine`: glyph/run/cluster ranges and line metrics;
- `TextLayoutData`: immutable complete layout;
- `TextLayoutPaintRun`: fill and text-decoration data derived independently of
  shaping.

## Cache invariants

The shared cache retains strong immutable snapshots and evicts the oldest entry
when its bounded capacity is exceeded. It does not retain a second copy of the
rich-text document.

A new shape/layout is required when any of these change:

- plain text;
- font face, style, size, weight or italic state;
- kerning, tracking, H/V scale or baseline shift;
- capitalization/super/subscript shaping mode;
- ligature or OpenType feature switches;
- paragraph alignment, indents, spacing, wrapping or box geometry.

Paint-only changes do not invalidate shaping:

- solid or gradient fill;
- underline;
- strikethrough.

They produce fresh `TextLayoutPaintRun` records which can be uploaded as GPU
material data while reusing the same glyph layout.

## Unicode and bidi

The persistent model continues to use UTF-8 byte ranges. The Qt adapter
converts those ranges explicitly to and from Qt UTF-16 positions. On Qt 6.5 and
newer it requests glyph-to-string indexes directly. Older Qt builds use cursor
mapping as a compatibility fallback.

Cluster rectangles use cursor advances rather than only glyph ink bounds. This
keeps whitespace, combining sequences, ligatures and right-to-left text
hittable and selectable. Split ligature clip rectangles are retained on each
run for later GPU clipping.

## Current integration

The editor and OBS source use the same evaluated-defaults helper and the same
immutable layout cache for text auto-sizing. Text transition range bounds also
consume the immutable cluster geometry.

The existing Qt raster adapter is intentionally still present for final frame
pixels. Replacing that adapter with persistent glyph textures, GPU quads and
shader effects is Phase 12C.

## Deliberate boundary

The canonical language field remains persisted metadata, but the current
`QTextLayout` public API does not accept a per-run BCP-47 shaping language. It
therefore does not participate in the Phase 12B cache key. A direct HarfBuzz
backend can connect that field later without changing the immutable layout
contract.
