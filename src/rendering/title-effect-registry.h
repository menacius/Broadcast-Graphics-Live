#pragma once

#include "layer-effects.h"

#include <vector>
#include <string>

struct gs_effect;
typedef struct gs_effect gs_effect_t;

struct TitleEffectDefinition {
    LayerEffectType type;
    const char *stable_id;
    const char *legacy_id;
    const char *display_name;
    const char *category;
    const char *relative_path;
    bool has_embedded_fallback = false;
};

class TitleEffectRegistry {
public:
    TitleEffectRegistry() = default;
    ~TitleEffectRegistry();

    TitleEffectRegistry(const TitleEffectRegistry &) = delete;
    TitleEffectRegistry &operator=(const TitleEffectRegistry &) = delete;

    gs_effect_t *compile(LayerEffectType type);
    gs_effect_t *compile(const std::string &stable_id);
    void reset();
    const char *last_error() const { return last_error_; }

    static const std::vector<TitleEffectDefinition> &definitions();
    static const TitleEffectDefinition *definition(LayerEffectType type);

private:
    struct CompiledEffect {
        LayerEffectType type;
        std::string stable_id;
        gs_effect_t *effect = nullptr;
    };

    std::vector<CompiledEffect> compiled_;
    const char *last_error_ = nullptr;
};
