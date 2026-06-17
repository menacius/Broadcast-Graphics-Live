#include "title-rich-text.h"
#ifndef OBS_GSP_RICH_TEXT_STANDALONE_TEST
#include "title-data.h"
#endif

#include <algorithm>
#include <cctype>
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
    if (layer.type != LayerType::Text && layer.type != LayerType::Ticker && layer.type != LayerType::Clock)
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
    layer.paragraph_space_before_prop.static_value = p.space_before;
    layer.paragraph_space_after_prop.static_value = p.space_after;
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

    const RichTextSelection after_selection{prefix + inserted_len, prefix + inserted_len};
    if (insertion_format) {
        doc.typing_format = *insertion_format;
        doc.has_typing_format = true;
    }

    doc.plain_text = next_text;
    doc.ranges = std::move(next_ranges);
    doc.selection = after_selection;

    /* Transactions are editor-runtime state only. Do not accumulate byte-sliced
     * Unicode substrings here; they can be invalid UTF-8 and are not required
     * for saving/rendering the canonical rich-text model. */
    doc.transactions.clear();
    doc.normalize();
}



#ifndef OBS_GSP_RICH_TEXT_STANDALONE_TEST
void rich_text_document_ensure_canonical(Layer &layer)
{
    if (layer.type != LayerType::Text && layer.type != LayerType::Ticker && layer.type != LayerType::Clock)
        return;

    if (layer.rich_text.empty()) {
        layer.rich_text = rich_text_document_from_layer_defaults(layer);
    } else {
        rich_text_document_sync_layer_defaults(layer.rich_text, layer);
        if (layer.rich_text.plain_text.empty() && !layer.text_content.empty())
            layer.rich_text.plain_text = layer.text_content;
    }

    /* No legacy rich text HTML path: the canonical model is plain_text +
     * manual ranges + automatic rules. This avoids QTextDocument HTML
     * round-tripping, stale spans, and mismatched Properties/Canvas state. */
    layer.rich_text_html.clear();
    layer.rich_text.normalize();
    layer.text_content = layer.rich_text.plain_text;
    rich_text_document_sync_layer_mirrors(layer);
}
#endif

static bool is_line_break_at(const std::string &text, size_t pos, size_t *advance = nullptr)
{
    if (pos >= text.size()) return false;
    if (text[pos] == '\r') {
        if (advance) *advance = (pos + 1 < text.size() && text[pos + 1] == '\n') ? 2 : 1;
        return true;
    }
    if (text[pos] == '\n') {
        if (advance) *advance = 1;
        return true;
    }
    return false;
}

static size_t utf8_codepoint_advance(const std::string &s, size_t pos)
{
    if (pos >= s.size()) return 0;
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return (pos + 1 < s.size()) ? 2 : 1;
    if ((c & 0xF0) == 0xE0) return (pos + 2 < s.size()) ? 3 : 1;
    if ((c & 0xF8) == 0xF0) return (pos + 3 < s.size()) ? 4 : 1;
    return 1;
}


static size_t utf8_byte_offset_for_codepoint_index(const std::string &text, size_t index)
{
    size_t pos = 0;
    size_t cp = 0;
    while (pos < text.size() && cp < index) {
        pos += std::max<size_t>(1, utf8_codepoint_advance(text, pos));
        ++cp;
    }
    return std::min(pos, text.size());
}

static bool custom_marker_matches_at(const std::string &text, size_t pos, const std::string &custom_chars)
{
    if (custom_chars.empty() || pos >= text.size()) return false;
    for (size_t cpos = 0; cpos < custom_chars.size();) {
        const size_t adv = std::max<size_t>(1, utf8_codepoint_advance(custom_chars, cpos));
        if (adv > 0 && cpos + adv <= custom_chars.size() &&
            pos + adv <= text.size() && text.compare(pos, adv, custom_chars, cpos, adv) == 0)
            return true;
        cpos += adv;
    }
    return false;
}

static size_t find_nth_marker(const std::string &text, const std::string &marker, size_t occurrence, const std::string &custom_chars);

static size_t marker_advance_at(const std::string &text, const std::string &marker, size_t pos, const std::string &custom_chars)
{
    if (pos > text.size()) return 0;
    if (marker == "text_start" || marker == "text_end" || marker == "character_index" ||
        marker == "paragraph_start" || marker == "paragraph_end")
        return 0;
    if (marker == "space")
        return (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ? 1 : 0;
    if (marker == "line_break" || marker == "newline") {
        size_t adv = 0;
        return is_line_break_at(text, pos, &adv) ? std::max<size_t>(1, adv) : 0;
    }
    if (marker == "custom_char") {
        if (custom_chars.empty() || pos >= text.size()) return 0;
        for (size_t cpos = 0; cpos < custom_chars.size();) {
            const size_t adv = std::max<size_t>(1, utf8_codepoint_advance(custom_chars, cpos));
            if (cpos + adv <= custom_chars.size() && pos + adv <= text.size() &&
                text.compare(pos, adv, custom_chars, cpos, adv) == 0)
                return adv;
            cpos += adv;
        }
    }
    return 0;
}

static std::vector<size_t> marker_positions(const std::string &text, const std::string &marker,
                                            const std::string &custom_chars)
{
    const size_t text_len = text.size();
    std::vector<size_t> positions;
    if (marker == "text_start") {
        positions.push_back(0);
        return positions;
    }
    if (marker == "text_end") {
        positions.push_back(text_len);
        return positions;
    }
    if (marker == "paragraph_start") {
        positions.push_back(0);
        for (size_t i = 0; i < text_len; ++i) {
            size_t adv = 0;
            if (is_line_break_at(text, i, &adv)) {
                positions.push_back(std::min(text_len, i + adv));
                i += adv ? adv - 1 : 0;
            }
        }
        return positions;
    }
    if (marker == "paragraph_end") {
        for (size_t i = 0; i < text_len; ++i) {
            size_t adv = 0;
            if (is_line_break_at(text, i, &adv)) {
                positions.push_back(i);
                i += adv ? adv - 1 : 0;
            }
        }
        positions.push_back(text_len);
        return positions;
    }
    if (marker == "character_index" || marker == "character_count" || marker == "word_count")
        return positions;

    for (size_t i = 0; i < text_len; ++i) {
        const size_t adv = marker_advance_at(text, marker, i, custom_chars);
        if (adv == 0)
            continue;
        positions.push_back(i);
        i += adv - 1;
    }
    return positions;
}

struct AutoStyleMarkerHit {
    size_t pos = 0;
    size_t advance = 0;
    bool found = false;
};

static bool is_word_byte(unsigned char ch)
{
    return std::isalnum(ch) || ch == '_' || ch >= 0x80;
}

static AutoStyleMarkerHit byte_offset_after_word_count(const std::string &text, size_t start_pos, size_t word_count)
{
    const size_t text_len = text.size();
    size_t pos = std::min(start_pos, text_len);
    size_t seen = 0;
    while (pos < text_len) {
        while (pos < text_len && !is_word_byte((unsigned char)text[pos]))
            ++pos;
        if (pos >= text_len)
            break;
        while (pos < text_len && is_word_byte((unsigned char)text[pos]))
            ++pos;
        ++seen;
        if (seen >= word_count)
            return {pos, 0, true};
    }
    return {text_len, 0, word_count == 0};
}

static AutoStyleMarkerHit byte_offset_after_character_count(const std::string &text, size_t start_pos, size_t count)
{
    const size_t text_len = text.size();
    size_t pos = std::min(start_pos, text_len);
    size_t seen = 0;
    while (pos < text_len && seen < count) {
        pos += std::max<size_t>(1, utf8_codepoint_advance(text, pos));
        ++seen;
    }
    return {std::min(pos, text_len), 0, seen >= count};
}

static size_t find_nth_marker(const std::string &text, const std::string &marker, size_t occurrence, const std::string &custom_chars)
{
    if (marker == "character_index" || marker == "character_count")
        return utf8_byte_offset_for_codepoint_index(text, occurrence);
    if (marker == "word_count")
        return byte_offset_after_word_count(text, 0, occurrence).pos;
    const auto positions = marker_positions(text, marker, custom_chars);
    if (positions.empty())
        return text.size();
    return positions[std::min(occurrence, positions.size() - 1)];
}

static AutoStyleMarkerHit find_next_marker_after(const std::string &text, const std::string &marker,
                                                 size_t start_pos, size_t occurrence_skip,
                                                 const std::string &custom_chars)
{
    const size_t text_len = text.size();
    if (marker == "text_end") return {text_len, 0, true};
    if (marker == "text_start") return {start_pos, 0, true};
    if (marker == "character_index")
        return {utf8_byte_offset_for_codepoint_index(text, occurrence_skip), 0, true};
    if (marker == "character_count")
        return byte_offset_after_character_count(text, start_pos, occurrence_skip);
    if (marker == "word_count")
        return byte_offset_after_word_count(text, start_pos, occurrence_skip);

    const auto positions = marker_positions(text, marker, custom_chars);
    size_t seen = 0;
    for (const size_t pos : positions) {
        if (pos <= start_pos)
            continue;
        if (seen++ == occurrence_skip)
            return {std::min(pos, text_len), marker_advance_at(text, marker, pos, custom_chars), true};
    }
    return {text_len, 0, false};
}

static std::vector<std::pair<size_t, size_t>> auto_style_rule_ranges(const RichTextAutoStyleRule &rule, const std::string &text)
{
    const size_t text_len = text.size();
    std::vector<std::pair<size_t, size_t>> ranges;

    /* Backwards compatibility for files/rules from the original start_to_char UI. */
    if (rule.condition_type == "start_to_char") {
        const size_t start = std::min(rule.start, text_len);
        const size_t end = std::min(start + rule.length, text_len);
        if (end > start) ranges.push_back({start, end});
        return ranges;
    }

    const std::string start_marker = rule.start_condition.empty() ? "text_start" : rule.start_condition;
    const std::string end_marker = rule.end_condition.empty() ? "text_end" : rule.end_condition;
    const bool start_is_custom = start_marker == "custom_char";
    const bool end_is_custom = end_marker == "custom_char";

    /* Absolute index ranges remain single ranges. They are explicit by design,
     * so strict stop matching is not needed for character_index end markers. */
    if (start_marker == "character_index" || end_marker == "character_index" ||
        start_marker == "character_count" || end_marker == "character_count" ||
        start_marker == "word_count" || end_marker == "word_count") {
        size_t start = find_nth_marker(text, start_marker, rule.start_offset, rule.start_custom_chars);
        size_t end = find_nth_marker(text, end_marker, rule.end_offset, rule.end_custom_chars);
        if (start_is_custom && !rule.include_start_marker)
            start = std::min(text_len, start + marker_advance_at(text, start_marker, start, rule.start_custom_chars));
        if (end_is_custom && rule.include_end_marker)
            end = std::min(text_len, end + marker_advance_at(text, end_marker, end, rule.end_custom_chars));
        if (end < start) std::swap(start, end);
        start = std::min(start, text_len);
        end = std::min(end, text_len);
        if (end > start) ranges.push_back({start, end});
        return ranges;
    }

    const auto starts = marker_positions(text, start_marker, rule.start_custom_chars);
    if (starts.empty())
        return ranges;

    size_t skipped_starts = 0;
    for (const size_t raw_start : starts) {
        if (skipped_starts++ < rule.start_offset)
            continue;
        size_t start = std::min(raw_start, text_len);
        if (start_is_custom && !rule.include_start_marker)
            start = std::min(text_len, start + marker_advance_at(text, start_marker, raw_start, rule.start_custom_chars));

        AutoStyleMarkerHit hit = find_next_marker_after(text, end_marker, start, rule.end_offset, rule.end_custom_chars);
        if (rule.require_stop_match && !hit.found)
            continue;
        size_t end = hit.pos;
        if (end_is_custom && rule.include_end_marker)
            end = std::min(text_len, end + hit.advance);
        end = std::min(end, text_len);
        if (end > start)
            ranges.push_back({start, end});
        if (start_marker == "text_start")
            break;
    }
    if (rule.match_mode == "first_match" && ranges.size() > 1)
        ranges.resize(1);
    return ranges;
}

static void merge_auto_format_bits(RichTextCharFormat &target, const RichTextCharFormat &source, uint32_t mask)
{
    if (mask & (1u << 0)) target.font_family = source.font_family;
    if (mask & (1u << 11)) target.font_style = source.font_style;
    if (mask & (1u << 1)) target.font_size = source.font_size;
    if (mask & (1u << 2)) target.bold = source.bold;
    if (mask & (1u << 3)) target.italic = source.italic;
    if (mask & (1u << 4)) target.underline = source.underline;
    if (mask & (1u << 5)) target.strikethrough = source.strikethrough;
    if (mask & (1u << 12)) { target.kerning = source.kerning; target.kerning_mode = source.kerning_mode; target.manual_kerning = source.manual_kerning; }
    if (mask & (1u << 6)) target.tracking = source.tracking;
    if (mask & (1u << 7)) target.scale_x = source.scale_x;
    if (mask & (1u << 8)) target.scale_y = source.scale_y;
    if (mask & (1u << 9)) target.baseline_shift = source.baseline_shift;
    if (mask & (1u << 10)) target.fill = source.fill;
    if (mask & (1u << 13)) target.text_style = source.text_style;
    if (mask & (1u << 14)) target.ligatures = source.ligatures;
    if (mask & (1u << 15)) target.stylistic_alternates = source.stylistic_alternates;
    if (mask & (1u << 16)) target.fractions = source.fractions;
    if (mask & (1u << 17)) target.opentype_features = source.opentype_features;
    if (mask & (1u << 18)) target.language = source.language;
}

RichTextDocument rich_text_document_with_auto_styles(const RichTextDocument &doc,
                                                    const RichTextAutoStyleResolver &resolver)
{
    RichTextDocument out = doc;
    if (!out.auto_style_enabled)
        return out;

    if (!out.auto_default_style_preset_id.empty()) {
        RichTextCharFormat fmt = out.auto_default_style_cached_format;
        uint32_t mask = out.auto_default_style_cached_mask;
        if (resolver) {
            RichTextCharFormat resolved;
            uint32_t resolved_mask = 0;
            if (resolver(out.auto_default_style_preset_id, resolved, resolved_mask)) {
                fmt = resolved;
                mask = resolved_mask;
            }
        }
        if (mask != 0)
            merge_auto_format_bits(out.default_format, fmt, mask);
    }

    const size_t text_len = out.plain_text.size();
    if (text_len == 0) {
        out.ranges.clear();
        return out;
    }

    std::vector<RichTextCharFormat> formats(text_len, out.default_format);
    std::vector<uint32_t> claimed_masks(text_len, 0);
    std::vector<bool> blocked_for_future_rules(text_len, false);

    auto merge_masked_range = [&](size_t start, size_t length, const RichTextCharFormat &format, uint32_t mask, const std::string &mode) {
        start = std::min(start, text_len);
        length = std::min(length, text_len - start);
        const size_t end = std::min(text_len, start + length);
        for (size_t i = start; i < end; ++i) {
            if (blocked_for_future_rules[i])
                continue;
            uint32_t effective_mask = mask;
            if (mode == "respect_previous")
                effective_mask &= ~claimed_masks[i];
            else if (mode == "apply_if_empty" && claimed_masks[i] != 0)
                effective_mask = 0;
            /* merge and override_previous both apply the selected preset fields;
             * override semantics happen naturally because later rules write last. */
            if (effective_mask == 0)
                continue;
            merge_auto_format_bits(formats[i], format, effective_mask);
            claimed_masks[i] |= effective_mask;
        }
    };

    auto mark_blocked = [&](size_t start, size_t length) {
        start = std::min(start, text_len);
        length = std::min(length, text_len - start);
        for (size_t i = start; i < start + length; ++i)
            blocked_for_future_rules[i] = true;
    };

    std::vector<std::vector<std::pair<size_t, size_t>>> resolved_rule_ranges(doc.auto_style_rules.size());
    for (size_t ri = 0; ri < doc.auto_style_rules.size(); ++ri)
        resolved_rule_ranges[ri] = auto_style_rule_ranges(doc.auto_style_rules[ri], out.plain_text);

    std::vector<std::vector<bool>> excluded_by_rule(doc.auto_style_rules.size(), std::vector<bool>(text_len, false));
    for (size_t ri = 0; ri < doc.auto_style_rules.size(); ++ri) {
        const auto &rule = doc.auto_style_rules[ri];
        if (!rule.enabled || rule.style_preset_id.empty())
            continue;
        RichTextCharFormat fmt = rule.cached_format;
        uint32_t mask = rule.cached_mask;
        if (resolver) {
            RichTextCharFormat resolved;
            uint32_t resolved_mask = 0;
            if (resolver(rule.style_preset_id, resolved, resolved_mask)) {
                fmt = resolved;
                mask = resolved_mask;
            }
        }
        if (mask == 0)
            continue;

        for (const auto &range : resolved_rule_ranges[ri]) {
            if (range.second <= range.first)
                continue;
            const size_t start = std::min(range.first, text_len);
            const size_t end = std::min(range.second, text_len);
            if (start >= end)
                continue;
            size_t seg_start = start;
            while (seg_start < end) {
                while (seg_start < end && excluded_by_rule[ri][seg_start])
                    ++seg_start;
                size_t seg_end = seg_start;
                while (seg_end < end && !excluded_by_rule[ri][seg_end])
                    ++seg_end;
                if (seg_end > seg_start)
                    merge_masked_range(seg_start, seg_end - seg_start, fmt, mask, rule.conflict_mode);
                seg_start = seg_end;
            }
        }

        if (rule.conflict_mode == "exclude_other_rules" && !rule.excludes_rule_ids.empty()) {
            for (size_t other = 0; other < doc.auto_style_rules.size(); ++other) {
                const std::string &other_id = doc.auto_style_rules[other].rule_id;
                const std::string other_index_id = std::to_string(other + 1);
                const bool should_exclude = std::find(rule.excludes_rule_ids.begin(), rule.excludes_rule_ids.end(), other_id) != rule.excludes_rule_ids.end() ||
                                            std::find(rule.excludes_rule_ids.begin(), rule.excludes_rule_ids.end(), other_index_id) != rule.excludes_rule_ids.end();
                if (!should_exclude)
                    continue;
                for (const auto &range : resolved_rule_ranges[ri]) {
                    const size_t start = std::min(range.first, text_len);
                    const size_t end = std::min(range.second, text_len);
                    for (size_t i = start; i < end; ++i)
                        excluded_by_rule[other][i] = true;
                }
            }
        }
        if (rule.stop_processing) {
            for (const auto &range : resolved_rule_ranges[ri])
                mark_blocked(range.first, range.second > range.first ? range.second - range.first : 0);
        }
    }

    /* Manual inline styles are intentionally applied last, so user-authored
     * character formatting overrides automatic rules. */
    for (const auto &range : doc.ranges) {
        if (range.length == 0 || range.start >= text_len)
            continue;
        const size_t end = std::min(text_len, range.start + range.length);
        for (size_t i = range.start; i < end; ++i)
            formats[i] = range.format;
    }

    out.ranges.clear();
    size_t start = 0;
    while (start < text_len) {
        size_t end = start + 1;
        while (end < text_len && same_format(formats[end], formats[start]))
            ++end;
        if (!same_format(formats[start], out.default_format))
            out.ranges.push_back({start, end - start, formats[start]});
        start = end;
    }
    return out;
}
