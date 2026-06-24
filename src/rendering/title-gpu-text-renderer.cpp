#include "title-gpu-text-renderer.h"
#include "title-gpu-text-sdf.h"

#include <QByteArray>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QRawFont>
#include <QTransform>

#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gsp::gpu_text {
namespace {

constexpr int kAtlasSize = 2048;
constexpr int kAtlasMaxPages = 8;
constexpr int kSdfSpread = 32;
constexpr int kAtlasGap = 1;
constexpr float kPi = 3.14159265358979323846f;

static constexpr const char *kGpuTextEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d glyphAtlas;
uniform int coverageMode;
uniform int drawPart;
uniform float sdfSpread;
uniform float2 materialOrigin;
uniform float2 materialSize;

uniform int fillType;
uniform int fillGradientType;
uniform int fillGradientSpread;
uniform float4 fillColor;
uniform float4 fillStartColor;
uniform float4 fillEndColor;
uniform float fillStartPos;
uniform float fillEndPos;
uniform float fillAngle;
uniform float2 fillCenter;
uniform float2 fillFocal;
uniform float fillScale;

uniform int strokeEnabled;
uniform float strokeWidth;
uniform float strokeOutside;
uniform float strokeInside;
uniform int strokeType;
uniform int strokeGradientType;
uniform int strokeGradientSpread;
uniform float4 strokeColor;
uniform float4 strokeStartColor;
uniform float4 strokeEndColor;
uniform float strokeStartPos;
uniform float strokeEndPos;
uniform float strokeAngle;
uniform float2 strokeCenter;
uniform float2 strokeFocal;
uniform float strokeScale;

sampler_state atlasSampler {
    Filter = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

struct VertDataIn {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
    float2 localPos : TEXCOORD1;
};
struct VertDataOut {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
    float2 localPos : TEXCOORD1;
};
VertDataOut VSDefault(VertDataIn v)
{
    VertDataOut o;
    o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj);
    o.uv = v.uv;
    o.localPos = v.localPos;
    return o;
}

float spreadValue(float value, int mode)
{
    if (mode == 1)
        return value - floor(value);
    if (mode == 2) {
        float repeated = value - floor(value * 0.5) * 2.0;
        return repeated <= 1.0 ? repeated : 2.0 - repeated;
    }
    return clamp(value, 0.0, 1.0);
}

float gradientPosition(float2 point, int type, int spreadMode,
                       float angleDegrees, float2 centerNormalized,
                       float2 focalNormalized, float scaleValue)
{
    point -= materialOrigin;
    float2 size = max(materialSize, float2(1.0, 1.0));
    float2 center = centerNormalized * size;
    float2 focal = focalNormalized * size;
    float safeScale = clamp(scaleValue, 0.01, 100.0);
    float angle = angleDegrees * 0.017453292519943295;
    float value = 0.0;
    if (type == 1) {
        float radius = max(size.x, size.y) * 0.5 * safeScale;
        float focalDistance = length(center - focal);
        float effectiveRadius = max(1.0, radius - min(focalDistance, radius * 0.95));
        value = length(point - focal) / effectiveRadius;
    } else if (type == 2) {
        float a = atan2(point.y - center.y, point.x - center.x);
        value = (-a - angle) / 6.283185307179586 + 0.5;
    } else {
        float lengthValue = max(1.0, length(size) * 0.5 * safeScale);
        float2 direction = float2(cos(angle), sin(angle));
        value = dot(point - (center - direction * lengthValue), direction) /
                (lengthValue * 2.0);
    }
    return spreadValue(value, spreadMode);
}

float4 gradientColor(float2 point, int type, int gradientType,
                     int spreadMode, float4 solidColor,
                     float4 startColor, float4 endColor,
                     float startPos, float endPos, float angle,
                     float2 center, float2 focal, float scaleValue)
{
    if (type == 0)
        return solidColor;
    float position = gradientPosition(point, gradientType, spreadMode,
                                      angle, center, focal, scaleValue);
    float denominator = max(abs(endPos - startPos), 0.000001);
    float mixValue = clamp((position - startPos) / denominator, 0.0, 1.0);
    if (endPos < startPos)
        mixValue = 1.0 - mixValue;
    return lerp(startColor, endColor, mixValue);
}

float4 premultiplied(float4 color, float coverage)
{
    float alpha = clamp(color.a * coverage, 0.0, 1.0);
    return float4(color.rgb * alpha, alpha);
}
float coverageInside(float distanceValue, float aa)
{
    return smoothstep(-aa, aa, distanceValue);
}

float4 PSText(VertDataOut v) : TARGET
{
    float signedDistance = coverageMode != 0
        ? sdfSpread
        : (glyphAtlas.Sample(atlasSampler, v.uv).r - 0.5) *
              (2.0 * sdfSpread);
    float aa = coverageMode != 0 ? 0.5 : max(fwidth(signedDistance), 0.55);
    float fillCoverage = coverageMode != 0
        ? 1.0
        : coverageInside(signedDistance, aa);

    float strokeCoverage = 0.0;
    if (coverageMode == 0 && strokeEnabled != 0 && strokeWidth > 0.0001) {
        /* Text-only placement contract: outer coverage expands the SDF edge,
         * inner coverage consumes the glyph interior, and mid splits exactly. */
        strokeCoverage = clamp(
            coverageInside(signedDistance + strokeOutside, aa) -
            coverageInside(signedDistance - strokeInside, aa),
            0.0, 1.0);
    }

    float4 fillMaterial = gradientColor(
        v.localPos, fillType, fillGradientType, fillGradientSpread,
        fillColor, fillStartColor, fillEndColor, fillStartPos, fillEndPos,
        fillAngle, fillCenter, fillFocal, fillScale);
    float4 strokeMaterial = gradientColor(
        v.localPos, strokeType, strokeGradientType, strokeGradientSpread,
        strokeColor, strokeStartColor, strokeEndColor,
        strokeStartPos, strokeEndPos, strokeAngle,
        strokeCenter, strokeFocal, strokeScale);

    if (drawPart != 0)
        return premultiplied(strokeMaterial, strokeCoverage);
    return premultiplied(fillMaterial, fillCoverage);
}

technique Draw {
    pass {
        vertex_shader = VSDefault(v);
        pixel_shader = PSText(v);
    }
}
)";

struct GlyphKey {
    uint64_t font = 0;
    uint64_t variant = 0;
    uint32_t glyph = 0;

    bool operator==(const GlyphKey &other) const
    {
        return font == other.font && variant == other.variant &&
               glyph == other.glyph;
    }
};

struct GlyphKeyHash {
    size_t operator()(const GlyphKey &key) const
    {
        uint64_t value = key.font;
        value ^= key.variant + 0x9e3779b97f4a7c15ULL +
                 (value << 6) + (value >> 2);
        value ^= static_cast<uint64_t>(key.glyph) +
                 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
        return static_cast<size_t>(value ^ (value >> 32));
    }
};

struct AtlasGlyph {
    int page = -1;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct AtlasPage {
    std::vector<uint8_t> pixels;
    int cursor_x = kAtlasGap;
    int cursor_y = kAtlasGap;
    int row_height = 0;
    bool dirty = true;
    gs_texture_t *texture = nullptr;

    AtlasPage() : pixels(static_cast<size_t>(kAtlasSize) * kAtlasSize, 0) {}
};

struct Vertex {
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    float local_x = 0.0f;
    float local_y = 0.0f;
};

struct Material {
    RichTextFill fill;
    RichTextStroke stroke;
};

enum class DrawPart : uint8_t {
    BehindStroke = 0,
    Fill = 1,
    FrontStroke = 2,
};

struct MaterialDomain {
    float x = 0.0f;
    float y = 0.0f;
    float width = 1.0f;
    float height = 1.0f;
};

struct Batch {
    int page = -1;
    size_t paint_index = 0;
    bool solid_geometry = false;
    DrawPart draw_part = DrawPart::Fill;
    Material material;
    MaterialDomain domain;
    std::vector<Vertex> vertices;
};

struct Quad {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 0.0f;
    float v1 = 0.0f;
    float local_x0 = 0.0f;
    float local_y0 = 0.0f;
    float local_x1 = 0.0f;
    float local_y1 = 0.0f;
};

static void hash_bytes(uint64_t &hash, const QByteArray &bytes)
{
    for (unsigned char value : bytes) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
}

static uint64_t raw_font_fingerprint(const QRawFont &font)
{
    const std::string family = font.familyName().toStdString();
    const std::string style = font.styleName().toStdString();
    const float pixel_size = static_cast<float>(font.pixelSize());
    uint64_t hash = 1469598103934665603ULL;
    hash_bytes(hash, QByteArray::fromStdString(family));
    hash_bytes(hash, QByteArray("\n", 1));
    hash_bytes(hash, QByteArray::fromStdString(style));
    hash_bytes(hash, font.fontTable("head"));
    hash_bytes(hash, font.fontTable("maxp"));
    hash_bytes(hash, font.fontTable("name"));
    uint32_t size_bits = 0;
    static_assert(sizeof(size_bits) == sizeof(pixel_size), "float size");
    std::memcpy(&size_bits, &pixel_size, sizeof(size_bits));
    hash ^= size_bits;
    hash *= 1099511628211ULL;
    return hash;
}

static uint64_t shaping_variant_fingerprint(
    const TextLayoutShapingStyle &style)
{
    uint64_t hash = 1469598103934665603ULL;
    auto add_u32 = [&](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<uint8_t>((value >> shift) & 0xFFu);
            hash *= 1099511628211ULL;
        }
    };
    auto add_float = [&](float value) {
        uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value), "float size");
        std::memcpy(&bits, &value, sizeof(bits));
        add_u32(bits);
    };
    add_u32(style.bold ? 1u : 0u);
    add_u32(style.italic ? 1u : 0u);
    add_u32(static_cast<uint32_t>(style.text_style));
    add_float(style.scale_x);
    add_float(style.scale_y);
    return hash;
}

static QRawFont raw_font_for_run(const TextLayoutRun &run)
{
    const TextLayoutFontKey &key = run.font;
    const int pixel_size =
        std::max(1, static_cast<int>(std::lround(key.pixel_size)));
    QFontDatabase database;
    QFont font = database.font(QString::fromStdString(key.family),
                               QString::fromStdString(key.style),
                               pixel_size);
    if (!key.style.empty())
        font.setStyleName(QString::fromStdString(key.style));
    font.setPixelSize(pixel_size);
    font.setBold(run.shaping_style.bold);
    font.setItalic(run.shaping_style.italic);
    const RichTextFontScaleMetrics scale = rich_text_font_scale_metrics(
        run.shaping_style.scale_x, run.shaping_style.scale_y);
    font.setStretch(scale.horizontal_stretch_percent);

    QRawFont raw = QRawFont::fromFont(font);
    if (raw.isValid())
        raw.setPixelSize(key.pixel_size);
    if (!raw.isValid() || raw_font_fingerprint(raw) != key.fingerprint)
        return {};
    return raw;
}

static bool raw_font_has_color_glyph_tables(const QRawFont &font)
{
    return !font.fontTable("COLR").isEmpty() ||
           !font.fontTable("CBDT").isEmpty() ||
           !font.fontTable("sbix").isEmpty() ||
           !font.fontTable("SVG ").isEmpty();
}

static uint32_t multiply_alpha(uint32_t argb, float multiplier)
{
    const uint32_t alpha = (argb >> 24) & 0xFFu;
    const uint32_t resolved = static_cast<uint32_t>(std::lround(
        std::clamp(multiplier, 0.0f, 1.0f) * static_cast<float>(alpha)));
    return (argb & 0x00FFFFFFu) | (std::min(255u, resolved) << 24);
}

static bool clip_quad(Quad &quad, float clip_x0, float clip_y0,
                      float clip_x1, float clip_y1)
{
    if (quad.x1 <= quad.x0 || quad.y1 <= quad.y0)
        return false;
    const float cx0 = std::max(quad.x0, clip_x0);
    const float cy0 = std::max(quad.y0, clip_y0);
    const float cx1 = std::min(quad.x1, clip_x1);
    const float cy1 = std::min(quad.y1, clip_y1);
    if (cx1 <= cx0 || cy1 <= cy0)
        return false;

    const float tx0 = (cx0 - quad.x0) / (quad.x1 - quad.x0);
    const float ty0 = (cy0 - quad.y0) / (quad.y1 - quad.y0);
    const float tx1 = (cx1 - quad.x0) / (quad.x1 - quad.x0);
    const float ty1 = (cy1 - quad.y0) / (quad.y1 - quad.y0);
    const float original_u0 = quad.u0;
    const float original_v0 = quad.v0;
    const float original_u1 = quad.u1;
    const float original_v1 = quad.v1;
    const float original_local_x0 = quad.local_x0;
    const float original_local_y0 = quad.local_y0;
    const float original_local_x1 = quad.local_x1;
    const float original_local_y1 = quad.local_y1;

    quad.u0 = original_u0 + (original_u1 - original_u0) * tx0;
    quad.v0 = original_v0 + (original_v1 - original_v0) * ty0;
    quad.u1 = original_u0 + (original_u1 - original_u0) * tx1;
    quad.v1 = original_v0 + (original_v1 - original_v0) * ty1;
    quad.local_x0 = original_local_x0 +
                    (original_local_x1 - original_local_x0) * tx0;
    quad.local_y0 = original_local_y0 +
                    (original_local_y1 - original_local_y0) * ty0;
    quad.local_x1 = original_local_x0 +
                    (original_local_x1 - original_local_x0) * tx1;
    quad.local_y1 = original_local_y0 +
                    (original_local_y1 - original_local_y0) * ty1;
    quad.x0 = cx0;
    quad.y0 = cy0;
    quad.x1 = cx1;
    quad.y1 = cy1;
    return true;
}

static void append_quad(std::vector<Vertex> &vertices, const Quad &source,
                        const PrepareOptions &options)
{
    Quad quad = source;
    if (!clip_quad(quad, options.clip_x, options.clip_y,
                   options.clip_x + options.clip_width,
                   options.clip_y + options.clip_height))
        return;

    const float scale = std::clamp(options.raster_scale, 0.01f, 8.0f);
    const Vertex a{quad.x0 * scale, quad.y0 * scale, quad.u0, quad.v0,
                   quad.local_x0, quad.local_y0};
    const Vertex b{quad.x1 * scale, quad.y0 * scale, quad.u1, quad.v0,
                   quad.local_x1, quad.local_y0};
    const Vertex c{quad.x1 * scale, quad.y1 * scale, quad.u1, quad.v1,
                   quad.local_x1, quad.local_y1};
    const Vertex d{quad.x0 * scale, quad.y1 * scale, quad.u0, quad.v1,
                   quad.local_x0, quad.local_y1};
    vertices.insert(vertices.end(), {a, b, c, a, c, d});
}

static void append_quad(std::vector<Vertex> &vertices,
                        float x0, float y0, float x1, float y1,
                        float u0, float v0, float u1, float v1,
                        float local_x0, float local_y0,
                        float local_x1, float local_y1,
                        const PrepareOptions &options)
{
    append_quad(vertices,
                Quad{x0, y0, x1, y1, u0, v0, u1, v1,
                     local_x0, local_y0, local_x1, local_y1},
                options);
}

static size_t paint_run_index_at(const std::vector<TextLayoutPaintRun> &runs,
                                 size_t byte_offset)
{
    for (size_t i = 0; i < runs.size(); ++i) {
        const size_t end = runs[i].byte_start + runs[i].byte_length;
        if (byte_offset >= runs[i].byte_start && byte_offset < end)
            return i;
    }
    return runs.empty() ? 0 : runs.size() - 1;
}

static gs_vertbuffer_t *create_vertex_buffer(const std::vector<Vertex> &vertices)
{
    if (vertices.empty())
        return nullptr;
    gs_vb_data *data = gs_vbdata_create();
    if (!data)
        return nullptr;
    data->num = vertices.size();
    data->points = static_cast<vec3 *>(bzalloc(sizeof(vec3) * data->num));
    data->num_tex = 2;
    data->tvarray = static_cast<gs_tvertarray *>(
        bzalloc(sizeof(gs_tvertarray) * data->num_tex));
    if (!data->points || !data->tvarray) {
        gs_vbdata_destroy(data);
        return nullptr;
    }
    data->tvarray[0].width = 2;
    data->tvarray[0].array = bzalloc(sizeof(vec2) * data->num);
    data->tvarray[1].width = 2;
    data->tvarray[1].array = bzalloc(sizeof(vec2) * data->num);
    if (!data->tvarray[0].array || !data->tvarray[1].array) {
        gs_vbdata_destroy(data);
        return nullptr;
    }
    auto *uv = static_cast<vec2 *>(data->tvarray[0].array);
    auto *local = static_cast<vec2 *>(data->tvarray[1].array);
    for (size_t i = 0; i < vertices.size(); ++i) {
        vec3_set(&data->points[i], vertices[i].x, vertices[i].y, 0.0f);
        vec2_set(&uv[i], vertices[i].u, vertices[i].v);
        vec2_set(&local[i], vertices[i].local_x, vertices[i].local_y);
    }
    return gs_vertexbuffer_create(data, 0);
}

static void set_vec2(gs_effect_t *effect, const char *name, float x, float y)
{
    if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, name)) {
        vec2 value;
        vec2_set(&value, x, y);
        gs_effect_set_vec2(param, &value);
    }
}

static void set_color(gs_effect_t *effect, const char *name, uint32_t argb)
{
    if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, name)) {
        vec4 value;
        vec4_set(&value,
                 static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                 static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                 static_cast<float>(argb & 0xFF) / 255.0f,
                 static_cast<float>((argb >> 24) & 0xFF) / 255.0f);
        gs_effect_set_vec4(param, &value);
    }
}

static void set_int(gs_effect_t *effect, const char *name, int value)
{
    if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, name))
        gs_effect_set_int(param, value);
}

static void set_float(gs_effect_t *effect, const char *name, float value)
{
    if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, name))
        gs_effect_set_float(param, value);
}

static int normalized_gradient_type(int gradient_type)
{
    switch (std::clamp(gradient_type, 0, 4)) {
    case 1:
        return 1;
    case 2:
        return 2;
    case 4:
        return 1;
    case 0:
    case 3:
    default:
        return 0;
    }
}

static int normalized_gradient_spread(int gradient_spread, int gradient_type)
{
    if (gradient_spread == 1 || gradient_spread == 2)
        return gradient_spread;
    return std::clamp(gradient_type, 0, 4) == 3 ? 1 : 0;
}

static void set_fill_params(gs_effect_t *effect, const char *prefix,
                            const RichTextFill &fill, float opacity_multiplier)
{
    const std::string type_name = std::string(prefix) + "Type";
    const std::string gradient_type_name = std::string(prefix) + "GradientType";
    const std::string spread_name = std::string(prefix) + "GradientSpread";
    const std::string color_name = std::string(prefix) + "Color";
    const std::string start_color_name = std::string(prefix) + "StartColor";
    const std::string end_color_name = std::string(prefix) + "EndColor";
    const std::string start_pos_name = std::string(prefix) + "StartPos";
    const std::string end_pos_name = std::string(prefix) + "EndPos";
    const std::string angle_name = std::string(prefix) + "Angle";
    const std::string center_name = std::string(prefix) + "Center";
    const std::string focal_name = std::string(prefix) + "Focal";
    const std::string scale_name = std::string(prefix) + "Scale";

    set_int(effect, type_name.c_str(), fill.type == 1 ? 1 : 0);
    set_int(effect, gradient_type_name.c_str(),
            normalized_gradient_type(fill.gradient_type));
    set_int(effect, spread_name.c_str(),
            normalized_gradient_spread(fill.gradient_spread,
                                       fill.gradient_type));
    set_color(effect, color_name.c_str(), multiply_alpha(fill.color, opacity_multiplier));
    set_color(effect, start_color_name.c_str(),
              multiply_alpha(fill.gradient_start_color,
                             opacity_multiplier * fill.gradient_opacity *
                                 fill.gradient_start_opacity));
    set_color(effect, end_color_name.c_str(),
              multiply_alpha(fill.gradient_end_color,
                             opacity_multiplier * fill.gradient_opacity *
                                 fill.gradient_end_opacity));
    set_float(effect, start_pos_name.c_str(),
              std::clamp(fill.gradient_start_pos, 0.0f, 1.0f));
    set_float(effect, end_pos_name.c_str(),
              std::clamp(fill.gradient_end_pos, 0.0f, 1.0f));
    set_float(effect, angle_name.c_str(), fill.gradient_angle);
    set_vec2(effect, center_name.c_str(), fill.gradient_center_x,
             fill.gradient_center_y);
    set_vec2(effect, focal_name.c_str(), fill.gradient_focal_x,
             fill.gradient_focal_y);
    set_float(effect, scale_name.c_str(),
              std::clamp(fill.gradient_scale, 0.01f, 100.0f));
}

} // namespace

struct Layer::Impl {
    PrepareOptions options;
    std::vector<Batch> batches;
    gs_texrender_t *targets[2] = {nullptr, nullptr};
    int active_target = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    bool pending = false;
};

struct Renderer::Impl {
    std::vector<AtlasPage> pages;
    std::unordered_map<GlyphKey, AtlasGlyph, GlyphKeyHash> glyphs;
    gs_effect_t *effect = nullptr;
    gs_texture_t *white_texture = nullptr;
    bool backend_available = true;
    std::string last_error;

    bool allocate_rect(int width, int height, AtlasGlyph &glyph)
    {
        if (width + kAtlasGap * 2 > kAtlasSize ||
            height + kAtlasGap * 2 > kAtlasSize)
            return false;
        for (size_t page_index = 0; page_index <= pages.size(); ++page_index) {
            if (page_index == pages.size()) {
                if (pages.size() >= kAtlasMaxPages)
                    return false;
                pages.emplace_back();
            }
            AtlasPage &page = pages[page_index];
            if (page.cursor_x + width + kAtlasGap > kAtlasSize) {
                page.cursor_x = kAtlasGap;
                page.cursor_y += page.row_height + kAtlasGap;
                page.row_height = 0;
            }
            if (page.cursor_y + height + kAtlasGap > kAtlasSize)
                continue;
            glyph.page = static_cast<int>(page_index);
            glyph.x = page.cursor_x;
            glyph.y = page.cursor_y;
            glyph.width = width;
            glyph.height = height;
            page.cursor_x += width + kAtlasGap;
            page.row_height = std::max(page.row_height, height);
            return true;
        }
        return false;
    }

    bool glyph_for(const TextLayoutRun &run, uint32_t glyph_id,
                   AtlasGlyph &out, std::string &reason)
    {
        const TextLayoutFontKey &font_key = run.font;
        const GlyphKey key{font_key.fingerprint,
                           shaping_variant_fingerprint(run.shaping_style),
                           glyph_id};
        const auto found = glyphs.find(key);
        if (found != glyphs.end()) {
            out = found->second;
            return true;
        }

        const QRawFont raw = raw_font_for_run(run);
        if (!raw.isValid()) {
            reason = "Could not reconstruct the exact shaped font face.";
            return false;
        }
        if (raw_font_has_color_glyph_tables(raw)) {
            reason = "Color-font glyphs require the compatibility raster path.";
            return false;
        }
        const QImage alpha = raw.alphaMapForGlyph(
            glyph_id, QRawFont::PixelAntialiasing, QTransform());
        if (alpha.isNull() || alpha.width() <= 0 || alpha.height() <= 0) {
            out = AtlasGlyph{};
            glyphs.emplace(key, out);
            return true;
        }
        QImage gray = alpha.convertToFormat(QImage::Format_Grayscale8);
        const int sdf_padding = (kSdfSpread + 2) * 2;
        if (gray.width() + sdf_padding > kAtlasSize ||
            gray.height() + sdf_padding > kAtlasSize) {
            reason = "Glyph dimensions exceed one persistent atlas page.";
            return false;
        }
        int sdf_width = 0;
        int sdf_height = 0;
        std::vector<uint8_t> sdf = build_glyph_sdf(
            gray.constBits(), gray.width(), gray.height(),
            gray.bytesPerLine(), kSdfSpread, sdf_width, sdf_height);
        if (sdf.empty()) {
            reason = "Could not build the glyph distance field.";
            return false;
        }
        AtlasGlyph allocated;
        if (!allocate_rect(sdf_width, sdf_height, allocated)) {
            reason = "The persistent glyph atlas reached its session limit.";
            return false;
        }
        AtlasPage &page = pages[static_cast<size_t>(allocated.page)];
        for (int y = 0; y < sdf_height; ++y) {
            std::copy(sdf.begin() + static_cast<size_t>(y) * sdf_width,
                      sdf.begin() + static_cast<size_t>(y + 1) * sdf_width,
                      page.pixels.begin() +
                          static_cast<size_t>(allocated.y + y) * kAtlasSize +
                          allocated.x);
        }
        page.dirty = true;
        glyphs.emplace(key, allocated);
        out = allocated;
        return true;
    }

    bool upload_pages()
    {
        for (AtlasPage &page : pages) {
            if (!page.dirty && page.texture)
                continue;
            if (!page.texture) {
                const uint8_t *planes[1] = {page.pixels.data()};
                page.texture = gs_texture_create(kAtlasSize, kAtlasSize,
                                                 GS_R8, 1, planes,
                                                 GS_DYNAMIC);
            } else {
                gs_texture_set_image(page.texture, page.pixels.data(),
                                     kAtlasSize, false);
            }
            if (!page.texture) {
                last_error = "Could not upload a persistent glyph-atlas page.";
                return false;
            }
            page.dirty = false;
        }
        if (!white_texture) {
            const uint8_t white = 255;
            const uint8_t *planes[1] = {&white};
            white_texture = gs_texture_create(1, 1, GS_R8, 1, planes, 0);
            if (!white_texture) {
                last_error = "Could not allocate the text-decoration texture.";
                return false;
            }
        }
        return true;
    }
};

Layer::Layer() : impl_(std::make_unique<Impl>()) {}
Layer::~Layer() = default;
Layer::Layer(Layer &&) noexcept = default;
Layer &Layer::operator=(Layer &&) noexcept = default;

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer &&) noexcept = default;
Renderer &Renderer::operator=(Renderer &&) noexcept = default;

bool Renderer::prepare(Layer &layer, const ImmutableTextLayout &layout,
                       const std::vector<TextLayoutPaintRun> &paint_runs,
                       const PrepareOptions &options, std::string *reason)
{
    if (!impl_->backend_available) {
        if (reason)
            *reason = impl_->last_error;
        return false;
    }
    if (!layout || !layout->valid || options.logical_width <= 0.0f ||
        options.logical_height <= 0.0f || options.text_width <= 0.0f ||
        options.text_height <= 0.0f) {
        if (reason)
            *reason = "Invalid immutable text layout or target geometry.";
        return false;
    }

    RichTextFill default_fill;
    default_fill.color = 0xFFFFFFFFu;
    TextLayoutPaintRun default_paint;
    default_paint.byte_start = 0;
    default_paint.byte_length = layout->text.size();
    default_paint.style.fill = default_fill;
    std::vector<TextLayoutPaintRun> fallback_runs;
    const std::vector<TextLayoutPaintRun> *resolved_runs_ptr = &paint_runs;
    if (paint_runs.empty()) {
        fallback_runs.push_back(default_paint);
        resolved_runs_ptr = &fallback_runs;
    }
    const std::vector<TextLayoutPaintRun> &resolved_runs = *resolved_runs_ptr;

    for (const TextLayoutPaintRun &run : resolved_runs) {
        if (run.style.stroke.enabled &&
            run.style.stroke.width > static_cast<float>(kSdfSpread - 3)) {
            if (reason)
                *reason = "Inline text stroke exceeds the SDF atlas spread.";
            return false;
        }
    }

    std::vector<Batch> behind_strokes;
    std::vector<Batch> fills;
    std::vector<Batch> front_strokes;
    auto batch_for = [&](std::vector<Batch> &phase, DrawPart draw_part,
                         int page, size_t paint_index, bool solid) -> Batch & {
        const int encoded_page = solid ? -1 : page;
        /* Merge only adjacent compatible quads inside the same global paint
         * phase. Grouping across phases would break Behind/Front semantics;
         * grouping non-adjacent glyphs would reorder combining marks and
         * overlapping scripts. */
        if (!phase.empty()) {
            Batch &last = phase.back();
            if (last.page == encoded_page &&
                last.paint_index == paint_index &&
                last.solid_geometry == solid &&
                last.draw_part == draw_part)
                return last;
        }
        Batch batch;
        batch.page = encoded_page;
        batch.paint_index = paint_index;
        batch.solid_geometry = solid;
        batch.draw_part = draw_part;
        batch.material.fill = resolved_runs[paint_index].style.fill;
        batch.material.stroke = resolved_runs[paint_index].style.stroke;
        batch.domain = {0.0f, 0.0f,
                        std::max(1.0f, options.text_width),
                        std::max(1.0f, options.text_height)};
        phase.push_back(std::move(batch));
        return phase.back();
    };

    std::vector<std::vector<TextLayoutPaintSlice>> cluster_slices;
    cluster_slices.reserve(layout->clusters.size());
    for (const TextLayoutCluster &cluster : layout->clusters) {
        std::vector<TextLayoutPaintSlice> slices =
            text_layout_cluster_paint_slices(*layout, cluster, resolved_runs);
        if (slices.empty()) {
            const size_t paint_index = std::min(
                paint_run_index_at(resolved_runs, cluster.byte_start),
                resolved_runs.size() - 1);
            slices.push_back(
                {paint_index, cluster.x, cluster.x + cluster.width});
        }
        cluster_slices.push_back(std::move(slices));
    }

    std::string failure_reason;
    for (const TextLayoutGlyph &glyph : layout->glyphs) {
        if (glyph.run_index >= layout->runs.size() ||
            glyph.cluster_index >= layout->clusters.size())
            continue;
        const TextLayoutRun &run = layout->runs[glyph.run_index];
        AtlasGlyph atlas_glyph;
        if (!impl_->glyph_for(run, glyph.glyph_id, atlas_glyph,
                              failure_reason)) {
            if (reason)
                *reason = failure_reason;
            return false;
        }
        if (atlas_glyph.page < 0 || atlas_glyph.width <= 0 ||
            atlas_glyph.height <= 0)
            continue;

        const float padding = static_cast<float>(kSdfSpread + 2);
        Quad glyph_quad;
        glyph_quad.x0 = options.text_offset_x + glyph.ink_x - padding;
        glyph_quad.y0 = options.text_offset_y + glyph.ink_y - padding;
        glyph_quad.x1 = glyph_quad.x0 + static_cast<float>(atlas_glyph.width);
        glyph_quad.y1 = glyph_quad.y0 + static_cast<float>(atlas_glyph.height);
        glyph_quad.u0 = static_cast<float>(atlas_glyph.x) / kAtlasSize;
        glyph_quad.v0 = static_cast<float>(atlas_glyph.y) / kAtlasSize;
        glyph_quad.u1 = static_cast<float>(atlas_glyph.x + atlas_glyph.width) /
                        kAtlasSize;
        glyph_quad.v1 = static_cast<float>(atlas_glyph.y + atlas_glyph.height) /
                        kAtlasSize;
        glyph_quad.local_x0 = glyph_quad.x0 - options.text_offset_x;
        glyph_quad.local_y0 = glyph_quad.y0 - options.text_offset_y;
        glyph_quad.local_x1 = glyph_quad.x1 - options.text_offset_x;
        glyph_quad.local_y1 = glyph_quad.y1 - options.text_offset_y;

        if (run.split_ligature && run.clip_width > 0.0f &&
            run.clip_height > 0.0f &&
            !clip_quad(glyph_quad,
                       options.text_offset_x + run.clip_x,
                       options.text_offset_y + run.clip_y,
                       options.text_offset_x + run.clip_x + run.clip_width,
                       options.text_offset_y + run.clip_y + run.clip_height))
            continue;

        const std::vector<TextLayoutPaintSlice> &slices =
            cluster_slices[glyph.cluster_index];
        const bool split_paint_cluster = slices.size() > 1;
        for (const TextLayoutPaintSlice &slice : slices) {
            const size_t paint_index =
                std::min(slice.paint_index, resolved_runs.size() - 1);
            Quad painted_quad = glyph_quad;
            if (split_paint_cluster &&
                !clip_quad(painted_quad,
                           options.text_offset_x + slice.x0,
                           -std::numeric_limits<float>::max(),
                           options.text_offset_x + slice.x1,
                           std::numeric_limits<float>::max()))
                continue;

            Batch &fill_batch = batch_for(
                fills, DrawPart::Fill, atlas_glyph.page, paint_index, false);
            append_quad(fill_batch.vertices, painted_quad, options);

            const RichTextStroke &stroke =
                resolved_runs[paint_index].style.stroke;
            if (!stroke.enabled || stroke.width <= 0.0001f)
                continue;
            /* Order is explicit for every alignment. An inner stroke placed
             * Behind is expected to be covered by an opaque fill; forcing it
             * to Front made the UI order control ineffective. */
            const int stroke_phase_index =
                text_stroke_draw_phase(stroke.on_front);
            std::vector<Batch> &stroke_phase = stroke_phase_index == 2
                ? front_strokes : behind_strokes;
            Batch &stroke_batch = batch_for(
                stroke_phase,
                stroke_phase_index == 2 ? DrawPart::FrontStroke
                                        : DrawPart::BehindStroke,
                atlas_glyph.page, paint_index, false);
            append_quad(stroke_batch.vertices, painted_quad, options);
        }
    }

    /* Underline/strikethrough remain vector geometry and are composited in the
     * fill phase. They never enter a QPainter text surface. */
    for (const TextLayoutRun &run : layout->runs) {
        if (run.cluster_count == 0 || run.line_index >= layout->lines.size())
            continue;
        const size_t paint_index = std::min(
            paint_run_index_at(resolved_runs, run.byte_start),
            resolved_runs.size() - 1);
        const TextLayoutPaintStyle &style = resolved_runs[paint_index].style;
        if (!style.underline && !style.strikethrough)
            continue;
        const TextLayoutLine &line = layout->lines[run.line_index];
        float left = std::numeric_limits<float>::max();
        float right = -std::numeric_limits<float>::max();
        for (uint32_t i = 0; i < run.cluster_count; ++i) {
            const TextLayoutCluster &cluster =
                layout->clusters[run.cluster_begin + i];
            left = std::min(left, cluster.x);
            right = std::max(right, cluster.x + cluster.width);
        }
        if (!(right > left))
            continue;
        const float pixel_size = std::max(1.0f, run.font.pixel_size);
        const float thickness = std::max(1.0f, pixel_size * 0.055f);
        auto add_decoration = [&](float y) {
            Batch &batch = batch_for(fills, DrawPart::Fill, -1,
                                     paint_index, true);
            const float x0 = options.text_offset_x + left;
            const float x1 = options.text_offset_x + right;
            const float y0 = options.text_offset_y + y;
            const float y1 = y0 + thickness;
            append_quad(batch.vertices, x0, y0, x1, y1,
                        0.0f, 0.0f, 1.0f, 1.0f,
                        left, y, right, y + thickness, options);
        };
        if (style.underline)
            add_decoration(line.baseline + pixel_size * 0.08f);
        if (style.strikethrough)
            add_decoration(line.baseline - pixel_size * 0.32f);
    }

    std::vector<Batch> batches;
    batches.reserve(behind_strokes.size() + fills.size() +
                    front_strokes.size());
    batches.insert(batches.end(),
                   std::make_move_iterator(behind_strokes.begin()),
                   std::make_move_iterator(behind_strokes.end()));
    batches.insert(batches.end(), std::make_move_iterator(fills.begin()),
                   std::make_move_iterator(fills.end()));
    batches.insert(batches.end(),
                   std::make_move_iterator(front_strokes.begin()),
                   std::make_move_iterator(front_strokes.end()));

    layer.impl_->options = options;
    layer.impl_->batches = std::move(batches);
    layer.impl_->pending = true;
    if (reason)
        reason->clear();
    return true;
}

bool Renderer::render(Layer &layer)
{
    Layer::Impl &state = *layer.impl_;
    if (!state.pending)
        return texture(layer) != nullptr;
    if (!impl_->backend_available)
        return false;
    if (!impl_->effect) {
        impl_->effect = gs_effect_create(kGpuTextEffect,
                                         "obs-gsp-gpu-text.effect", nullptr);
        if (!impl_->effect) {
            impl_->backend_available = false;
            impl_->last_error =
                "Could not compile the Phase 12C GPU text shader.";
            return false;
        }
    }
    if (!impl_->upload_pages())
        return false;

    const float scale = std::clamp(state.options.raster_scale, 0.01f, 8.0f);
    const uint32_t width = static_cast<uint32_t>(std::clamp(
        static_cast<int>(std::ceil(state.options.logical_width * scale)),
        1, 16384));
    const uint32_t height = static_cast<uint32_t>(std::clamp(
        static_cast<int>(std::ceil(state.options.logical_height * scale)),
        1, 16384));
    const int render_index = state.active_target == 0 ? 1 : 0;
    if (!state.targets[render_index])
        state.targets[render_index] = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    gs_texrender_t *target = state.targets[render_index];
    if (!target) {
        impl_->last_error = "Could not allocate a GPU text layer target.";
        return false;
    }

    gs_texrender_reset(target);
    if (!gs_texrender_begin(target, width, height)) {
        impl_->last_error = "Could not begin the GPU text layer target.";
        return false;
    }
    gs_ortho(0.0f, static_cast<float>(width), 0.0f,
             static_cast<float>(height), -100.0f, 100.0f);
    vec4 clear;
    vec4_zero(&clear);
    gs_clear(GS_CLEAR_COLOR, &clear, 1.0f, 0);
    gs_blend_state_push();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    bool success = true;
    for (const Batch &batch : state.batches) {
        if (batch.vertices.empty())
            continue;
        gs_vertbuffer_t *vertex_buffer = create_vertex_buffer(batch.vertices);
        if (!vertex_buffer) {
            impl_->last_error = "Could not allocate a GPU glyph-quad buffer.";
            success = false;
            break;
        }
        gs_texture_t *atlas = batch.solid_geometry
            ? impl_->white_texture
            : impl_->pages[static_cast<size_t>(batch.page)].texture;
        if (gs_eparam_t *param =
                gs_effect_get_param_by_name(impl_->effect, "glyphAtlas"))
            gs_effect_set_texture(param, atlas);
        set_int(impl_->effect, "coverageMode",
                batch.solid_geometry ? 1 : 0);
        set_int(impl_->effect, "drawPart",
                batch.draw_part == DrawPart::Fill ? 0 : 1);
        set_float(impl_->effect, "sdfSpread",
                  static_cast<float>(kSdfSpread));
        set_vec2(impl_->effect, "materialOrigin", batch.domain.x,
                 batch.domain.y);
        set_vec2(impl_->effect, "materialSize", batch.domain.width,
                 batch.domain.height);
        set_fill_params(impl_->effect, "fill", batch.material.fill, 1.0f);
        set_int(impl_->effect, "strokeEnabled",
                batch.draw_part == DrawPart::Fill
                    ? 0
                    : (batch.material.stroke.enabled ? 1 : 0));
        const float stroke_width =
            std::max(0.0f, batch.material.stroke.width);
        const TextStrokeCoverageExtents stroke_extents =
            text_stroke_coverage_extents(
                stroke_width, batch.material.stroke.alignment);
        set_float(impl_->effect, "strokeWidth", stroke_width);
        set_float(impl_->effect, "strokeOutside",
                  stroke_extents.outside);
        set_float(impl_->effect, "strokeInside",
                  stroke_extents.inside);
        set_fill_params(impl_->effect, "stroke",
                        batch.material.stroke.fill,
                        batch.material.stroke.opacity);

        gs_load_vertexbuffer(vertex_buffer);
        while (gs_effect_loop(impl_->effect, "Draw"))
            gs_draw(GS_TRIS, 0,
                    static_cast<uint32_t>(batch.vertices.size()));
        gs_load_vertexbuffer(nullptr);
        gs_vertexbuffer_destroy(vertex_buffer);
    }
    gs_blend_state_pop();
    gs_texrender_end(target);
    if (!success)
        return false;
    gs_texture_t *rendered = gs_texrender_get_texture(target);
    if (!rendered) {
        impl_->last_error = "GPU text rendering produced no texture.";
        return false;
    }
    state.active_target = render_index;
    state.width = width;
    state.height = height;
    state.pending = false;
    state.batches.clear();
    impl_->last_error.clear();
    return true;
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
    state.width = 0;
    state.height = 0;
    state.pending = false;
    state.batches.clear();
}

void Renderer::reset()
{
    for (AtlasPage &page : impl_->pages) {
        if (page.texture)
            gs_texture_destroy(page.texture);
        page.texture = nullptr;
    }
    impl_->pages.clear();
    impl_->glyphs.clear();
    if (impl_->white_texture)
        gs_texture_destroy(impl_->white_texture);
    impl_->white_texture = nullptr;
    if (impl_->effect)
        gs_effect_destroy(impl_->effect);
    impl_->effect = nullptr;
    impl_->backend_available = true;
    impl_->last_error.clear();
}

gs_texture_t *Renderer::texture(const Layer &layer) const
{
    const Layer::Impl &state = *layer.impl_;
    if (state.active_target < 0 || state.active_target > 1 ||
        !state.targets[state.active_target])
        return nullptr;
    return gs_texrender_get_texture(state.targets[state.active_target]);
}

uint32_t Renderer::texture_width(const Layer &layer) const
{
    return layer.impl_->width;
}

uint32_t Renderer::texture_height(const Layer &layer) const
{
    return layer.impl_->height;
}

bool Renderer::owns_texture(const Layer &layer,
                            const gs_texture_t *candidate) const
{
    if (!candidate)
        return false;
    for (gs_texrender_t *target : layer.impl_->targets) {
        if (target && gs_texrender_get_texture(target) == candidate)
            return true;
    }
    return false;
}

bool Renderer::backend_available() const
{
    return impl_->backend_available;
}

const char *Renderer::last_error() const
{
    return impl_->last_error.empty() ? nullptr : impl_->last_error.c_str();
}

} // namespace gsp::gpu_text
