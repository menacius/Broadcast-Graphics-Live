#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

struct RichTextFill {
    int type = 0; /* 0=solid, 1=gradient */
    uint32_t color = 0xFFFFFFFF;
    int gradient_type = 0;
    int gradient_spread = 0;
    uint32_t gradient_start_color = 0xFF4B6EA8;
    uint32_t gradient_end_color = 0xFF1B1B1B;
    float gradient_start_pos = 0.0f;
    float gradient_end_pos = 1.0f;
    float gradient_start_opacity = 1.0f;
    float gradient_end_opacity = 1.0f;
    float gradient_opacity = 1.0f;
    float gradient_angle = 0.0f;
    float gradient_center_x = 0.5f;
    float gradient_center_y = 0.5f;
    float gradient_scale = 1.0f;
    float gradient_focal_x = 0.5f;
    float gradient_focal_y = 0.5f;
};

struct RichTextCharFormat {
    std::string font_family = "Helvetica Neue";
    std::string font_style = "Regular";
    int font_size = 72;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    bool kerning = true;
    int kerning_mode = 0;
    float manual_kerning = 0.0f;
    float tracking = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float baseline_shift = 0.0f;
    int text_style = 0;
    bool ligatures = true;
    bool stylistic_alternates = false;
    bool fractions = false;
    bool opentype_features = false;
    std::string language = "English";
    RichTextFill fill;
};

struct RichTextParagraphFormat {
    int align_h = 1;
    int align_v = 1;
    float indent_left = 0.0f;
    float indent_right = 0.0f;
    float indent_first_line = 0.0f;
    float line_spacing = 0.0f;
    float space_before = 0.0f;
    float space_after = 0.0f;
    bool hyphenate = false;
};

struct RichTextBlock {
    size_t start = 0;
    size_t length = 0;
    RichTextParagraphFormat format;
};

struct RichTextRange {
    size_t start = 0;
    size_t length = 0;
    RichTextCharFormat format;
};


struct RichTextAutoStyleRule {
    bool enabled = true;
    std::string style_preset_id;
    std::string rule_id;
    std::string display_name;

    /* Rule engine options. These fields keep automatic styling deterministic:
     * rules produce candidate ranges, then the resolver decides how they merge.
     * conflict_mode: override_previous, respect_previous, apply_if_empty, merge, exclude_other_rules
     * match_mode: all_matches, first_match
     */
    std::string conflict_mode = "override_previous";
    std::string match_mode = "all_matches";
    bool stop_processing = false;
    std::vector<std::string> excludes_rule_ids;

    /* Automatic style range. Each rule has two independently configurable
     * boundaries: where the style starts and where it ends. The marker values
     * are string identifiers so the UI/data model can grow without breaking
     * older title files. Supported markers today:
     *   text_start, text_end, character_index, space, line_break, newline,
     *   paragraph_start, paragraph_end, custom_char, character_count, word_count
     */
    std::string start_condition = "text_start";
    std::string end_condition = "character_index";
    size_t start_offset = 0;
    size_t end_offset = 0;
    std::string start_custom_chars;
    std::string end_custom_chars;

    /* Stop matching safety. When true, marker-based ranges are discarded if
     * the configured To marker cannot be found after the From marker. This
     * prevents accidental styling to the end of a line/text box when a live
     * cue is missing a delimiter. Custom markers can optionally be included
     * in the styled range. */
    bool require_stop_match = true;
    bool include_start_marker = true;
    bool include_end_marker = false;

    /* Legacy fields kept for loading older files created by the first auto
     * styling implementation. New code should use the boundary fields above. */
    std::string condition_type = "range_markers";
    size_t start = 0;
    size_t length = 0;

    RichTextCharFormat cached_format;
    uint32_t cached_mask = 0;
};

struct RichTextSelection {
    size_t anchor = 0;
    size_t head = 0;
};

struct RichTextTransaction {
    std::string type;
    size_t position = 0;
    std::string removed_text;
    std::string inserted_text;
    RichTextSelection before_selection;
    RichTextSelection after_selection;
};

struct RichTextDocument {
    int version = 1;
    std::string plain_text;
    RichTextCharFormat default_format;
    RichTextParagraphFormat default_paragraph_format;
    /* Clean rich-text architecture: this is the only persistent insertion/cursor
     * format. Layer scalar fields are mirrors for legacy rendering/serialization
     * only; the properties panel must use this when the text cursor is collapsed. */
    bool has_typing_format = false;
    RichTextCharFormat typing_format;
    std::vector<RichTextBlock> blocks;
    std::vector<RichTextRange> ranges;

    bool auto_style_enabled = false;
    std::string auto_default_style_preset_id;
    RichTextCharFormat auto_default_style_cached_format;
    uint32_t auto_default_style_cached_mask = 0;
    std::vector<RichTextAutoStyleRule> auto_style_rules;

    RichTextSelection selection;
    std::vector<RichTextTransaction> transactions;

    void normalize();
    bool empty() const { return plain_text.empty() && ranges.empty() && blocks.empty(); }
};

RichTextDocument rich_text_document_from_layer_defaults(const struct Layer &layer);
void rich_text_document_sync_layer_defaults(RichTextDocument &doc, const struct Layer &layer);
void rich_text_document_sync_layer_mirrors(struct Layer &layer);
/* Ensure text layers use RichTextDocument as the single source of truth.
 * Legacy rich_text_html is intentionally discarded: manual styles, auto styles,
 * properties panel, inline editor, and renderer all read/write rich_text runs. */
void rich_text_document_ensure_canonical(struct Layer &layer);
void rich_text_document_replace_text(RichTextDocument &doc, const std::string &next_text,
                                     const RichTextCharFormat *insertion_format = nullptr);

using RichTextAutoStyleResolver = std::function<bool(const std::string &preset_id, RichTextCharFormat &format, uint32_t &mask)>;
RichTextDocument rich_text_document_with_auto_styles(const RichTextDocument &doc,
                                                    const RichTextAutoStyleResolver &resolver);
