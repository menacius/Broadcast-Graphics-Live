#include "title-rich-text.h"
#ifndef OBS_GSP_RICH_TEXT_STANDALONE_TEST
#include "title-data.h"
#endif

#include <algorithm>
#include <utility>

#ifndef OBS_GSP_RICH_TEXT_STANDALONE_TEST
static RichTextCharFormat layer_char_format(const Layer &layer)
{
    RichTextCharFormat f;
    f.font_family = layer.font_family;
    f.font_style = layer.font_style;
    f.font_size = layer.font_size;
    f.bold = layer.font_bold;
    f.italic = layer.font_italic;
    f.underline = layer.text_underline;
    f.strikethrough = layer.text_strikethrough;
    f.kerning = layer.font_kerning;
    f.kerning_mode = layer.kerning_mode;
    f.manual_kerning = layer.manual_kerning;
    f.tracking = layer.char_tracking;
    f.scale_x = layer.char_scale_x;
    f.scale_y = layer.char_scale_y;
    f.baseline_shift = layer.baseline_shift;
    f.text_style = layer.text_style;
    f.ligatures = layer.text_ligatures;
    f.stylistic_alternates = layer.text_stylistic_alternates;
    f.fractions = layer.text_fractions;
    f.opentype_features = layer.text_opentype_features;
    f.language = layer.text_language;
    f.fill.type = layer.fill_type;
    f.fill.color = layer.text_color;
    f.fill.gradient_type = layer.gradient_type;
    f.fill.gradient_start_color = layer.gradient_start_color;
    f.fill.gradient_end_color = layer.gradient_end_color;
    f.fill.gradient_start_pos = layer.gradient_start_pos;
    f.fill.gradient_end_pos = layer.gradient_end_pos;
    f.fill.gradient_start_opacity = layer.gradient_start_opacity;
    f.fill.gradient_end_opacity = layer.gradient_end_opacity;
    f.fill.gradient_opacity = layer.gradient_opacity;
    f.fill.gradient_angle = layer.gradient_angle;
    f.fill.gradient_center_x = layer.gradient_center_x;
    f.fill.gradient_center_y = layer.gradient_center_y;
    f.fill.gradient_scale = layer.gradient_scale;
    f.fill.gradient_focal_x = layer.gradient_focal_x;
    f.fill.gradient_focal_y = layer.gradient_focal_y;
    return f;
}

static RichTextParagraphFormat layer_paragraph_format(const Layer &layer)
{
    RichTextParagraphFormat f;
    f.align_h = layer.align_h;
    f.align_v = layer.align_v;
    f.indent_left = layer.paragraph_indent_left;
    f.indent_right = layer.paragraph_indent_right;
    f.indent_first_line = layer.paragraph_indent_first_line;
    f.line_spacing = layer.text_leading;
    f.space_before = layer.paragraph_space_before;
    f.space_after = layer.paragraph_space_after;
    f.hyphenate = layer.paragraph_hyphenate;
    return f;
}

#endif

static bool same_format(const RichTextCharFormat &a, const RichTextCharFormat &b)
{
    return a.font_family == b.font_family && a.font_style == b.font_style &&
           a.font_size == b.font_size && a.bold == b.bold && a.italic == b.italic &&
           a.underline == b.underline && a.strikethrough == b.strikethrough &&
           a.kerning == b.kerning && a.kerning_mode == b.kerning_mode &&
           a.manual_kerning == b.manual_kerning && a.tracking == b.tracking &&
           a.scale_x == b.scale_x && a.scale_y == b.scale_y &&
           a.baseline_shift == b.baseline_shift && a.text_style == b.text_style &&
           a.ligatures == b.ligatures && a.stylistic_alternates == b.stylistic_alternates &&
           a.fractions == b.fractions && a.opentype_features == b.opentype_features &&
           a.language == b.language && a.fill.type == b.fill.type &&
           a.fill.color == b.fill.color && a.fill.gradient_type == b.fill.gradient_type &&
           a.fill.gradient_start_color == b.fill.gradient_start_color &&
           a.fill.gradient_end_color == b.fill.gradient_end_color &&
           a.fill.gradient_start_pos == b.fill.gradient_start_pos &&
           a.fill.gradient_end_pos == b.fill.gradient_end_pos &&
           a.fill.gradient_start_opacity == b.fill.gradient_start_opacity &&
           a.fill.gradient_end_opacity == b.fill.gradient_end_opacity &&
           a.fill.gradient_opacity == b.fill.gradient_opacity &&
           a.fill.gradient_angle == b.fill.gradient_angle &&
           a.fill.gradient_center_x == b.fill.gradient_center_x &&
           a.fill.gradient_center_y == b.fill.gradient_center_y &&
           a.fill.gradient_scale == b.fill.gradient_scale &&
           a.fill.gradient_focal_x == b.fill.gradient_focal_x &&
           a.fill.gradient_focal_y == b.fill.gradient_focal_y;
}

void RichTextDocument::normalize()
{
    const size_t text_len = plain_text.size();
    selection.anchor = std::min(selection.anchor, text_len);
    selection.head = std::min(selection.head, text_len);

    std::vector<RichTextRange> clipped;
    clipped.reserve(ranges.size());
    for (auto range : ranges) {
        if (range.start >= text_len || range.length == 0) continue;
        range.length = std::min(range.length, text_len - range.start);
        if (same_format(range.format, default_format)) continue;
        clipped.push_back(range);
    }
    std::sort(clipped.begin(), clipped.end(), [](const auto &a, const auto &b) {
        if (a.start != b.start) return a.start < b.start;
        return a.length < b.length;
    });
    ranges.clear();
    for (const auto &range : clipped) {
        if (!ranges.empty()) {
            auto &prev = ranges.back();
            const size_t prev_end = prev.start + prev.length;
            if (prev_end >= range.start && same_format(prev.format, range.format)) {
                prev.length = std::max(prev_end, range.start + range.length) - prev.start;
                continue;
            }
            if (range.start < prev_end) {
                RichTextRange adjusted = range;
                adjusted.start = prev_end;
                if (adjusted.start >= range.start + range.length) continue;
                adjusted.length = range.start + range.length - adjusted.start;
                ranges.push_back(adjusted);
                continue;
            }
        }
        ranges.push_back(range);
    }

    blocks.clear();
    size_t start = 0;
    while (start <= text_len) {
        const size_t nl = plain_text.find('\n', start);
        RichTextBlock block;
        block.start = start;
        block.length = (nl == std::string::npos) ? text_len - start : nl - start;
        block.format = default_paragraph_format;
        blocks.push_back(block);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
}

#ifndef OBS_GSP_RICH_TEXT_STANDALONE_TEST
RichTextDocument rich_text_document_from_layer_defaults(const Layer &layer)
{
    RichTextDocument doc;
    doc.plain_text = layer.text_content;
    doc.default_format = layer_char_format(layer);
    doc.default_paragraph_format = layer_paragraph_format(layer);
    doc.selection = {doc.plain_text.size(), doc.plain_text.size()};
    doc.typing_format = doc.default_format;
    doc.has_typing_format = true;
    doc.normalize();
    return doc;
}

void rich_text_document_sync_layer_defaults(RichTextDocument &doc, const Layer &layer)
{
    doc.default_format = layer_char_format(layer);
    doc.default_paragraph_format = layer_paragraph_format(layer);
    if (!doc.has_typing_format)
        doc.typing_format = doc.default_format;
    doc.normalize();
}

static void set_static_argb_channels(AnimatedProperty &a, AnimatedProperty &r,
                                     AnimatedProperty &g, AnimatedProperty &b,
                                     uint32_t argb)
{
    a.static_value = (argb >> 24) & 0xFF;
    r.static_value = (argb >> 16) & 0xFF;
    g.static_value = (argb >> 8) & 0xFF;
    b.static_value = argb & 0xFF;
}

void rich_text_document_sync_layer_mirrors(Layer &layer)
{
    if (layer.type != LayerType::Text && layer.type != LayerType::Ticker)
        return;
    if (layer.rich_text.empty())
        layer.rich_text = rich_text_document_from_layer_defaults(layer);
    layer.rich_text.normalize();

    const RichTextCharFormat &f = layer.rich_text.has_typing_format ? layer.rich_text.typing_format : layer.rich_text.default_format;
    layer.text_content = layer.rich_text.plain_text;
    layer.font_family = f.font_family;
    layer.font_style = f.font_style;
    layer.font_size = f.font_size;
    layer.font_bold = f.bold;
    layer.font_italic = f.italic;
    layer.text_underline = f.underline;
    layer.text_strikethrough = f.strikethrough;
    layer.font_kerning = f.kerning;
    layer.kerning_mode = f.kerning_mode;
    layer.manual_kerning = f.manual_kerning;
    layer.char_tracking = f.tracking;
    layer.char_scale_x = f.scale_x;
    layer.char_scale_y = f.scale_y;
    layer.baseline_shift = f.baseline_shift;
    layer.text_style = f.text_style;
    layer.text_ligatures = f.ligatures;
    layer.text_stylistic_alternates = f.stylistic_alternates;
    layer.text_fractions = f.fractions;
    layer.text_opentype_features = f.opentype_features;
    layer.text_language = f.language;
    layer.fill_type = f.fill.type;
    layer.text_color = f.fill.color;
    layer.gradient_type = f.fill.gradient_type;
    layer.gradient_start_color = f.fill.gradient_start_color;
    layer.gradient_end_color = f.fill.gradient_end_color;
    layer.gradient_start_pos = f.fill.gradient_start_pos;
    layer.gradient_end_pos = f.fill.gradient_end_pos;
    layer.gradient_start_opacity = f.fill.gradient_start_opacity;
    layer.gradient_end_opacity = f.fill.gradient_end_opacity;
    layer.gradient_opacity = f.fill.gradient_opacity;
    layer.gradient_angle = f.fill.gradient_angle;
    layer.gradient_center_x = f.fill.gradient_center_x;
    layer.gradient_center_y = f.fill.gradient_center_y;
    layer.gradient_scale = f.fill.gradient_scale;
    layer.gradient_focal_x = f.fill.gradient_focal_x;
    layer.gradient_focal_y = f.fill.gradient_focal_y;
    set_static_argb_channels(layer.text_color_a, layer.text_color_r, layer.text_color_g,
                             layer.text_color_b, layer.text_color);

    const RichTextParagraphFormat &p = layer.rich_text.default_paragraph_format;
    layer.align_h = p.align_h;
    layer.align_v = p.align_v;
    layer.paragraph_indent_left = p.indent_left;
    layer.paragraph_indent_right = p.indent_right;
    layer.paragraph_indent_first_line = p.indent_first_line;
    layer.text_leading = p.line_spacing;
    layer.paragraph_space_before = p.space_before;
    layer.paragraph_space_after = p.space_after;
    layer.paragraph_hyphenate = p.hyphenate;
    layer.paragraph_indent_left_prop.static_value = p.indent_left;
    layer.paragraph_indent_right_prop.static_value = p.indent_right;
    layer.paragraph_indent_first_line_prop.static_value = p.indent_first_line;
}

#endif

void rich_text_document_replace_text(RichTextDocument &doc, const std::string &next_text,
                                     const RichTextCharFormat *insertion_format)
{
    const std::string old = doc.plain_text;
    if (old == next_text) {
        doc.normalize();
        return;
    }

    size_t prefix = 0;
    const size_t min_len = std::min(old.size(), next_text.size());
    while (prefix < min_len && old[prefix] == next_text[prefix]) ++prefix;
    size_t old_suffix = old.size();
    size_t new_suffix = next_text.size();
    while (old_suffix > prefix && new_suffix > prefix && old[old_suffix - 1] == next_text[new_suffix - 1]) {
        --old_suffix;
        --new_suffix;
    }

    const size_t removed_len = old_suffix - prefix;
    const size_t inserted_len = new_suffix - prefix;
    std::vector<RichTextRange> next_ranges;
    for (auto range : doc.ranges) {
        const size_t range_end = range.start + range.length;
        if (range_end <= prefix) {
            next_ranges.push_back(range);
        } else if (range.start >= old_suffix) {
            range.start = range.start - removed_len + inserted_len;
            next_ranges.push_back(range);
        } else {
            if (range.start < prefix)
                next_ranges.push_back({range.start, prefix - range.start, range.format});
            if (range_end > old_suffix)
                next_ranges.push_back({prefix + inserted_len, range_end - old_suffix, range.format});
        }
    }
    if (inserted_len > 0)
        next_ranges.push_back({prefix, inserted_len, insertion_format ? *insertion_format : doc.default_format});

    RichTextTransaction tr;
    tr.type = "replace_text";
    tr.position = prefix;
    tr.removed_text = old.substr(prefix, removed_len);
    tr.inserted_text = next_text.substr(prefix, inserted_len);
    tr.before_selection = doc.selection;
    tr.after_selection = {prefix + inserted_len, prefix + inserted_len};
    if (insertion_format) {
        doc.typing_format = *insertion_format;
        doc.has_typing_format = true;
    }

    doc.plain_text = next_text;
    doc.ranges = std::move(next_ranges);
    doc.selection = tr.after_selection;
    doc.transactions.push_back(std::move(tr));
    if (doc.transactions.size() > 100)
        doc.transactions.erase(doc.transactions.begin(), doc.transactions.begin() + (doc.transactions.size() - 100));
    doc.normalize();
}
