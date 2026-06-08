#include "title-rich-text.h"

#include <cassert>
#include <iostream>

static RichTextDocument text_doc(const std::string &text)
{
    RichTextDocument doc;
    doc.plain_text = text;
    doc.default_format.font_family = "Inter";
    doc.default_format.font_size = 40;
    doc.default_format.fill.color = 0xFFFFFFFF;
    doc.ranges = {{0, text.size(), doc.default_format}};
    doc.normalize();
    return doc;
}

int main()
{
    RichTextDocument doc = text_doc("HelloWorld");
    RichTextCharFormat red = doc.default_format;
    red.fill.color = 0xFFFF0000;
    red.font_size = 30;
    RichTextCharFormat blue = doc.default_format;
    blue.fill.color = 0xFF0000FF;
    blue.font_size = 72;
    blue.text_style = 1;
    blue.ligatures = false;
    blue.kerning_mode = 2;
    blue.manual_kerning = 12.0f;
    doc.ranges = {{0, 5, red}, {5, 5, blue}};
    doc.normalize();
    assert(doc.ranges.size() == 2);
    assert(doc.ranges[1].format.text_style == 1);
    assert(!doc.ranges[1].format.ligatures);
    assert(doc.ranges[1].format.kerning_mode == 2);

    rich_text_document_replace_text(doc, "Hello Big World");
    assert(doc.plain_text == "Hello Big World");
    assert(doc.transactions.size() == 1);
    assert(doc.transactions.back().position == 5);
    assert(doc.transactions.back().inserted_text == " Big ");
    assert(doc.ranges.size() == 3);
    assert(doc.ranges[0].start == 0 && doc.ranges[0].length == 5);
    assert(doc.ranges[1].start == 5 && doc.ranges[1].length == 5);
    assert(doc.ranges[2].start == 10 && doc.ranges[2].length == 5);

    RichTextCharFormat gradient = doc.default_format;
    gradient.fill.type = 1;
    gradient.fill.gradient_start_color = 0xFFFFAA00;
    gradient.fill.gradient_end_color = 0xFF0033FF;
    gradient.fill.gradient_start_opacity = 0.75f;
    gradient.fill.gradient_end_opacity = 0.5f;
    gradient.fill.gradient_opacity = 0.8f;
    gradient.fill.gradient_center_x = 0.25f;
    gradient.fill.gradient_center_y = 0.75f;
    gradient.fill.gradient_scale = 1.5f;
    gradient.fill.gradient_focal_x = 0.2f;
    gradient.fill.gradient_focal_y = 0.8f;
    rich_text_document_replace_text(doc, "Hello Big Wide World", &gradient);
    bool has_gradient = false;
    for (const auto &range : doc.ranges) {
        has_gradient = has_gradient || (range.format.fill.type == 1);
        if (range.format.fill.type == 1) {
            assert(range.format.fill.gradient_start_opacity == 0.75f);
            assert(range.format.fill.gradient_end_opacity == 0.5f);
            assert(range.format.fill.gradient_opacity == 0.8f);
            assert(range.format.fill.gradient_center_x == 0.25f);
            assert(range.format.fill.gradient_center_y == 0.75f);
            assert(range.format.fill.gradient_scale == 1.5f);
            assert(range.format.fill.gradient_focal_x == 0.2f);
            assert(range.format.fill.gradient_focal_y == 0.8f);
        }
    }
    assert(has_gradient);

    RichTextDocument undo_snapshot = doc;
    rich_text_document_replace_text(doc, "Hello World");
    doc.normalize();
    for (const auto &range : doc.ranges)
        assert(range.start + range.length <= doc.plain_text.size());

    RichTextDocument redo_snapshot = doc;
    doc = undo_snapshot;
    assert(doc.plain_text == "Hello Big Wide World");
    doc = redo_snapshot;
    assert(doc.plain_text == "Hello World");

    RichTextDocument reloaded = doc;
    reloaded.normalize();
    assert(reloaded.plain_text == doc.plain_text);
    assert(reloaded.ranges.size() == doc.ranges.size());

    std::cout << "rich text model transactions, mixed styles, gradients, property replacement, serialization-copy reload, and undo/redo snapshots passed\n";
    return 0;
}
