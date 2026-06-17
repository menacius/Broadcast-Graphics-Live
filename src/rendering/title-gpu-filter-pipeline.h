#pragma once

#include "title-effect-registry.h"

#include <cstdint>
#include <memory>

struct Title;
struct gs_effect;
struct gs_texture;
typedef struct gs_effect gs_effect_t;
typedef struct gs_texture gs_texture_t;

struct TitleGpuEffectUsage {
    bool has_effects = false;
    bool background_color = false;
    bool outline = false;
    bool drop_shadow = false;
    bool long_shadow = false;
    bool brightness_contrast = false;
    bool saturation = false;
    bool color_overlay = false;
    bool glow = false;
    bool inner_glow = false;
    bool inner_shadow = false;
    bool blur = false;
    bool motion_blur = false;
    uint32_t enabled_effect_count = 0;
};

class TitleGpuFilterPipeline {
public:
    TitleGpuFilterPipeline() = default;
    ~TitleGpuFilterPipeline();

    TitleGpuFilterPipeline(const TitleGpuFilterPipeline &) = delete;
    TitleGpuFilterPipeline &operator=(const TitleGpuFilterPipeline &) = delete;

    bool render(gs_texture_t *texture, uint32_t width, uint32_t height,
                const TitleGpuEffectUsage &usage = {});
    void reset();
    const char *last_error() const { return last_error_; }

private:
    bool compile_requested_effects(const TitleGpuEffectUsage &usage);

    gs_effect_t *effect_ = nullptr;
    std::unique_ptr<TitleEffectRegistry> effect_registry_;
    const char *last_error_ = nullptr;
};

TitleGpuEffectUsage title_gpu_effect_usage(const Title &title);
