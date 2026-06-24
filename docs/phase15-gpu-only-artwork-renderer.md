# Phase 15: GPU-only artwork renderer

Phase 15 removes the former Cairo/Pango, QPainter artwork, QTextDocument drawing,
CPU mask/effect compositing and QImage frame-render compatibility paths. Text,
images, vector shapes, masks, effects, editor/live presentation and prerender now
share the same GPU graph.

`QPainter` and `QTextDocument` remain only in editor UI and inline text-editing
code. They do not generate title artwork. A `QImage` may exist transiently for
image-file decode, the final asynchronous SSD readback, screenshot/export capture
or disk-cache hydration, but no public playback/render API returns a CPU frame.

The RAM preference is clamped dynamically from 16 MiB through one half of the
installed physical memory. The selected budget belongs entirely to the sparse,
content-addressed GPU tile cache.
