#include "title-gpu-vector-renderer.h"

#include <QPainterPathStroker>
#include <QLineF>
#include <QPolygonF>
#include <QTransform>

#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace gsp::gpu_vector {
namespace {

constexpr int kMaxGradientStops = 10;
constexpr float kEpsilon = 0.00001f;

static constexpr const char *kVectorEffect = R"(
uniform float4x4 ViewProj;
uniform int materialType;
uniform int gradientType;
uniform int gradientSpread;
uniform float4 solidColor;
uniform float4 stopColor0; uniform float stopPos0;
uniform float4 stopColor1; uniform float stopPos1;
uniform float4 stopColor2; uniform float stopPos2;
uniform float4 stopColor3; uniform float stopPos3;
uniform float4 stopColor4; uniform float stopPos4;
uniform float4 stopColor5; uniform float stopPos5;
uniform float4 stopColor6; uniform float stopPos6;
uniform float4 stopColor7; uniform float stopPos7;
uniform float4 stopColor8; uniform float stopPos8;
uniform float4 stopColor9; uniform float stopPos9;
uniform int stopCount;
uniform float angleDegrees;
uniform float2 gradientCenter;
uniform float2 gradientFocal;
uniform float gradientScale;
uniform float2 materialSize;
struct VertDataIn { float4 pos : POSITION; float2 localPos : TEXCOORD0; };
struct VertDataOut { float4 pos : POSITION; float2 localPos : TEXCOORD0; };
VertDataOut VSDefault(VertDataIn v) { VertDataOut o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.localPos = v.localPos; return o; }
float spreadValue(float value, int mode) {
    if (mode == 1) { float repeated = value - floor(value * 0.5) * 2.0; return repeated <= 1.0 ? repeated : 2.0 - repeated; }
    if (mode == 2) return value - floor(value);
    return clamp(value, 0.0, 1.0);
}
float gradientPosition(float2 point) {
    float2 size = max(materialSize, float2(1.0, 1.0));
    float2 center = gradientCenter * size;
    float2 focal = gradientFocal * size;
    float scaleValue = clamp(gradientScale, 0.01, 100.0);
    float angle = angleDegrees * 0.017453292519943295;
    float value = 0.0;
    if (gradientType == 1) {
        float radius = max(size.x, size.y) * 0.5 * scaleValue;
        float focalDistance = length(center - focal);
        value = length(point - focal) / max(1.0, radius - min(focalDistance, radius * 0.95));
    } else if (gradientType == 2) {
        float a = atan2(point.y - center.y, point.x - center.x);
        value = (-a - angle) / 6.283185307179586 + 0.5;
    } else {
        float lengthValue = max(1.0, length(size) * 0.5 * scaleValue);
        float2 direction = float2(cos(angle), sin(angle));
        value = dot(point - (center - direction * lengthValue), direction) / (lengthValue * 2.0);
    }
    return spreadValue(value, gradientSpread);
}
float4 stopColor(int index) {
    if (index <= 0) return stopColor0; if (index == 1) return stopColor1;
    if (index == 2) return stopColor2; if (index == 3) return stopColor3;
    if (index == 4) return stopColor4; if (index == 5) return stopColor5;
    if (index == 6) return stopColor6; if (index == 7) return stopColor7;
    if (index == 8) return stopColor8; return stopColor9;
}
float stopPosition(int index) {
    if (index <= 0) return stopPos0; if (index == 1) return stopPos1;
    if (index == 2) return stopPos2; if (index == 3) return stopPos3;
    if (index == 4) return stopPos4; if (index == 5) return stopPos5;
    if (index == 6) return stopPos6; if (index == 7) return stopPos7;
    if (index == 8) return stopPos8; return stopPos9;
}
float4 sampleGradient(float value) {
    int count = max(2, stopCount);
    float4 previousColor = stopColor(0); float previousPos = stopPosition(0);
    for (int index = 1; index < 10; ++index) {
        if (index >= count) break;
        float currentPos = stopPosition(index); float4 currentColor = stopColor(index);
        if (value <= currentPos || index == count - 1) {
            float amount = clamp((value - previousPos) / max(abs(currentPos - previousPos), 0.000001), 0.0, 1.0);
            return lerp(previousColor, currentColor, amount);
        }
        previousColor = currentColor; previousPos = currentPos;
    }
    return previousColor;
}
float4 PSVector(VertDataOut v) : TARGET {
    float4 color = materialType == 0 ? solidColor : sampleGradient(gradientPosition(v.localPos));
    float alpha = clamp(color.a, 0.0, 1.0);
    return float4(color.rgb * alpha, alpha);
}
technique Draw { pass { vertex_shader = VSDefault(v); pixel_shader = PSVector(v); } }
)";

struct Vertex { float x = 0.0f; float y = 0.0f; };

struct Material {
    int type = 0;
    uint32_t solid = 0xFFFFFFFFu;
    int gradient_type = 0;
    int spread = 0;
    float angle = 0.0f;
    float center_x = 0.5f;
    float center_y = 0.5f;
    float focal_x = 0.5f;
    float focal_y = 0.5f;
    float scale = 1.0f;
    std::vector<GradientStop> stops;
};

struct Batch {
    Material material;
    std::vector<Vertex> vertices;
};

static float signed_area(const std::vector<QPointF> &points)
{
    double area = 0.0;
    for (size_t i = 0; i < points.size(); ++i) {
        const QPointF &a = points[i];
        const QPointF &b = points[(i + 1) % points.size()];
        area += a.x() * b.y() - b.x() * a.y();
    }
    return static_cast<float>(area * 0.5);
}

static float cross(const QPointF &a, const QPointF &b, const QPointF &c)
{
    return static_cast<float>((b.x() - a.x()) * (c.y() - a.y()) -
                              (b.y() - a.y()) * (c.x() - a.x()));
}

static bool point_in_triangle(const QPointF &p, const QPointF &a,
                              const QPointF &b, const QPointF &c)
{
    const float c1 = cross(a, b, p);
    const float c2 = cross(b, c, p);
    const float c3 = cross(c, a, p);
    const bool negative = c1 < -kEpsilon || c2 < -kEpsilon || c3 < -kEpsilon;
    const bool positive = c1 > kEpsilon || c2 > kEpsilon || c3 > kEpsilon;
    return !(negative && positive);
}

static void append_polygon_triangles(const QPolygonF &polygon,
                                     std::vector<Vertex> &vertices)
{
    std::vector<QPointF> points;
    points.reserve(static_cast<size_t>(polygon.size()));
    for (const QPointF &point : polygon) {
        if (!points.empty() && QLineF(points.back(), point).length() < 0.0001)
            continue;
        points.push_back(point);
    }
    if (points.size() > 2 && QLineF(points.front(), points.back()).length() < 0.0001)
        points.pop_back();
    if (points.size() < 3)
        return;
    if (signed_area(points) < 0.0f)
        std::reverse(points.begin(), points.end());

    std::vector<size_t> indices(points.size());
    for (size_t i = 0; i < indices.size(); ++i)
        indices[i] = i;
    size_t guard = 0;
    while (indices.size() > 2 && guard++ < points.size() * points.size() * 2) {
        bool clipped = false;
        for (size_t i = 0; i < indices.size(); ++i) {
            const size_t previous = indices[(i + indices.size() - 1) % indices.size()];
            const size_t current = indices[i];
            const size_t next = indices[(i + 1) % indices.size()];
            if (cross(points[previous], points[current], points[next]) <= kEpsilon)
                continue;
            bool contains = false;
            for (size_t candidate : indices) {
                if (candidate == previous || candidate == current || candidate == next)
                    continue;
                if (point_in_triangle(points[candidate], points[previous],
                                      points[current], points[next])) {
                    contains = true;
                    break;
                }
            }
            if (contains)
                continue;
            vertices.push_back({static_cast<float>(points[previous].x()), static_cast<float>(points[previous].y())});
            vertices.push_back({static_cast<float>(points[current].x()), static_cast<float>(points[current].y())});
            vertices.push_back({static_cast<float>(points[next].x()), static_cast<float>(points[next].y())});
            indices.erase(indices.begin() + static_cast<std::ptrdiff_t>(i));
            clipped = true;
            break;
        }
        if (!clipped) {
            for (size_t i = 1; i + 1 < indices.size(); ++i) {
                const QPointF &a = points[indices[0]];
                const QPointF &b = points[indices[i]];
                const QPointF &c = points[indices[i + 1]];
                vertices.push_back({static_cast<float>(a.x()), static_cast<float>(a.y())});
                vertices.push_back({static_cast<float>(b.x()), static_cast<float>(b.y())});
                vertices.push_back({static_cast<float>(c.x()), static_cast<float>(c.y())});
            }
            break;
        }
    }
}

static std::vector<Vertex> triangulate(const QPainterPath &path)
{
    std::vector<Vertex> result;
    const QList<QPolygonF> polygons = path.toFillPolygons(QTransform());
    for (const QPolygonF &polygon : polygons)
        append_polygon_triangles(polygon, result);
    return result;
}

static QPainterPath stroke_geometry(const PrepareOptions &options)
{
    if (!options.stroke_enabled || options.stroke_width <= 0.0001f)
        return {};
    QPainterPathStroker stroker;
    const qreal width = options.stroke_alignment == 1
        ? options.stroke_width : options.stroke_width * 2.0f;
    stroker.setWidth(std::max<qreal>(0.001, width));
    stroker.setCapStyle(Qt::RoundCap);
    switch (options.stroke_join_style) {
    case 0: stroker.setJoinStyle(Qt::MiterJoin); break;
    case 2: stroker.setJoinStyle(Qt::BevelJoin); break;
    default: stroker.setJoinStyle(Qt::RoundJoin); break;
    }
    QPainterPath geometry = stroker.createStroke(options.path);
    if (options.stroke_alignment == 0)
        geometry = geometry.subtracted(options.path);
    else if (options.stroke_alignment == 2)
        geometry = geometry.intersected(options.path);
    geometry.setFillRule(Qt::OddEvenFill);
    return geometry;
}

static uint32_t multiply_alpha(uint32_t color, float multiplier)
{
    const uint32_t alpha = (color >> 24) & 0xFFu;
    const uint32_t resolved = static_cast<uint32_t>(std::lround(
        std::clamp(multiplier, 0.0f, 1.0f) * static_cast<float>(alpha)));
    return (color & 0x00FFFFFFu) | (std::min(255u, resolved) << 24);
}

static Material fill_material(const PrepareOptions &options)
{
    Material material;
    material.type = options.fill_type == 0 ? 0 : 1;
    material.solid = options.fill_color;
    material.gradient_type = std::clamp(options.fill_gradient_type, 0, 2);
    material.spread = std::clamp(options.fill_gradient_spread, 0, 2);
    material.angle = options.fill_gradient_angle;
    material.center_x = options.fill_gradient_center_x;
    material.center_y = options.fill_gradient_center_y;
    material.focal_x = options.fill_gradient_focal_x;
    material.focal_y = options.fill_gradient_focal_y;
    material.scale = options.fill_gradient_scale;
    material.stops = options.fill_gradient_stops;
    material.stops.push_back({multiply_alpha(options.fill_gradient_start_color,
                                             options.fill_gradient_opacity * options.fill_gradient_start_opacity),
                              options.fill_gradient_start_pos, 1.0f});
    material.stops.push_back({multiply_alpha(options.fill_gradient_end_color,
                                             options.fill_gradient_opacity * options.fill_gradient_end_opacity),
                              options.fill_gradient_end_pos, 1.0f});
    return material;
}

static Material stroke_material(const PrepareOptions &options)
{
    Material material;
    material.type = options.stroke_fill_type == 2 ? 1 : 0;
    material.solid = multiply_alpha(options.stroke_color, options.stroke_opacity);
    material.gradient_type = std::clamp(options.stroke_gradient_type, 0, 2);
    material.spread = std::clamp(options.stroke_gradient_spread, 0, 2);
    material.angle = options.stroke_gradient_angle;
    material.center_x = options.stroke_gradient_center_x;
    material.center_y = options.stroke_gradient_center_y;
    material.focal_x = options.stroke_gradient_focal_x;
    material.focal_y = options.stroke_gradient_focal_y;
    material.scale = options.stroke_gradient_scale;
    material.stops = options.stroke_gradient_stops;
    material.stops.push_back({multiply_alpha(options.stroke_gradient_start_color,
                                             options.stroke_opacity * options.stroke_gradient_opacity * options.stroke_gradient_start_opacity),
                              options.stroke_gradient_start_pos, 1.0f});
    material.stops.push_back({multiply_alpha(options.stroke_gradient_end_color,
                                             options.stroke_opacity * options.stroke_gradient_opacity * options.stroke_gradient_end_opacity),
                              options.stroke_gradient_end_pos, 1.0f});
    return material;
}

static gs_vertbuffer_t *create_vertex_buffer(const std::vector<Vertex> &vertices,
                                             float scale)
{
    if (vertices.empty())
        return nullptr;
    gs_vb_data *data = gs_vbdata_create();
    if (!data)
        return nullptr;
    data->num = vertices.size();
    data->points = static_cast<vec3 *>(bzalloc(sizeof(vec3) * data->num));
    data->num_tex = 1;
    data->tvarray = static_cast<gs_tvertarray *>(bzalloc(sizeof(gs_tvertarray)));
    if (!data->points || !data->tvarray) {
        gs_vbdata_destroy(data);
        return nullptr;
    }
    data->tvarray[0].width = 2;
    data->tvarray[0].array = bzalloc(sizeof(vec2) * data->num);
    if (!data->tvarray[0].array) {
        gs_vbdata_destroy(data);
        return nullptr;
    }
    auto *local = static_cast<vec2 *>(data->tvarray[0].array);
    for (size_t i = 0; i < vertices.size(); ++i) {
        vec3_set(&data->points[i], vertices[i].x * scale, vertices[i].y * scale, 0.0f);
        vec2_set(&local[i], vertices[i].x, vertices[i].y);
    }
    return gs_vertexbuffer_create(data, 0);
}

static void set_color(gs_effect_t *effect, const char *name, uint32_t argb)
{
    if (gs_eparam_t *parameter = gs_effect_get_param_by_name(effect, name)) {
        vec4 color;
        vec4_set(&color,
                 static_cast<float>((argb >> 16) & 0xFFu) / 255.0f,
                 static_cast<float>((argb >> 8) & 0xFFu) / 255.0f,
                 static_cast<float>(argb & 0xFFu) / 255.0f,
                 static_cast<float>((argb >> 24) & 0xFFu) / 255.0f);
        gs_effect_set_vec4(parameter, &color);
    }
}

static void set_material(gs_effect_t *effect, Material material,
                         float width, float height)
{
    std::sort(material.stops.begin(), material.stops.end(),
              [](const GradientStop &a, const GradientStop &b) {
                  return a.position < b.position;
              });
    if (material.stops.empty())
        material.stops.push_back({material.solid, 0.0f, 1.0f});
    if (material.stops.size() == 1)
        material.stops.push_back({material.stops.front().color, 1.0f, 1.0f});
    if (material.stops.size() > kMaxGradientStops)
        material.stops.resize(kMaxGradientStops);
    if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "materialType")) gs_effect_set_int(p, material.type);
    if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "gradientType")) gs_effect_set_int(p, material.gradient_type);
    if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "gradientSpread")) gs_effect_set_int(p, material.spread);
    if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "stopCount")) gs_effect_set_int(p, static_cast<int>(material.stops.size()));
    if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "angleDegrees")) gs_effect_set_float(p, material.angle);
    if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "gradientScale")) gs_effect_set_float(p, material.scale);
    if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "gradientCenter")) { vec2 value; vec2_set(&value, material.center_x, material.center_y); gs_effect_set_vec2(p, &value); }
    if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "gradientFocal")) { vec2 value; vec2_set(&value, material.focal_x, material.focal_y); gs_effect_set_vec2(p, &value); }
    if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, "materialSize")) { vec2 value; vec2_set(&value, width, height); gs_effect_set_vec2(p, &value); }
    set_color(effect, "solidColor", material.solid);
    for (int i = 0; i < kMaxGradientStops; ++i) {
        const GradientStop &stop = material.stops[std::min<size_t>(static_cast<size_t>(i), material.stops.size() - 1)];
        const uint32_t color = multiply_alpha(stop.color, stop.opacity);
        const std::string color_name = "stopColor" + std::to_string(i);
        const std::string position_name = "stopPos" + std::to_string(i);
        set_color(effect, color_name.c_str(), color);
        if (gs_eparam_t *p = gs_effect_get_param_by_name(effect, position_name.c_str()))
            gs_effect_set_float(p, std::clamp(stop.position, 0.0f, 1.0f));
    }
}

} // namespace

struct Layer::Impl {
    PrepareOptions options;
    Batch behind_stroke;
    Batch fill;
    Batch front_stroke;
    gs_texrender_t *targets[2] = {nullptr, nullptr};
    int active_target = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    bool pending = false;
};

struct Renderer::Impl {
    gs_effect_t *effect = nullptr;
    bool available = true;
    std::string last_error;
};

Layer::Layer() : impl_(std::make_unique<Impl>()) {}
Layer::~Layer() = default;
Layer::Layer(Layer &&) noexcept = default;
Layer &Layer::operator=(Layer &&) noexcept = default;
Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer &&) noexcept = default;
Renderer &Renderer::operator=(Renderer &&) noexcept = default;

bool Renderer::prepare(Layer &layer, const PrepareOptions &options,
                       std::string *reason)
{
    if (!impl_->available || options.logical_width <= 0.0f ||
        options.logical_height <= 0.0f || options.path.isEmpty()) {
        if (reason)
            *reason = impl_->available ? "Invalid GPU vector geometry." : impl_->last_error;
        return false;
    }
    Layer::Impl &state = *layer.impl_;
    state.options = options;
    state.fill = {};
    state.behind_stroke = {};
    state.front_stroke = {};
    if (options.fill_enabled) {
        state.fill.material = fill_material(options);
        state.fill.vertices = triangulate(options.path);
    }
    if (options.stroke_enabled && options.stroke_width > 0.0001f) {
        Batch &stroke = options.stroke_on_front ? state.front_stroke : state.behind_stroke;
        stroke.material = stroke_material(options);
        stroke.vertices = triangulate(stroke_geometry(options));
    }
    if (state.fill.vertices.empty() && state.behind_stroke.vertices.empty() &&
        state.front_stroke.vertices.empty()) {
        if (reason)
            *reason = "GPU vector tessellation produced no triangles.";
        return false;
    }
    state.pending = true;
    if (reason)
        reason->clear();
    return true;
}

bool Renderer::render(Layer &layer)
{
    Layer::Impl &state = *layer.impl_;
    if (!state.pending)
        return texture(layer) != nullptr;
    if (!impl_->effect) {
        impl_->effect = gs_effect_create(kVectorEffect,
                                         "obs-gsp-gpu-vector.effect", nullptr);
        if (!impl_->effect) {
            impl_->available = false;
            impl_->last_error = "Could not compile the GPU vector material shader.";
            return false;
        }
    }
    const float scale = std::clamp(state.options.raster_scale, 0.01f, 8.0f);
    const uint32_t width = static_cast<uint32_t>(std::clamp(
        static_cast<int>(std::ceil(state.options.logical_width * scale)), 1, 16384));
    const uint32_t height = static_cast<uint32_t>(std::clamp(
        static_cast<int>(std::ceil(state.options.logical_height * scale)), 1, 16384));
    const int render_index = state.active_target == 0 ? 1 : 0;
    if (!state.targets[render_index])
        state.targets[render_index] = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    gs_texrender_t *target = state.targets[render_index];
    if (!target)
        return false;
    gs_texrender_reset(target);
    if (!gs_texrender_begin(target, width, height))
        return false;
    gs_ortho(0.0f, static_cast<float>(width), 0.0f,
             static_cast<float>(height), -100.0f, 100.0f);
    vec4 clear;
    vec4_zero(&clear);
    gs_clear(GS_CLEAR_COLOR, &clear, 1.0f, 0);
    gs_blend_state_push();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    auto draw_batch = [&](const Batch &batch) {
        if (batch.vertices.empty())
            return true;
        gs_vertbuffer_t *buffer = create_vertex_buffer(batch.vertices, scale);
        if (!buffer)
            return false;
        set_material(impl_->effect, batch.material,
                     state.options.logical_width, state.options.logical_height);
        gs_load_vertexbuffer(buffer);
        while (gs_effect_loop(impl_->effect, "Draw"))
            gs_draw(GS_TRIS, 0, static_cast<uint32_t>(batch.vertices.size()));
        gs_load_vertexbuffer(nullptr);
        gs_vertexbuffer_destroy(buffer);
        return true;
    };

    const bool success = draw_batch(state.behind_stroke) &&
                         draw_batch(state.fill) &&
                         draw_batch(state.front_stroke);
    gs_blend_state_pop();
    gs_texrender_end(target);
    if (!success)
        return false;
    state.active_target = render_index;
    state.width = width;
    state.height = height;
    state.pending = false;
    return gs_texrender_get_texture(target) != nullptr;
}

void Renderer::release_layer(Layer &layer)
{
    Layer::Impl &state = *layer.impl_;
    for (gs_texrender_t *&target : state.targets) {
        if (target)
            gs_texrender_destroy(target);
        target = nullptr;
    }
    state.active_target = -1;
    state.width = state.height = 0;
    state.pending = false;
}

void Renderer::reset()
{
    if (impl_->effect)
        gs_effect_destroy(impl_->effect);
    impl_->effect = nullptr;
    impl_->available = true;
    impl_->last_error.clear();
}

gs_texture_t *Renderer::texture(const Layer &layer) const
{
    const Layer::Impl &state = *layer.impl_;
    return state.active_target >= 0 && state.targets[state.active_target]
        ? gs_texrender_get_texture(state.targets[state.active_target]) : nullptr;
}
uint32_t Renderer::texture_width(const Layer &layer) const { return layer.impl_->width; }
uint32_t Renderer::texture_height(const Layer &layer) const { return layer.impl_->height; }
bool Renderer::owns_texture(const Layer &layer, const gs_texture_t *texture_value) const
{
    return texture_value && texture(layer) == texture_value;
}
bool Renderer::backend_available() const { return impl_->available; }
const char *Renderer::last_error() const { return impl_->last_error.c_str(); }

} // namespace gsp::gpu_vector
