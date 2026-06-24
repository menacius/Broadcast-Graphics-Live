#!/usr/bin/env python3
"""Structural acceptance audit for Phase 12D editor GPU text integration."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
LAYOUT_H = (ROOT / "src/text/title-text-layout.h").read_text(encoding="utf-8", errors="replace")
LAYOUT = (ROOT / "src/text/title-text-layout.cpp").read_text(encoding="utf-8", errors="replace")
CANVAS = (ROOT / "src/canvas/canvas-preview.cpp").read_text(encoding="utf-8", errors="replace")
INLINE = (ROOT / "src/canvas/canvas-preview-inline-text.cpp").read_text(encoding="utf-8", errors="replace")
EDITOR_INTERNAL = (ROOT / "src/editor/title-editor-internal.h").read_text(encoding="utf-8", errors="replace")
CACHE = (ROOT / "src/cache/cache-manager.cpp").read_text(encoding="utf-8", errors="replace")
DOC = (ROOT / "docs/phase12d-editor-gpu-integration.md").read_text(encoding="utf-8", errors="replace")

passes: list[str] = []
errors: list[str] = []


def require(text: str, tokens: tuple[str, ...], label: str) -> None:
    missing = [token for token in tokens if token not in text]
    if missing:
        errors.append(f"{label} misses: {', '.join(missing)}")
    else:
        passes.append(label)


require(
    LAYOUT_H + LAYOUT,
    (
        "struct TextLayoutRect",
        "text_layout_selection_rects",
        "text_layout_caret_rect",
        "TextLayoutCursorBoundary",
        "cluster.right_to_left",
    ),
    "immutable layout exports Unicode/RTL-aware selection and caret geometry",
)

require(
    INLINE,
    (
        "inline_text_selection_view_polygons",
        "text_layout_selection_rects(*layout, start, end)",
        "inline_text_caret_view_polygon",
        "text_layout_caret_rect(*layout",
        "layer_to_canvas(*layer",
        "canvas_to_view(",
    ),
    "canvas overlay transforms selection and caret from shared layout geometry",
)

if "text_edit_selection_viewport_rects" in CANVAS + INLINE + EDITOR_INTERNAL:
    errors.append("editor selection still depends on QTextDocument/QTextLayout viewport geometry")
else:
    passes.append("legacy QTextDocument selection geometry path is removed")

require(
    CANVAS,
    (
        "inline_text_editor_->setCursorWidth(0)",
        "color:rgba(255,255,255,0)",
        "selection-background-color:rgba(0,0,0,0)",
        "inline_text_caret_view_polygon(text_caret)",
    ),
    "QTextEdit remains a transparent IME bridge while the canvas owns visible caret rendering",
)

cursor_handler = re.search(
    r"auto emit_cursor_changed = \[this\]\(\) \{(?P<body>.*?)\n    \};",
    CANVAS,
    re.S,
)
if not cursor_handler:
    errors.append("cursor/selection event handler was not found")
else:
    body = cursor_handler.group("body")
    if "sync_inline_text_layer(false)" not in body or "invalidate_canvas_overlay_caches" not in body:
        errors.append("cursor/selection handler does not update canonical selection and overlay")
    elif "render_to_frame" in body or "gpu_model_dirty_" in body or "dirty_ = true" in body:
        errors.append("cursor/selection movement still invalidates or synchronously renders artwork")
    else:
        passes.append("cursor and selection movement is overlay-only and does not rerender artwork")

size_handler = re.search(
    r"documentSizeChanged, this, \[this\]\(const QSizeF &\) \{(?P<body>.*?)\n        \}\);",
    CANVAS,
    re.S,
)
if not size_handler:
    errors.append("transparent QTextEdit document-size handler was not found")
else:
    body = size_handler.group("body")
    if "refresh_inline_text_edit(true, true)" in body:
        passes.append("delayed QTextDocument size changes preserve pre-12D point-text growth")
    else:
        errors.append("delayed QTextDocument size changes no longer complete point-text auto-grow")

require(
    INLINE,
    (
        "doc->idealWidth()",
        "layout->documentSize()",
        "layer->text_box_width_to_text",
        "layer->text_box_height_to_text",
        "inline_text_document_local_rect",
    ),
    "inline editing retains the established unconstrained QTextDocument sizing contract",
)

refresh = re.search(
    r"void CanvasPreview::refresh_inline_text_edit\(.*?\n\}(?=\n\nQRectF CanvasPreview::inline_text_document_local_rect)",
    INLINE,
    re.S,
)
if not refresh:
    errors.append("inline text refresh function was not found")
else:
    body = refresh.group(0)
    if "gpu_model_dirty_ = true" not in body:
        errors.append("content edits do not invalidate the GPU text model")
    elif "render_to_frame();" not in body or "repaint(editor_rect);" not in body:
        errors.append("inline edits no longer publish the expanded textbox in the established transaction")
    else:
        passes.append("content edits publish model and auto-grown geometry in one edit transaction")

require(
    CACHE,
    ("gpu-renderer-v24-phase15-visibility-recovery",),
    "Phase 15 recovery ABI invalidates blank Phase 15 and older text/editor frame payloads",
)

require(
    DOC,
    (
        "same immutable text layout",
        "transparent IME/input bridge",
        "Behind -> Fill -> Front",
        "outer",
        "mid",
        "inner",
    ),
    "Phase 12D integration and text-only stroke contract are documented",
)

for item in passes:
    print(f"  PASS: {item}")
if errors:
    for item in errors:
        print(f"  FAIL: {item}")
    print(f"Result: {len(passes)} passed, {len(errors)} failed")
    sys.exit(1)
print(f"Result: {len(passes)} passed, 0 failed")
