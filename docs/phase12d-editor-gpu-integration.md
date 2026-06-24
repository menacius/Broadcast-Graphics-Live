# Phase 12D — Editor GPU Text Integration

Phase 12D makes the editor canvas consume the same immutable text layout and GPU text artwork path as the OBS source. The visible text remains GPU-rendered at all times; the embedded `QTextEdit` is retained only as a transparent IME/input bridge.

## Shared editor geometry

- Selection rectangles are derived from `TextLayoutCluster` cursor boundaries.
- Caret geometry is derived from the same cached layout, including ligatures, UTF-8 boundaries, multiline text and RTL clusters.
- Selection and caret rectangles are transformed through the real layer-to-canvas and canvas-to-view transforms.
- Selection and caret use immutable layout geometry, while point-text auto-size intentionally keeps the live `QTextDocument` measurement contract so text can grow beyond the previous textbox bounds.

Cursor or selection movement invalidates only the canvas overlay. It does not mark the artwork model dirty. Actual content or formatting edits mark the GPU model dirty; delayed `documentSizeChanged` notifications complete the same auto-grow transaction and publish the expanded textbox immediately, preserving the pre-12D editing behavior.

## Text-only stroke contract

The GPU text SDF renderer now receives explicit outside and inside coverage extents:

- **outer**: full width outside, none inside;
- **mid**: half outside and half inside;
- **inner**: none outside, full width inside.

Stroke order is independent of alignment. Batches are always composed as **Behind -> Fill -> Front**, so an inner stroke placed behind an opaque fill can be fully covered as requested. The same contract is enforced in the Qt compatibility text adapter by removing outlines from the fill document, rendering grouped stroke-only documents, applying outer/inner glyph masks, and composing Behind -> Fill -> Front. Shape/primitive rendering is unchanged.

## Cache identity

The renderer cache ABI is `gpu-renderer-v20-phase12d1-text-stroke-composition`, invalidating frames produced before the corrected text-stroke composition and restored input-growth contract.
