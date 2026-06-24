#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>


enum RichTextCharFormatMask : uint32_t {
    RichTextCharFontFamily = 1u << 0,
    RichTextCharFontSize = 1u << 1,
    RichTextCharBold = 1u << 2,
    RichTextCharItalic = 1u << 3,
    RichTextCharUnderline = 1u << 4,
    RichTextCharStrikethrough = 1u << 5,
    RichTextCharTracking = 1u << 6,
    RichTextCharScaleX = 1u << 7,
    RichTextCharScaleY = 1u << 8,
    RichTextCharBaselineShift = 1u << 9,
    RichTextCharFillColor = 1u << 10,
    RichTextCharFontStyle = 1u << 11,
    RichTextCharKerning = 1u << 12,
    RichTextCharTextStyle = 1u << 13,
    RichTextCharLigatures = 1u << 14,
    RichTextCharStylisticAlternates = 1u << 15,
    RichTextCharFractions = 1u << 16,
    RichTextCharOpenTypeFeatures = 1u << 17,
    RichTextCharLanguage = 1u << 18,
    RichTextCharStroke = 1u << 19,
    RichTextCharAll = (1u << 20) - 1u,
};

enum RichTextParagraphFormatMask : uint32_t {
    RichTextParagraphAlignH = 1u << 0,
    RichTextParagraphAlignV = 1u << 1,
    RichTextParagraphIndentLeft = 1u << 2,
    RichTextParagraphIndentRight = 1u << 3,
    RichTextParagraphIndentFirstLine = 1u << 4,
    RichTextParagraphLineSpacing = 1u << 5,
    RichTextParagraphSpaceBefore = 1u << 6,
    RichTextParagraphSpaceAfter = 1u << 7,
    RichTextParagraphHyphenate = 1u << 8,
    RichTextParagraphAll = (1u << 9) - 1u,
};

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

struct RichTextStroke {
    bool enabled = false;
    float width = 0.0f;
    float opacity = 1.0f;
    bool on_front = false;
    int alignment = 0; /* 0=outer, 1=mid, 2=inner */
    bool antialias = true;
    int join_style = 0; /* 0=miter, 1=round, 2=bevel */
    RichTextFill fill;
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
    RichTextStroke stroke;
};

struct RichTextFontScaleMetrics {
    float vertical_factor = 1.0f;
    int horizontal_stretch_percent = 100;
};

/* QTextDocument/QFont do not expose an independent vertical glyph transform.
 * Until the GPU glyph renderer replaces the Qt adapter, preserve independent
 * H/V character scale by scaling the font size vertically and compensating
 * the horizontal font stretch by scale_x / scale_y. */
RichTextFontScaleMetrics rich_text_font_scale_metrics(float scale_x, float scale_y);

/* Frame-evaluated layer defaults shared by the editor, source renderer and
 * immutable layout stage. Sparse character and paragraph ranges remain
 * untouched and continue to inherit properties they do not override. */
struct RichTextEvaluatedDefaults {
    int font_size = 72;
    float tracking = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float baseline_shift = 0.0f;
    uint32_t solid_fill_color = 0xFFFFFFFF;
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
    /* Paragraph-level sparse override mask. Every paragraph has a block, but
     * mask==0 means it inherits the document paragraph defaults completely. */
    uint32_t mask = 0;
};

struct RichTextRange {
    size_t start = 0;
    size_t length = 0;
    RichTextCharFormat format;
    /* Sparse override mask. Version 1 files without a mask are migrated by
     * inferring which snapshot fields differ from the document default. */
    uint32_t mask = 0;
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
    int version = 2;
    std::string plain_text;
    RichTextCharFormat default_format;
    RichTextParagraphFormat default_paragraph_format;
    /* Clean rich-text architecture: this is the only persistent insertion/cursor
     * format. Layer scalar fields are mirrors for legacy rendering/serialization
     * only; the properties panel must use this when the text cursor is collapsed. */
    bool has_typing_format = false;
    RichTextCharFormat typing_format;
    uint32_t typing_format_mask = 0;
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
void rich_text_document_sync_layer_mirrors(struct Layer &layer);
/* Ensure text layers use RichTextDocument as the single source of truth.
 * Manual styles, auto styles,
 * properties panel, inline editor, and renderer all read/write rich_text runs. */
RichTextDocument rich_text_document_with_evaluated_defaults(
    RichTextDocument document, const RichTextEvaluatedDefaults &defaults);
void rich_text_document_ensure_canonical(struct Layer &layer);
void rich_text_document_replace_text(RichTextDocument &doc, const std::string &next_text);
void rich_text_document_replace_text(RichTextDocument &doc, const std::string &next_text,
                                     const RichTextCharFormat &insertion_format,
                                     uint32_t insertion_mask);

/* Canonical character-format helpers shared by editor, renderer and shaping. */
uint32_t rich_text_char_format_difference_mask(const RichTextCharFormat &a,
                                               const RichTextCharFormat &b,
                                               uint32_t mask = RichTextCharAll);
void rich_text_merge_char_format(RichTextCharFormat &target,
                                 const RichTextCharFormat &source, uint32_t mask);
RichTextCharFormat rich_text_format_at(const RichTextDocument &doc, size_t byte_offset);
uint32_t rich_text_format_mask_at(const RichTextDocument &doc, size_t byte_offset);
RichTextCharFormat rich_text_effective_typing_format(const RichTextDocument &doc);
void rich_text_document_apply_format(RichTextDocument &doc, size_t start, size_t length,
                                     const RichTextCharFormat &format, uint32_t mask);
uint32_t rich_text_paragraph_format_difference_mask(const RichTextParagraphFormat &a,
                                                    const RichTextParagraphFormat &b,
                                                    uint32_t mask = RichTextParagraphAll);
void rich_text_merge_paragraph_format(RichTextParagraphFormat &target,
                                      const RichTextParagraphFormat &source, uint32_t mask);
RichTextParagraphFormat rich_text_paragraph_format_at(const RichTextDocument &doc,
                                                      size_t paragraph_byte_offset);
uint32_t rich_text_paragraph_format_mask_at(const RichTextDocument &doc,
                                            size_t paragraph_byte_offset);
void rich_text_document_apply_paragraph_format(RichTextDocument &doc,
                                               size_t selection_start,
                                               size_t selection_end,
                                               const RichTextParagraphFormat &format,
                                               uint32_t mask);
void rich_text_document_clear_char_format_mask(RichTextDocument &doc, uint32_t mask);
void rich_text_document_clear_paragraph_format_mask(RichTextDocument &doc, uint32_t mask);

/* The canonical model uses UTF-8 byte offsets, always snapped to codepoint
 * boundaries. These helpers make adapters (Qt/HarfBuzz) explicit. */
bool rich_text_utf8_is_boundary(const std::string &text, size_t byte_offset);
size_t rich_text_utf8_previous_boundary(const std::string &text, size_t byte_offset);
size_t rich_text_utf8_next_boundary(const std::string &text, size_t byte_offset);

using RichTextAutoStyleResolver = std::function<bool(const std::string &preset_id, RichTextCharFormat &format, uint32_t &mask)>;
RichTextDocument rich_text_document_with_auto_styles(const RichTextDocument &doc,
                                                    const RichTextAutoStyleResolver &resolver);
