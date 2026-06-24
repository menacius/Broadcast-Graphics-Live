#pragma once

#include "layer-model.h"

#include <QPainterPath>
#include <graphics/graphics.h>

#include <memory>
#include <vector>
#include <string>

namespace gsp::gpu_vector {

struct PrepareOptions {
    QPainterPath path;
    float logical_width = 0.0f;
    float logical_height = 0.0f;
    float raster_scale = 1.0f;
    bool fill_enabled = true;
    int fill_type = 0;
    uint32_t fill_color = 0xFFFFFFFFu;
    int fill_gradient_type = 0;
    int fill_gradient_spread = 0;
    uint32_t fill_gradient_start_color = 0xFFFFFFFFu;
    uint32_t fill_gradient_end_color = 0xFF000000u;
    float fill_gradient_start_pos = 0.0f;
    float fill_gradient_end_pos = 1.0f;
    float fill_gradient_start_opacity = 1.0f;
    float fill_gradient_end_opacity = 1.0f;
    float fill_gradient_opacity = 1.0f;
    float fill_gradient_angle = 0.0f;
    float fill_gradient_center_x = 0.5f;
    float fill_gradient_center_y = 0.5f;
    float fill_gradient_scale = 1.0f;
    float fill_gradient_focal_x = 0.5f;
    float fill_gradient_focal_y = 0.5f;
    std::vector<GradientStop> fill_gradient_stops;

    bool stroke_enabled = false;
    float stroke_width = 0.0f;
    float stroke_opacity = 1.0f;
    int stroke_alignment = 1;
    int stroke_join_style = 1;
    bool stroke_on_front = true;
    int stroke_fill_type = 1;
    uint32_t stroke_color = 0xFF000000u;
    int stroke_gradient_type = 0;
    int stroke_gradient_spread = 0;
    uint32_t stroke_gradient_start_color = 0xFFFFFFFFu;
    uint32_t stroke_gradient_end_color = 0xFF000000u;
    float stroke_gradient_start_pos = 0.0f;
    float stroke_gradient_end_pos = 1.0f;
    float stroke_gradient_start_opacity = 1.0f;
    float stroke_gradient_end_opacity = 1.0f;
    float stroke_gradient_opacity = 1.0f;
    float stroke_gradient_angle = 0.0f;
    float stroke_gradient_center_x = 0.5f;
    float stroke_gradient_center_y = 0.5f;
    float stroke_gradient_scale = 1.0f;
    float stroke_gradient_focal_x = 0.5f;
    float stroke_gradient_focal_y = 0.5f;
    std::vector<GradientStop> stroke_gradient_stops;
};

class Layer {
public:
    Layer();
    ~Layer();
    Layer(Layer &&) noexcept;
    Layer &operator=(Layer &&) noexcept;
    Layer(const Layer &) = delete;
    Layer &operator=(const Layer &) = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class Renderer;
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(Renderer &&) noexcept;
    Renderer &operator=(Renderer &&) noexcept;
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    bool prepare(Layer &layer, const PrepareOptions &options,
                 std::string *reason = nullptr);
    bool render(Layer &layer);
    void release_layer(Layer &layer);
    void reset();

    gs_texture_t *texture(const Layer &layer) const;
    uint32_t texture_width(const Layer &layer) const;
    uint32_t texture_height(const Layer &layer) const;
    bool owns_texture(const Layer &layer, const gs_texture_t *texture) const;
    bool backend_available() const;
    const char *last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gsp::gpu_vector
