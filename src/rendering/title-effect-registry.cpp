#include "title-effect-registry.h"

#include "title-logger.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <QString>

#include <algorithm>

namespace {

const std::vector<TitleEffectDefinition> kEffectDefinitions = {
    {LayerEffectType::BackgroundColor, "background-color", "Background Color", "effect-transitions/shaders/background-color/background-color.effect"},
    {LayerEffectType::Outline, "outline", "Outline", "effect-transitions/shaders/outline/outline.effect"},
    {LayerEffectType::DropShadow, "drop-shadow", "Drop Shadow", "effect-transitions/shaders/drop-shadow/drop-shadow.effect"},
    {LayerEffectType::LongShadow, "long-shadow", "Long Shadow", "effect-transitions/shaders/long-shadow/long-shadow.effect"},
    {LayerEffectType::BrightnessContrast, "brightness-contrast", "Brightness/Contrast", "effect-transitions/shaders/brightness-contrast/brightness-contrast.effect"},
    {LayerEffectType::Saturation, "saturation", "Saturation", "effect-transitions/shaders/saturation/saturation.effect"},
    {LayerEffectType::ColorOverlay, "color-overlay", "Color Overlay", "effect-transitions/shaders/color-overlay/color-overlay.effect"},
    {LayerEffectType::Glow, "glow", "Glow", "effect-transitions/shaders/glow/glow.effect"},
    {LayerEffectType::InnerGlow, "inner-glow", "Inner Glow", "effect-transitions/shaders/inner-glow/inner-glow.effect"},
    {LayerEffectType::InnerShadow, "inner-shadow", "Inner Shadow", "effect-transitions/shaders/inner-shadow/inner-shadow.effect"},
    {LayerEffectType::Blur, "blur", "Blur", "effect-transitions/shaders/blur/blur.effect"},
    {LayerEffectType::MotionBlur, "motion-blur", "Motion Blur", "effect-transitions/shaders/motion-blur/motion-blur.effect"},
    {LayerEffectType::Bloom, "bloom", "Bloom", "effect-transitions/shaders/bloom/bloom.effect"},
    {LayerEffectType::Emboss, "emboss", "Emboss", "effect-transitions/shaders/emboss/emboss.effect"},
};

} // namespace

TitleEffectRegistry::~TitleEffectRegistry()
{
    reset();
}

void TitleEffectRegistry::reset()
{
    for (CompiledEffect &compiled : compiled_) {
        if (compiled.effect)
            gs_effect_destroy(compiled.effect);
    }
    compiled_.clear();
}

gs_effect_t *TitleEffectRegistry::compile(LayerEffectType type)
{
    last_error_ = nullptr;
    auto existing = std::find_if(compiled_.begin(), compiled_.end(),
                                 [type](const CompiledEffect &compiled) {
                                     return compiled.type == type;
                                 });
    if (existing != compiled_.end())
        return existing->effect;

    const TitleEffectDefinition *def = definition(type);
    if (!def) {
        last_error_ = "Unknown effect type.";
        return nullptr;
    }

    char *path = obs_module_file(def->relative_path);
    if (!path) {
        last_error_ = "Effect asset path could not be resolved.";
        return nullptr;
    }

    char *errors = nullptr;
    gs_effect_t *effect = gs_effect_create_from_file(path, &errors);
    if (!effect) {
        OGS_LOG_WARNING("Effects", QStringLiteral("Failed to compile effect %1 from %2: %3")
                                       .arg(QString::fromUtf8(def->id),
                                            QString::fromUtf8(path),
                                            QString::fromUtf8(errors ? errors : "unknown shader error")));
        last_error_ = "Effect shader could not be compiled.";
        if (errors)
            bfree(errors);
        bfree(path);
        return nullptr;
    }

    if (errors)
        bfree(errors);
    OGS_LOG_DEBUG("Effects", QStringLiteral("Compiled effect %1 from %2")
                                 .arg(QString::fromUtf8(def->id), QString::fromUtf8(path)));
    bfree(path);

    compiled_.push_back({type, effect});
    return effect;
}

const std::vector<TitleEffectDefinition> &TitleEffectRegistry::definitions()
{
    return kEffectDefinitions;
}

const TitleEffectDefinition *TitleEffectRegistry::definition(LayerEffectType type)
{
    auto it = std::find_if(kEffectDefinitions.begin(), kEffectDefinitions.end(),
                           [type](const TitleEffectDefinition &def) {
                               return def.type == type;
                           });
    return it == kEffectDefinitions.end() ? nullptr : &*it;
}
