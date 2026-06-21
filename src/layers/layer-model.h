#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "layer-effects.h"
#include "animation.h"
#include "title-rich-text.h"

/* ══════════════════════════════════════════════════════════════════
 *  Layer type
 * ══════════════════════════════════════════════════════════════════ */
enum class LayerType {
    Text,
    SolidRect,
    Image,
    Shape,      /* vector primitives */
    Clock,
    Ticker,
};

enum class ShapeType {
    Rectangle,
    RoundedRectangle,
    Ellipse,
    Triangle,
    Star,
    Polygon,
    Diamond,
    Line,
    Path,      /* arbitrary cubic Bézier path created by the Pen tool */
};

enum class CornerType {
    Round,
    Straight,
    Concave,
    Cutout,
};

enum class MaskMode {
    None,
    Alpha,
    InvertedAlpha,
    Luma,
    InvertedLuma,
};

enum class ImageScaleFilter {
    Disable,
    Bilinear,
    Bicubic,
    Lanczos,
    Area,
};

enum class ImageBoxMode {
    FitImageToBox = 0,
    FillHorizontal = 1,
    FillVertical = 2,
    LegacyFitHorizontalCrop = 3,
    LegacyFitVerticalCrop = 4,
    StretchToFill = 5,
    FitToLongSide = 6,
    FitToShortSide = 7,
};


struct BezierPathPoint {
    /* Coordinates are normalized to the layer editable box. Handle positions
     * are absolute (not offsets) in the same normalized coordinate space. */
    double x = 0.0;
    double y = 0.0;
    double in_x = 0.0;
    double in_y = 0.0;
    double out_x = 0.0;
    double out_y = 0.0;
    bool has_in = false;
    bool has_out = false;
    bool smooth = false;
    /* True on the first anchor of every additional contour in a compound
     * path. The first point is always treated as a contour start for backward
     * compatibility, even when this flag is false. */
    bool starts_subpath = false;
    /* Illustrator-style live-corner radius in layer-local pixels. */
    double corner_radius = 0.0;
};

struct GradientStop {
    uint32_t color = 0xFFFFFFFF;
    float position = 0.5f;
    float opacity = 1.0f;
};

/* ══════════════════════════════════════════════════════════════════
 *  Layer
 * ══════════════════════════════════════════════════════════════════ */
struct Layer {
    std::string id;          /* UUID */
    std::string name;
    LayerType   type = LayerType::Text;
    bool        visible  = true;
    bool        locked   = false;
    bool        properties_expanded = false;
    std::string parent_id;
    std::string mask_source_id;
    MaskMode    mask_mode = MaskMode::None;
    EffectBlendMode blend_mode = EffectBlendMode::Normal; /* AE-style layer mode */
    bool        use_as_scene_mask = false;
    bool        effect_stack_respects_masks = false; /* When true, stackable effects are applied after the layer track matte/mask. */
    std::vector<LayerEffect> effects;

    /* Timeline in/out (seconds) within parent title clip */
    double      in_time  = 0.0;
    double      out_time = 5.0;

    /* ----- Animated properties ----- */
    AnimatedVec2Property position { "position", {0.0, 0.0} };
    AnimatedVec2Property scale    { "scale",    {1.0, 1.0} };
    bool             scale_lock = true;
    AnimatedProperty rotation{ "rotation", 0.0 };
    AnimatedProperty opacity { "opacity",  1.0 };

    /* ----- Text-specific ----- */
    std::string text_content  = "Title";
    /* Rich HTML captured by direct on-canvas editing. When non-empty it
     * stores mixed inline font/color/weight/italic/underline formatting
     * while text_content remains the plain-text fallback for live text and
     * older project files. */
    std::string rich_text_html;
    RichTextDocument rich_text; /* Structured source-of-truth rich text document. */
    std::string clock_format  = "H:i:s";  /* PHP date()-style format for clock layers */
    bool        expose_text    = false;
    bool        exposed_hide_if_empty = false;
    bool        exposed_single_value = false;
    bool        live_cue_hidden_if_empty = false; /* Runtime-only: hide exposed text/image layer for empty cue values. */
    bool        ignore_persistence = false; /* Let this layer continue animating during Background Persistence holds. Disabled for exposed dock text. */
    std::string font_family   = "Helvetica Neue";
    std::string font_style    = "Regular";
    int         font_size     = 72;
    AnimatedProperty font_size_prop { "font_size", 72.0 };
    bool        font_bold     = false;
    bool        font_italic   = false;
    bool        font_kerning  = true;
    int         kerning_mode  = 0;  /* 0=metrics, 1=optical, 2=manual */
    float       manual_kerning = 0.0f;
    float       text_leading  = 0.0f;
    float       char_tracking = 0.0f;
    AnimatedProperty char_tracking_prop { "char_tracking", 0.0 };
    float       char_scale_x  = 1.0f;
    AnimatedProperty char_scale_x_prop { "char_scale_x", 1.0 };
    float       char_scale_y  = 1.0f;
    AnimatedProperty char_scale_y_prop { "char_scale_y", 1.0 };
    float       baseline_shift = 0.0f;
    AnimatedProperty baseline_shift_prop { "baseline_shift", 0.0 };
    int         text_style    = 0;  /* 0=normal, 1=all caps, 2=small caps, 3=superscript, 4=subscript */
    bool        text_underline = false;
    bool        text_strikethrough = false;
    bool        text_ligatures = true;
    bool        text_stylistic_alternates = false;
    bool        text_fractions = false;
    bool        text_opentype_features = false;
    std::string text_language = "English";
    int         text_overflow_mode = 0;  /* 0=wrap, 1=clip, 2=horizontal fit */
    float       text_fit_min_scale = 0.5f;
    bool        text_box_width_to_text = false;
    bool        text_box_height_to_text = false;
    float       max_text_box_width = 1920.0f;
    float       max_text_box_height = 1080.0f;

    /* ----- Ticker-specific -----
     * style: 0=horizontal scrolling, 1=vertical line-by-line, 2=vertical smooth.
     * direction: horizontal 0=left-to-right, 1=right-to-left; vertical 0=top-to-bottom, 1=bottom-to-top.
     * speed is pixels/second. line_hold is seconds between line-by-line moves.
     */
    int         ticker_style = 0;
    double      ticker_speed = 120.0;
    double      ticker_line_hold = 2.0;
    int         ticker_direction = 1;

    uint32_t    text_color    = 0xFFFFFFFF;  /* ARGB */

    /* ----- Outline shared by text and solid/shape layers ----- */
    bool        outline_enabled = false;
    int         stroke_fill_type = 1;  /* 0=none, 1=color, 2=gradient */
    uint32_t    stroke_color  = 0xFF000000;
    float       stroke_width  = 0.0f;
    float       outline_opacity = 1.0f;
    int         outline_join_style = 1;  /* 0=miter, 1=round, 2=bevel */
    bool        outline_on_front = false;
    int         outline_alignment = 0;  /* 0=outer, 1=mid/centered, 2=inner */
    bool        outline_antialias = true;
    int         stroke_gradient_type = 0;  /* 0=linear, 1=radial, 2=conical */
    int         stroke_gradient_spread = 0; /* 0=no/pad, 1=reflect, 2=repeat */
    uint32_t    stroke_gradient_start_color = 0xFFFFFFFF;
    uint32_t    stroke_gradient_end_color   = 0xFF000000;
    float       stroke_gradient_start_pos = 0.0f;
    float       stroke_gradient_end_pos   = 1.0f;
    float       stroke_gradient_start_opacity = 1.0f;
    float       stroke_gradient_end_opacity   = 1.0f;
    float       stroke_gradient_opacity   = 1.0f;
    float       stroke_gradient_angle     = 0.0f;
    float       stroke_gradient_center_x  = 0.5f;
    float       stroke_gradient_center_y  = 0.5f;
    float       stroke_gradient_scale     = 1.0f;
    float       stroke_gradient_focal_x   = 0.5f;
    float       stroke_gradient_focal_y   = 0.5f;
    std::vector<GradientStop> stroke_gradient_stops; /* additional intermediate gradient stops between start/end */

    int         align_h       = 1;  /* 0=left 1=center 2=right 3=justify last left 4=justify last center 5=justify last right 6=justify all */
    int         align_v       = 1;  /* 0=top  1=middle 2=bottom */
    float       paragraph_indent_left = 0.0f;
    float       paragraph_indent_right = 0.0f;
    float       paragraph_indent_first_line = 0.0f;
    AnimatedProperty paragraph_indent_left_prop { "paragraph_indent_left", 0.0 };
    AnimatedProperty paragraph_indent_right_prop { "paragraph_indent_right", 0.0 };
    AnimatedProperty paragraph_indent_first_line_prop { "paragraph_indent_first_line", 0.0 };
    float       paragraph_space_before = 0.0f;
    AnimatedProperty paragraph_space_before_prop { "paragraph_space_before", 0.0 };
    float       paragraph_space_after = 0.0f;
    AnimatedProperty paragraph_space_after_prop { "paragraph_space_after", 0.0 };
    bool        paragraph_hyphenate = false;

    /* ----- Solid / shape ----- */
    uint32_t    fill_color    = 0xFF222222;
    int         fill_type     = 0;  /* 0=solid, 1=gradient */
    int         gradient_type = 0;  /* 0=linear, 1=radial, 2=conical */
    int         gradient_spread = 0; /* 0=no/pad, 1=reflect, 2=repeat */
    uint32_t    gradient_start_color = 0xFF4B6EA8;
    uint32_t    gradient_end_color   = 0xFF1B1B1B;
    float       gradient_start_pos = 0.0f;
    float       gradient_end_pos   = 1.0f;
    float       gradient_start_opacity = 1.0f;
    float       gradient_end_opacity   = 1.0f;
    float       gradient_opacity   = 1.0f;
    float       gradient_angle     = 0.0f;
    float       gradient_center_x  = 0.5f;
    float       gradient_center_y  = 0.5f;
    float       gradient_scale     = 1.0f;
    float       gradient_focal_x   = 0.5f;
    float       gradient_focal_y   = 0.5f;
    std::vector<GradientStop> gradient_stops; /* additional intermediate gradient stops between start/end */

    /* Optional box background for text/image layers. */
    bool        background_enabled = false;
    uint32_t    background_color = 0xFF000000;
    float       background_opacity = 0.35f;
    float       background_padding_x = 0.0f; /* legacy symmetric horizontal padding */
    float       background_padding_y = 0.0f; /* legacy symmetric vertical padding */
    float       background_padding_left = 0.0f;
    float       background_padding_right = 0.0f;
    float       background_padding_top = 0.0f;
    float       background_padding_bottom = 0.0f;
    float       background_corner_radius = 0.0f; /* legacy shared radius */
    float       background_corner_radius_tl = 0.0f;
    float       background_corner_radius_tr = 0.0f;
    float       background_corner_radius_br = 0.0f;
    float       background_corner_radius_bl = 0.0f;
    CornerType  background_corner_type = CornerType::Round;
    int         background_fill_type = 0;  /* 0=solid, 1=gradient */
    uint32_t    background_stroke_color = 0x00000000;
    float       background_stroke_width = 0.0f;
    float       background_stroke_opacity = 1.0f;
    int         background_stroke_fill_type = 0;  /* reserved: 0=solid */
    int         background_gradient_type = 0;  /* 0=linear, 1=radial, 2=conical */
    int         background_gradient_spread = 0; /* 0=no/pad, 1=reflect, 2=repeat */
    uint32_t    background_gradient_start_color = 0xFF4B6EA8;
    uint32_t    background_gradient_end_color   = 0xFF1B1B1B;
    float       background_gradient_start_pos = 0.0f;
    float       background_gradient_end_pos   = 1.0f;
    float       background_gradient_start_opacity = 1.0f;
    float       background_gradient_end_opacity   = 1.0f;
    float       background_gradient_opacity   = 1.0f;
    float       background_gradient_angle     = 0.0f;
    float       background_gradient_center_x  = 0.5f;
    float       background_gradient_center_y  = 0.5f;
    float       background_gradient_scale     = 1.0f;
    float       background_gradient_focal_x   = 0.5f;
    float       background_gradient_focal_y   = 0.5f;
    std::vector<GradientStop> background_gradient_stops; /* additional intermediate gradient stops between start/end */
    AnimatedProperty background_enabled_prop { "background_enabled", 0.0 };
    AnimatedProperty background_opacity_prop { "background_opacity", 0.35 };
    AnimatedProperty background_padding_x_prop { "background_padding_x", 0.0 };
    AnimatedProperty background_padding_y_prop { "background_padding_y", 0.0 };
    AnimatedProperty background_padding_left_prop { "background_padding_left", 0.0 };
    AnimatedProperty background_padding_right_prop { "background_padding_right", 0.0 };
    AnimatedProperty background_padding_top_prop { "background_padding_top", 0.0 };
    AnimatedProperty background_padding_bottom_prop { "background_padding_bottom", 0.0 };
    AnimatedProperty background_corner_radius_prop { "background_corner_radius", 0.0 };
    AnimatedProperty background_corner_radius_tl_prop { "background_corner_radius_tl", 0.0 };
    AnimatedProperty background_corner_radius_tr_prop { "background_corner_radius_tr", 0.0 };
    AnimatedProperty background_corner_radius_br_prop { "background_corner_radius_br", 0.0 };
    AnimatedProperty background_corner_radius_bl_prop { "background_corner_radius_bl", 0.0 };
    AnimatedProperty background_stroke_width_prop { "background_stroke_width", 0.0 };
    AnimatedProperty background_stroke_opacity_prop { "background_stroke_opacity", 1.0 };
    AnimatedProperty background_color_a { "background_color_a", 255.0 };
    AnimatedProperty background_color_r { "background_color_r", 0.0 };
    AnimatedProperty background_color_g { "background_color_g", 0.0 };
    AnimatedProperty background_color_b { "background_color_b", 0.0 };
    AnimatedProperty background_stroke_color_a { "background_stroke_color_a", 0.0 };
    AnimatedProperty background_stroke_color_r { "background_stroke_color_r", 0.0 };
    AnimatedProperty background_stroke_color_g { "background_stroke_color_g", 0.0 };
    AnimatedProperty background_stroke_color_b { "background_stroke_color_b", 0.0 };

    float       rect_width    = 1920.0f;
    float       rect_height   = 100.0f;
    float       corner_radius = 0.0f;
    float       corner_radius_tl = 0.0f;
    float       corner_radius_tr = 0.0f;
    float       corner_radius_br = 0.0f;
    float       corner_radius_bl = 0.0f;
    bool        corner_radius_locked = true;
    CornerType  corner_type = CornerType::Round;
    ShapeType   shape_type = ShapeType::Rectangle;
    std::vector<BezierPathPoint> path_points;
    bool        path_closed = true;
    int         shape_points = 5;
    int         shape_sides = 6;
    float       shape_inner_radius = 0.20f;
    float       shape_outer_radius = 0.5f;
    float       shape_roundness = 0.0f;       /* polygon / star outer corners */
    float       shape_inner_roundness = 0.0f; /* star inner corners */
    bool        scale_stroke_with_shape = false;
    bool        scale_corners_with_shape = false;

    /* Keyframable geometry mirrors the static fields above so older saved
     * titles remain readable while new titles can animate size/origin.
     */
    AnimatedVec2Property size { "size", {1920.0, 100.0} };

    /* ----- Geometry anchor / origin -----
     * Normalized inside the editable bounding box: 0.0 = left/top,
     * 0.5 = center, 1.0 = right/bottom. The layer position is this origin.
     */
    float       origin_x      = 0.5f;
    float       origin_y      = 0.5f;
    AnimatedVec2Property origin_prop { "origin", {0.5, 0.5} };

    /* ----- Drop shadow ----- */
    bool        shadow_enabled = false;
    uint32_t    shadow_color   = 0x99000000;
    float       shadow_opacity = 0.6f;
    float       shadow_distance = 8.0f;
    float       shadow_angle = 135.0f;
    float       shadow_blur = 4.0f;
    float       shadow_spread = 0.0f;
    ShadowBlurType shadow_blur_type = ShadowBlurType::StackFast;
    bool        long_shadow_enabled = false;
    uint32_t    long_shadow_color = 0x99000000;
    float       long_shadow_opacity = 0.45f;
    float       long_shadow_length = 0.0f;
    float       long_shadow_angle = 135.0f;
    float       long_shadow_falloff = 1.0f;
    LongShadowBlurType long_shadow_blur_type = LongShadowBlurType::None;
    float       long_shadow_blur = 8.0f;
    AnimatedProperty shadow_enabled_prop { "shadow_enabled", 0.0 };
    AnimatedProperty shadow_opacity_prop { "shadow_opacity", 0.6 };
    AnimatedProperty shadow_distance_prop { "shadow_distance", 8.0 };
    AnimatedProperty shadow_angle_prop { "shadow_angle", 135.0 };
    AnimatedProperty shadow_blur_prop { "shadow_blur", 4.0 };
    AnimatedProperty shadow_spread_prop { "shadow_spread", 0.0 };
    AnimatedProperty shadow_color_a { "shadow_color_a", 153.0 };
    AnimatedProperty shadow_color_r { "shadow_color_r", 0.0 };
    AnimatedProperty shadow_color_g { "shadow_color_g", 0.0 };
    AnimatedProperty shadow_color_b { "shadow_color_b", 0.0 };

    /* ----- Keyframable color channels, 0-255 ARGB. */
    AnimatedProperty text_color_a { "text_color_a", 255.0 };
    AnimatedProperty text_color_r { "text_color_r", 255.0 };
    AnimatedProperty text_color_g { "text_color_g", 255.0 };
    AnimatedProperty text_color_b { "text_color_b", 255.0 };
    AnimatedProperty fill_color_a { "fill_color_a", 255.0 };
    AnimatedProperty fill_color_r { "fill_color_r",  34.0 };
    AnimatedProperty fill_color_g { "fill_color_g",  34.0 };
    AnimatedProperty fill_color_b { "fill_color_b",  34.0 };

    /* ----- Image ----- */
    std::string image_path;
    bool        lock_aspect_ratio = true;
    bool        image_box_lock_aspect_ratio = false;
    ImageScaleFilter scale_filter = ImageScaleFilter::Bilinear;
    ImageBoxMode image_box_mode = ImageBoxMode::FitImageToBox;
    bool        image_size_auto_fit = true;
    bool        image_crop_when_outside_box = false;
    float       image_anchor_x = 0.5f;
    float       image_anchor_y = 0.5f;
    float       image_width = 1920.0f;
    float       image_height = 1080.0f;
    AnimatedVec2Property image_size { "image_size", {1920.0, 1080.0} };
};
