#pragma once

#include <cstdint>
#include <vector>

#include "animation.h"

/* ══════════════════════════════════════════════════════════════════
 *  Stackable layer effects
 * ══════════════════════════════════════════════════════════════════ */
enum class ShadowBlurType {
    Box = 0,
    Gaussian = 1,
    StackFast = 2,
    AlphaMask = 3,
    Triangular = 4,
    DualKawase = 5,
};

enum class LongShadowBlurType {
    None = 0,
    Box = 1,
    Gaussian = 2,
    StackFast = 3,
};

enum class LayerEffectType {
    BackgroundColor = 0,
    Outline = 1,
    DropShadow = 2,
    LongShadow = 3,
    BrightnessContrast = 4,
    Saturation = 5,
    ColorOverlay = 6,
    Glow = 7,
    InnerGlow = 8,
    InnerShadow = 9,
    Blur = 10,
    MotionBlur = 11,
};

enum class EffectBlendMode {
    Normal = 0,
    Multiply = 1,
    Additive = 2,
    Screen = 3,
    Overlay = 4,
    Color = 5,
};

struct LayerEffect {
    LayerEffectType type = LayerEffectType::BackgroundColor;
    bool enabled = true;

    /* OBS color-correction compatible values for stackable layer color effects. */
    float brightness = 0.0f;     /* -1.0 .. 1.0 additive RGB offset */
    float contrast = 1.0f;       /* 0.0 .. 4.0 multiplier around 0.5 */
    float saturation = 1.0f;     /* 0.0 .. 4.0 luma/chroma mix */
    uint32_t tint_color = 0xFFFFFFFF; /* Kept for backward-compatible Color Overlay JSON. */
    float tint_amount = 1.0f;    /* 0.0 .. 1.0 color overlay amount */
    uint32_t effect_color = 0xFFFFFFFF;
    float effect_opacity = 1.0f;
    float effect_size = 16.0f;
    float effect_distance = 8.0f;
    float effect_angle = 135.0f;
    float effect_spread = 0.0f;
    float effect_falloff = 1.0f;
    int effect_blur_type = (int)ShadowBlurType::StackFast;
    int effect_samples = 8;
    bool effect_centered = true;
    EffectBlendMode blend_mode = EffectBlendMode::Normal;

    /* Effect-owned style data. Legacy layer fields may still be read while
     * loading old projects, but active editor/render state belongs here.
     */
    int effect_fill_type = 0; /* background: 0=solid, 1=gradient; outline: 0=none, 1=color, 2=gradient */
    int effect_join_style = 1;
    bool effect_on_front = true;
    bool effect_antialias = true;
    bool effect_owned_style_loaded = false;
    uint32_t effect_stroke_color = 0x00000000;
    float effect_stroke_width = 0.0f;
    float effect_stroke_opacity = 1.0f;
    float effect_padding_left = 0.0f;
    float effect_padding_right = 0.0f;
    float effect_padding_top = 0.0f;
    float effect_padding_bottom = 0.0f;
    float effect_corner_radius_tl = 0.0f;
    float effect_corner_radius_tr = 0.0f;
    float effect_corner_radius_br = 0.0f;
    float effect_corner_radius_bl = 0.0f;
    int effect_corner_type = 0;
    int effect_gradient_type = 0;
    uint32_t effect_gradient_start_color = 0xFF4B6EA8;
    uint32_t effect_gradient_end_color = 0xFF1B1B1B;
    float effect_gradient_start_pos = 0.0f;
    float effect_gradient_end_pos = 1.0f;
    float effect_gradient_start_opacity = 1.0f;
    float effect_gradient_end_opacity = 1.0f;
    float effect_gradient_opacity = 1.0f;
    float effect_gradient_angle = 0.0f;
    float effect_gradient_center_x = 0.5f;
    float effect_gradient_center_y = 0.5f;
    float effect_gradient_scale = 1.0f;
    float effect_gradient_focal_x = 0.5f;
    float effect_gradient_focal_y = 0.5f;

    AnimatedProperty enabled_prop { "effect_enabled", 1.0 };
    AnimatedProperty opacity_prop { "effect_opacity", 1.0 };
    AnimatedProperty size_prop { "effect_size", 16.0 };
    AnimatedProperty distance_prop { "effect_distance", 8.0 };
    AnimatedProperty angle_prop { "effect_angle", 135.0 };
    AnimatedProperty spread_prop { "effect_spread", 0.0 };
    AnimatedProperty falloff_prop { "effect_falloff", 1.0 };
    AnimatedProperty stroke_width_prop { "effect_stroke_width", 0.0 };
    AnimatedProperty stroke_opacity_prop { "effect_stroke_opacity", 1.0 };
    AnimatedProperty padding_left_prop { "effect_padding_left", 0.0 };
    AnimatedProperty padding_right_prop { "effect_padding_right", 0.0 };
    AnimatedProperty padding_top_prop { "effect_padding_top", 0.0 };
    AnimatedProperty padding_bottom_prop { "effect_padding_bottom", 0.0 };
    AnimatedProperty corner_radius_tl_prop { "effect_corner_radius_tl", 0.0 };
    AnimatedProperty corner_radius_tr_prop { "effect_corner_radius_tr", 0.0 };
    AnimatedProperty corner_radius_br_prop { "effect_corner_radius_br", 0.0 };
    AnimatedProperty corner_radius_bl_prop { "effect_corner_radius_bl", 0.0 };
    AnimatedProperty color_a { "effect_color_a", 255.0 };
    AnimatedProperty color_r { "effect_color_r", 255.0 };
    AnimatedProperty color_g { "effect_color_g", 255.0 };
    AnimatedProperty color_b { "effect_color_b", 255.0 };
    AnimatedProperty stroke_color_a { "effect_stroke_color_a", 0.0 };
    AnimatedProperty stroke_color_r { "effect_stroke_color_r", 0.0 };
    AnimatedProperty stroke_color_g { "effect_stroke_color_g", 0.0 };
    AnimatedProperty stroke_color_b { "effect_stroke_color_b", 0.0 };
};
