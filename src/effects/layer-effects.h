#pragma once

#include <cstdint>

/* ══════════════════════════════════════════════════════════════════
 *  Stackable layer effects
 * ══════════════════════════════════════════════════════════════════ */
enum class ShadowBlurType {
    Box = 0,
    Gaussian = 1,
    StackFast = 2,
    AlphaMask = 3,
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
    int effect_blur_type = (int)ShadowBlurType::StackFast;
    EffectBlendMode blend_mode = EffectBlendMode::Normal;
};
