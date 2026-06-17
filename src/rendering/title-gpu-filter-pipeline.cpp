#include "title-gpu-filter-pipeline.h"

#include "title-data.h"
#include "title-effect-registry.h"

#include <graphics/graphics.h>
#include <obs-module.h>
#include <util/bmem.h>

#include <algorithm>

namespace {

static constexpr const char *kGpuFilterPipelineEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float enabledEffectCount;
uniform bool hasBackgroundColor;
uniform bool hasOutline;
uniform bool hasDropShadow;
uniform bool hasLongShadow;
uniform bool hasBrightnessContrast;
uniform bool hasSaturation;
uniform bool hasColorOverlay;
uniform bool hasGlow;
uniform bool hasInnerGlow;
uniform bool hasInnerShadow;
uniform bool hasBlur;
uniform bool hasMotionBlur;

sampler_state textureSampler {
    Filter   = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

struct VertDataIn {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct VertDataOut {
    float4 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

VertDataOut VSDefault(VertDataIn v_in)
{
    VertDataOut vert_out;
    vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
    vert_out.uv = v_in.uv;
    return vert_out;
}

float4 PSFilter(VertDataOut v_in) : TARGET
{
    return image.Sample(textureSampler, v_in.uv);
}

technique Draw
{
    pass
    {
        vertex_shader = VSDefault(v_in);
        pixel_shader = PSFilter(v_in);
    }
}
)";

static gs_effect_t *create_composite_filter_effect(const char **last_error)
{
    char *path = obs_module_file("effects/composite-filter/composite-filter.effect");
    if (path) {
        char *errors = nullptr;
        gs_effect_t *effect = gs_effect_create_from_file(path, &errors);
        if (effect) {
            if (errors)
                bfree(errors);
            bfree(path);
            return effect;
        }
        if (errors)
            bfree(errors);
        bfree(path);
    }

    gs_effect_t *fallback = gs_effect_create(
        kGpuFilterPipelineEffect, "obs-gsp-gpu-filter-pipeline.effect", nullptr);
    if (!fallback && last_error)
        *last_error = "GPU composite shader could not be compiled from file or fallback source.";
    return fallback;
}

} // namespace

TitleGpuFilterPipeline::~TitleGpuFilterPipeline()
{
    reset();
}

void TitleGpuFilterPipeline::reset()
{
    if (effect_) {
        gs_effect_destroy(effect_);
        effect_ = nullptr;
    }
    if (effect_registry_)
        effect_registry_->reset();
}

bool TitleGpuFilterPipeline::compile_requested_effects(const TitleGpuEffectUsage &usage)
{
    if (!usage.has_effects)
        return true;
    if (!effect_registry_)
        effect_registry_ = std::make_unique<TitleEffectRegistry>();

    auto compile_if = [this](bool enabled, LayerEffectType type) {
        if (!enabled)
            return true;
        if (effect_registry_->compile(type))
            return true;
        last_error_ = effect_registry_->last_error();
        return false;
    };

    return compile_if(usage.background_color, LayerEffectType::BackgroundColor) &&
           compile_if(usage.outline, LayerEffectType::Outline) &&
           compile_if(usage.drop_shadow, LayerEffectType::DropShadow) &&
           compile_if(usage.long_shadow, LayerEffectType::LongShadow) &&
           compile_if(usage.brightness_contrast, LayerEffectType::BrightnessContrast) &&
           compile_if(usage.saturation, LayerEffectType::Saturation) &&
           compile_if(usage.color_overlay, LayerEffectType::ColorOverlay) &&
           compile_if(usage.glow, LayerEffectType::Glow) &&
           compile_if(usage.inner_glow, LayerEffectType::InnerGlow) &&
           compile_if(usage.inner_shadow, LayerEffectType::InnerShadow) &&
           compile_if(usage.blur, LayerEffectType::Blur) &&
           compile_if(usage.motion_blur, LayerEffectType::MotionBlur);
}

bool TitleGpuFilterPipeline::render(gs_texture_t *texture, uint32_t width, uint32_t height,
                                    const TitleGpuEffectUsage &usage)
{
    last_error_ = nullptr;
    if (!texture) {
        last_error_ = "GPU effects need a valid OBS texture.";
        return false;
    }

    if (!compile_requested_effects(usage))
        return false;

    if (!effect_)
        effect_ = create_composite_filter_effect(&last_error_);
    if (!effect_) {
        if (!last_error_)
            last_error_ = "GPU effect shader could not be created.";
        return false;
    }

    gs_eparam_t *image = gs_effect_get_param_by_name(effect_, "image");
    if (!image) {
        last_error_ = "GPU effect shader is missing the image parameter.";
        return false;
    }

    gs_effect_set_texture(image, texture);

    auto set_bool = [this](const char *name, bool value) {
        if (gs_eparam_t *param = gs_effect_get_param_by_name(effect_, name))
            gs_effect_set_bool(param, value);
    };
    auto set_float = [this](const char *name, float value) {
        if (gs_eparam_t *param = gs_effect_get_param_by_name(effect_, name))
            gs_effect_set_float(param, value);
    };

    set_float("enabledEffectCount", static_cast<float>(usage.enabled_effect_count));
    set_bool("hasBackgroundColor", usage.background_color);
    set_bool("hasOutline", usage.outline);
    set_bool("hasDropShadow", usage.drop_shadow);
    set_bool("hasLongShadow", usage.long_shadow);
    set_bool("hasBrightnessContrast", usage.brightness_contrast);
    set_bool("hasSaturation", usage.saturation);
    set_bool("hasColorOverlay", usage.color_overlay);
    set_bool("hasGlow", usage.glow);
    set_bool("hasInnerGlow", usage.inner_glow);
    set_bool("hasInnerShadow", usage.inner_shadow);
    set_bool("hasBlur", usage.blur);
    set_bool("hasMotionBlur", usage.motion_blur);

    while (gs_effect_loop(effect_, "Draw"))
        gs_draw_sprite(texture, 0, width, height);
    return true;
}

TitleGpuEffectUsage title_gpu_effect_usage(const Title &title)
{
    TitleGpuEffectUsage usage;
    for (const auto &layer : title.layers) {
        if (!layer)
            continue;

        const auto mark = [&usage](LayerEffectType type) {
            usage.has_effects = true;
            ++usage.enabled_effect_count;
            switch (type) {
            case LayerEffectType::BackgroundColor:
                usage.background_color = true;
                break;
            case LayerEffectType::Outline:
                usage.outline = true;
                break;
            case LayerEffectType::DropShadow:
                usage.drop_shadow = true;
                break;
            case LayerEffectType::LongShadow:
                usage.long_shadow = true;
                break;
            case LayerEffectType::BrightnessContrast:
                usage.brightness_contrast = true;
                break;
            case LayerEffectType::Saturation:
                usage.saturation = true;
                break;
            case LayerEffectType::ColorOverlay:
                usage.color_overlay = true;
                break;
            case LayerEffectType::Glow:
                usage.glow = true;
                break;
            case LayerEffectType::InnerGlow:
                usage.inner_glow = true;
                break;
            case LayerEffectType::InnerShadow:
                usage.inner_shadow = true;
                break;
            case LayerEffectType::Blur:
                usage.blur = true;
                break;
            case LayerEffectType::MotionBlur:
                usage.motion_blur = true;
                break;
            }
        };

        if (layer->effects.empty()) {
            if (layer->background_enabled)
                mark(LayerEffectType::BackgroundColor);
            if (layer->outline_enabled)
                mark(LayerEffectType::Outline);
            if (layer->shadow_enabled)
                mark(LayerEffectType::DropShadow);
            if (layer->long_shadow_enabled)
                mark(LayerEffectType::LongShadow);
            continue;
        }

        for (const auto &effect : layer->effects) {
            if (effect.enabled)
                mark(effect.type);
        }
    }
    return usage;
}
