#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct RichTextFill {
    int type = 0; /* 0=solid, 1=gradient */
    uint32_t color = 0xFFFFFFFF;
    int gradient_type = 0;
    uint32_t gradient_start_color = 0xFF4B6EA8;
    uint32_t gradient_end_color = 0xFF1B1B1B;
    float gradient_start_pos = 0.0f;
    float gradient_end_pos = 1.0f;
    float gradient_angle = 0.0f;
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
    std::string plain_text = "Title";
    RichTextCharFormat default_format;
    RichTextParagraphFormat default_paragraph_format;
    std::vector<RichTextBlock> blocks;
    std::vector<RichTextRange> ranges;
    RichTextSelection selection;
    std::vector<RichTextTransaction> transactions;

    void normalize();
    bool empty() const { return plain_text.empty() && ranges.empty() && blocks.empty(); }
};

RichTextDocument rich_text_document_from_layer_defaults(const struct Layer &layer);
void rich_text_document_sync_layer_defaults(RichTextDocument &doc, const struct Layer &layer);
void rich_text_document_replace_text(RichTextDocument &doc, const std::string &next_text,
                                     const RichTextCharFormat *insertion_format = nullptr);
