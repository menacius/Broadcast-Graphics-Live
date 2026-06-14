/*
 * title-source.cpp
 *
 * OBS source: renders a Title to an OBS texture via Cairo.
 *
 * Cairo renders to a CPU RGBA buffer; we upload it to a gs_texture
 * each frame (or only on change for static titles).
 *
 * Build dependency: cairo, pango, pangocairo
 */

#include "title-source.h"
#include "title-data.h"
#include "plugin-main.h"
#include "title-localization.h"
#include "title-gpu-filter-pipeline.h"
#include "title-preferences.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/threading.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QSvgRenderer>
#include <QString>
#include <QStringList>
#include <QLocale>
#include <QPointF>
#include <QPainter>
#include <QBrush>
#include <QPainterPath>
#include <QFont>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QTextLayout>
#include <QTextDocument>
#include <QTextOption>
#include <QTextCursor>
#include <QDateTime>
#include <QFileInfo>
#include <QTransform>
#include <QColor>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QConicalGradient>
#include <QCryptographicHash>
#include <QByteArray>

#include <memory>
#include <string>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>
#include <mutex>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr uint32_t kMaxSourceDimension = 16384;

static uint32_t clamped_source_dimension(int value)
{
    return static_cast<uint32_t>(std::clamp(value, 1, static_cast<int>(kMaxSourceDimension)));
}

static bool image_path_is_svg(const QString &path)
{
    return path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".svgz"), Qt::CaseInsensitive);
}

static void unpremultiply_bgra_for_obs(uint8_t *pixels, size_t pixel_count)
{
    if (!pixels) return;

    for (size_t i = 0; i < pixel_count; ++i) {
        uint8_t *px = pixels + i * 4;
        const uint8_t alpha = px[3];
        if (alpha == 0) {
            px[0] = 0;
            px[1] = 0;
            px[2] = 0;
            continue;
        }
        if (alpha == 255) continue;

        const uint32_t half_alpha = alpha / 2u;
        px[0] = static_cast<uint8_t>(std::min(255u,
            (static_cast<uint32_t>(px[0]) * 255u + half_alpha) / alpha));
        px[1] = static_cast<uint8_t>(std::min(255u,
            (static_cast<uint32_t>(px[1]) * 255u + half_alpha) / alpha));
        px[2] = static_cast<uint8_t>(std::min(255u,
            (static_cast<uint32_t>(px[2]) * 255u + half_alpha) / alpha));
    }
}

struct CachedLayerImage {
    QImage image;
    qint64 last_modified_msecs = 0;
    qint64 file_size = -1;
};

static cairo_filter_t cairo_filter_for_image_scale_filter(ImageScaleFilter filter)
{
    switch (filter) {
    case ImageScaleFilter::Disable:
        return CAIRO_FILTER_NEAREST;
    case ImageScaleFilter::Bilinear:
        return CAIRO_FILTER_BILINEAR;
    case ImageScaleFilter::Bicubic:
    case ImageScaleFilter::Lanczos:
        return CAIRO_FILTER_BEST;
    case ImageScaleFilter::Area:
        return CAIRO_FILTER_GOOD;
    default:
        return CAIRO_FILTER_BILINEAR;
    }
}


struct CairoSurfaceDeleter {
    void operator()(cairo_surface_t *surface) const
    {
        if (surface) cairo_surface_destroy(surface);
    }
};

struct CairoContextDeleter {
    void operator()(cairo_t *context) const
    {
        if (context) cairo_destroy(context);
    }
};

using CairoSurfacePtr = std::unique_ptr<cairo_surface_t, CairoSurfaceDeleter>;
using CairoContextPtr = std::unique_ptr<cairo_t, CairoContextDeleter>;

static CairoSurfacePtr make_image_surface_for_qimage(QImage &image)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return nullptr;
    CairoSurfacePtr surface(cairo_image_surface_create_for_data(
        image.bits(), CAIRO_FORMAT_ARGB32, image.width(), image.height(), image.bytesPerLine()));
    return cairo_surface_status(surface.get()) == CAIRO_STATUS_SUCCESS ? std::move(surface) : CairoSurfacePtr();
}

static CairoSurfacePtr make_image_surface_for_const_qimage(const QImage &image)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return nullptr;
    CairoSurfacePtr surface(cairo_image_surface_create_for_data(
        const_cast<uchar *>(image.constBits()), CAIRO_FORMAT_ARGB32,
        image.width(), image.height(), image.bytesPerLine()));
    return cairo_surface_status(surface.get()) == CAIRO_STATUS_SUCCESS ? std::move(surface) : CairoSurfacePtr();
}

static CairoContextPtr make_cairo_context(cairo_surface_t *surface)
{
    if (!surface) return nullptr;
    CairoContextPtr cr(cairo_create(surface));
    return cairo_status(cr.get()) == CAIRO_STATUS_SUCCESS ? std::move(cr) : CairoContextPtr();
}

static QImage load_cached_layer_image(const QString &path, const QSize &fallback_size = QSize())
{
    QFileInfo info(path);
    const qint64 modified = info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0;
    const qint64 size_on_disk = info.exists() ? info.size() : -1;
    const bool is_svg = image_path_is_svg(path);
    const QString cache_key = QStringLiteral("%1|%2x%3").arg(path).arg(fallback_size.width()).arg(fallback_size.height());

    static std::mutex cache_mutex;
    static std::unordered_map<std::string, CachedLayerImage> cache;

    const std::string key = cache_key.toStdString();
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end() && it->second.last_modified_msecs == modified &&
            it->second.file_size == size_on_disk) {
            return it->second.image;
        }
    }

    QImage loaded;
    if (is_svg) {
        QSvgRenderer renderer(path);
        if (!renderer.isValid()) return QImage();

        QSize svg_size = fallback_size.isValid() && !fallback_size.isEmpty()
            ? fallback_size
            : renderer.defaultSize();
        if (!svg_size.isValid() || svg_size.isEmpty())
            svg_size = renderer.viewBox().size();
        if (!svg_size.isValid() || svg_size.isEmpty())
            svg_size = QSize(256, 256);

        loaded = QImage(svg_size, QImage::Format_ARGB32_Premultiplied);
        loaded.fill(Qt::transparent);
        QPainter painter(&loaded);
        renderer.render(&painter);
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (fallback_size.isValid() && !fallback_size.isEmpty())
            reader.setScaledSize(fallback_size);
        loaded = reader.read();
    }

    if (loaded.isNull()) return QImage();
    if (loaded.format() != QImage::Format_ARGB32_Premultiplied)
        loaded = loaded.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (cache.size() > 64)
            cache.clear();
        cache[key] = CachedLayerImage{loaded, modified, size_on_disk};
    }
    return loaded;
}
}

/* ══════════════════════════════════════════════════════════════════
 *  Source private data
 * ══════════════════════════════════════════════════════════════════ */
static Title snapshot_title_for_render(const Title &title)
{
    Title snapshot = title;
    snapshot.layers.clear();
    snapshot.layers.reserve(title.layers.size());
    for (const auto &layer : title.layers) {
        if (layer)
            snapshot.layers.push_back(std::make_shared<Layer>(*layer));
    }
    return snapshot;
}

struct TitleSourceData {
    obs_source_t *source  = nullptr;

    /* Settings */
    std::string title_id;
    bool        loop         = true;
    float       speed        = 1.0f;
    bool        auto_advance = false;  /* future: playlist mode */

    enum class CuePhase { FreeRun, IntroLoop, OutroThenIntro, OutroOnly };

    /* Playback state */
    double      playhead     = 0.0;    /* seconds */
    bool        playing      = true;
    bool        playback_reverse = false;
    uint64_t    seen_cue_revision = 0;
    CuePhase    cue_phase    = CuePhase::FreeRun;
    int         active_cue_row = -1;
    std::chrono::steady_clock::time_point last_tick;
    std::chrono::steady_clock::time_point last_clock_refresh;
    bool        first_tick   = true;
    bool        waiting_for_cue = true;

    /* GPU texture */
    std::mutex    texture_mutex;
    gs_texture_t *texture    = nullptr;
    gs_texrender_t *scene_mask_scene_texrender = nullptr;
    gs_texture_t *scene_mask_alpha_texture = nullptr;
    gs_effect_t  *scene_mask_effect = nullptr;
    std::unique_ptr<TitleGpuFilterPipeline> gpu_filter_pipeline;
    uint32_t      scene_mask_alpha_w = 0;
    uint32_t      scene_mask_alpha_h = 0;
    uint32_t      tex_w      = 0;
    uint32_t      tex_h      = 0;

    /* CPU render buffer */
    std::vector<uint8_t> pixel_buf;   /* BGRA row-major */

    /* Dirty flag – avoid re-uploading unchanged frames */
    bool dirty = true;
    uint64_t seen_store_revision = 0;

    struct CachedEffectLayer {
        std::string key;
        QImage image;
        QPointF origin;
    };
    std::unordered_map<std::string, CachedEffectLayer> effect_layer_cache;

    struct SceneMaskConfig {
        std::string layer_id;
        std::string scene_name;
        double zoom = 1.0;
        double x = 0.0;
        double y = 0.0;
        bool move_with_mask = true;
    };
    std::vector<SceneMaskConfig> scene_masks;

    struct ActiveSceneMaskScene {
        std::string name;
        obs_source_t *source = nullptr;
    };
    bool scene_mask_foreground_active = false;
    std::vector<ActiveSceneMaskScene> active_scene_mask_scenes;
};


static bool layer_has_animation(const Layer &layer)
{
    return layer.pos_x.is_animated() ||
           layer.pos_y.is_animated() ||
           layer.scale_x.is_animated() ||
           layer.scale_y.is_animated() ||
           layer.rotation.is_animated() ||
           layer.opacity.is_animated() ||
           layer.box_width.is_animated() ||
           layer.box_height.is_animated() ||
           layer.origin_x_prop.is_animated() ||
           layer.origin_y_prop.is_animated() ||
           layer.paragraph_indent_left_prop.is_animated() ||
           layer.paragraph_indent_right_prop.is_animated() ||
           layer.paragraph_indent_first_line_prop.is_animated() ||
           layer.font_size_prop.is_animated() ||
           layer.char_scale_x_prop.is_animated() ||
           layer.char_scale_y_prop.is_animated() ||
           layer.char_tracking_prop.is_animated() ||
           layer.baseline_shift_prop.is_animated() ||
           layer.paragraph_space_before_prop.is_animated() ||
           layer.paragraph_space_after_prop.is_animated() ||
           layer.shadow_enabled_prop.is_animated() ||
           layer.shadow_opacity_prop.is_animated() ||
           layer.shadow_distance_prop.is_animated() ||
           layer.shadow_angle_prop.is_animated() ||
           layer.shadow_blur_prop.is_animated() ||
           layer.shadow_spread_prop.is_animated() ||
           layer.shadow_color_a.is_animated() ||
           layer.shadow_color_r.is_animated() ||
           layer.shadow_color_g.is_animated() ||
           layer.shadow_color_b.is_animated() ||
           layer.background_enabled_prop.is_animated() ||
           layer.background_opacity_prop.is_animated() ||
           layer.background_padding_x_prop.is_animated() ||
           layer.background_padding_y_prop.is_animated() ||
           layer.background_corner_radius_prop.is_animated() ||
           layer.background_color_a.is_animated() ||
           layer.background_color_r.is_animated() ||
           layer.background_color_g.is_animated() ||
           layer.background_color_b.is_animated() ||
           layer.text_color_a.is_animated() ||
           layer.text_color_r.is_animated() ||
           layer.text_color_g.is_animated() ||
           layer.text_color_b.is_animated() ||
           layer.fill_color_a.is_animated() ||
           layer.fill_color_r.is_animated() ||
           layer.fill_color_g.is_animated() ||
           layer.fill_color_b.is_animated();
}

static bool include_property_bounds(const Layer &layer, const AnimatedProperty &prop,
                                    double &first_time, double &last_time)
{
    if (prop.keyframes.empty()) return false;
    first_time = std::min(first_time, layer.in_time + prop.keyframes.front().time);
    last_time = std::max(last_time, layer.in_time + prop.keyframes.back().time);
    return true;
}

static bool layer_animation_keyframe_bounds(const Layer &layer, double &first_time, double &last_time)
{
    bool has_bounds = false;
    has_bounds |= include_property_bounds(layer, layer.pos_x, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.pos_y, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.scale_x, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.scale_y, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.rotation, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.opacity, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.box_width, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.box_height, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.origin_x_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.origin_y_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.paragraph_indent_left_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.paragraph_indent_right_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.paragraph_indent_first_line_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.font_size_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.char_scale_x_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.char_scale_y_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.char_tracking_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.baseline_shift_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.paragraph_space_before_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.paragraph_space_after_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_enabled_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_opacity_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_distance_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_angle_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_blur_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_spread_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.shadow_color_b, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_enabled_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_opacity_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_padding_x_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_padding_y_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_corner_radius_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.background_color_b, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.text_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.text_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.text_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.text_color_b, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.fill_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.fill_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.fill_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.fill_color_b, first_time, last_time);
    return has_bounds;
}

static bool title_has_clock_layer(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    return std::any_of(title->layers.begin(), title->layers.end(),
                       [](const std::shared_ptr<Layer> &layer) {
                           return layer && layer->type == LayerType::Clock;
                       });
}

static bool title_has_ticker_layer(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    return std::any_of(title->layers.begin(), title->layers.end(),
                       [](const std::shared_ptr<Layer> &layer) {
                           return layer && layer->type == LayerType::Ticker;
                       });
}

static bool title_has_animation(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    return std::any_of(title->layers.begin(), title->layers.end(),
                       [](const std::shared_ptr<Layer> &layer) {
                           return layer && layer_has_animation(*layer);
                       });
}


static std::vector<std::shared_ptr<Layer>> scene_mask_layers(const std::shared_ptr<Title> &title)
{
    std::vector<std::shared_ptr<Layer>> masks;
    if (!title) return masks;
    for (const auto &layer : title->layers) {
        if (layer && layer->use_as_scene_mask)
            masks.push_back(layer);
    }
    return masks;
}

static std::vector<const Layer *> scene_mask_layers(const Title &title)
{
    std::vector<const Layer *> masks;
    for (const auto &layer : title.layers) {
        if (layer && layer->use_as_scene_mask)
            masks.push_back(layer.get());
    }
    return masks;
}

static std::string scene_mask_key(const std::string &layer_id, const char *suffix)
{
    QByteArray encoded = QByteArray(layer_id.c_str(), (int)layer_id.size()).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    return std::string(PROP_SCENE_MASK_PREFIX) + encoded.constData() + "_" + suffix;
}

static void set_scene_mask_property_defaults(obs_data_t *settings, const std::string &title_id)
{
    if (!settings)
        return;

    if (auto title = TitleDataStore::instance().get_title(title_id)) {
        for (const auto &layer : scene_mask_layers(title)) {
            obs_data_set_default_double(settings, scene_mask_key(layer->id, "zoom_percent").c_str(), 100.0);
            obs_data_set_default_double(settings, scene_mask_key(layer->id, "x").c_str(), 0.0);
            obs_data_set_default_double(settings, scene_mask_key(layer->id, "y").c_str(), 0.0);
            obs_data_set_default_bool(settings, scene_mask_key(layer->id, "move_with_mask").c_str(), true);
        }
    }
}

static void refresh_scene_mask_configs(TitleSourceData *data, obs_data_t *settings)
{
    if (!data || !settings)
        return;

    data->scene_masks.clear();
    set_scene_mask_property_defaults(settings, data->title_id);
    if (auto title = TitleDataStore::instance().get_title(data->title_id)) {
        for (const auto &layer : scene_mask_layers(title)) {
            TitleSourceData::SceneMaskConfig cfg;
            cfg.layer_id = layer->id;
            const std::string scene_key = scene_mask_key(layer->id, "scene");
            const std::string zoom_key = scene_mask_key(layer->id, "zoom_percent");
            const std::string x_key = scene_mask_key(layer->id, "x");
            const std::string y_key = scene_mask_key(layer->id, "y");
            const std::string move_with_mask_key = scene_mask_key(layer->id, "move_with_mask");
            cfg.scene_name = obs_data_get_string(settings, scene_key.c_str());
            const double zoom_percent = obs_data_get_double(settings, zoom_key.c_str());
            cfg.zoom = std::clamp(zoom_percent > 0.0 ? zoom_percent : 100.0, 1.0, 800.0) / 100.0;
            cfg.x = obs_data_get_double(settings, x_key.c_str());
            cfg.y = obs_data_get_double(settings, y_key.c_str());
            cfg.move_with_mask = obs_data_get_bool(settings, move_with_mask_key.c_str());
            data->scene_masks.push_back(std::move(cfg));
        }
    }
}

static void release_active_scene_mask_scenes(TitleSourceData *data)
{
    if (!data)
        return;

    for (auto &active : data->active_scene_mask_scenes) {
        if (active.source) {
            obs_source_dec_active(active.source);
            obs_source_release(active.source);
            active.source = nullptr;
        }
    }
    data->active_scene_mask_scenes.clear();
}

static void activate_scene_mask_scenes(TitleSourceData *data)
{
    if (!data || !data->scene_mask_foreground_active)
        return;

    release_active_scene_mask_scenes(data);

    std::unordered_set<std::string> activated_names;
    for (const auto &cfg : data->scene_masks) {
        if (cfg.scene_name.empty() || !activated_names.insert(cfg.scene_name).second)
            continue;

        obs_source_t *scene = obs_get_source_by_name(cfg.scene_name.c_str());
        if (!scene)
            continue;

        obs_source_inc_active(scene);
        data->active_scene_mask_scenes.push_back({cfg.scene_name, scene});
    }
}


static std::vector<std::shared_ptr<Layer>> order_exposed_text_layers(
    const std::vector<std::shared_ptr<Layer>> &exposed,
    const std::vector<std::string> &column_order)
{
    if (column_order.empty())
        return exposed;

    std::vector<std::shared_ptr<Layer>> ordered;
    ordered.reserve(exposed.size());
    for (const auto &layer_id : column_order) {
        auto it = std::find_if(exposed.begin(), exposed.end(),
                               [&](const std::shared_ptr<Layer> &layer) {
                                   return layer && layer->id == layer_id;
                               });
        if (it != exposed.end())
            ordered.push_back(*it);
    }
    for (const auto &layer : exposed) {
        if (!layer) continue;
        auto it = std::find_if(ordered.begin(), ordered.end(),
                               [&](const std::shared_ptr<Layer> &ordered_layer) {
                                   return ordered_layer && ordered_layer->id == layer->id;
                               });
        if (it == ordered.end())
            ordered.push_back(layer);
    }
    return ordered;
}

static std::vector<std::shared_ptr<Layer>> exposed_text_layers(const std::shared_ptr<Title> &title)
{
    std::vector<std::shared_ptr<Layer>> exposed;
    if (!title) return exposed;
    for (const auto &layer : title->layers) {
        if (!layer) continue;
        if ((layer->type == LayerType::Text || layer->type == LayerType::Ticker) && layer->expose_text)
            exposed.push_back(layer);
    }
    return order_exposed_text_layers(exposed, title->live_text_column_order);
}


static std::vector<std::shared_ptr<Layer>> exposed_text_layers(const Title &title)
{
    std::vector<std::shared_ptr<Layer>> exposed;
    for (const auto &layer : title.layers) {
        if (!layer) continue;
        if ((layer->type == LayerType::Text || layer->type == LayerType::Ticker) && layer->expose_text)
            exposed.push_back(layer);
    }
    return order_exposed_text_layers(exposed, title.live_text_column_order);
}

static double cue_persistence_hold_time(const Title &title)
{
    if (title.playback_mode == 1)
        return std::clamp(title.loop_end, title.loop_start, title.duration);
    if (title.playback_mode == 2)
        return std::clamp(title.pause_time, 0.0, title.duration);
    return std::clamp(title.duration, 0.0, title.duration);
}

static void clear_cue_persistence_transition(const std::shared_ptr<Title> &title)
{
    if (!title || !title->cue_persistence_transition) return;
    title->cue_persistence_transition = false;
    title->cue_persistent_text_columns.clear();
    TitleDataStore::instance().touch_runtime_change();
}

static int exposed_text_layer_index(const std::vector<std::shared_ptr<Layer>> &exposed, const std::shared_ptr<Layer> &layer)
{
    for (int i = 0; i < (int)exposed.size(); ++i) {
        if (exposed[i] == layer)
            return i;
    }
    return -1;
}



static void apply_live_text_row(const std::shared_ptr<Title> &title, int row)
{
    if (!title || row < 0 || row >= (int)title->live_text_rows.size()) return;
    auto exposed = exposed_text_layers(title);
    for (int col = 0; col < (int)exposed.size() && col < (int)title->live_text_rows[row].size(); ++col) {
        exposed[col]->text_content = title->live_text_rows[row][col];
        exposed[col]->rich_text = rich_text_document_from_layer_defaults(*exposed[col]);
        exposed[col]->rich_text_html.clear();
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Helper: ARGB uint32 → r,g,b,a doubles (0..1)
 * ══════════════════════════════════════════════════════════════════ */
static void unpack_color(uint32_t c,
                          double &r, double &g, double &b, double &a)
{
    a = ((c >> 24) & 0xFF) / 255.0;
    r = ((c >> 16) & 0xFF) / 255.0;
    g = ((c >>  8) & 0xFF) / 255.0;
    b = ((c >>  0) & 0xFF) / 255.0;
}


static double eval_text_font_size(const Layer &layer, double t);
static double eval_char_tracking(const Layer &layer, double t);
static double eval_char_scale_x(const Layer &layer, double t);
static double eval_char_scale_y(const Layer &layer, double t);
static double eval_baseline_shift(const Layer &layer, double t);
static double eval_paragraph_space_before(const Layer &layer, double t);
static double eval_paragraph_space_after(const Layer &layer, double t);

static QFont font_for_layer(const Layer &layer, double t = 0.0);
static QString display_text_for_style(const Layer &layer);
static QString overflow_layout_text(const QString &text, const Layer &layer);

static bool is_text_box_auto_size_layer(const Layer &layer)
{
    return layer.type == LayerType::Text || layer.type == LayerType::Clock;
}

static double natural_text_width(const Layer &layer)
{
    if (!is_text_box_auto_size_layer(layer)) return 1.0;
    QFontMetricsF metrics(font_for_layer(layer));
    QString text = display_text_for_style(layer);
    if (layer.text_overflow_mode == 2)
        text = overflow_layout_text(text, layer);

    double width = 1.0;
    for (const QString &line : text.split('\n'))
        width = std::max(width, static_cast<double>(metrics.horizontalAdvance(line)));
    return std::ceil(width);
}

static double natural_text_height(const Layer &layer, double width)
{
    if (!is_text_box_auto_size_layer(layer)) return 1.0;
    QFont font = font_for_layer(layer);
    QFontMetricsF metrics(font);
    QString text = display_text_for_style(layer);
    if (layer.text_overflow_mode == 2)
        text = overflow_layout_text(text, layer);

    QTextOption option;
    option.setWrapMode(layer.text_overflow_mode == 0
                           ? QTextOption::WrapAtWordBoundaryOrAnywhere
                           : QTextOption::NoWrap);

    double total_height = 0.0;
    const double leading = std::clamp((double)layer.text_leading, -200.0, 500.0);
    bool first_line = true;
    for (const QString &paragraph : text.split('\n')) {
        if (paragraph.isEmpty()) {
            if (!first_line) total_height += leading;
            total_height += metrics.lineSpacing();
            first_line = false;
            continue;
        }
        QTextLayout layout(paragraph, font);
        layout.setTextOption(option);
        layout.beginLayout();
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(layer.text_overflow_mode == 0 ? std::max(1.0, width) : 1000000.0);
            if (!first_line) total_height += leading;
            total_height += line.height();
            first_line = false;
            if (layer.text_overflow_mode != 0) break;
        }
        layout.endLayout();
    }
    return std::ceil(std::max(1.0, total_height));
}

static double eval_box_width(const Layer &layer, double t)
{
    double width = layer.box_width.is_animated()
        ? layer.box_width.evaluate(t)
        : static_cast<double>(layer.rect_width);
    if (layer.text_box_width_to_text && is_text_box_auto_size_layer(layer))
        width = std::min(natural_text_width(layer), std::max(1.0, (double)layer.max_text_box_width));
    return std::max(0.0, width);
}

static double eval_box_height(const Layer &layer, double t)
{
    double height = layer.box_height.is_animated()
        ? layer.box_height.evaluate(t)
        : static_cast<double>(layer.rect_height);
    if (layer.text_box_height_to_text && is_text_box_auto_size_layer(layer)) {
        const double width = eval_box_width(layer, t);
        height = std::min(natural_text_height(layer, width), std::max(1.0, (double)layer.max_text_box_height));
    }
    return std::max(0.0, height);
}


static const Layer *find_layer_by_id(const Title &title, const std::string &id)
{
    if (id.empty()) return nullptr;
    for (const auto &candidate : title.layers) {
        if (candidate && candidate->id == id)
            return candidate.get();
    }
    return nullptr;
}

static bool layer_chain_visible(const Title &title, const Layer &layer, double title_time, int depth = 0)
{
    if (depth > 64 || !layer.visible || title_time < layer.in_time || title_time > layer.out_time)
        return false;
    if (layer.parent_id.empty())
        return true;
    const Layer *parent = find_layer_by_id(title, layer.parent_id);
    return parent ? layer_chain_visible(title, *parent, title_time, depth + 1) : true;
}

static bool layer_is_track_matte_source(const Title &title, const Layer &layer)
{
    if (layer.id.empty())
        return false;
    for (const auto &candidate : title.layers) {
        if (!candidate || candidate.get() == &layer)
            continue;
        if (candidate->mask_mode != MaskMode::None && candidate->mask_source_id == layer.id)
            return true;
    }
    return false;
}

static bool layer_should_render_as_visible_content(const Title &title, const Layer &layer)
{
    /*
     * After Effects-style track mattes: a layer selected as another layer's
     * mask is not composited as normal visible artwork, but it is still
     * rendered into the matte surface by render_layer_with_mask().
     */
    return !layer.use_as_scene_mask && !layer_is_track_matte_source(title, layer);
}

static void apply_layer_world_transform(cairo_t *cr, const Title &title, const Layer &layer,
                                        double title_time, int depth = 0)
{
    if (depth > 64)
        return;
    if (!layer.parent_id.empty()) {
        if (const Layer *parent = find_layer_by_id(title, layer.parent_id))
            apply_layer_world_transform(cr, title, *parent, title_time, depth + 1);
    }
    const double lt = std::max(0.0, title_time - layer.in_time);
    cairo_translate(cr, layer.pos_x.evaluate(lt), layer.pos_y.evaluate(lt));
    cairo_rotate(cr, layer.rotation.evaluate(lt) * kPi / 180.0);
    cairo_scale(cr, layer.scale_x.evaluate(lt), layer.scale_y.evaluate(lt));
}

static double layer_chain_opacity(const Title &title, const Layer &layer, double title_time, int depth = 0)
{
    if (depth > 64)
        return 1.0;
    const double lt = std::max(0.0, title_time - layer.in_time);
    double opacity = layer.opacity.evaluate(lt);
    if (!layer.parent_id.empty()) {
        if (const Layer *parent = find_layer_by_id(title, layer.parent_id))
            opacity *= layer_chain_opacity(title, *parent, title_time, depth + 1);
    }
    return opacity;
}

static void cairo_add_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    r = std::clamp(r, 0.0, std::min(w, h) / 2.0);
    if (r <= 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + r,     y + r,     r, kPi,       3*kPi/2);
    cairo_arc(cr, x + w - r, y + r,     r, 3*kPi/2,   2*kPi);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,          kPi/2);
    cairo_arc(cr, x + r,     y + h - r, r, kPi/2,     kPi);
    cairo_close_path(cr);
}

static void cairo_add_rounded_rect_corners(cairo_t *cr, double x, double y, double w, double h,
                                           double top_left, double top_right,
                                           double bottom_right, double bottom_left,
                                           CornerType corner_type = CornerType::Round)
{
    const double max_radius = std::max(0.0, std::min(w, h) / 2.0);
    const double tl = std::clamp(top_left, 0.0, max_radius);
    const double tr = std::clamp(top_right, 0.0, max_radius);
    const double br = std::clamp(bottom_right, 0.0, max_radius);
    const double bl = std::clamp(bottom_left, 0.0, max_radius);
    if (tl <= 0.0 && tr <= 0.0 && br <= 0.0 && bl <= 0.0) {
        cairo_rectangle(cr, x, y, w, h);
        return;
    }
    auto add_corner = [&](double corner_x, double corner_y, double to_x, double to_y,
                          double cutout_x, double cutout_y, double radius) {
        if (radius <= 0.0) {
            cairo_line_to(cr, corner_x, corner_y);
            return;
        }
        switch (corner_type) {
        case CornerType::Straight:
            cairo_line_to(cr, to_x, to_y);
            break;
        case CornerType::Cutout:
            cairo_line_to(cr, cutout_x, cutout_y);
            cairo_line_to(cr, to_x, to_y);
            break;
        case CornerType::Concave:
            cairo_curve_to(cr, cutout_x, cutout_y, cutout_x, cutout_y, to_x, to_y);
            break;
        case CornerType::Round:
        default:
            break;
        }
    };
    cairo_new_sub_path(cr);
    cairo_move_to(cr, x + tl, y);
    cairo_line_to(cr, x + w - tr, y);
    if (tr > 0.0 && corner_type == CornerType::Round)
        cairo_arc(cr, x + w - tr, y + tr, tr, 3 * kPi / 2, 2 * kPi);
    else
        add_corner(x + w, y, x + w, y + tr, x + w - tr, y + tr, tr);
    cairo_line_to(cr, x + w, y + h - br);
    if (br > 0.0 && corner_type == CornerType::Round)
        cairo_arc(cr, x + w - br, y + h - br, br, 0, kPi / 2);
    else
        add_corner(x + w, y + h, x + w - br, y + h, x + w - br, y + h - br, br);
    cairo_line_to(cr, x + bl, y + h);
    if (bl > 0.0 && corner_type == CornerType::Round)
        cairo_arc(cr, x + bl, y + h - bl, bl, kPi / 2, kPi);
    else
        add_corner(x, y + h, x, y + h - bl, x + bl, y + h - bl, bl);
    cairo_line_to(cr, x, y + tl);
    if (tl > 0.0 && corner_type == CornerType::Round)
        cairo_arc(cr, x + tl, y + tl, tl, kPi, 3 * kPi / 2);
    else
        add_corner(x, y, x + tl, y, x + tl, y + tl, tl);
    cairo_close_path(cr);
}

static QPainterPath painter_rounded_rect_corners(const QRectF &rect, double top_left, double top_right,
                                                 double bottom_right, double bottom_left,
                                                 CornerType corner_type = CornerType::Round)
{
    const double max_radius = std::max(0.0, std::min(rect.width(), rect.height()) / 2.0);
    const double tl = std::clamp(top_left, 0.0, max_radius);
    const double tr = std::clamp(top_right, 0.0, max_radius);
    const double br = std::clamp(bottom_right, 0.0, max_radius);
    const double bl = std::clamp(bottom_left, 0.0, max_radius);
    QPainterPath path;
    if (tl <= 0.0 && tr <= 0.0 && br <= 0.0 && bl <= 0.0) {
        path.addRect(rect);
        return path;
    }
    auto add_corner = [&](const QPointF &corner, const QPointF &to, const QPointF &cutout, double radius) {
        if (radius <= 0.0) {
            path.lineTo(corner);
            return;
        }
        switch (corner_type) {
        case CornerType::Straight:
            path.lineTo(to);
            break;
        case CornerType::Cutout:
            path.lineTo(cutout);
            path.lineTo(to);
            break;
        case CornerType::Concave:
            path.cubicTo(cutout, cutout, to);
            break;
        case CornerType::Round:
        default:
            path.quadTo(corner, to);
            break;
        }
    };
    path.moveTo(rect.left() + tl, rect.top());
    path.lineTo(rect.right() - tr, rect.top());
    add_corner(rect.topRight(), QPointF(rect.right(), rect.top() + tr), QPointF(rect.right() - tr, rect.top() + tr), tr);
    path.lineTo(rect.right(), rect.bottom() - br);
    add_corner(rect.bottomRight(), QPointF(rect.right() - br, rect.bottom()), QPointF(rect.right() - br, rect.bottom() - br), br);
    path.lineTo(rect.left() + bl, rect.bottom());
    add_corner(rect.bottomLeft(), QPointF(rect.left(), rect.bottom() - bl), QPointF(rect.left() + bl, rect.bottom() - bl), bl);
    path.lineTo(rect.left(), rect.top() + tl);
    add_corner(rect.topLeft(), QPointF(rect.left() + tl, rect.top()), QPointF(rect.left() + tl, rect.top() + tl), tl);
    path.closeSubpath();
    return path;
}

static QPainterPath painter_layer_rounded_rect_path(const Layer &layer, const QRectF &rect)
{
    return painter_rounded_rect_corners(rect, layer.corner_radius_tl, layer.corner_radius_tr,
                                        layer.corner_radius_br, layer.corner_radius_bl,
                                        layer.corner_type);
}

static void cairo_add_regular_polygon(cairo_t *cr, double cx, double cy, double rx, double ry,
                                      int count, double start_angle)
{
    count = std::max(3, count);
    for (int i = 0; i < count; ++i) {
        const double a = start_angle + 2.0 * kPi * i / count;
        const double x = cx + std::cos(a) * rx;
        const double y = cy + std::sin(a) * ry;
        if (i == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
    }
    cairo_close_path(cr);
}

static void cairo_add_star(cairo_t *cr, double cx, double cy, double rx, double ry,
                           int points, double inner_radius, double outer_radius)
{
    points = std::clamp(points, 3, 64);
    inner_radius = std::clamp(inner_radius, 0.0, 1.0);
    outer_radius = std::clamp(outer_radius, 0.0, 1.0);
    if (outer_radius <= 0.0) outer_radius = 0.5;
    for (int i = 0; i < points * 2; ++i) {
        const bool outer = (i % 2) == 0;
        const double factor = outer ? outer_radius * 2.0 : inner_radius * 2.0;
        const double a = -kPi / 2.0 + kPi * i / points;
        const double x = cx + std::cos(a) * rx * factor;
        const double y = cy + std::sin(a) * ry * factor;
        if (i == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
    }
    cairo_close_path(cr);
}


static QPainterPath painter_layer_shape_path(const Layer &layer, double w, double h)
{
    QPainterPath path;
    const ShapeType st = layer.type == LayerType::Shape ? layer.shape_type : ShapeType::RoundedRectangle;
    switch (st) {
    case ShapeType::Ellipse:
        path.addEllipse(QRectF(0, 0, w, h)); break;
    case ShapeType::Triangle:
    case ShapeType::Polygon:
    case ShapeType::Diamond: {
        const int sides = st == ShapeType::Triangle ? 3 : (st == ShapeType::Diamond ? 4 : std::clamp(layer.shape_sides, 3, 64));
        const double start = -kPi / 2.0;
        for (int i = 0; i < sides; ++i) {
            const double a = start + 2.0 * kPi * i / sides;
            QPointF pt(w / 2.0 + std::cos(a) * w / 2.0, h / 2.0 + std::sin(a) * h / 2.0);
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        path.closeSubpath(); break;
    }
    case ShapeType::Star: {
        const int points = std::clamp(layer.shape_points, 3, 64);
        const double inner = std::clamp((double)layer.shape_inner_radius, 0.0, 1.0) * 2.0;
        const double outer = std::clamp((double)layer.shape_outer_radius, 0.0, 1.0) * 2.0;
        for (int i = 0; i < points * 2; ++i) {
            const double factor = (i % 2 == 0) ? outer : inner;
            const double a = -kPi / 2.0 + kPi * i / points;
            QPointF pt(w / 2.0 + std::cos(a) * w / 2.0 * factor, h / 2.0 + std::sin(a) * h / 2.0 * factor);
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        path.closeSubpath(); break;
    }
    case ShapeType::Line:
        path.moveTo(0, h / 2.0); path.lineTo(w, h / 2.0); break;
    case ShapeType::RoundedRectangle:
        path = painter_layer_rounded_rect_path(layer, QRectF(0, 0, w, h)); break;
    case ShapeType::Rectangle:
    default:
        path = painter_layer_rounded_rect_path(layer, QRectF(0, 0, w, h)); break;
    }
    return path;
}

static void cairo_add_layer_shape(cairo_t *cr, const Layer &layer, double w, double h)
{
    switch (layer.type == LayerType::Shape ? layer.shape_type : ShapeType::RoundedRectangle) {
    case ShapeType::Ellipse:
        cairo_save(cr); cairo_translate(cr, w / 2.0, h / 2.0); cairo_scale(cr, w / 2.0, h / 2.0);
        cairo_arc(cr, 0, 0, 1.0, 0, 2 * kPi); cairo_restore(cr); cairo_close_path(cr); break;
    case ShapeType::Triangle:
        cairo_add_regular_polygon(cr, w / 2.0, h / 2.0, w / 2.0, h / 2.0, 3, -kPi / 2.0); break;
    case ShapeType::Star:
        cairo_add_star(cr, w / 2.0, h / 2.0, w / 2.0, h / 2.0, layer.shape_points,
                       layer.shape_inner_radius, layer.shape_outer_radius); break;
    case ShapeType::Polygon:
        cairo_add_regular_polygon(cr, w / 2.0, h / 2.0, w / 2.0, h / 2.0,
                                  layer.shape_sides, -kPi / 2.0); break;
    case ShapeType::Diamond:
        cairo_add_regular_polygon(cr, w / 2.0, h / 2.0, w / 2.0, h / 2.0, 4, -kPi / 2.0); break;
    case ShapeType::Line:
        cairo_move_to(cr, 0, h / 2.0); cairo_line_to(cr, w, h / 2.0); break;
    case ShapeType::RoundedRectangle:
        cairo_add_rounded_rect_corners(cr, 0, 0, w, h, layer.corner_radius_tl, layer.corner_radius_tr,
                                       layer.corner_radius_br, layer.corner_radius_bl,
                                       layer.corner_type); break;
    case ShapeType::Rectangle:
    default:
        cairo_add_rounded_rect_corners(cr, 0, 0, w, h, layer.corner_radius_tl, layer.corner_radius_tr,
                                       layer.corner_radius_br, layer.corner_radius_bl,
                                       layer.corner_type); break;
    }
}

static double eval_origin_x(const Layer &layer, double t)
{
    return std::clamp(layer.origin_x_prop.is_animated()
                          ? layer.origin_x_prop.evaluate(t)
                          : (double)layer.origin_x,
                      0.0, 1.0);
}

static double eval_origin_y(const Layer &layer, double t)
{
    return std::clamp(layer.origin_y_prop.is_animated()
                          ? layer.origin_y_prop.evaluate(t)
                          : (double)layer.origin_y,
                      0.0, 1.0);
}

static int eval_channel(const AnimatedProperty &prop, double fallback, double t)
{
    return (int)std::clamp(std::round(prop.is_animated() ? prop.evaluate(t) : fallback),
                           0.0, 255.0);
}

static uint32_t eval_text_color(const Layer &layer, double t)
{
    return ((uint32_t)eval_channel(layer.text_color_a, (layer.text_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(layer.text_color_r, (layer.text_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(layer.text_color_g, (layer.text_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(layer.text_color_b, layer.text_color & 0xFF, t);
}

static uint32_t eval_fill_color(const Layer &layer, double t)
{
    return ((uint32_t)eval_channel(layer.fill_color_a, (layer.fill_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(layer.fill_color_r, (layer.fill_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(layer.fill_color_g, (layer.fill_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(layer.fill_color_b, layer.fill_color & 0xFF, t);
}

static QColor gradient_color_with_opacity(uint32_t argb, double gradient_opacity, double stop_opacity)
{
    QColor color((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, (argb >> 24) & 0xFF);
    color.setAlphaF(std::clamp((double)color.alphaF() * gradient_opacity * stop_opacity, 0.0, 1.0));
    return color;
}


static void apply_extra_gradient_stops(QGradient &gradient, const std::vector<GradientStop> &stops, double gradient_opacity)
{
    for (const auto &stop : stops) {
        const double pos = std::clamp((double)stop.position, 0.0, 1.0);
        gradient.setColorAt(pos, gradient_color_with_opacity(stop.color, gradient_opacity, stop.opacity));
    }
}

static void apply_base_gradient_stops(QGradient &gradient,
                                      double start_pos, uint32_t start_color, double start_opacity,
                                      double end_pos, uint32_t end_color, double end_opacity,
                                      const std::vector<GradientStop> &extra_stops,
                                      double opacity)
{
    gradient.setColorAt(start_pos, gradient_color_with_opacity(start_color, opacity, start_opacity));
    gradient.setColorAt(end_pos, gradient_color_with_opacity(end_color, opacity, end_opacity));
    apply_extra_gradient_stops(gradient, extra_stops, opacity);
}

static QBrush make_gradient_brush(int gradient_type,
                                  const QRectF &box,
                                  double opacity,
                                  double center_x, double center_y,
                                  double focal_x, double focal_y,
                                  double scale, double angle_degrees,
                                  double start_pos, uint32_t start_color, double start_opacity,
                                  double end_pos, uint32_t end_color, double end_opacity,
                                  const std::vector<GradientStop> &extra_stops)
{
    const int type = std::clamp(gradient_type, 0, 4);
    const double cx = box.left() + center_x * box.width();
    const double cy = box.top() + center_y * box.height();
    const double safe_scale = std::clamp(scale, 0.01, 100.0);
    const double length = std::max(1.0, std::hypot(box.width(), box.height()) * 0.5 * safe_scale);
    const double angle = angle_degrees * kPi / 180.0;
    const double dx = std::cos(angle) * length;
    const double dy = std::sin(angle) * length;

    if (type == 1 || type == 4) {
        const double radius = std::max(box.width(), box.height()) * 0.5 * safe_scale;
        QRadialGradient gradient(QPointF(cx, cy), std::max(1.0, radius),
                                 type == 1 ? QPointF(box.left() + focal_x * box.width(),
                                                     box.top() + focal_y * box.height())
                                           : QPointF(cx, cy));
        apply_base_gradient_stops(gradient, start_pos, start_color, start_opacity,
                                  end_pos, end_color, end_opacity, extra_stops, opacity);
        return QBrush(gradient);
    }

    if (type == 2) {
        QConicalGradient gradient(QPointF(cx, cy), -angle_degrees);
        apply_base_gradient_stops(gradient, start_pos, start_color, start_opacity,
                                  end_pos, end_color, end_opacity, extra_stops, opacity);
        return QBrush(gradient);
    }

    QLinearGradient gradient(QPointF(cx - dx, cy - dy), QPointF(cx + dx, cy + dy));
    if (type == 3)
        gradient.setSpread(QGradient::ReflectSpread);
    apply_base_gradient_stops(gradient, start_pos, start_color, start_opacity,
                              end_pos, end_color, end_opacity, extra_stops, opacity);
    return QBrush(gradient);
}

static QBrush gradient_fill_brush(const Layer &layer, const QRectF &box, double layer_opacity = 1.0)
{
    return make_gradient_brush(layer.gradient_type, box,
                               std::clamp((double)layer.gradient_opacity * layer_opacity, 0.0, 1.0),
                               layer.gradient_center_x, layer.gradient_center_y,
                               layer.gradient_focal_x, layer.gradient_focal_y,
                               layer.gradient_scale, layer.gradient_angle,
                               std::clamp((double)layer.gradient_start_pos, 0.0, 1.0),
                               layer.gradient_start_color, layer.gradient_start_opacity,
                               std::clamp((double)layer.gradient_end_pos, 0.0, 1.0),
                               layer.gradient_end_color, layer.gradient_end_opacity,
                               layer.gradient_stops);
}

static QBrush background_gradient_fill_brush(const Layer &layer, const QRectF &box, double layer_opacity = 1.0)
{
    return make_gradient_brush(layer.background_gradient_type, box,
                               std::clamp((double)layer.background_gradient_opacity * layer_opacity, 0.0, 1.0),
                               layer.background_gradient_center_x, layer.background_gradient_center_y,
                               layer.background_gradient_focal_x, layer.background_gradient_focal_y,
                               layer.background_gradient_scale, layer.background_gradient_angle,
                               std::clamp((double)layer.background_gradient_start_pos, 0.0, 1.0),
                               layer.background_gradient_start_color, layer.background_gradient_start_opacity,
                               std::clamp((double)layer.background_gradient_end_pos, 0.0, 1.0),
                               layer.background_gradient_end_color, layer.background_gradient_end_opacity,
                               layer.background_gradient_stops);
}

static QBrush stroke_gradient_fill_brush(const Layer &layer, const QRectF &box, double layer_opacity = 1.0)
{
    return make_gradient_brush(layer.stroke_gradient_type, box,
                               std::clamp((double)layer.stroke_gradient_opacity * layer_opacity, 0.0, 1.0),
                               layer.stroke_gradient_center_x, layer.stroke_gradient_center_y,
                               layer.stroke_gradient_focal_x, layer.stroke_gradient_focal_y,
                               layer.stroke_gradient_scale, layer.stroke_gradient_angle,
                               std::clamp((double)layer.stroke_gradient_start_pos, 0.0, 1.0),
                               layer.stroke_gradient_start_color, layer.stroke_gradient_start_opacity,
                               std::clamp((double)layer.stroke_gradient_end_pos, 0.0, 1.0),
                               layer.stroke_gradient_end_color, layer.stroke_gradient_end_opacity,
                               layer.stroke_gradient_stops);
}

static cairo_pattern_t *create_fill_gradient_pattern(const Layer &layer, double x, double y, double w, double h, double layer_alpha)
{
    const double opacity = std::clamp((double)layer.gradient_opacity * layer_alpha, 0.0, 1.0);
    const double cx = x + (double)layer.gradient_center_x * w;
    const double cy = y + (double)layer.gradient_center_y * h;
    const double scale = std::clamp((double)layer.gradient_scale, 0.01, 100.0);
    const double start_pos = std::clamp((double)layer.gradient_start_pos, 0.0, 1.0);
    const double end_pos = std::clamp((double)layer.gradient_end_pos, 0.0, 1.0);
    cairo_pattern_t *pattern = nullptr;
    if (layer.gradient_type == 1 || layer.gradient_type == 4) {
        const double radius = std::max(w, h) * 0.5 * scale;
        const double fx = x + (double)layer.gradient_focal_x * w;
        const double fy = y + (double)layer.gradient_focal_y * h;
        pattern = cairo_pattern_create_radial(fx, fy, 0.0, cx, cy, std::max(1.0, radius));
    } else {
        const double length = std::hypot(w, h) * 0.5 * scale;
        const double angle = layer.gradient_angle * kPi / 180.0;
        const double dx = std::cos(angle) * length;
        const double dy = std::sin(angle) * length;
        pattern = cairo_pattern_create_linear(cx - dx, cy - dy, cx + dx, cy + dy);
    }
    auto add_stop = [&](double pos, uint32_t argb, double stop_opacity) {
        QColor color = gradient_color_with_opacity(argb, opacity, stop_opacity);
        cairo_pattern_add_color_stop_rgba(pattern, pos, color.redF(), color.greenF(), color.blueF(), color.alphaF());
    };
    add_stop(start_pos, layer.gradient_start_color, layer.gradient_start_opacity);
    add_stop(end_pos, layer.gradient_end_color, layer.gradient_end_opacity);
    for (const auto &stop : layer.gradient_stops)
        add_stop(stop.position, stop.color, stop.opacity);
    cairo_pattern_set_extend(pattern, layer.gradient_type == 3 ? CAIRO_EXTEND_REFLECT : CAIRO_EXTEND_PAD);
    return pattern;
}

static cairo_pattern_t *create_stroke_gradient_pattern(const Layer &layer, double x, double y, double w, double h, double layer_alpha)
{
    const double opacity = std::clamp((double)layer.stroke_gradient_opacity * layer_alpha, 0.0, 1.0);
    const double cx = x + (double)layer.stroke_gradient_center_x * w;
    const double cy = y + (double)layer.stroke_gradient_center_y * h;
    const double scale = std::clamp((double)layer.stroke_gradient_scale, 0.01, 100.0);
    const double start_pos = std::clamp((double)layer.stroke_gradient_start_pos, 0.0, 1.0);
    const double end_pos = std::clamp((double)layer.stroke_gradient_end_pos, 0.0, 1.0);
    cairo_pattern_t *pattern = nullptr;
    if (layer.stroke_gradient_type == 1 || layer.stroke_gradient_type == 4) {
        const double radius = std::max(w, h) * 0.5 * scale;
        const double fx = x + (double)layer.stroke_gradient_focal_x * w;
        const double fy = y + (double)layer.stroke_gradient_focal_y * h;
        pattern = cairo_pattern_create_radial(fx, fy, 0.0, cx, cy, std::max(1.0, radius));
    } else {
        const double length = std::hypot(w, h) * 0.5 * scale;
        const double angle = layer.stroke_gradient_angle * kPi / 180.0;
        const double dx = std::cos(angle) * length;
        const double dy = std::sin(angle) * length;
        pattern = cairo_pattern_create_linear(cx - dx, cy - dy, cx + dx, cy + dy);
    }
    auto add_stop = [&](double pos, uint32_t argb, double stop_opacity) {
        QColor color = gradient_color_with_opacity(argb, opacity, stop_opacity);
        cairo_pattern_add_color_stop_rgba(pattern, pos, color.redF(), color.greenF(), color.blueF(), color.alphaF());
    };
    add_stop(start_pos, layer.stroke_gradient_start_color, layer.stroke_gradient_start_opacity);
    add_stop(end_pos, layer.stroke_gradient_end_color, layer.stroke_gradient_end_opacity);
    for (const auto &stop : layer.stroke_gradient_stops)
        add_stop(stop.position, stop.color, stop.opacity);
    cairo_pattern_set_extend(pattern, layer.stroke_gradient_type == 3 ? CAIRO_EXTEND_REFLECT : CAIRO_EXTEND_PAD);
    return pattern;
}

static cairo_pattern_t *create_background_gradient_pattern(const Layer &layer, double x, double y, double w, double h, double layer_alpha)
{
    const double opacity = std::clamp((double)layer.background_gradient_opacity * layer_alpha, 0.0, 1.0);
    const double cx = x + (double)layer.background_gradient_center_x * w;
    const double cy = y + (double)layer.background_gradient_center_y * h;
    const double scale = std::clamp((double)layer.background_gradient_scale, 0.01, 100.0);
    const double start_pos = std::clamp((double)layer.background_gradient_start_pos, 0.0, 1.0);
    const double end_pos = std::clamp((double)layer.background_gradient_end_pos, 0.0, 1.0);
    cairo_pattern_t *pattern = nullptr;
    if (layer.background_gradient_type == 1 || layer.background_gradient_type == 4) {
        const double radius = std::max(w, h) * 0.5 * scale;
        const double fx = x + (double)layer.background_gradient_focal_x * w;
        const double fy = y + (double)layer.background_gradient_focal_y * h;
        pattern = cairo_pattern_create_radial(fx, fy, 0.0, cx, cy, std::max(1.0, radius));
    } else {
        const double length = std::hypot(w, h) * 0.5 * scale;
        const double angle = layer.background_gradient_angle * kPi / 180.0;
        const double dx = std::cos(angle) * length;
        const double dy = std::sin(angle) * length;
        pattern = cairo_pattern_create_linear(cx - dx, cy - dy, cx + dx, cy + dy);
    }
    auto add_stop = [&](double pos, uint32_t argb, double stop_opacity) {
        QColor color = gradient_color_with_opacity(argb, opacity, stop_opacity);
        cairo_pattern_add_color_stop_rgba(pattern, pos, color.redF(), color.greenF(), color.blueF(), color.alphaF());
    };
    add_stop(start_pos, layer.background_gradient_start_color, layer.background_gradient_start_opacity);
    add_stop(end_pos, layer.background_gradient_end_color, layer.background_gradient_end_opacity);
    for (const auto &stop : layer.background_gradient_stops)
        add_stop(stop.position, stop.color, stop.opacity);
    cairo_pattern_set_extend(pattern, layer.background_gradient_type == 3 ? CAIRO_EXTEND_REFLECT : CAIRO_EXTEND_PAD);
    return pattern;
}

static bool layer_effect_enabled(const Layer &layer, LayerEffectType type, bool legacy_enabled)
{
    if (layer.effects.empty())
        return legacy_enabled;
    for (const auto &effect : layer.effects) {
        if (effect.type == type && effect.enabled)
            return true;
    }
    return false;
}

static bool eval_outline_enabled(const Layer &layer, double)
{
    return layer.stroke_fill_type != 0 &&
           layer_effect_enabled(layer, LayerEffectType::Outline, layer.outline_enabled);
}

static uint32_t eval_outline_color(const Layer &layer, double)
{
    return layer.stroke_color;
}

static double eval_outline_width(const Layer &layer, double)
{
    return eval_outline_enabled(layer, 0.0) ? std::max(0.0f, layer.stroke_width) : 0.0;
}

static double eval_outline_opacity(const Layer &layer, double)
{
    return std::clamp((double)layer.outline_opacity, 0.0, 1.0);
}

static bool eval_outline_on_front(const Layer &layer, double)
{
    return layer.outline_on_front;
}

static bool eval_outline_antialias(const Layer &layer, double)
{
    return layer.outline_antialias;
}

static cairo_antialias_t outline_cairo_antialias(const Layer &layer)
{
    return layer.outline_antialias ? CAIRO_ANTIALIAS_DEFAULT : CAIRO_ANTIALIAS_NONE;
}

static cairo_line_join_t outline_cairo_join_style(const Layer &layer)
{
    switch (layer.outline_join_style) {
    case 0: return CAIRO_LINE_JOIN_MITER;
    case 2: return CAIRO_LINE_JOIN_BEVEL;
    case 1:
    default: return CAIRO_LINE_JOIN_ROUND;
    }
}

static Qt::PenJoinStyle outline_pen_join_style(const Layer &layer)
{
    switch (layer.outline_join_style) {
    case 0: return Qt::MiterJoin;
    case 2: return Qt::BevelJoin;
    case 1:
    default: return Qt::RoundJoin;
    }
}

static bool eval_shadow_enabled(const Layer &layer, double t)
{
    const bool legacy = layer.shadow_enabled_prop.is_animated()
        ? layer.shadow_enabled_prop.evaluate(t) >= 0.5
        : layer.shadow_enabled;
    return layer_effect_enabled(layer, LayerEffectType::DropShadow, legacy);
}

static bool eval_long_shadow_enabled(const Layer &layer, double)
{
    return layer_effect_enabled(layer, LayerEffectType::LongShadow, layer.long_shadow_enabled);
}

static double eval_shadow_opacity(const Layer &layer, double t)
{
    return std::clamp(layer.shadow_opacity_prop.is_animated() ? layer.shadow_opacity_prop.evaluate(t) : (double)layer.shadow_opacity, 0.0, 1.0);
}

static double eval_shadow_distance(const Layer &layer, double t)
{
    return std::max(0.0, layer.shadow_distance_prop.is_animated() ? layer.shadow_distance_prop.evaluate(t) : (double)layer.shadow_distance);
}

static double eval_shadow_angle(const Layer &layer, double t)
{
    return layer.shadow_angle_prop.is_animated() ? layer.shadow_angle_prop.evaluate(t) : (double)layer.shadow_angle;
}

static double eval_shadow_blur(const Layer &layer, double t)
{
    return std::max(0.0, layer.shadow_blur_prop.is_animated() ? layer.shadow_blur_prop.evaluate(t) : (double)layer.shadow_blur);
}

static double eval_shadow_spread(const Layer &layer, double t)
{
    return std::max(0.0, layer.shadow_spread_prop.is_animated() ? layer.shadow_spread_prop.evaluate(t) : (double)layer.shadow_spread);
}

static uint32_t eval_shadow_color(const Layer &layer, double t)
{
    return ((uint32_t)eval_channel(layer.shadow_color_a, (layer.shadow_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(layer.shadow_color_r, (layer.shadow_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(layer.shadow_color_g, (layer.shadow_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(layer.shadow_color_b, layer.shadow_color & 0xFF, t);
}

static QPointF shadow_offset(const Layer &layer, double t)
{
    double radians = eval_shadow_angle(layer, t) * kPi / 180.0;
    double distance = eval_shadow_distance(layer, t);
    return QPointF(std::cos(radians) * distance,
                   std::sin(radians) * distance);
}

static QColor color_from_argb(uint32_t argb);

struct ShadowRenderParams {
    double dx = 0.0;
    double dy = 0.0;
    double blur = 0.0;
    double spread = 0.0;
    int blur_type = (int)ShadowBlurType::StackFast;
    bool drop_enabled = false;
    uint32_t color = 0x99000000;
    double opacity = 1.0;
    bool long_enabled = false;
    uint32_t long_color = 0x99000000;
    double long_opacity = 0.0;
    double long_length = 0.0;
    double long_angle = 0.0;
    double long_falloff = 1.0;
    int long_blur_type = (int)LongShadowBlurType::None;
    double long_blur = 0.0;
};

struct CachedShadowImage {
    QImage image;
    QPointF origin;
};

static QString image_content_hash(const QImage &image)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(reinterpret_cast<const char *>(image.constBits()), image.bytesPerLine() * image.height());
    return QString::fromLatin1(hash.result().toHex());
}

static QString shadow_param_key(const Layer &layer, const ShadowRenderParams &p, const QString &shape_key)
{
    return QStringLiteral("%1|%2|%3|%4,%5,%6,%7,%8,%9,%10,%11,%12,%13,%14,%15,%16,%17,%18")
        .arg(QString::fromStdString(layer.id), shape_key)
        .arg(p.dx, 0, 'f', 2).arg(p.dy, 0, 'f', 2).arg(p.blur, 0, 'f', 2)
        .arg(p.spread, 0, 'f', 2).arg(p.blur_type).arg(p.drop_enabled ? 1 : 0)
        .arg(p.color).arg(p.opacity, 0, 'f', 3)
        .arg(p.long_enabled ? 1 : 0).arg(p.long_color).arg(p.long_opacity, 0, 'f', 3)
        .arg(p.long_length, 0, 'f', 2).arg(p.long_angle, 0, 'f', 2).arg(p.long_falloff, 0, 'f', 3)
        .arg(p.long_blur_type).arg(p.long_blur, 0, 'f', 2);
}

static ShadowRenderParams evaluated_shadow_params(const Layer &layer, double t)
{
    ShadowRenderParams p;
    p.drop_enabled = eval_shadow_enabled(layer, t);
    if (p.drop_enabled) {
        QPointF off = shadow_offset(layer, t);
        p.dx = off.x();
        p.dy = off.y();
        p.blur = eval_shadow_blur(layer, t);
        p.spread = eval_shadow_spread(layer, t);
        p.blur_type = (int)layer.shadow_blur_type;
        p.color = eval_shadow_color(layer, t);
        p.opacity = eval_shadow_opacity(layer, t) * (((p.color >> 24) & 0xFF) / 255.0);
    }
    p.long_enabled = eval_long_shadow_enabled(layer, t) && layer.long_shadow_length > 0.0f && layer.long_shadow_opacity > 0.0f;
    if (p.long_enabled) {
        p.long_color = layer.long_shadow_color;
        p.long_opacity = std::clamp((double)layer.long_shadow_opacity, 0.0, 1.0) * (((p.long_color >> 24) & 0xFF) / 255.0);
        p.long_length = std::max(0.0, (double)layer.long_shadow_length);
        p.long_angle = layer.long_shadow_angle;
        p.long_falloff = std::clamp((double)layer.long_shadow_falloff, 0.0, 4.0);
        p.long_blur_type = (int)layer.long_shadow_blur_type;
        p.long_blur = p.long_blur_type == (int)LongShadowBlurType::None
                          ? 0.0
                          : std::max(0.0, (double)layer.long_shadow_blur);
    }
    return p;
}

static void box_blur_alpha(std::vector<uint8_t> &alpha, int w, int h, int radius)
{
    if (radius <= 0 || w <= 0 || h <= 0) return;
    std::vector<uint8_t> tmp(alpha.size());
    const int window = radius * 2 + 1;
    for (int y = 0; y < h; ++y) {
        int sum = 0;
        for (int x = -radius; x <= radius; ++x)
            sum += alpha[y * w + std::clamp(x, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            tmp[y * w + x] = static_cast<uint8_t>(sum / window);
            sum -= alpha[y * w + std::clamp(x - radius, 0, w - 1)];
            sum += alpha[y * w + std::clamp(x + radius + 1, 0, w - 1)];
        }
    }
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int y = -radius; y <= radius; ++y)
            sum += tmp[std::clamp(y, 0, h - 1) * w + x];
        for (int y = 0; y < h; ++y) {
            alpha[y * w + x] = static_cast<uint8_t>(sum / window);
            sum -= tmp[std::clamp(y - radius, 0, h - 1) * w + x];
            sum += tmp[std::clamp(y + radius + 1, 0, h - 1) * w + x];
        }
    }
}

static std::vector<int> blur_pass_radii(double blur, int blur_type)
{
    const int radius = (int)std::round(std::max(0.0, blur));
    if (radius <= 0)
        return {};

    switch ((ShadowBlurType)std::clamp(blur_type, 0, (int)ShadowBlurType::DualKawase)) {
    case ShadowBlurType::Box:
        return {radius};
    case ShadowBlurType::Gaussian: {
        const int r = std::max(1, (int)std::round(radius * 0.58));
        return {r, r, r};
    }
    case ShadowBlurType::Triangular: {
        const int r = std::max(1, (int)std::round(radius * 0.5));
        return {r, r};
    }
    case ShadowBlurType::DualKawase:
        return {std::max(1, radius / 3), std::max(1, radius / 4), std::max(1, radius / 5)};
    case ShadowBlurType::AlphaMask:
        return {std::max(1, radius / 2)};
    case ShadowBlurType::StackFast:
    default:
        return {std::max(1, radius / 2), std::max(1, radius / 3)};
    }
}

static void blur_alpha_for_type(std::vector<uint8_t> &alpha, int w, int h, double blur, int blur_type)
{
    for (int radius : blur_pass_radii(blur, blur_type))
        box_blur_alpha(alpha, w, h, radius);
}

static int shadow_blur_type_for_long_shadow(int blur_type)
{
    switch ((LongShadowBlurType)std::clamp(blur_type, 0, (int)LongShadowBlurType::StackFast)) {
    case LongShadowBlurType::Box: return (int)ShadowBlurType::Box;
    case LongShadowBlurType::Gaussian: return (int)ShadowBlurType::Gaussian;
    case LongShadowBlurType::StackFast: return (int)ShadowBlurType::StackFast;
    case LongShadowBlurType::None:
    default: return -1;
    }
}

static void composite_mask_alpha(std::vector<uint8_t> &dst, int dw, int dh, const QImage &mask,
                                 int ox, int oy, double opacity)
{
    if (opacity <= 0.0) return;
    const QImage src = mask.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < src.height(); ++y) {
        int dy = y + oy;
        if (dy < 0 || dy >= dh) continue;
        const QRgb *line = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            int dx = x + ox;
            if (dx < 0 || dx >= dw) continue;
            int a = (int)std::round(qAlpha(line[x]) * opacity);
            if (a <= 0) continue;
            uint8_t &d = dst[dy * dw + dx];
            d = (uint8_t)std::min(255, (int)d + a - ((int)d * a) / 255);
        }
    }
}

static CachedShadowImage build_shadow_image(const Layer &layer, const ShadowRenderParams &p,
                                            const QString &shape_key, const QImage &mask)
{
    static std::mutex cache_mutex;
    static std::unordered_map<std::string, CachedShadowImage> cache;
    const QString key = shadow_param_key(layer, p, shape_key);
    const std::string skey = key.toStdString();
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(skey);
        if (it != cache.end()) return it->second;
    }

    const double long_rad = p.long_angle * kPi / 180.0;
    const double long_dx = p.long_enabled ? std::cos(long_rad) * p.long_length : 0.0;
    const double long_dy = p.long_enabled ? std::sin(long_rad) * p.long_length : 0.0;
    const int blur_pad = (int)std::ceil(std::max(0.0, p.blur) * 3.0 + p.spread + 2.0);
    const double min_x = std::min({0.0, p.dx, long_dx}) - blur_pad;
    const double min_y = std::min({0.0, p.dy, long_dy}) - blur_pad;
    const double max_x = std::max({0.0, p.dx, long_dx}) + mask.width() + blur_pad;
    const double max_y = std::max({0.0, p.dy, long_dy}) + mask.height() + blur_pad;
    const int w = std::max(1, (int)std::ceil(max_x - min_x));
    const int h = std::max(1, (int)std::ceil(max_y - min_y));
    std::vector<uint8_t> drop_alpha((size_t)w * (size_t)h, 0);
    std::vector<uint8_t> long_alpha((size_t)w * (size_t)h, 0);

    auto add_alpha = [&](std::vector<uint8_t> &target, double opacity, double x, double y) {
        composite_mask_alpha(target, w, h, mask,
                             (int)std::round(x - min_x), (int)std::round(y - min_y), opacity);
        if (p.spread > 0.0) {
            const int s = (int)std::ceil(p.spread);
            for (int oy : {-s, 0, s})
                for (int ox : {-s, 0, s})
                    if (ox || oy) composite_mask_alpha(target, w, h, mask,
                        (int)std::round(x + ox - min_x), (int)std::round(y + oy - min_y), opacity * 0.55);
        }
    };

    if (p.long_enabled) {
        /* Sharp long shadows are a cheap directional extrusion by default.
         * Use a coarse, adaptive number of stamps so editing length/angle remains responsive;
         * optional long-shadow blur is applied only when explicitly selected.
         */
        const int steps = std::clamp((int)std::ceil(p.long_length / 16.0), 1, 32);
        for (int i = steps; i >= 1; --i) {
            const double u = (double)i / steps;
            const double fade = std::pow(1.0 - u, p.long_falloff);
            add_alpha(long_alpha, p.long_opacity * fade, long_dx * u, long_dy * u);
        }
        const int mapped_long_blur = shadow_blur_type_for_long_shadow(p.long_blur_type);
        if (mapped_long_blur >= 0 && p.long_blur > 0.0)
            blur_alpha_for_type(long_alpha, w, h, p.long_blur, mapped_long_blur);
    }
    if (p.drop_enabled && p.opacity > 0.0) {
        add_alpha(drop_alpha, p.opacity, p.dx, p.dy);
        blur_alpha_for_type(drop_alpha, w, h, p.blur, p.blur_type);
    }

    QColor dc = color_from_argb(p.color);
    QColor lc = color_from_argb(p.long_color);
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; ++x) {
            int la = long_alpha[y * w + x];
            int da = drop_alpha[y * w + x];
            int out_a = la + da - (la * da) / 255;
            if (out_a <= 0) { line[x] = qRgba(0, 0, 0, 0); continue; }
            int pr = (lc.red() * la + dc.red() * da * (255 - la) / 255) / 255;
            int pg = (lc.green() * la + dc.green() * da * (255 - la) / 255) / 255;
            int pb = (lc.blue() * la + dc.blue() * da * (255 - la) / 255) / 255;
            pr = std::clamp(pr, 0, 255);
            pg = std::clamp(pg, 0, 255);
            pb = std::clamp(pb, 0, 255);
            line[x] = qRgba(pr, pg, pb, out_a);
        }
    }
    CachedShadowImage out{image, QPointF(min_x, min_y)};
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (cache.size() > 128) cache.clear();
        cache[skey] = out;
    }
    return out;
}

static void paint_qimage(cairo_t *cr, const QImage &image, double x, double y, double alpha)
{
    if (!cr || image.isNull() || image.width() <= 0 || image.height() <= 0 || alpha <= 0.0) return;
    auto surface = make_image_surface_for_const_qimage(image);
    if (!surface) return;
    cairo_set_source_surface(cr, surface.get(), x, y);
    cairo_paint_with_alpha(cr, std::clamp(alpha, 0.0, 1.0));
}

/* ══════════════════════════════════════════════════════════════════
 *  Cairo rendering
 * ══════════════════════════════════════════════════════════════════ */


static QLocale locale_for_text_transform(const QString &text)
{
    QLocale locale;
    for (const QChar ch : text) {
        uint u = ch.unicode();
        if (u >= 0x0370 && u <= 0x03FF)
            return QLocale(QLocale::Greek, QLocale::Greece);
        if (QStringLiteral("ıİşŞğĞçÇ").contains(ch))
            return QLocale(QLocale::Turkish, QLocale::Turkey);
        if (ch == QChar(0x00DF))
            return QLocale(QLocale::German, QLocale::Germany);
    }
    return locale;
}


static QString php_date_format(const QString &format, const QDateTime &date_time)
{
    QString out;
    const QDate date = date_time.date();
    const QTime time = date_time.time();
    for (int i = 0; i < format.size(); ++i) {
        const QChar token = format.at(i);
        if (token == QLatin1Char('\\') && i + 1 < format.size()) {
            out.append(format.at(++i));
            continue;
        }
        switch (token.unicode()) {
        case 'd': out += QString("%1").arg(date.day(), 2, 10, QChar('0')); break;
        case 'D': out += date_time.toString("ddd"); break;
        case 'j': out += QString::number(date.day()); break;
        case 'l': out += date_time.toString("dddd"); break;
        case 'F': out += date_time.toString("MMMM"); break;
        case 'm': out += QString("%1").arg(date.month(), 2, 10, QChar('0')); break;
        case 'M': out += date_time.toString("MMM"); break;
        case 'n': out += QString::number(date.month()); break;
        case 'Y': out += QString::number(date.year()); break;
        case 'y': out += QString("%1").arg(date.year() % 100, 2, 10, QChar('0')); break;
        case 'a': out += (time.hour() < 12 ? "am" : "pm"); break;
        case 'A': out += (time.hour() < 12 ? "AM" : "PM"); break;
        case 'g': { int h = time.hour() % 12; out += QString::number(h == 0 ? 12 : h); break; }
        case 'G': out += QString::number(time.hour()); break;
        case 'h': { int h = time.hour() % 12; out += QString("%1").arg(h == 0 ? 12 : h, 2, 10, QChar('0')); break; }
        case 'H': out += QString("%1").arg(time.hour(), 2, 10, QChar('0')); break;
        case 'i': out += QString("%1").arg(time.minute(), 2, 10, QChar('0')); break;
        case 's': out += QString("%1").arg(time.second(), 2, 10, QChar('0')); break;
        case 'U': out += QString::number(date_time.toSecsSinceEpoch()); break;
        default: out.append(token); break;
        }
    }
    return out;
}

static QString clock_text_for_layer(const Layer &layer)
{
    QString format = QString::fromStdString(layer.clock_format);
    if (format.isEmpty()) format = QStringLiteral("H:i:s");
    return php_date_format(format, QDateTime::currentDateTime());
}

static QString display_text_for_style(const Layer &layer)
{
    QString text = layer.type == LayerType::Clock
        ? clock_text_for_layer(layer)
        : QString::fromStdString(layer.text_content);
    if (layer.text_style == 1)
        return locale_for_text_transform(text).toUpper(text);
    return text;
}

static void apply_text_style_to_font(QFont &font, const Layer &layer)
{
    if (layer.text_style == 2)
        font.setCapitalization(QFont::SmallCaps);
    if (layer.text_style == 3 || layer.text_style == 4)
        font.setPixelSize(std::max(1, (int)std::round(font.pixelSize() * 0.65)));
}

static QFont font_for_layer(const Layer &layer, double t)
{
    const QString family = QString::fromStdString(layer.font_family);
    const QString style = QString::fromStdString(layer.font_style);
    QFontDatabase fdb;
    QFont font = !style.isEmpty()
        ? fdb.font(family, style, (int)std::round(eval_text_font_size(layer, t)))
        : QFont(family);
    font.setFamily(family);
    font.setPixelSize((int)std::round(eval_text_font_size(layer, t)));
    if (!style.isEmpty())
        font.setStyleName(style);
    font.setBold(layer.font_bold);
    font.setItalic(layer.font_italic);
    font.setUnderline(layer.text_underline);
    font.setStrikeOut(layer.text_strikethrough);
    font.setKerning(layer.kerning_mode != 2 && layer.font_kerning);
    const float effective_tracking = (float)eval_char_tracking(layer, t) + (layer.kerning_mode == 2 ? layer.manual_kerning : 0.0f);
    font.setLetterSpacing(QFont::AbsoluteSpacing, effective_tracking);
    font.setStretch(std::clamp((int)std::round(eval_char_scale_x(layer, t) * 100.0), 1, 4000));
    apply_text_style_to_font(font, layer);
    return font;
}

static QPainterPath apply_vertical_character_scale(const QPainterPath &path, const QRectF &rect,
                                                   Qt::Alignment alignment, const Layer &layer, double t = 0.0)
{
    double scale_y = std::clamp(eval_char_scale_y(layer, t), 0.1, 5.0);
    if (std::abs(scale_y - 1.0) < 0.0001)
        return path;

    QRectF bounds = path.boundingRect();
    double anchor_y = bounds.top();
    if (alignment & Qt::AlignVCenter)
        anchor_y = bounds.center().y();
    else if (alignment & Qt::AlignBottom)
        anchor_y = bounds.bottom();
    else if (!bounds.isEmpty())
        anchor_y = rect.top();

    QTransform xf;
    xf.translate(0.0, anchor_y);
    xf.scale(1.0, scale_y);
    xf.translate(0.0, -anchor_y);
    return xf.map(path);
}

static QRectF text_rect_for_style(const QRectF &rect, const Layer &layer)
{
    if (layer.text_style == 3)
        return rect.adjusted(0.0, 0.0, 0.0, -rect.height() * 0.28);
    if (layer.text_style == 4)
        return rect.adjusted(0.0, rect.height() * 0.28, 0.0, 0.0);
    return rect;
}

static QString overflow_layout_text(const QString &text, const Layer &layer)
{
    if (layer.text_overflow_mode == 2) {
        QString single = text;
        single.replace('\r', ' ');
        single.replace('\n', ' ');
        return single;
    }
    return text;
}

static double eval_text_font_size(const Layer &layer, double t)
{
    return std::clamp(layer.font_size_prop.is_animated()
                          ? layer.font_size_prop.evaluate(t)
                          : (double)layer.font_size,
                      1.0, 512.0);
}

static double eval_char_tracking(const Layer &layer, double t)
{
    return std::clamp(layer.char_tracking_prop.is_animated()
                          ? layer.char_tracking_prop.evaluate(t)
                          : (double)layer.char_tracking,
                      -1000.0, 1000.0);
}

static double eval_char_scale_x(const Layer &layer, double t)
{
    return std::clamp(layer.char_scale_x_prop.is_animated()
                          ? layer.char_scale_x_prop.evaluate(t)
                          : (double)layer.char_scale_x,
                      0.01, 100.0);
}

static double eval_char_scale_y(const Layer &layer, double t)
{
    return std::clamp(layer.char_scale_y_prop.is_animated()
                          ? layer.char_scale_y_prop.evaluate(t)
                          : (double)layer.char_scale_y,
                      0.01, 100.0);
}

static double eval_baseline_shift(const Layer &layer, double t)
{
    return std::clamp(layer.baseline_shift_prop.is_animated()
                          ? layer.baseline_shift_prop.evaluate(t)
                          : (double)layer.baseline_shift,
                      -1000.0, 1000.0);
}

static double eval_paragraph_space_before(const Layer &layer, double t)
{
    return std::clamp(layer.paragraph_space_before_prop.is_animated()
                          ? layer.paragraph_space_before_prop.evaluate(t)
                          : (double)layer.paragraph_space_before,
                      -10000.0, 10000.0);
}

static double eval_paragraph_space_after(const Layer &layer, double t)
{
    return std::clamp(layer.paragraph_space_after_prop.is_animated()
                          ? layer.paragraph_space_after_prop.evaluate(t)
                          : (double)layer.paragraph_space_after,
                      -10000.0, 10000.0);
}

static double eval_paragraph_indent_left(const Layer &layer, double t)
{
    return std::clamp(layer.paragraph_indent_left_prop.is_animated()
                          ? layer.paragraph_indent_left_prop.evaluate(t)
                          : (double)layer.paragraph_indent_left,
                      -10000.0, 10000.0);
}

static double eval_paragraph_indent_right(const Layer &layer, double t)
{
    return std::clamp(layer.paragraph_indent_right_prop.is_animated()
                          ? layer.paragraph_indent_right_prop.evaluate(t)
                          : (double)layer.paragraph_indent_right,
                      -10000.0, 10000.0);
}

static double eval_paragraph_indent_first_line(const Layer &layer, double t)
{
    return std::clamp(layer.paragraph_indent_first_line_prop.is_animated()
                          ? layer.paragraph_indent_first_line_prop.evaluate(t)
                          : (double)layer.paragraph_indent_first_line,
                      -10000.0, 10000.0);
}

static double horizontal_fit_scale(const QFont &font, const QRectF &rect,
                                   const QString &text, const Layer &layer, double)
{
    if (layer.text_overflow_mode != 2) return 1.0;
    QFontMetricsF metrics(font);
    const double text_width = static_cast<double>(metrics.horizontalAdvance(overflow_layout_text(text, layer)));
    double natural_width = std::max(1.0, text_width);
    if (natural_width <= rect.width()) return 1.0;
    return std::clamp(rect.width() / natural_width,
                      std::clamp((double)layer.text_fit_min_scale, 0.05, 1.0),
                      1.0);
}

static QPainterPath text_overflow_path(const QFont &font, const QRectF &rect,
                                       Qt::Alignment alignment, const QString &text,
                                       const Layer &layer, double t, double *fit_scale = nullptr)
{
    QPainterPath path;
    QFontMetricsF metrics(font);
    const double indent_left = eval_paragraph_indent_left(layer, t);
    const double indent_right = eval_paragraph_indent_right(layer, t);
    const double first_indent = eval_paragraph_indent_first_line(layer, t);
    const double space_before = eval_paragraph_space_before(layer, t);
    const double space_after = eval_paragraph_space_after(layer, t);
    const double paragraph_left = rect.left() + indent_left;
    const double paragraph_right = rect.right() - indent_right;
    const double paragraph_width = std::max(1.0, paragraph_right - paragraph_left);
    const int align_h = std::clamp(layer.align_h, 0, 6);

    auto aligned_x = [&](double line_left, double line_width, double text_width, int mode) {
        if (mode == 1 || mode == 4)
            return line_left + (line_width - text_width) / 2.0;
        if (mode == 2 || mode == 5)
            return line_left + line_width - text_width;
        return line_left;
    };

    if (layer.text_overflow_mode == 2) {
        QString single = overflow_layout_text(text, layer);
        QRectF bounds = metrics.boundingRect(single);
        QRectF fit_rect(paragraph_left + first_indent, rect.top(),
                        std::max(1.0, paragraph_width - first_indent), rect.height());
        double scale = horizontal_fit_scale(font, fit_rect, text, layer, t);
        if (fit_scale) *fit_scale = scale;
        double visual_width = bounds.width() * scale;
        double x = aligned_x(fit_rect.left(), fit_rect.width(), visual_width, align_h);
        double y = rect.top() - bounds.top();
        if (alignment & Qt::AlignVCenter) y = rect.top() + (rect.height() - bounds.height()) / 2.0 - bounds.top();
        else if (alignment & Qt::AlignBottom) y = rect.bottom() - bounds.height() - bounds.top();
        path.addText(QPointF(0, y), font, single);
        QTransform xf;
        xf.translate(x, 0.0);
        xf.scale(scale, 1.0);
        return xf.map(path);
    }
    if (fit_scale) *fit_scale = 1.0;

    struct Line {
        QString text;
        double width = 0.0;
        double ascent = 0.0;
        double height = 0.0;
        int paragraph = 0;
        bool first_in_paragraph = false;
        bool last_in_paragraph = false;
    };
    std::vector<Line> lines;
    const QStringList paragraphs = text.split('\n');
    QTextOption option;
    option.setWrapMode(layer.text_overflow_mode == 0
                           ? (layer.paragraph_hyphenate ? QTextOption::WrapAnywhere : QTextOption::WrapAtWordBoundaryOrAnywhere)
                           : QTextOption::NoWrap);
    for (int paragraph_index = 0; paragraph_index < paragraphs.size(); ++paragraph_index) {
        const QString &paragraph = paragraphs[paragraph_index];
        const size_t first_line_index = lines.size();
        if (paragraph.isEmpty()) {
            lines.push_back({QString(), 0.0, metrics.ascent(), metrics.lineSpacing(), paragraph_index, true, true});
            continue;
        }
        QTextLayout layout(paragraph, font);
        layout.setTextOption(option);
        layout.beginLayout();
        int line_index = 0;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            const double line_indent = line_index == 0 ? first_indent : 0.0;
            const double line_width = std::max(1.0, paragraph_width - line_indent);
            line.setLineWidth(layer.text_overflow_mode == 0 ? line_width : 1000000.0);
            int start = line.textStart();
            int len = line.textLength();
            lines.push_back({paragraph.mid(start, len), line.naturalTextWidth(), line.ascent(), line.height(),
                             paragraph_index, line_index == 0, false});
            ++line_index;
            if (layer.text_overflow_mode != 0) break;
        }
        layout.endLayout();
        if (lines.size() > first_line_index)
            lines.back().last_in_paragraph = true;
    }
    double total_height = 0.0;
    const double leading = std::clamp((double)layer.text_leading, -200.0, 500.0);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].first_in_paragraph) total_height += space_before;
        total_height += lines[i].height;
        if (lines[i].last_in_paragraph)
            total_height += space_after;
        else if (i + 1 < lines.size())
            total_height += leading;
    }
    double y = rect.top();
    if (alignment & Qt::AlignVCenter) y = rect.top() + (rect.height() - total_height) / 2.0;
    else if (alignment & Qt::AlignBottom) y = rect.bottom() - total_height;

    auto add_justified_line = [&](const Line &line, double line_left, double line_width, double baseline_y) {
        QStringList words = line.text.simplified().split(' ', Qt::SkipEmptyParts);
        if (words.size() < 2 || line.width >= line_width) {
            path.addText(QPointF(line_left, baseline_y), font, line.text);
            return;
        }
        double words_width = 0.0;
        for (const QString &word : words)
            words_width += metrics.horizontalAdvance(word);
        const double extra_space = std::max(0.0, (line_width - words_width) / (words.size() - 1));
        double word_x = line_left;
        for (int i = 0; i < words.size(); ++i) {
            path.addText(QPointF(word_x, baseline_y), font, words[i]);
            word_x += metrics.horizontalAdvance(words[i]) + extra_space;
        }
    };

    for (const auto &line : lines) {
        if (line.first_in_paragraph) y += space_before;
        const double line_indent = line.first_in_paragraph ? first_indent : 0.0;
        const double line_left = paragraph_left + line_indent;
        const double line_width = std::max(1.0, paragraph_width - line_indent);
        const bool justify_line = align_h == 6 || (align_h >= 3 && align_h <= 5 && !line.last_in_paragraph);
        if (justify_line) {
            add_justified_line(line, line_left, line_width, y + line.ascent);
        } else {
            double x = aligned_x(line_left, line_width, line.width, align_h);
            path.addText(QPointF(x, y + line.ascent), font, line.text);
        }
        y += line.height;
        if (line.last_in_paragraph)
            y += space_after;
        else
            y += leading;
    }
    return path;
}


static double ticker_time_seconds()
{
    return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

static QStringList ticker_lines(const QString &text)
{
    QString normalized = text;
    normalized.replace('\r', '\n');
    QStringList raw_lines = normalized.split('\n');
    QStringList lines;
    for (const QString &line : raw_lines) {
        if (!line.trimmed().isEmpty())
            lines << line;
    }
    if (lines.isEmpty()) lines << QString();
    return lines;
}

static QPainterPath ticker_text_path(const QFont &font, const QRectF &rect,
                                     Qt::Alignment alignment, const QString &text,
                                     const Layer &layer)
{
    QPainterPath path;
    QFontMetricsF metrics(font);
    const double speed = std::max(1.0, layer.ticker_speed);
    const double now = ticker_time_seconds();

    if (layer.ticker_style == 0) {
        QString single = text;
        single.replace('\r', ' ');
        single.replace('\n', QStringLiteral("     •     "));
        QRectF bounds = metrics.boundingRect(single);
        const double text_w = std::max(1.0, bounds.width());
        const double travel = rect.width() + text_w;
        const double progress = std::fmod(now * speed, travel);
        const double x = layer.ticker_direction == 0
            ? rect.left() - text_w + progress
            : rect.right() - progress;
        double y = rect.top() - bounds.top();
        if (alignment & Qt::AlignVCenter) y = rect.top() + (rect.height() - bounds.height()) / 2.0 - bounds.top();
        else if (alignment & Qt::AlignBottom) y = rect.bottom() - bounds.height() - bounds.top();
        path.addText(QPointF(x, y), font, single);
        return path;
    }

    const QStringList lines = ticker_lines(text);
    const int line_count = std::max(1, static_cast<int>(lines.size()));
    const double line_h = std::max(1.0, metrics.lineSpacing() + std::clamp((double)layer.text_leading, -200.0, 500.0));
    if (layer.ticker_style == 1) {
        const double hold = std::max(0.1, layer.ticker_line_hold);
        int idx = (int)std::floor(now / hold) % line_count;
        if (layer.ticker_direction == 0) idx = line_count - 1 - idx;
        QString line = lines.at(idx);
        double line_w = metrics.horizontalAdvance(line);
        double x = rect.left();
        if (alignment & Qt::AlignHCenter) x = rect.left() + (rect.width() - line_w) / 2.0;
        else if (alignment & Qt::AlignRight) x = rect.right() - line_w;
        QRectF bounds = metrics.boundingRect(line);
        double y = rect.top() + (rect.height() - bounds.height()) / 2.0 - bounds.top();
        path.addText(QPointF(x, y), font, line);
        return path;
    }

    const double content_h = line_count * line_h;
    const double travel = rect.height() + content_h;
    const double progress = std::fmod(now * speed, travel);
    double y = layer.ticker_direction == 0
        ? rect.top() - content_h + progress
        : rect.bottom() - progress;
    for (const QString &line : lines) {
        double line_w = metrics.horizontalAdvance(line);
        double x = rect.left();
        if (alignment & Qt::AlignHCenter) x = rect.left() + (rect.width() - line_w) / 2.0;
        else if (alignment & Qt::AlignRight) x = rect.right() - line_w;
        path.addText(QPointF(x, y + metrics.ascent()), font, line);
        y += line_h;
    }
    return path;
}

static QColor color_from_argb(uint32_t argb)
{
    return QColor((argb >> 16) & 0xFF,
                  (argb >> 8) & 0xFF,
                  argb & 0xFF,
                  (argb >> 24) & 0xFF);
}

static bool eval_background_enabled(const Layer &layer, double t)
{
    const bool legacy = layer.background_enabled_prop.is_animated()
        ? layer.background_enabled_prop.evaluate(t) >= 0.5
        : layer.background_enabled;
    return layer_effect_enabled(layer, LayerEffectType::BackgroundColor, legacy);
}

static double eval_background_opacity(const Layer &layer, double t)
{
    return std::clamp(layer.background_opacity_prop.is_animated()
                          ? layer.background_opacity_prop.evaluate(t)
                          : (double)layer.background_opacity,
                      0.0, 1.0);
}

static double eval_background_padding_x(const Layer &layer, double t)
{
    return std::max(0.0, layer.background_padding_x_prop.is_animated()
                             ? layer.background_padding_x_prop.evaluate(t)
                             : (double)layer.background_padding_x);
}

static double eval_background_padding_y(const Layer &layer, double t)
{
    return std::max(0.0, layer.background_padding_y_prop.is_animated()
                             ? layer.background_padding_y_prop.evaluate(t)
                             : (double)layer.background_padding_y);
}

static double eval_background_corner_radius(const Layer &layer, double t)
{
    return std::max(0.0, layer.background_corner_radius_prop.is_animated()
                             ? layer.background_corner_radius_prop.evaluate(t)
                             : (double)layer.background_corner_radius);
}

static uint32_t eval_background_color(const Layer &layer, double t)
{
    return ((uint32_t)eval_channel(layer.background_color_a, (layer.background_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(layer.background_color_r, (layer.background_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(layer.background_color_g, (layer.background_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(layer.background_color_b, layer.background_color & 0xFF, t);
}

static QColor evaluated_background_color(const Layer &layer, double t)
{
    QColor color = color_from_argb(eval_background_color(layer, t));
    color.setAlphaF(std::clamp((double)color.alphaF() * eval_background_opacity(layer, t), 0.0, 1.0));
    return color;
}




static bool rich_text_format_differs_from_default(const RichTextCharFormat &a, const RichTextCharFormat &b)
{
    return a.font_family != b.font_family || a.font_style != b.font_style ||
           a.font_size != b.font_size || a.bold != b.bold || a.italic != b.italic ||
           a.underline != b.underline || a.strikethrough != b.strikethrough ||
           a.kerning != b.kerning || a.kerning_mode != b.kerning_mode ||
           std::abs(a.manual_kerning - b.manual_kerning) >= 0.0001f ||
           std::abs(a.tracking - b.tracking) >= 0.0001f ||
           std::abs(a.scale_x - b.scale_x) >= 0.0001f ||
           std::abs(a.scale_y - b.scale_y) >= 0.0001f ||
           std::abs(a.baseline_shift - b.baseline_shift) >= 0.0001f ||
           a.text_style != b.text_style || a.ligatures != b.ligatures ||
           a.stylistic_alternates != b.stylistic_alternates || a.fractions != b.fractions ||
           a.opentype_features != b.opentype_features || a.language != b.language ||
           a.fill.type != b.fill.type || a.fill.color != b.fill.color ||
           a.fill.gradient_type != b.fill.gradient_type ||
           a.fill.gradient_start_color != b.fill.gradient_start_color ||
           a.fill.gradient_end_color != b.fill.gradient_end_color ||
           std::abs(a.fill.gradient_start_pos - b.fill.gradient_start_pos) >= 0.0001f ||
           std::abs(a.fill.gradient_end_pos - b.fill.gradient_end_pos) >= 0.0001f ||
           std::abs(a.fill.gradient_start_opacity - b.fill.gradient_start_opacity) >= 0.0001f ||
           std::abs(a.fill.gradient_end_opacity - b.fill.gradient_end_opacity) >= 0.0001f ||
           std::abs(a.fill.gradient_opacity - b.fill.gradient_opacity) >= 0.0001f ||
           std::abs(a.fill.gradient_angle - b.fill.gradient_angle) >= 0.0001f ||
           std::abs(a.fill.gradient_center_x - b.fill.gradient_center_x) >= 0.0001f ||
           std::abs(a.fill.gradient_center_y - b.fill.gradient_center_y) >= 0.0001f ||
           std::abs(a.fill.gradient_scale - b.fill.gradient_scale) >= 0.0001f ||
           std::abs(a.fill.gradient_focal_x - b.fill.gradient_focal_x) >= 0.0001f ||
           std::abs(a.fill.gradient_focal_y - b.fill.gradient_focal_y) >= 0.0001f;
}

static bool rich_text_model_requires_document_renderer(const RichTextDocument &model)
{
    if (model.plain_text.empty() || model.ranges.empty())
        return false;
    if (model.ranges.size() > 1)
        return true;
    const RichTextRange &range = model.ranges.front();
    return range.start != 0 || range.length != model.plain_text.size() ||
           rich_text_format_differs_from_default(range.format, model.default_format);
}

static void apply_rich_text_extended_font_properties(QFont &font, const RichTextCharFormat &format)
{
    if (!format.font_style.empty())
        font.setStyleName(QString::fromStdString(format.font_style));
    font.setKerning(format.kerning_mode != 2 && format.kerning);
    font.setLetterSpacing(QFont::AbsoluteSpacing,
                          format.tracking + (format.kerning_mode == 2 ? format.manual_kerning : 0.0f));
    font.setStretch(std::clamp((int)std::round(format.scale_x * 100.0f), 1, 4000));
    font.setCapitalization(QFont::MixedCase);
    if (format.text_style == 1)
        font.setCapitalization(QFont::AllUppercase);
    else if (format.text_style == 2)
        font.setCapitalization(QFont::SmallCaps);
    if (format.text_style == 3 || format.text_style == 4)
        font.setPixelSize(std::max(1, (int)std::round(font.pixelSize() * 0.65)));
    if (!format.ligatures)
        font.setStyleStrategy((QFont::StyleStrategy)(font.styleStrategy() | QFont::PreferNoShaping));
}

static void apply_rich_text_extended_char_format(QTextCharFormat &out, const RichTextCharFormat &format)
{
    if (format.text_style == 3)
        out.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
    else if (format.text_style == 4)
        out.setVerticalAlignment(QTextCharFormat::AlignSubScript);
    else
        out.setVerticalAlignment(QTextCharFormat::AlignNormal);
    out.setFontKerning(format.kerning_mode != 2 && format.kerning);
    out.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    out.setFontLetterSpacing(format.tracking + (format.kerning_mode == 2 ? format.manual_kerning : 0.0f));
}

static QBrush rich_text_fill_brush(const RichTextFill &fill, const QRectF &box)
{
    if (fill.type != 1)
        return QBrush(color_from_argb(fill.color));

    return make_gradient_brush(fill.gradient_type, box,
                               std::clamp((double)fill.gradient_opacity, 0.0, 1.0),
                               fill.gradient_center_x, fill.gradient_center_y,
                               fill.gradient_focal_x, fill.gradient_focal_y,
                               fill.gradient_scale, fill.gradient_angle,
                               std::clamp((double)fill.gradient_start_pos, 0.0, 1.0),
                               fill.gradient_start_color, fill.gradient_start_opacity,
                               std::clamp((double)fill.gradient_end_pos, 0.0, 1.0),
                               fill.gradient_end_color, fill.gradient_end_opacity,
                               {});
}

static QTextCharFormat text_format_from_rich_format(const RichTextCharFormat &format, const QFont &fallback_font,
                                                     const QRectF &text_rect)
{
    QTextCharFormat out;
    QFont font = fallback_font;
    if (!format.font_family.empty()) font.setFamily(QString::fromStdString(format.font_family));
    font.setPixelSize(std::max(1, format.font_size));
    font.setBold(format.bold);
    font.setItalic(format.italic);
    font.setUnderline(format.underline);
    font.setStrikeOut(format.strikethrough);
    apply_rich_text_extended_font_properties(font, format);
    out.setFont(font);
    out.setFontUnderline(format.underline);
    out.setFontStrikeOut(format.strikethrough);
    apply_rich_text_extended_char_format(out, format);
    out.setForeground(rich_text_fill_brush(format.fill, text_rect));
    return out;
}

static void apply_rich_text_ranges(QTextDocument &doc, const Layer &layer, const QFont &font,
                                   const QRectF &text_rect)
{
    const RichTextDocument &model = layer.rich_text;
    QTextCursor all(&doc);
    all.select(QTextCursor::Document);
    all.mergeCharFormat(text_format_from_rich_format(model.default_format, font, text_rect));
    for (const auto &range : model.ranges) {
        if (range.length == 0 || range.start >= model.plain_text.size()) continue;
        QTextCursor cursor(&doc);
        cursor.setPosition((int)std::min(range.start, model.plain_text.size()));
        cursor.setPosition((int)std::min(range.start + range.length, model.plain_text.size()), QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(text_format_from_rich_format(range.format, font, text_rect));
    }
}

static std::unique_ptr<QTextDocument> rich_text_document_for_layer(const Layer &layer, const QFont &font,
                                                  const QRectF &text_rect, double t)
{
    auto doc = std::make_unique<QTextDocument>();
    doc->setDocumentMargin(0.0);
    doc->setDefaultFont(font);
    const QColor default_color = color_from_argb(eval_text_color(layer, t));
    doc->setDefaultStyleSheet(QStringLiteral("body{color:%1;}").arg(default_color.name(QColor::HexRgb)));
    QTextOption option = doc->defaultTextOption();
    option.setUseDesignMetrics(true);
    option.setWrapMode(layer.text_overflow_mode == 0
                           ? (layer.paragraph_hyphenate ? QTextOption::WrapAnywhere : QTextOption::WrapAtWordBoundaryOrAnywhere)
                           : QTextOption::NoWrap);
    Qt::Alignment align = Qt::AlignLeft;
    if (layer.align_h == 1 || layer.align_h == 4) align = Qt::AlignHCenter;
    else if (layer.align_h == 2 || layer.align_h == 5) align = Qt::AlignRight;
    else if (layer.align_h >= 3) align = Qt::AlignJustify;
    option.setAlignment(align);
    doc->setDefaultTextOption(option);
    doc->setTextWidth(layer.text_overflow_mode == 2 ? -1.0 : std::max(1.0, text_rect.width()));
    if (layer.type != LayerType::Ticker && (!layer.rich_text.plain_text.empty() || !layer.rich_text.ranges.empty())) {
        doc->setPlainText(QString::fromStdString(layer.rich_text.plain_text));
        apply_rich_text_ranges(*doc, layer, font, QRectF(0.0, 0.0, text_rect.width(), text_rect.height()));
    } else if (!layer.rich_text_html.empty()) {
        doc->setHtml(QString::fromStdString(layer.rich_text_html));
    } else {
        doc->setPlainText(display_text_for_style(layer));
    }

    return doc;
}

static void draw_rich_text_document(QPainter &painter, const Layer &layer, const QFont &font,
                                    const QRectF &text_rect, double t)
{
    auto doc = rich_text_document_for_layer(layer, font, text_rect, t);
    QSizeF doc_size = doc->size();
    QPointF origin = text_rect.topLeft();
    if (layer.align_v == 1)
        origin.setY(text_rect.top() + (text_rect.height() - doc_size.height()) / 2.0);
    else if (layer.align_v == 2)
        origin.setY(text_rect.bottom() - doc_size.height());
    if (std::abs(eval_baseline_shift(layer, t)) > 0.0001)
        origin.setY(origin.y() - eval_baseline_shift(layer, t));
    if (layer.text_overflow_mode == 2 && doc_size.width() > text_rect.width()) {
        const double scale = std::clamp(text_rect.width() / std::max(1.0, doc_size.width()),
                                        std::clamp((double)layer.text_fit_min_scale, 0.05, 1.0), 1.0);
        painter.save();
        painter.translate(origin);
        painter.scale(scale, 1.0);
        doc->drawContents(&painter, QRectF(QPointF(0, 0), doc_size));
        painter.restore();
        return;
    }
    painter.save();
    painter.translate(origin);
    doc->drawContents(&painter, QRectF(QPointF(0, 0), doc_size));
    painter.restore();
}

static void render_layer_text(cairo_t *cr, const Title &title, const Layer &layer, double title_time,
                               int canvas_w, int canvas_h)
{
    (void)canvas_w;
    (void)canvas_h;

    const double t = std::max(0.0, title_time - layer.in_time);
    double alpha = layer_chain_opacity(title, layer, title_time);
    double box_w = eval_box_width(layer, t);
    double box_h = eval_box_height(layer, t);
    if (box_w <= 0.0 || box_h <= 0.0) return;

    ShadowRenderParams shadow_params = evaluated_shadow_params(layer, t);
    const bool render_shadow = shadow_params.drop_enabled || shadow_params.long_enabled;
    int pad = render_shadow
        ? (int)std::ceil(std::max({std::abs(shadow_params.dx), std::abs(shadow_params.dy), shadow_params.long_length}) + shadow_params.blur * 3.0 + shadow_params.spread + 4.0)
        : 0;
    if (eval_background_enabled(layer, t))
        pad += (int)std::ceil(std::max(eval_background_padding_x(layer, t), eval_background_padding_y(layer, t)));
    int img_w = std::max(1, (int)std::ceil(box_w) + pad * 2);
    int img_h = std::max(1, (int)std::ceil(box_h) + pad * 2);
    QImage text_image(img_w, img_h, QImage::Format_ARGB32_Premultiplied);
    text_image.fill(Qt::transparent);

    QPainter painter(&text_image);
    const bool previous_shape_aa = painter.testRenderHint(QPainter::Antialiasing);
    const bool previous_text_aa = painter.testRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font = font_for_layer(layer, t);
    painter.setFont(font);

    QRectF base_rect(pad, pad, box_w, box_h);
    if (eval_background_enabled(layer, t)) {
        const double bg_pad_x = eval_background_padding_x(layer, t);
        const double bg_pad_y = eval_background_padding_y(layer, t);
        const double bg_corner = eval_background_corner_radius(layer, t);
        QRectF bg_rect = base_rect.adjusted(-bg_pad_x, -bg_pad_y, bg_pad_x, bg_pad_y);
        QColor bg = evaluated_background_color(layer, t);
        if (bg.alpha() > 0 || layer.background_fill_type == 1) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(layer.background_fill_type == 1 ? background_gradient_fill_brush(layer, bg_rect, eval_background_opacity(layer, t)) : QBrush(bg));
            painter.drawRoundedRect(bg_rect, bg_corner, bg_corner);
        }
    }

    QRectF text_rect = text_rect_for_style(base_rect, layer);
    QString text = display_text_for_style(layer);
    painter.save();
    painter.setClipRect(text_rect);
    Qt::Alignment align = Qt::AlignVCenter | Qt::AlignHCenter;
    if (layer.align_h == 0) align = (align & ~Qt::AlignHorizontal_Mask) | Qt::AlignLeft;
    if (layer.align_h == 2) align = (align & ~Qt::AlignHorizontal_Mask) | Qt::AlignRight;
    if (layer.align_v == 0) align = (align & ~Qt::AlignVertical_Mask) | Qt::AlignTop;
    if (layer.align_v == 2) align = (align & ~Qt::AlignVertical_Mask) | Qt::AlignBottom;
    const bool has_rich_text = layer.type != LayerType::Ticker &&
        (rich_text_model_requires_document_renderer(layer.rich_text) || !layer.rich_text_html.empty());
    QPainterPath text_path;
    if (!has_rich_text) {
        text_path = layer.type == LayerType::Ticker
            ? ticker_text_path(font, text_rect, align, text, layer)
            : text_overflow_path(font, text_rect, align, text, layer, t);
        text_path = apply_vertical_character_scale(text_path, text_rect, align, layer, t);
        if (std::abs(eval_baseline_shift(layer, t)) > 0.0001)
            text_path.translate(0.0, -eval_baseline_shift(layer, t));
    }

    if (render_shadow) {
        QImage shadow_mask(img_w, img_h, QImage::Format_ARGB32_Premultiplied);
        shadow_mask.fill(Qt::transparent);
        QPainter mask_painter(&shadow_mask);
        mask_painter.setRenderHint(QPainter::Antialiasing, true);
        mask_painter.setPen(Qt::NoPen);
        mask_painter.setBrush(Qt::white);
        if (has_rich_text)
            draw_rich_text_document(mask_painter, layer, font, text_rect, t);
        else
            mask_painter.drawPath(text_path);
        mask_painter.end();
        const QString shape_key = QStringLiteral("text|%1x%2|%3")
            .arg(img_w).arg(img_h).arg(image_content_hash(shadow_mask));
        CachedShadowImage shadow = build_shadow_image(layer, shadow_params, shape_key, shadow_mask);
        painter.save();
        painter.setClipping(false);
        painter.drawImage(shadow.origin, shadow.image);
        painter.restore();
    }

    double outline_width = eval_outline_width(layer, t);
    QColor outline = color_from_argb(eval_outline_color(layer, t));
    outline.setAlphaF(std::clamp((double)outline.alphaF() * eval_outline_opacity(layer, t), 0.0, 1.0));
    QColor fill = color_from_argb(eval_text_color(layer, t));
    fill.setAlphaF(std::clamp((double)fill.alphaF(), 0.0, 1.0));
    auto draw_text_fill = [&]() {
        if (has_rich_text) {
            painter.save();
            painter.setOpacity(fill.alphaF());
            const bool legacy_html_gradient = !layer.rich_text_html.empty() &&
                                              layer.rich_text.plain_text.empty() &&
                                              layer.rich_text.ranges.empty() &&
                                              layer.fill_type == 1;
            if (legacy_html_gradient) {
                QImage gradient_mask(img_w, img_h, QImage::Format_ARGB32_Premultiplied);
                gradient_mask.fill(Qt::transparent);
                QPainter mask_painter(&gradient_mask);
                mask_painter.setRenderHint(QPainter::Antialiasing, true);
                mask_painter.setRenderHint(QPainter::TextAntialiasing, true);
                draw_rich_text_document(mask_painter, layer, font, text_rect, t);
                mask_painter.end();

                QPainter gradient_painter(&gradient_mask);
                gradient_painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
                gradient_painter.fillRect(QRectF(0.0, 0.0, img_w, img_h), gradient_fill_brush(layer, text_rect));
                gradient_painter.end();
                painter.drawImage(QPointF(0.0, 0.0), gradient_mask);
            } else {
                draw_rich_text_document(painter, layer, font, text_rect, t);
            }
            painter.restore();
            return;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(layer.fill_type == 1 ? gradient_fill_brush(layer, text_rect) : QBrush(fill));
        painter.drawPath(text_path);
    };
    auto draw_text_outline = [&]() {
        if (has_rich_text) return;
        if (outline_width <= 0.0) return;
        if (layer.stroke_fill_type == 1 && outline.alpha() <= 0) return;
        bool previous_aa = painter.testRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::Antialiasing, eval_outline_antialias(layer, t));
        const QBrush stroke_brush = layer.stroke_fill_type == 2
            ? stroke_gradient_fill_brush(layer, text_rect, eval_outline_opacity(layer, t))
            : QBrush(outline);
        painter.setPen(QPen(stroke_brush, outline_width, Qt::SolidLine, Qt::RoundCap, outline_pen_join_style(layer)));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(text_path);
        painter.setRenderHint(QPainter::Antialiasing, previous_aa);
    };
    if (!eval_outline_on_front(layer, t)) draw_text_outline();
    draw_text_fill();
    if (eval_outline_on_front(layer, t)) draw_text_outline();
    painter.setRenderHint(QPainter::TextAntialiasing, previous_text_aa);
    painter.setRenderHint(QPainter::Antialiasing, previous_shape_aa);
    painter.restore();
    painter.end();

    auto text_surface = make_image_surface_for_qimage(text_image);
    if (!text_surface) return;

    cairo_save(cr);
    apply_layer_world_transform(cr, title, layer, title_time);
    cairo_set_source_surface(cr, text_surface.get(), -eval_origin_x(layer, t) * box_w - pad, -eval_origin_y(layer, t) * box_h - pad);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

static void render_layer_rect(cairo_t *cr, const Title &title, const Layer &layer, double title_time)
{
    const double t = std::max(0.0, title_time - layer.in_time);
    double alpha = layer_chain_opacity(title, layer, title_time);

    double w = eval_box_width(layer, t);
    double h = eval_box_height(layer, t);
    if (w <= 0.0 || h <= 0.0) return;
    double x = -eval_origin_x(layer, t) * w;
    double y = -eval_origin_y(layer, t) * h;

    double fr, fg, fb, fa;
    unpack_color(eval_fill_color(layer, t), fr, fg, fb, fa);

    cairo_save(cr);
    apply_layer_world_transform(cr, title, layer, title_time);
    cairo_translate(cr, x, y);

    ShadowRenderParams shadow_params = evaluated_shadow_params(layer, t);
    if (shadow_params.drop_enabled || shadow_params.long_enabled) {
        const int mw = std::max(1, (int)std::ceil(w));
        const int mh = std::max(1, (int)std::ceil(h));
        QImage mask(mw, mh, QImage::Format_ARGB32_Premultiplied);
        mask.fill(Qt::transparent);
        QPainter mp(&mask);
        mp.setRenderHint(QPainter::Antialiasing, true);
        mp.setPen(Qt::NoPen);
        mp.setBrush(Qt::white);
        QPainterPath shape_path = painter_layer_shape_path(layer, w, h);
        if (layer.shape_type == ShapeType::Line) {
            QPen pen(Qt::white, std::max(1.0, eval_outline_width(layer, t)), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            mp.setPen(pen);
            mp.setBrush(Qt::NoBrush);
        }
        mp.drawPath(shape_path);
        mp.end();
        const QString shape_key = QStringLiteral("rect|%1|%2x%3|%4|%5|%6|%7|%8|%9|%10")
            .arg((int)layer.shape_type).arg(mw).arg(mh)
            .arg(layer.corner_radius_tl, 0, 'f', 2)
            .arg(layer.corner_radius_tr, 0, 'f', 2)
            .arg(layer.corner_radius_br, 0, 'f', 2)
            .arg(layer.corner_radius_bl, 0, 'f', 2)
            .arg((int)layer.corner_type)
            .arg(layer.shape_points).arg(layer.shape_sides);
        CachedShadowImage shadow = build_shadow_image(layer, shadow_params, shape_key, mask);
        paint_qimage(cr, shadow.image, shadow.origin.x(), shadow.origin.y(), alpha);
    }

    cairo_add_layer_shape(cr, layer, w, h);
    double outline_width = eval_outline_width(layer, t);
    uint32_t outline_color = eval_outline_color(layer, t);
    bool has_outline = outline_width > 0.0 &&
                       (layer.stroke_fill_type == 2 || ((outline_color >> 24) & 0xFF) > 0);
    auto stroke_outline = [&]() {
        cairo_set_antialias(cr, outline_cairo_antialias(layer));
        cairo_set_line_width(cr, outline_width);
        cairo_set_line_join(cr, outline_cairo_join_style(layer));
        cairo_pattern_t *stroke_gradient = nullptr;
        if (layer.stroke_fill_type == 2) {
            stroke_gradient = create_stroke_gradient_pattern(layer, 0.0, 0.0, w, h,
                                                             alpha * eval_outline_opacity(layer, t));
            cairo_set_source(cr, stroke_gradient);
        } else {
            double sr, sg, sb, sa;
            unpack_color(outline_color, sr, sg, sb, sa);
            cairo_set_source_rgba(cr, sr, sg, sb, sa * alpha * eval_outline_opacity(layer, t));
        }
        cairo_stroke_preserve(cr);
        if (stroke_gradient) cairo_pattern_destroy(stroke_gradient);
        cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
    };
    if (has_outline && !eval_outline_on_front(layer, t))
        stroke_outline();
    cairo_pattern_t *gradient_pattern = nullptr;
    if (layer.fill_type == 1) {
        gradient_pattern = create_fill_gradient_pattern(layer, 0.0, 0.0, w, h, alpha);
        cairo_set_source(cr, gradient_pattern);
    } else {
        cairo_set_source_rgba(cr, fr, fg, fb, fa * alpha);
    }
    if (has_outline && eval_outline_on_front(layer, t)) {
        cairo_fill_preserve(cr);
        if (gradient_pattern) cairo_pattern_destroy(gradient_pattern);
        stroke_outline();
        cairo_new_path(cr);
    } else {
        cairo_fill(cr);
        if (gradient_pattern) cairo_pattern_destroy(gradient_pattern);
    }
    cairo_restore(cr);
}


static void render_layer_image(cairo_t *cr, const Title &title, const Layer &layer, double title_time)
{
    if (layer.image_path.empty()) return;
    const double t = std::max(0.0, title_time - layer.in_time);
    double alpha = layer_chain_opacity(title, layer, title_time);
    double w = eval_box_width(layer, t);
    double h = eval_box_height(layer, t);
    if (w <= 0.0 || h <= 0.0) return;

    const int max_sample_dim = std::clamp(std::max(title.width, title.height) * 2, 512, 4096);
    const QSize sample_size(std::clamp((int)std::ceil(w), 1, max_sample_dim),
                            std::clamp((int)std::ceil(h), 1, max_sample_dim));
    QImage argb = load_cached_layer_image(QString::fromStdString(layer.image_path), sample_size);
    if (argb.isNull() || argb.width() <= 0 || argb.height() <= 0) return;

    auto img_surface = make_image_surface_for_const_qimage(argb);
    if (!img_surface) return;

    cairo_save(cr);
    apply_layer_world_transform(cr, title, layer, title_time);
    const double origin_x = eval_origin_x(layer, t);
    const double origin_y = eval_origin_y(layer, t);
    ShadowRenderParams shadow_params = evaluated_shadow_params(layer, t);
    if (shadow_params.drop_enabled || shadow_params.long_enabled) {
        QImage mask = argb.scaled(std::max(1, (int)std::ceil(w)), std::max(1, (int)std::ceil(h)),
                                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                          .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QFileInfo shadow_image_info(QString::fromStdString(layer.image_path));
        const QString shape_key = QStringLiteral("image|%1|%2x%3|%4|%5")
            .arg(QString::fromStdString(layer.image_path)).arg(mask.width()).arg(mask.height())
            .arg(shadow_image_info.exists() ? shadow_image_info.lastModified().toMSecsSinceEpoch() : 0)
            .arg(shadow_image_info.exists() ? shadow_image_info.size() : -1);
        CachedShadowImage shadow = build_shadow_image(layer, shadow_params, shape_key, mask);
        paint_qimage(cr, shadow.image, -origin_x * w + shadow.origin.x(), -origin_y * h + shadow.origin.y(), alpha);
    }
    if (eval_background_enabled(layer, t)) {
        const double bg_pad_x = eval_background_padding_x(layer, t);
        const double bg_pad_y = eval_background_padding_y(layer, t);
        QColor bg = evaluated_background_color(layer, t);
        double br, bgc, bb, ba;
        br = bg.redF(); bgc = bg.greenF(); bb = bg.blueF(); ba = bg.alphaF();
        if (ba > 0.0 || layer.background_fill_type == 1) {
            const double x = -origin_x * w - bg_pad_x;
            const double y = -origin_y * h - bg_pad_y;
            const double bw = w + bg_pad_x * 2.0;
            const double bh = h + bg_pad_y * 2.0;
            const double radius = std::min<double>(eval_background_corner_radius(layer, t), std::min(bw, bh) / 2.0);
            if (radius > 0.0) {
                cairo_new_sub_path(cr);
                cairo_arc(cr, x + radius,      y + radius,      radius, kPi,     3*kPi/2);
                cairo_arc(cr, x + bw - radius, y + radius,      radius, 3*kPi/2, 2*kPi);
                cairo_arc(cr, x + bw - radius, y + bh - radius, radius, 0,       kPi/2);
                cairo_arc(cr, x + radius,      y + bh - radius, radius, kPi/2,   kPi);
                cairo_close_path(cr);
            } else {
                cairo_rectangle(cr, x, y, bw, bh);
            }
            cairo_pattern_t *gradient_pattern = nullptr;
            if (layer.background_fill_type == 1) {
                gradient_pattern = create_background_gradient_pattern(layer, x, y, bw, bh, alpha * eval_background_opacity(layer, t));
                cairo_set_source(cr, gradient_pattern);
            } else {
                cairo_set_source_rgba(cr, br, bgc, bb, ba * alpha);
            }
            cairo_fill(cr);
            if (gradient_pattern) cairo_pattern_destroy(gradient_pattern);
        }
    }
    cairo_scale(cr, w / argb.width(), h / argb.height());
    cairo_set_source_surface(cr, img_surface.get(),
                             -origin_x * argb.width(),
                             -origin_y * argb.height());
    cairo_pattern_set_filter(cairo_get_source(cr),
                             cairo_filter_for_image_scale_filter(layer.scale_filter));
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}


static void render_layer_unmasked(cairo_t *cr, const Title &title, const Layer &layer,
                                  double title_time, int canvas_w, int canvas_h)
{
    switch (layer.type) {
    case LayerType::Text:
    case LayerType::Clock:
    case LayerType::Ticker:
        render_layer_text(cr, title, layer, title_time, canvas_w, canvas_h);
        break;
    case LayerType::SolidRect:
    case LayerType::Shape:
        render_layer_rect(cr, title, layer, title_time);
        break;
    case LayerType::Image:
        render_layer_image(cr, title, layer, title_time);
        break;
    default:
        break;
    }
}



static bool layer_effect_requires_stack_surface(const LayerEffect &effect)
{
    if (!effect.enabled) return false;
    switch (effect.type) {
    case LayerEffectType::DropShadow:
    case LayerEffectType::LongShadow:
    case LayerEffectType::ColorOverlay:
    case LayerEffectType::Glow:
    case LayerEffectType::InnerGlow:
    case LayerEffectType::InnerShadow:
    case LayerEffectType::Blur:
    case LayerEffectType::MotionBlur:
        return true;
    case LayerEffectType::BrightnessContrast:
        return std::abs(effect.brightness) > 0.0001f || std::abs(effect.contrast - 1.0f) > 0.0001f;
    case LayerEffectType::Saturation:
        return std::abs(effect.saturation - 1.0f) > 0.0001f;
    default:
        return false;
    }
}

static bool layer_has_stackable_pixel_effects(const Layer &layer)
{
    for (const auto &effect : layer.effects) {
        if (layer_effect_requires_stack_surface(effect))
            return true;
    }
    return false;
}

static bool effect_is_motion_blur(const LayerEffect &effect)
{
    return effect.enabled && effect.type == LayerEffectType::MotionBlur;
}

static const LayerEffect *layer_motion_blur_effect(const Layer &layer)
{
    for (auto it = layer.effects.rbegin(); it != layer.effects.rend(); ++it) {
        if (effect_is_motion_blur(*it))
            return &*it;
    }
    return nullptr;
}

static bool layer_has_motion_blur(const Layer &layer)
{
    return layer_motion_blur_effect(layer) != nullptr;
}

static bool gpu_effects_requested_for_source(const TitleSourceData *data)
{
    (void)data;
    return false;
}

static Layer layer_without_stackable_pixel_effects(const Layer &layer)
{
    Layer base_layer = layer;
    base_layer.effects.erase(std::remove_if(base_layer.effects.begin(), base_layer.effects.end(),
                                            layer_effect_requires_stack_surface),
                             base_layer.effects.end());
    return base_layer;
}

static Layer layer_without_motion_blur_effects(const Layer &layer)
{
    Layer base_layer = layer;
    base_layer.effects.erase(std::remove_if(base_layer.effects.begin(), base_layer.effects.end(),
                                            effect_is_motion_blur),
                             base_layer.effects.end());
    return base_layer;
}

static std::vector<uint8_t> surface_alpha(cairo_surface_t *surface)
{
    cairo_surface_flush(surface);
    uint8_t *data = cairo_image_surface_get_data(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    std::vector<uint8_t> alpha((size_t)std::max(0, width) * (size_t)std::max(0, height), 0);
    if (!data || width <= 0 || height <= 0 || stride <= 0)
        return alpha;
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = data + y * stride;
        for (int x = 0; x < width; ++x)
            alpha[y * width + x] = row[x * 4 + 3];
    }
    return alpha;
}

static std::vector<uint8_t> surface_pixels(cairo_surface_t *surface)
{
    cairo_surface_flush(surface);
    uint8_t *data = cairo_image_surface_get_data(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    std::vector<uint8_t> pixels((size_t)std::max(0, width) * (size_t)std::max(0, height) * 4, 0);
    if (!data || width <= 0 || height <= 0 || stride <= 0)
        return pixels;
    for (int y = 0; y < height; ++y)
        std::copy(data + y * stride, data + y * stride + width * 4, pixels.begin() + (size_t)y * width * 4);
    return pixels;
}

static void write_surface_pixels(cairo_surface_t *surface, const std::vector<uint8_t> &pixels)
{
    uint8_t *data = cairo_image_surface_get_data(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    if (!data || width <= 0 || height <= 0 || stride <= 0 || pixels.size() < (size_t)width * (size_t)height * 4)
        return;
    for (int y = 0; y < height; ++y)
        std::copy(pixels.begin() + (size_t)y * width * 4,
                  pixels.begin() + (size_t)(y + 1) * width * 4,
                  data + y * stride);
    cairo_surface_mark_dirty(surface);
}

static void clear_surface(cairo_t *cr)
{
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void composite_premultiplied_over(std::vector<uint8_t> &dst, const std::vector<uint8_t> &src)
{
    const size_t count = std::min(dst.size(), src.size()) / 4;
    for (size_t i = 0; i < count; ++i) {
        uint8_t *d = dst.data() + i * 4;
        const uint8_t *s = src.data() + i * 4;
        const int inv_a = 255 - s[3];
        d[0] = (uint8_t)std::min(255, (int)s[0] + ((int)d[0] * inv_a + 127) / 255);
        d[1] = (uint8_t)std::min(255, (int)s[1] + ((int)d[1] * inv_a + 127) / 255);
        d[2] = (uint8_t)std::min(255, (int)s[2] + ((int)d[2] * inv_a + 127) / 255);
        d[3] = (uint8_t)std::min(255, (int)s[3] + ((int)d[3] * inv_a + 127) / 255);
    }
}

static void box_blur_pixels(std::vector<uint8_t> &pixels, int w, int h, int radius)
{
    if (radius <= 0 || w <= 0 || h <= 0 || pixels.size() < (size_t)w * (size_t)h * 4)
        return;

    std::vector<uint8_t> tmp(pixels.size());
    const int window = radius * 2 + 1;

    for (int y = 0; y < h; ++y) {
        int sum[4] = {0, 0, 0, 0};
        for (int x = -radius; x <= radius; ++x) {
            const uint8_t *px = &pixels[((size_t)y * w + std::clamp(x, 0, w - 1)) * 4];
            for (int c = 0; c < 4; ++c) sum[c] += px[c];
        }
        for (int x = 0; x < w; ++x) {
            uint8_t *dst = &tmp[((size_t)y * w + x) * 4];
            for (int c = 0; c < 4; ++c) dst[c] = (uint8_t)(sum[c] / window);
            const uint8_t *remove = &pixels[((size_t)y * w + std::clamp(x - radius, 0, w - 1)) * 4];
            const uint8_t *add = &pixels[((size_t)y * w + std::clamp(x + radius + 1, 0, w - 1)) * 4];
            for (int c = 0; c < 4; ++c) sum[c] += add[c] - remove[c];
        }
    }

    for (int x = 0; x < w; ++x) {
        int sum[4] = {0, 0, 0, 0};
        for (int y = -radius; y <= radius; ++y) {
            const uint8_t *px = &tmp[((size_t)std::clamp(y, 0, h - 1) * w + x) * 4];
            for (int c = 0; c < 4; ++c) sum[c] += px[c];
        }
        for (int y = 0; y < h; ++y) {
            uint8_t *dst = &pixels[((size_t)y * w + x) * 4];
            for (int c = 0; c < 4; ++c) dst[c] = (uint8_t)(sum[c] / window);
            const uint8_t *remove = &tmp[((size_t)std::clamp(y - radius, 0, h - 1) * w + x) * 4];
            const uint8_t *add = &tmp[((size_t)std::clamp(y + radius + 1, 0, h - 1) * w + x) * 4];
            for (int c = 0; c < 4; ++c) sum[c] += add[c] - remove[c];
        }
    }
}

static void blur_surface_for_type(cairo_surface_t *surface, double blur, int blur_type, double amount)
{
    cairo_surface_flush(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    if (width <= 0 || height <= 0)
        return;

    const auto passes = blur_pass_radii(blur, blur_type);
    if (passes.empty())
        return;

    std::vector<uint8_t> original = surface_pixels(surface);
    std::vector<uint8_t> blurred = original;
    for (int radius : passes)
        box_blur_pixels(blurred, width, height, radius);

    const double mix = std::clamp(amount, 0.0, 1.0);
    if (mix < 1.0) {
        for (size_t i = 0; i < blurred.size(); ++i)
            blurred[i] = (uint8_t)std::lround(original[i] * (1.0 - mix) + blurred[i] * mix);
    }
    write_surface_pixels(surface, blurred);
}

static void offset_alpha(const std::vector<uint8_t> &src, std::vector<uint8_t> &dst,
                         int w, int h, double dx, double dy, double opacity = 1.0)
{
    if (opacity <= 0.0 || w <= 0 || h <= 0) return;
    const int ox = (int)std::round(dx);
    const int oy = (int)std::round(dy);
    for (int y = 0; y < h; ++y) {
        const int sy = y - oy;
        if (sy < 0 || sy >= h) continue;
        for (int x = 0; x < w; ++x) {
            const int sx = x - ox;
            if (sx < 0 || sx >= w) continue;
            const int a = (int)std::round(src[sy * w + sx] * opacity);
            if (a <= 0) continue;
            uint8_t &d = dst[y * w + x];
            d = (uint8_t)std::min(255, (int)d + a - ((int)d * a) / 255);
        }
    }
}

static double blend_channel(double base, double src, EffectBlendMode mode)
{
    switch (mode) {
    case EffectBlendMode::Multiply: return base * src;
    case EffectBlendMode::Additive: return std::min(1.0, base + src);
    case EffectBlendMode::Screen: return 1.0 - (1.0 - base) * (1.0 - src);
    case EffectBlendMode::Overlay: return base < 0.5 ? 2.0 * base * src : 1.0 - 2.0 * (1.0 - base) * (1.0 - src);
    case EffectBlendMode::Color:
    case EffectBlendMode::Normal:
    default: return src;
    }
}

static void blend_color_mode(double br, double bg, double bb, double sr, double sg, double sb,
                             double &rr, double &rg, double &rb)
{
    QColor base = QColor::fromRgbF(std::clamp(br, 0.0, 1.0), std::clamp(bg, 0.0, 1.0), std::clamp(bb, 0.0, 1.0));
    QColor src = QColor::fromRgbF(std::clamp(sr, 0.0, 1.0), std::clamp(sg, 0.0, 1.0), std::clamp(sb, 0.0, 1.0));
    float bh, bs, bl, ba;
    float sh, ss, sl, sa;
    base.getHslF(&bh, &bs, &bl, &ba);
    src.getHslF(&sh, &ss, &sl, &sa);
    QColor out;
    out.setHslF(sh < 0.0 ? bh : sh, ss, bl, ba);
    rr = out.redF();
    rg = out.greenF();
    rb = out.blueF();
}

static void composite_solid_alpha(cairo_surface_t *surface, const std::vector<uint8_t> &mask,
                                  uint32_t argb, double opacity, EffectBlendMode mode,
                                  bool preserve_destination_alpha)
{
    cairo_surface_flush(surface);
    uint8_t *data = cairo_image_surface_get_data(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    if (!data || width <= 0 || height <= 0 || stride <= 0 || mask.size() < (size_t)width * (size_t)height)
        return;

    const double src_a_base = std::clamp(opacity, 0.0, 1.0) * (((argb >> 24) & 0xFF) / 255.0);
    if (src_a_base <= 0.0) return;
    const double sr = ((argb >> 16) & 0xFF) / 255.0;
    const double sg = ((argb >> 8) & 0xFF) / 255.0;
    const double sb = (argb & 0xFF) / 255.0;

    for (int y = 0; y < height; ++y) {
        uint8_t *row = data + y * stride;
        for (int x = 0; x < width; ++x) {
            const double sa = src_a_base * (mask[y * width + x] / 255.0);
            if (sa <= 0.0) continue;
            uint8_t *px = row + x * 4;
            const double da = px[3] / 255.0;
            const double br = da > 0.0 ? (px[2] / 255.0) / da : 0.0;
            const double bg = da > 0.0 ? (px[1] / 255.0) / da : 0.0;
            const double bb = da > 0.0 ? (px[0] / 255.0) / da : 0.0;

            double rr, rg, rb;
            if (mode == EffectBlendMode::Color)
                blend_color_mode(br, bg, bb, sr, sg, sb, rr, rg, rb);
            else {
                rr = blend_channel(br, sr, mode);
                rg = blend_channel(bg, sg, mode);
                rb = blend_channel(bb, sb, mode);
            }

            const double out_a = preserve_destination_alpha ? da : (sa + da * (1.0 - sa));
            double out_r = br, out_g = bg, out_b = bb;
            if (out_a > 0.0) {
                if (preserve_destination_alpha) {
                    out_r = br * (1.0 - sa) + rr * sa;
                    out_g = bg * (1.0 - sa) + rg * sa;
                    out_b = bb * (1.0 - sa) + rb * sa;
                } else {
                    out_r = (rr * sa + br * da * (1.0 - sa)) / out_a;
                    out_g = (rg * sa + bg * da * (1.0 - sa)) / out_a;
                    out_b = (rb * sa + bb * da * (1.0 - sa)) / out_a;
                }
            }
            px[0] = (uint8_t)std::lround(std::clamp(out_b, 0.0, 1.0) * out_a * 255.0);
            px[1] = (uint8_t)std::lround(std::clamp(out_g, 0.0, 1.0) * out_a * 255.0);
            px[2] = (uint8_t)std::lround(std::clamp(out_r, 0.0, 1.0) * out_a * 255.0);
            px[3] = (uint8_t)std::lround(std::clamp(out_a, 0.0, 1.0) * 255.0);
        }
    }
    cairo_surface_mark_dirty(surface);
}

static void composite_solid_alpha_behind(cairo_surface_t *surface, const std::vector<uint8_t> &mask,
                                         uint32_t argb, double opacity)
{
    cairo_surface_flush(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    if (width <= 0 || height <= 0 || mask.size() < (size_t)width * (size_t)height)
        return;

    std::vector<uint8_t> original = surface_pixels(surface);
    std::vector<uint8_t> under_pixels((size_t)width * (size_t)height * 4, 0);
    CairoSurfacePtr under(cairo_image_surface_create_for_data(
        under_pixels.data(), CAIRO_FORMAT_ARGB32, width, height, width * 4));
    if (!under || cairo_surface_status(under.get()) != CAIRO_STATUS_SUCCESS)
        return;

    composite_solid_alpha(under.get(), mask, argb, opacity, EffectBlendMode::Normal, false);
    cairo_surface_flush(under.get());
    composite_premultiplied_over(under_pixels, original);
    write_surface_pixels(surface, under_pixels);
}

static void apply_directional_motion_blur(cairo_surface_t *surface, const LayerEffect &effect)
{
    cairo_surface_flush(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    if (width <= 0 || height <= 0)
        return;

    const std::vector<uint8_t> original = surface_pixels(surface);
    if (original.empty())
        return;

    const int samples = std::clamp(effect.effect_samples, 2, 64);
    const double distance = std::max(0.0f, effect.effect_size);
    const double amount = std::clamp((double)effect.effect_opacity, 0.0, 1.0);
    if (distance <= 0.0 || amount <= 0.0)
        return;

    const double radians = effect.effect_angle * kPi / 180.0;
    const double dx = std::cos(radians) * distance;
    const double dy = std::sin(radians) * distance;
    std::vector<uint8_t> blurred(original.size(), 0);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int sum[4] = {0, 0, 0, 0};
            int used = 0;
            for (int s = 0; s < samples; ++s) {
                const double f = samples == 1 ? 0.0 : (double)s / (double)(samples - 1);
                const double offset = effect.effect_centered ? (f - 0.5) : -f;
                const int sx = (int)std::lround(x + dx * offset);
                const int sy = (int)std::lround(y + dy * offset);
                if (sx < 0 || sx >= width || sy < 0 || sy >= height)
                    continue;
                const uint8_t *src = original.data() + ((size_t)sy * width + sx) * 4;
                for (int c = 0; c < 4; ++c)
                    sum[c] += src[c];
                ++used;
            }
            uint8_t *dst = blurred.data() + ((size_t)y * width + x) * 4;
            if (used <= 0) {
                const uint8_t *src = original.data() + ((size_t)y * width + x) * 4;
                std::copy(src, src + 4, dst);
            } else {
                for (int c = 0; c < 4; ++c)
                    dst[c] = (uint8_t)(sum[c] / used);
            }
        }
    }

    if (amount < 1.0) {
        for (size_t i = 0; i < blurred.size(); ++i)
            blurred[i] = (uint8_t)std::lround(original[i] * (1.0 - amount) + blurred[i] * amount);
    }
    write_surface_pixels(surface, blurred);
}

static void apply_color_adjustment(cairo_surface_t *surface, const LayerEffect &effect)
{
    cairo_surface_flush(surface);
    uint8_t *data = cairo_image_surface_get_data(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    if (!data || width <= 0 || height <= 0 || stride <= 0)
        return;

    auto clamp01 = [](double value) { return std::clamp(value, 0.0, 1.0); };
    constexpr double luma_r = 0.2126;
    constexpr double luma_g = 0.7090;
    constexpr double luma_b = 0.0784;

    for (int y = 0; y < height; ++y) {
        uint8_t *row = data + y * stride;
        for (int x = 0; x < width; ++x) {
            uint8_t *px = row + x * 4;
            const double alpha = px[3] / 255.0;
            if (alpha <= 0.0) continue;
            double b = (px[0] / 255.0) / alpha;
            double g = (px[1] / 255.0) / alpha;
            double r = (px[2] / 255.0) / alpha;
            if (effect.type == LayerEffectType::BrightnessContrast) {
                const double brightness = std::clamp((double)effect.brightness, -1.0, 1.0);
                const double contrast = std::clamp((double)effect.contrast, 0.0, 4.0);
                r = (r - 0.5) * contrast + 0.5 + brightness;
                g = (g - 0.5) * contrast + 0.5 + brightness;
                b = (b - 0.5) * contrast + 0.5 + brightness;
            } else if (effect.type == LayerEffectType::Saturation) {
                const double saturation = std::clamp((double)effect.saturation, 0.0, 4.0);
                const double luma = r * luma_r + g * luma_g + b * luma_b;
                r = luma + (r - luma) * saturation;
                g = luma + (g - luma) * saturation;
                b = luma + (b - luma) * saturation;
            }
            px[0] = (uint8_t)std::lround(clamp01(b) * alpha * 255.0);
            px[1] = (uint8_t)std::lround(clamp01(g) * alpha * 255.0);
            px[2] = (uint8_t)std::lround(clamp01(r) * alpha * 255.0);
        }
    }
    cairo_surface_mark_dirty(surface);
}

static void apply_stackable_pixel_effects_to_surface(cairo_surface_t *surface, const Layer &layer, double t)
{
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
        return;

    for (const auto &effect : layer.effects) {
        if (!effect.enabled) continue;
        const int width = cairo_image_surface_get_width(surface);
        const int height = cairo_image_surface_get_height(surface);
        if (width <= 0 || height <= 0) return;
        std::vector<uint8_t> alpha;
        switch (effect.type) {
        case LayerEffectType::BrightnessContrast:
        case LayerEffectType::Saturation:
            apply_color_adjustment(surface, effect);
            break;
        case LayerEffectType::ColorOverlay:
            alpha = surface_alpha(surface);
            composite_solid_alpha(surface, alpha, effect.effect_color, effect.effect_opacity, effect.blend_mode, true);
            break;
        case LayerEffectType::Blur:
            blur_surface_for_type(surface, effect.effect_size, effect.effect_blur_type, effect.effect_opacity);
            break;
        case LayerEffectType::MotionBlur:
            apply_directional_motion_blur(surface, effect);
            break;
        case LayerEffectType::Glow: {
            alpha = surface_alpha(surface);
            std::vector<uint8_t> glow = alpha;
            blur_alpha_for_type(glow, width, height, effect.effect_size, effect.effect_blur_type);
            for (size_t i = 0; i < glow.size(); ++i)
                glow[i] = (uint8_t)std::max(0, (int)glow[i] - (int)alpha[i]);
            composite_solid_alpha(surface, glow, effect.effect_color, effect.effect_opacity, effect.blend_mode, false);
            break;
        }
        case LayerEffectType::InnerGlow: {
            alpha = surface_alpha(surface);
            std::vector<uint8_t> inv(alpha.size());
            for (size_t i = 0; i < alpha.size(); ++i) inv[i] = 255 - alpha[i];
            blur_alpha_for_type(inv, width, height, effect.effect_size, effect.effect_blur_type);
            for (size_t i = 0; i < inv.size(); ++i)
                inv[i] = (uint8_t)(((int)inv[i] * (int)alpha[i]) / 255);
            composite_solid_alpha(surface, inv, effect.effect_color, effect.effect_opacity, effect.blend_mode, true);
            break;
        }
        case LayerEffectType::InnerShadow: {
            alpha = surface_alpha(surface);
            std::vector<uint8_t> shifted(alpha.size(), 0);
            const double rad = effect.effect_angle * kPi / 180.0;
            offset_alpha(alpha, shifted, width, height, std::cos(rad) * effect.effect_distance,
                         std::sin(rad) * effect.effect_distance, 1.0);
            blur_alpha_for_type(shifted, width, height, effect.effect_size, effect.effect_blur_type);
            for (size_t i = 0; i < shifted.size(); ++i)
                shifted[i] = (uint8_t)(((255 - (int)shifted[i]) * (int)alpha[i]) / 255);
            composite_solid_alpha(surface, shifted, effect.effect_color, effect.effect_opacity, effect.blend_mode, true);
            break;
        }
        case LayerEffectType::DropShadow: {
            if (effect.effect_opacity <= 0.0f) break;
            alpha = surface_alpha(surface);
            std::vector<uint8_t> shadow(alpha.size(), 0);
            const double rad = effect.effect_angle * kPi / 180.0;
            offset_alpha(alpha, shadow, width, height,
                         std::cos(rad) * effect.effect_distance,
                         std::sin(rad) * effect.effect_distance, 1.0);
            if (effect.effect_spread > 0.0f)
                blur_alpha_for_type(shadow, width, height, effect.effect_spread, (int)ShadowBlurType::AlphaMask);
            blur_alpha_for_type(shadow, width, height, effect.effect_size, effect.effect_blur_type);
            composite_solid_alpha_behind(surface, shadow, effect.effect_color, effect.effect_opacity);
            break;
        }
        case LayerEffectType::LongShadow: {
            if (effect.effect_opacity <= 0.0f || effect.effect_distance <= 0.0f) break;
            alpha = surface_alpha(surface);
            std::vector<uint8_t> long_shadow(alpha.size(), 0);
            const double rad = effect.effect_angle * kPi / 180.0;
            const double dx = std::cos(rad) * effect.effect_distance;
            const double dy = std::sin(rad) * effect.effect_distance;
            const int steps = std::clamp((int)std::ceil(effect.effect_distance / 16.0f), 1, 64);
            for (int i = steps; i >= 1; --i) {
                const double u = (double)i / steps;
                const double fade = std::pow(1.0 - u, std::max(0.0f, effect.effect_falloff));
                offset_alpha(alpha, long_shadow, width, height, dx * u, dy * u, fade);
            }
            const int mapped_long_blur = shadow_blur_type_for_long_shadow(effect.effect_blur_type);
            if (mapped_long_blur >= 0 && effect.effect_size > 0.0f)
                blur_alpha_for_type(long_shadow, width, height, effect.effect_size, mapped_long_blur);
            composite_solid_alpha_behind(surface, long_shadow, effect.effect_color, effect.effect_opacity);
            break;
        }
        default:
            break;
        }
    }
}


static bool layer_has_non_transform_animation(const Layer &layer)
{
    return layer.box_width.is_animated() ||
           layer.box_height.is_animated() ||
           layer.origin_x_prop.is_animated() ||
           layer.origin_y_prop.is_animated() ||
           layer.paragraph_indent_left_prop.is_animated() ||
           layer.paragraph_indent_right_prop.is_animated() ||
           layer.paragraph_indent_first_line_prop.is_animated() ||
           layer.font_size_prop.is_animated() ||
           layer.char_scale_x_prop.is_animated() ||
           layer.char_scale_y_prop.is_animated() ||
           layer.char_tracking_prop.is_animated() ||
           layer.baseline_shift_prop.is_animated() ||
           layer.paragraph_space_before_prop.is_animated() ||
           layer.paragraph_space_after_prop.is_animated() ||
           layer.shadow_enabled_prop.is_animated() ||
           layer.shadow_opacity_prop.is_animated() ||
           layer.shadow_distance_prop.is_animated() ||
           layer.shadow_angle_prop.is_animated() ||
           layer.shadow_blur_prop.is_animated() ||
           layer.shadow_spread_prop.is_animated() ||
           layer.shadow_color_a.is_animated() ||
           layer.shadow_color_r.is_animated() ||
           layer.shadow_color_g.is_animated() ||
           layer.shadow_color_b.is_animated() ||
           layer.background_enabled_prop.is_animated() ||
           layer.background_opacity_prop.is_animated() ||
           layer.background_padding_x_prop.is_animated() ||
           layer.background_padding_y_prop.is_animated() ||
           layer.background_corner_radius_prop.is_animated() ||
           layer.background_color_a.is_animated() ||
           layer.background_color_r.is_animated() ||
           layer.background_color_g.is_animated() ||
           layer.background_color_b.is_animated() ||
           layer.text_color_a.is_animated() ||
           layer.text_color_r.is_animated() ||
           layer.text_color_g.is_animated() ||
           layer.text_color_b.is_animated() ||
           layer.fill_color_a.is_animated() ||
           layer.fill_color_r.is_animated() ||
           layer.fill_color_g.is_animated() ||
           layer.fill_color_b.is_animated();
}

static QRect image_alpha_bounds(const QImage &image)
{
    if (image.isNull()) return QRect();
    int min_x = image.width(), min_y = image.height(), max_x = -1, max_y = -1;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(row[x]) == 0) continue;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    }
    return max_x >= min_x ? QRect(QPoint(min_x, min_y), QPoint(max_x, max_y)) : QRect();
}

static void neutralize_layer_transform_for_effect_cache(Layer &layer, double opacity, double anchor_x, double anchor_y)
{
    layer.parent_id.clear();
    layer.pos_x.static_value = anchor_x;
    layer.pos_x.keyframes.clear();
    layer.pos_y.static_value = anchor_y;
    layer.pos_y.keyframes.clear();
    layer.scale_x.static_value = 1.0;
    layer.scale_x.keyframes.clear();
    layer.scale_y.static_value = 1.0;
    layer.scale_y.keyframes.clear();
    layer.rotation.static_value = 0.0;
    layer.rotation.keyframes.clear();
    layer.opacity.static_value = opacity;
    layer.opacity.keyframes.clear();
}

static std::string effect_layer_cache_key(const TitleSourceData *data, const Title &title, const Layer &layer,
                                          double title_time, int canvas_w, int canvas_h,
                                          bool force_time_key = false)
{
    const double t = std::max(0.0, title_time - layer.in_time);
    std::ostringstream key;
    key << layer.id << "|rev=" << (data ? data->seen_store_revision : 0)
        << "|canvas=" << canvas_w << 'x' << canvas_h
        << "|type=" << (int)layer.type
        << "|box=" << eval_box_width(layer, t) << 'x' << eval_box_height(layer, t)
        << "|origin=" << eval_origin_x(layer, t) << ',' << eval_origin_y(layer, t)
        << "|stack=" << layer.effects.size();

    if (force_time_key || layer_has_non_transform_animation(layer) || layer.type == LayerType::Ticker)
        key << "|time=" << (int64_t)std::llround(t * 1000.0);

    ShadowRenderParams shadow = evaluated_shadow_params(layer, t);
    key << "|shadow=" << shadow.drop_enabled << ',' << shadow.color << ',' << shadow.opacity << ','
        << shadow.dx << ',' << shadow.dy << ',' << shadow.blur << ',' << shadow.spread << ',' << shadow.blur_type
        << "|long=" << shadow.long_enabled << ',' << shadow.long_color << ',' << shadow.long_opacity << ','
        << shadow.long_length << ',' << shadow.long_angle << ',' << shadow.long_falloff << ','
        << (int)shadow.long_blur_type << ',' << shadow.long_blur;

    if (layer.type == LayerType::Image && !layer.image_path.empty()) {
        QFileInfo info(QString::fromStdString(layer.image_path));
        key << "|image=" << layer.image_path << ','
            << (info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0) << ','
            << (info.exists() ? info.size() : -1);
    }

    for (const auto &effect : layer.effects) {
        key << "|e=" << (int)effect.type << ',' << effect.enabled << ','
            << effect.brightness << ',' << effect.contrast << ',' << effect.saturation << ','
            << effect.tint_color << ',' << effect.tint_amount << ',' << effect.effect_color << ','
            << effect.effect_opacity << ',' << effect.effect_size << ',' << effect.effect_distance << ','
            << effect.effect_angle << ',' << effect.effect_spread << ',' << effect.effect_falloff << ','
            << effect.effect_blur_type << ',' << effect.effect_samples << ',' << effect.effect_centered << ','
            << (int)effect.blend_mode;
    }
    return key.str();
}

static const TitleSourceData::CachedEffectLayer *ensure_cached_effect_layer(
    TitleSourceData *data, const Title &title, const Layer &layer, double title_time,
    int canvas_w, int canvas_h, const std::string &cache_suffix = std::string(),
    bool force_time_key = false, bool allow_plain_layer = false)
{
    if (!data || layer.type == LayerType::Clock || (!allow_plain_layer && !layer_has_stackable_pixel_effects(layer)))
        return nullptr;

    const std::string cache_id = (layer.id.empty() ? std::string("__anonymous_effect_layer") : layer.id) + cache_suffix;
    const std::string key = effect_layer_cache_key(data, title, layer, title_time, canvas_w, canvas_h, force_time_key);
    auto it = data->effect_layer_cache.find(cache_id);
    if (it == data->effect_layer_cache.end() || it->second.key != key) {
        QImage canvas(std::max(1, canvas_w), std::max(1, canvas_h), QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        auto surface = make_image_surface_for_qimage(canvas);
        auto layer_cr = make_cairo_context(surface.get());
        if (!surface || !layer_cr)
            return nullptr;
        cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_CLEAR);
        cairo_paint(layer_cr.get());
        cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_OVER);

        Layer base_layer = layer_without_stackable_pixel_effects(layer);
        const double cache_anchor_x = canvas.width() * 0.5;
        const double cache_anchor_y = canvas.height() * 0.5;
        neutralize_layer_transform_for_effect_cache(base_layer, 1.0, cache_anchor_x, cache_anchor_y);

        render_layer_unmasked(layer_cr.get(), title, base_layer, title_time, canvas_w, canvas_h);
        layer_cr.reset();
        cairo_surface_flush(surface.get());

        const double t = std::max(0.0, title_time - layer.in_time);
        apply_stackable_pixel_effects_to_surface(surface.get(), layer, t);
        cairo_surface_flush(surface.get());
        surface.reset();

        QRect bounds = image_alpha_bounds(canvas);
        TitleSourceData::CachedEffectLayer cached;
        cached.key = key;
        if (!bounds.isValid() || bounds.isEmpty()) {
            cached.image = QImage();
            cached.origin = QPointF(0.0, 0.0);
        } else {
            cached.image = canvas.copy(bounds);
            cached.origin = QPointF(bounds.x() - cache_anchor_x, bounds.y() - cache_anchor_y);
        }
        if (data->effect_layer_cache.size() > 128)
            data->effect_layer_cache.clear();
        it = data->effect_layer_cache.insert_or_assign(cache_id, std::move(cached)).first;
    }

    return &it->second;
}

static bool render_cached_effect_layer(cairo_t *cr, TitleSourceData *data, const Title &title, const Layer &layer,
                                       double title_time, int canvas_w, int canvas_h)
{
    if (!data || !layer_has_stackable_pixel_effects(layer) || layer.type == LayerType::Clock)
        return false;

    const auto *cached = ensure_cached_effect_layer(data, title, layer, title_time, canvas_w, canvas_h);
    if (!cached)
        return false;
    if (cached->image.isNull())
        return true;

    auto cached_surface = make_image_surface_for_const_qimage(cached->image);
    if (!cached_surface)
        return false;
    cairo_save(cr);
    apply_layer_world_transform(cr, title, layer, title_time);
    cairo_set_source_surface(cr, cached_surface.get(), cached->origin.x(), cached->origin.y());
    cairo_paint_with_alpha(cr, std::clamp(layer_chain_opacity(title, layer, title_time), 0.0, 1.0));
    cairo_restore(cr);
    return true;
}

static void render_layer_unmasked_with_stackable_effects(cairo_t *cr, TitleSourceData *data, const Title &title,
                                                         const Layer &layer, double title_time,
                                                         int canvas_w, int canvas_h);

static double source_frame_duration()
{
    struct obs_video_info ovi = {};
    if (obs_get_video_info(&ovi) && ovi.fps_num > 0 && ovi.fps_den > 0)
        return (double)ovi.fps_den / (double)ovi.fps_num;
    return 1.0 / 60.0;
}

static bool render_motion_blurred_layer(cairo_t *cr, TitleSourceData *data, const Title &title, const Layer &layer,
                                        double title_time, int canvas_w, int canvas_h)
{
    const LayerEffect *motion = layer_motion_blur_effect(layer);
    if (!motion || layer.type == LayerType::Clock)
        return false;

    const int samples = std::clamp(motion->effect_samples, 2, 64);
    const double amount = std::clamp((double)motion->effect_opacity, 0.0, 1.0);
    const double shutter_angle = std::clamp((double)motion->effect_size, 0.0, 720.0);
    Layer base_layer = layer_without_motion_blur_effects(layer);

    auto paint_fast_motion = [&](auto render_sample, auto render_original) -> bool {
        render_original(cr);
        const double frame_seconds = std::max(1.0 / 240.0, source_frame_duration());
        const double shutter_seconds = frame_seconds * shutter_angle / 360.0;
        const double title_duration = std::max(0.0, title.duration);
        int used_samples = 0;
        const double sample_alpha = amount / samples;

        for (int i = 0; i < samples; ++i) {
            const double f = samples == 1 ? 0.0 : (double)i / (double)(samples - 1);
            const double shutter_pos = motion->effect_centered ? (f - 0.5) : (f - 1.0);
            const double sample_time = std::clamp(title_time + shutter_seconds * shutter_pos, 0.0, title_duration);
            if (!layer_chain_visible(title, layer, sample_time))
                continue;

            cairo_push_group(cr);
            render_sample(cr, sample_time);
            cairo_pop_group_to_source(cr);
            cairo_paint_with_alpha(cr, sample_alpha);
            ++used_samples;
        }

        return true;
    };

    if (!data) {
        if (amount <= 0.0 || shutter_angle <= 0.0) {
            render_layer_unmasked_with_stackable_effects(cr, nullptr, title, base_layer, title_time, canvas_w, canvas_h);
            return true;
        }

        return paint_fast_motion(
            [&](cairo_t *sample_cr, double sample_time) {
                render_layer_unmasked_with_stackable_effects(sample_cr, nullptr, title, base_layer,
                                                             sample_time, canvas_w, canvas_h);
            },
            [&](cairo_t *sample_cr) {
                render_layer_unmasked_with_stackable_effects(sample_cr, nullptr, title, base_layer,
                                                             title_time, canvas_w, canvas_h);
            });
    }

    if (amount <= 0.0 || shutter_angle <= 0.0)
        return render_cached_effect_layer(cr, data, title, base_layer, title_time, canvas_w, canvas_h);

    if (gpu_effects_requested_for_source(data))
        base_layer = layer_without_stackable_pixel_effects(base_layer);

    const auto *cached = ensure_cached_effect_layer(
        data, title, base_layer, title_time, canvas_w, canvas_h, "::motion-base", false,
        gpu_effects_requested_for_source(data));
    if (!cached)
        return false;
    if (cached->image.isNull())
        return true;

    auto cached_surface = make_image_surface_for_const_qimage(cached->image);
    if (!cached_surface)
        return false;

    return paint_fast_motion(
        [&](cairo_t *sample_cr, double sample_time) {
            cairo_save(sample_cr);
            apply_layer_world_transform(sample_cr, title, layer, sample_time);
            cairo_set_source_surface(sample_cr, cached_surface.get(), cached->origin.x(), cached->origin.y());
            cairo_paint_with_alpha(sample_cr, std::clamp(layer_chain_opacity(title, layer, sample_time), 0.0, 1.0));
            cairo_restore(sample_cr);
        },
        [&](cairo_t *sample_cr) {
            cairo_save(sample_cr);
            apply_layer_world_transform(sample_cr, title, layer, title_time);
            cairo_set_source_surface(sample_cr, cached_surface.get(), cached->origin.x(), cached->origin.y());
            cairo_paint_with_alpha(sample_cr, std::clamp(layer_chain_opacity(title, layer, title_time), 0.0, 1.0));
            cairo_restore(sample_cr);
        });
}

static void render_layer_unmasked_with_stackable_effects(cairo_t *cr, TitleSourceData *data, const Title &title, const Layer &layer,
                                                         double title_time, int canvas_w, int canvas_h)
{
    if (layer_has_motion_blur(layer) &&
        render_motion_blurred_layer(cr, data, title, layer, title_time, canvas_w, canvas_h))
        return;

    if (gpu_effects_requested_for_source(data)) {
        Layer base_layer = layer_without_stackable_pixel_effects(layer);
        render_layer_unmasked(cr, title, base_layer, title_time, canvas_w, canvas_h);
        return;
    }

    if (render_cached_effect_layer(cr, data, title, layer, title_time, canvas_w, canvas_h))
        return;

    if (!layer_has_stackable_pixel_effects(layer)) {
        render_layer_unmasked(cr, title, layer, title_time, canvas_w, canvas_h);
        return;
    }

    Layer base_layer = layer_without_stackable_pixel_effects(layer);

    CairoSurfacePtr layer_surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, canvas_w, canvas_h));
    auto layer_cr = make_cairo_context(layer_surface.get());
    if (!layer_surface || cairo_surface_status(layer_surface.get()) != CAIRO_STATUS_SUCCESS || !layer_cr)
        return;
    cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_CLEAR);
    cairo_paint(layer_cr.get());
    cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_OVER);
    render_layer_unmasked(layer_cr.get(), title, base_layer, title_time, canvas_w, canvas_h);
    layer_cr.reset();

    const double t = std::max(0.0, title_time - layer.in_time);
    apply_stackable_pixel_effects_to_surface(layer_surface.get(), layer, t);
    cairo_set_source_surface(cr, layer_surface.get(), 0, 0);
    cairo_paint(cr);
}


static cairo_operator_t cairo_operator_for_layer_blend_mode(EffectBlendMode mode)
{
#if defined(CAIRO_VERSION) && CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 10, 0)
    switch (mode) {
    case EffectBlendMode::Multiply: return CAIRO_OPERATOR_MULTIPLY;
    case EffectBlendMode::Additive: return CAIRO_OPERATOR_ADD;
    case EffectBlendMode::Screen: return CAIRO_OPERATOR_SCREEN;
    case EffectBlendMode::Overlay: return CAIRO_OPERATOR_OVERLAY;
    case EffectBlendMode::Color: return CAIRO_OPERATOR_HSL_COLOR;
    case EffectBlendMode::Normal:
    default: return CAIRO_OPERATOR_OVER;
    }
#else
    (void)mode;
    return CAIRO_OPERATOR_OVER;
#endif
}

static void composite_layer_surface_with_mode(cairo_t *cr, cairo_surface_t *surface, EffectBlendMode mode)
{
    if (!cr || !surface)
        return;
    cairo_save(cr);
    cairo_set_operator(cr, cairo_operator_for_layer_blend_mode(mode));
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);
}

static void render_layer_with_mask(cairo_t *cr, TitleSourceData *data, const Title &title, const Layer &layer,
                                   double title_time, int canvas_w, int canvas_h)
{
    if (layer.use_as_scene_mask)
        return;
    if (layer.mask_mode == MaskMode::None || layer.mask_source_id.empty()) {
        render_layer_unmasked_with_stackable_effects(cr, data, title, layer, title_time, canvas_w, canvas_h);
        return;
    }
    const Layer *mask = find_layer_by_id(title, layer.mask_source_id);
    if (!mask || !layer_chain_visible(title, *mask, title_time)) {
        if (layer.mask_mode == MaskMode::InvertedAlpha)
            render_layer_unmasked_with_stackable_effects(cr, data, title, layer, title_time, canvas_w, canvas_h);
        return;
    }

    CairoSurfacePtr layer_surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, canvas_w, canvas_h));
    CairoSurfacePtr mask_surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, canvas_w, canvas_h));
    auto layer_cr = make_cairo_context(layer_surface.get());
    auto mask_cr = make_cairo_context(mask_surface.get());
    if (!layer_surface || !mask_surface ||
        cairo_surface_status(layer_surface.get()) != CAIRO_STATUS_SUCCESS ||
        cairo_surface_status(mask_surface.get()) != CAIRO_STATUS_SUCCESS ||
        !layer_cr || !mask_cr) {
        render_layer_unmasked_with_stackable_effects(cr, data, title, layer, title_time, canvas_w, canvas_h);
        return;
    }
    cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_CLEAR);
    cairo_paint(layer_cr.get());
    cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_OVER);
    cairo_set_operator(mask_cr.get(), CAIRO_OPERATOR_CLEAR);
    cairo_paint(mask_cr.get());
    cairo_set_operator(mask_cr.get(), CAIRO_OPERATOR_OVER);
    const bool effects_after_mask = layer.effect_stack_respects_masks && layer_has_stackable_pixel_effects(layer);
    if (effects_after_mask) {
        Layer base_layer = layer_without_stackable_pixel_effects(layer);
        render_layer_unmasked(layer_cr.get(), title, base_layer, title_time, canvas_w, canvas_h);
    } else {
        render_layer_unmasked_with_stackable_effects(layer_cr.get(), data, title, layer, title_time, canvas_w, canvas_h);
    }
    render_layer_unmasked(mask_cr.get(), title, *mask, title_time, canvas_w, canvas_h);
    layer_cr.reset();
    mask_cr.reset();

    if (effects_after_mask) {
        auto tmp_cr = make_cairo_context(layer_surface.get());
        if (!tmp_cr) return;
        cairo_set_operator(tmp_cr.get(), layer.mask_mode == MaskMode::Alpha ? CAIRO_OPERATOR_DEST_IN : CAIRO_OPERATOR_DEST_OUT);
        cairo_set_source_rgba(tmp_cr.get(), 0, 0, 0, 1);
        cairo_mask_surface(tmp_cr.get(), mask_surface.get(), 0, 0);
        tmp_cr.reset();

        const double lt = std::max(0.0, title_time - layer.in_time);
        apply_stackable_pixel_effects_to_surface(layer_surface.get(), layer, lt);
        cairo_set_source_surface(cr, layer_surface.get(), 0, 0);
        cairo_paint(cr);
    } else if (layer.mask_mode == MaskMode::Alpha) {
        cairo_set_source_surface(cr, layer_surface.get(), 0, 0);
        cairo_mask_surface(cr, mask_surface.get(), 0, 0);
    } else {
        auto tmp_cr = make_cairo_context(layer_surface.get());
        if (!tmp_cr) return;
        cairo_set_operator(tmp_cr.get(), CAIRO_OPERATOR_DEST_OUT);
        cairo_set_source_rgba(tmp_cr.get(), 0, 0, 0, 1);
        cairo_mask_surface(tmp_cr.get(), mask_surface.get(), 0, 0);
        tmp_cr.reset();
        cairo_set_source_surface(cr, layer_surface.get(), 0, 0);
        cairo_paint(cr);
    }
}

/* Composite a full title frame into pixel_buf */
static void render_title_frame(TitleSourceData *data,
                                const Title &title, double t)
{
    uint32_t w = clamped_source_dimension(title.width);
    uint32_t h = clamped_source_dimension(title.height);

    /* (Re)allocate buffer & texture if size changed */
    if (data->tex_w != w || data->tex_h != h) {
        bool texture_created = false;
        {
            std::lock_guard<std::mutex> lock(data->texture_mutex);
            obs_enter_graphics();
            if (data->texture) gs_texture_destroy(data->texture);
            if (data->scene_mask_alpha_texture) gs_texture_destroy(data->scene_mask_alpha_texture);
            data->texture = gs_texture_create(w, h, GS_BGRA, 1, nullptr, GS_DYNAMIC);
            data->scene_mask_alpha_texture = nullptr;
            data->scene_mask_alpha_w = 0;
            data->scene_mask_alpha_h = 0;
            texture_created = data->texture != nullptr;
            obs_leave_graphics();
        }

        if (!texture_created) {
            data->tex_w = 0;
            data->tex_h = 0;
            data->pixel_buf.clear();
            data->dirty = false;
            return;
        }

        data->tex_w = w;
        data->tex_h = h;
        data->pixel_buf.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
        data->effect_layer_cache.clear();
    }

    /* Cairo surface over our buffer */
    CairoSurfacePtr surface(cairo_image_surface_create_for_data(
        data->pixel_buf.data(),
        CAIRO_FORMAT_ARGB32,  /* == BGRA on LE – matches GS_BGRA */
        (int)w, (int)h,
        (int)w * 4));
    auto cr = make_cairo_context(surface.get());
    if (!surface || cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS || !cr) {
        data->dirty = false;
        return;
    }

    /* Clear with background */
    double br, bg, bb, ba;
    unpack_color(title.bg_color, br, bg, bb, ba);
    cairo_set_operator(cr.get(), CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr.get(), br, bg, bb, ba);
    cairo_paint(cr.get());
    cairo_set_operator(cr.get(), CAIRO_OPERATOR_OVER);

    const bool background_persistence = title.cue_background_persistence &&
        title.cue_persistence_transition && title.current_cue_row >= 0 && !title.live_text_rows.empty();
    const double persistence_time = cue_persistence_hold_time(title);
    const auto exposed = background_persistence ? exposed_text_layers(title) : std::vector<std::shared_ptr<Layer>>();

    /* Render layers bottom → top */
    for (auto &layer : title.layers) {
        if (!layer) continue;

        double layer_time = t;
        if (background_persistence) {
            const int exposed_index = exposed_text_layer_index(exposed, layer);
            const bool persistent_text = exposed_index >= 0 && title.cue_text_persistence &&
                exposed_index < (int)title.cue_persistent_text_columns.size() &&
                title.cue_persistent_text_columns[exposed_index];
            if (exposed_index < 0 || persistent_text)
                layer_time = persistence_time;
        }

        if (!layer_chain_visible(title, *layer, layer_time)) continue;
        if (!layer_should_render_as_visible_content(title, *layer)) continue;

        if (layer->blend_mode == EffectBlendMode::Normal) {
            render_layer_with_mask(cr.get(), data, title, *layer, layer_time, (int)w, (int)h);
        } else {
            CairoSurfacePtr layer_surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)w, (int)h));
            auto layer_cr = make_cairo_context(layer_surface.get());
            if (layer_surface && cairo_surface_status(layer_surface.get()) == CAIRO_STATUS_SUCCESS && layer_cr) {
                cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_CLEAR);
                cairo_paint(layer_cr.get());
                cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_OVER);
                render_layer_with_mask(layer_cr.get(), data, title, *layer, layer_time, (int)w, (int)h);
                layer_cr.reset();
                composite_layer_surface_with_mode(cr.get(), layer_surface.get(), layer->blend_mode);
            } else {
                render_layer_with_mask(cr.get(), data, title, *layer, layer_time, (int)w, (int)h);
            }
        }
    }

    cr.reset();
    cairo_surface_flush(surface.get());
    surface.reset();

    /*
     * Cairo renders CAIRO_FORMAT_ARGB32 as premultiplied BGRA on little-endian
     * platforms. OBS' default source effect samples straight-alpha textures, so
     * uploading Cairo's premultiplied color channels directly causes OBS to
     * multiply edge pixels a second time during scene compositing. Convert the
     * finished frame to straight-alpha BGRA before the texture upload to keep
     * antialiased transparent and semi-transparent edges from developing dark
     * fringes or halos over other sources.
     */
    unpremultiply_bgra_for_obs(data->pixel_buf.data(),
                               static_cast<size_t>(w) * static_cast<size_t>(h));

    /* Upload to GPU */
    {
        std::lock_guard<std::mutex> lock(data->texture_mutex);
        if (data->texture) {
            obs_enter_graphics();
            const uint8_t *ptr = data->pixel_buf.data();
            uint32_t linesize  = w * 4;
            gs_texture_set_image(data->texture, ptr, linesize, false);
            obs_leave_graphics();
        }
    }

    data->dirty = false;
}

QImage render_title_to_image(const Title &title, double t)
{
    const int w = std::max(1, title.width);
    const int h = std::max(1, title.height);
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);

    auto surface = make_image_surface_for_qimage(image);
    auto cr = make_cairo_context(surface.get());
    if (!surface || !cr) {
        image.fill(Qt::transparent);
        return image;
    }

    double br, bg, bb, ba;
    unpack_color(title.bg_color, br, bg, bb, ba);
    cairo_set_operator(cr.get(), CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr.get(), br, bg, bb, ba);
    cairo_paint(cr.get());
    cairo_set_operator(cr.get(), CAIRO_OPERATOR_OVER);

    const double clamped_time = std::clamp(t, 0.0, std::max(0.0, title.duration));
    for (auto &layer : title.layers) {
        if (!layer || !layer_chain_visible(title, *layer, clamped_time)) continue;
        if (!layer_should_render_as_visible_content(title, *layer)) continue;

        if (layer->blend_mode == EffectBlendMode::Normal) {
            render_layer_with_mask(cr.get(), nullptr, title, *layer, clamped_time, w, h);
        } else {
            CairoSurfacePtr layer_surface(cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h));
            auto layer_cr = make_cairo_context(layer_surface.get());
            if (layer_surface && cairo_surface_status(layer_surface.get()) == CAIRO_STATUS_SUCCESS && layer_cr) {
                cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_CLEAR);
                cairo_paint(layer_cr.get());
                cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_OVER);
                render_layer_with_mask(layer_cr.get(), nullptr, title, *layer, clamped_time, w, h);
                layer_cr.reset();
                composite_layer_surface_with_mode(cr.get(), layer_surface.get(), layer->blend_mode);
            } else {
                render_layer_with_mask(cr.get(), nullptr, title, *layer, clamped_time, w, h);
            }
        }
    }

    cr.reset();
    cairo_surface_flush(surface.get());
    return image;
}

/* ══════════════════════════════════════════════════════════════════
 *  OBS source callbacks
 * ══════════════════════════════════════════════════════════════════ */
static const char *source_get_name(void *)
{
    return obsgs_tr_c("OBSTitles.SourceName");
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
    auto *data = new TitleSourceData();
    data->source = source;
    if (settings) {
        data->title_id = obs_data_get_string(settings, PROP_TITLE_ID);
        data->loop = obs_data_get_bool(settings, PROP_LOOP);
        data->speed = (float)obs_data_get_double(settings, PROP_SPEED);
        refresh_scene_mask_configs(data, settings);
    }
    data->last_tick = std::chrono::steady_clock::now();
    data->last_clock_refresh = data->last_tick;
    if (auto title = TitleDataStore::instance().get_title(data->title_id))
        data->seen_cue_revision = title->cue_revision;
    data->seen_store_revision = TitleDataStore::instance().revision();
    data->playing = false;
    data->waiting_for_cue = true;
    data->active_cue_row = -1;
    data->dirty = true;
    return data;
}

static void source_destroy(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data) return;
    release_active_scene_mask_scenes(data);
    {
        std::lock_guard<std::mutex> lock(data->texture_mutex);
        obs_enter_graphics();
        if (data->texture) gs_texture_destroy(data->texture);
        if (data->scene_mask_alpha_texture) gs_texture_destroy(data->scene_mask_alpha_texture);
        if (data->scene_mask_scene_texrender) gs_texrender_destroy(data->scene_mask_scene_texrender);
        if (data->scene_mask_effect) gs_effect_destroy(data->scene_mask_effect);
        data->gpu_filter_pipeline.reset();
        obs_leave_graphics();
        data->texture = nullptr;
        data->scene_mask_alpha_texture = nullptr;
        data->scene_mask_scene_texrender = nullptr;
        data->scene_mask_effect = nullptr;
        data->scene_mask_alpha_w = 0;
        data->scene_mask_alpha_h = 0;
        data->tex_w = 0;
        data->tex_h = 0;
    }
    delete data;
}

static void source_update(void *priv, obs_data_t *settings)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data || !settings) return;
    const bool keep_scene_masks_foreground = data->scene_mask_foreground_active;
    if (keep_scene_masks_foreground)
        release_active_scene_mask_scenes(data);

    data->title_id = obs_data_get_string(settings, PROP_TITLE_ID);
    data->loop     = obs_data_get_bool(settings,   PROP_LOOP);
    data->speed    = (float)obs_data_get_double(settings, PROP_SPEED);
    refresh_scene_mask_configs(data, settings);
    if (keep_scene_masks_foreground)
        activate_scene_mask_scenes(data);
    data->playhead = 0.0;
    data->playback_reverse = false;
    data->cue_phase = TitleSourceData::CuePhase::FreeRun;
    data->playing = false;
    data->waiting_for_cue = true;
    data->active_cue_row = -1;
    if (auto title = TitleDataStore::instance().get_title(data->title_id))
        data->seen_cue_revision = title->cue_revision;
    else
        data->seen_cue_revision = 0;
    data->last_clock_refresh = std::chrono::steady_clock::now();
    data->seen_store_revision = TitleDataStore::instance().revision();
    data->effect_layer_cache.clear();
    data->dirty    = true;
}

static void source_activate(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data)
        return;
    data->scene_mask_foreground_active = true;
    activate_scene_mask_scenes(data);
}

static void source_deactivate(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data)
        return;
    data->scene_mask_foreground_active = false;
    release_active_scene_mask_scenes(data);
}

static uint32_t source_get_width(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data) return 1920;
    auto title = TitleDataStore::instance().get_title(data->title_id);
    return title ? clamped_source_dimension(title->width) : 1920;
}

static uint32_t source_get_height(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data) return 1080;
    auto title = TitleDataStore::instance().get_title(data->title_id);
    return title ? clamped_source_dimension(title->height) : 1080;
}

static void source_video_tick(void *priv, float seconds)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data || data->title_id.empty()) return;

    auto title = TitleDataStore::instance().get_title(data->title_id);
    if (!title) return;

    if (title->cue_revision != data->seen_cue_revision) {
        double loop_end = std::clamp(title->loop_end, title->loop_start, title->duration);
        double pause_time = std::clamp(title->pause_time, 0.0, title->duration);
        bool has_pending = title->pending_cue_row >= 0 &&
                           title->pending_cue_row < (int)title->live_text_rows.size();
        bool has_current = title->current_cue_row >= 0 &&
                           title->current_cue_row < (int)title->live_text_rows.size();
        bool is_uncue = !has_pending && !has_current && data->active_cue_row >= 0;
        if (title->playback_mode == 1) {
            if (has_pending) {
                data->playhead = loop_end;
                data->cue_phase = TitleSourceData::CuePhase::OutroThenIntro;
            } else if (is_uncue) {
                data->playhead = loop_end;
                data->cue_phase = TitleSourceData::CuePhase::OutroOnly;
            } else {
                data->playhead = 0.0;
                data->cue_phase = TitleSourceData::CuePhase::IntroLoop;
            }
        } else if (title->playback_mode == 2 && (has_pending || is_uncue)) {
            data->playhead = pause_time;
            data->cue_phase = has_pending
                ? TitleSourceData::CuePhase::OutroThenIntro
                : TitleSourceData::CuePhase::OutroOnly;
        } else {
            if (has_pending) {
                apply_live_text_row(title, title->pending_cue_row);
                title->current_cue_row = title->pending_cue_row;
                title->pending_cue_row = -1;
                has_current = true;
                TitleDataStore::instance().touch_runtime_change();
            }
            if (!is_uncue)
                data->playhead = 0.0;
            data->cue_phase = TitleSourceData::CuePhase::FreeRun;
        }
        if (has_current)
            data->active_cue_row = title->current_cue_row;
        data->seen_cue_revision = title->cue_revision;
        data->playback_reverse = false;
        data->waiting_for_cue = false;
        data->playing = true;
        data->dirty = true;
    }

    const bool has_clock_layer = title_has_clock_layer(title);
    const bool has_ticker_layer = title_has_ticker_layer(title);
    const bool has_timeline_animation = title_has_animation(title);
    const bool static_clock_title = has_clock_layer && !has_timeline_animation;

    if (data->playing && !static_clock_title) {
        double dt = (double)seconds * data->speed;
        double duration = std::max(0.001, title->duration);
        double loop_start = std::clamp(title->loop_start, 0.0, title->duration);
        double loop_end = std::clamp(title->loop_end, loop_start, title->duration);

        const bool ping_pong_loop = title->playback_mode == 1 && title->loop_type == 1 &&
                                    (data->cue_phase == TitleSourceData::CuePhase::FreeRun ||
                                     data->cue_phase == TitleSourceData::CuePhase::IntroLoop);
        if (ping_pong_loop) {
            data->playhead += data->playback_reverse ? -dt : dt;
        } else {
            data->playhead += dt;
        }

        if (data->cue_phase == TitleSourceData::CuePhase::IntroLoop && loop_end > loop_start) {
            double loop_len = std::max(0.001, loop_end - loop_start);
            if (title->loop_type == 1) {
                if (!data->playback_reverse && data->playhead >= loop_end) {
                    clear_cue_persistence_transition(title);
                    data->playhead = loop_end - std::fmod(data->playhead - loop_end, loop_len);
                    data->playback_reverse = true;
                } else if (data->playback_reverse && data->playhead <= loop_start) {
                    data->playhead = loop_start + std::fmod(loop_start - data->playhead, loop_len);
                    data->playback_reverse = false;
                }
            } else if (data->playhead >= loop_end) {
                clear_cue_persistence_transition(title);
                data->playhead = loop_start + std::fmod(data->playhead - loop_start, loop_len);
            }
        } else if ((data->cue_phase == TitleSourceData::CuePhase::OutroThenIntro ||
                    data->cue_phase == TitleSourceData::CuePhase::OutroOnly) &&
                   data->playhead >= title->duration) {
            double next_intro_time = std::max(0.0, data->playhead - title->duration);
            if (data->cue_phase == TitleSourceData::CuePhase::OutroOnly) {
                data->playhead = title->duration;
                data->playing = false;
                data->cue_phase = TitleSourceData::CuePhase::FreeRun;
                data->active_cue_row = -1;
                title->current_cue_row = -1;
                title->pending_cue_row = -1;
                title->cue_persistence_transition = false;
                title->cue_persistent_text_columns.clear();
                TitleDataStore::instance().touch_runtime_change();
            } else {
                if (title->pending_cue_row >= 0 && title->pending_cue_row < (int)title->live_text_rows.size()) {
                    apply_live_text_row(title, title->pending_cue_row);
                    title->current_cue_row = title->pending_cue_row;
                    title->pending_cue_row = -1;
                    data->active_cue_row = title->current_cue_row;
                    TitleDataStore::instance().touch_runtime_change();
                }
                if (title->playback_mode == 1) {
                    if (loop_end > loop_start && next_intro_time >= loop_end) {
                        next_intro_time = loop_start + std::fmod(next_intro_time - loop_start,
                                                                 std::max(0.001, loop_end - loop_start));
                    }
                    data->playhead = std::clamp(next_intro_time, 0.0, title->duration);
                    data->cue_phase = TitleSourceData::CuePhase::IntroLoop;
                } else {
                    data->playhead = 0.0;
                    data->cue_phase = TitleSourceData::CuePhase::FreeRun;
                }
            }
            data->playback_reverse = false;
        } else if (data->cue_phase == TitleSourceData::CuePhase::FreeRun) {
            if (title->playback_mode == 1) {
                double loop_len = std::max(0.001, loop_end - loop_start);
                if (loop_end <= loop_start + 0.0001) {
                    if (data->playhead >= title->duration)
                        data->playhead = std::fmod(data->playhead, duration);
                } else if (title->loop_type == 1) {
                    if (!data->playback_reverse && data->playhead >= loop_end) {
                        data->playhead = loop_end - std::fmod(data->playhead - loop_end, loop_len);
                        data->playback_reverse = true;
                    } else if (data->playback_reverse && data->playhead <= loop_start) {
                        data->playhead = loop_start + std::fmod(loop_start - data->playhead, loop_len);
                        data->playback_reverse = false;
                    }
                } else if (data->playhead >= loop_end) {
                    data->playhead = loop_start + std::fmod(data->playhead - loop_end, loop_len);
                }
            } else if (title->playback_mode == 2) {
                double pause_time = std::clamp(title->pause_time, 0.0, title->duration);
                if (data->playhead >= pause_time) {
                    data->playhead = pause_time;
                    data->playing = false;
                    clear_cue_persistence_transition(title);
                }
            } else if (data->playhead >= title->duration) {
                data->playhead = title->duration;
                data->playing  = false;
                if (title->current_cue_row >= 0 || title->pending_cue_row >= 0 || data->active_cue_row >= 0) {
                    title->current_cue_row = -1;
                    title->pending_cue_row = -1;
                    title->cue_persistence_transition = false;
                    title->cue_persistent_text_columns.clear();
                    data->active_cue_row = -1;
                    TitleDataStore::instance().touch_runtime_change();
                }
            }
        }
        data->dirty = true;
    }


    if (has_ticker_layer)
        data->dirty = true;

    if (static_clock_title || (!data->playing && has_clock_layer)) {
        auto now = std::chrono::steady_clock::now();
        if (now - data->last_clock_refresh >= std::chrono::seconds(1)) {
            data->last_clock_refresh = now;
            data->dirty = true;
        }
    }

    uint64_t revision = TitleDataStore::instance().revision();
    if (revision != data->seen_store_revision) {
        data->seen_store_revision = revision;
        if (data->source) {
            obs_data_t *settings = obs_source_get_settings(data->source);
            if (settings) {
                const bool keep_scene_masks_foreground = data->scene_mask_foreground_active;
                if (keep_scene_masks_foreground)
                    release_active_scene_mask_scenes(data);
                refresh_scene_mask_configs(data, settings);
                if (keep_scene_masks_foreground)
                    activate_scene_mask_scenes(data);
                obs_data_release(settings);
            }
        }
        data->effect_layer_cache.clear();
        data->dirty = true;
    }

    if (data->dirty) {
        Title render_snapshot = snapshot_title_for_render(*title);
        render_title_frame(data, render_snapshot, data->playhead);
    }
}



static void apply_layer_world_transform_gs(const Title &title, const Layer &layer,
                                           double title_time, int depth = 0)
{
    if (depth > 64)
        return;
    if (!layer.parent_id.empty()) {
        if (const Layer *parent = find_layer_by_id(title, layer.parent_id))
            apply_layer_world_transform_gs(title, *parent, title_time, depth + 1);
    }
    const double lt = std::max(0.0, title_time - layer.in_time);
    gs_matrix_translate3f((float)layer.pos_x.evaluate(lt), (float)layer.pos_y.evaluate(lt), 0.0f);
    gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, (float)(layer.rotation.evaluate(lt) * kPi / 180.0));
    gs_matrix_scale3f((float)layer.scale_x.evaluate(lt), (float)layer.scale_y.evaluate(lt), 1.0f);
}

static const TitleSourceData::SceneMaskConfig *scene_mask_config_for_layer(
    const TitleSourceData &data, const std::string &layer_id)
{
    for (const auto &cfg : data.scene_masks) {
        if (cfg.layer_id == layer_id)
            return &cfg;
    }
    return nullptr;
}

struct HiddenSceneMaskItem {
    obs_sceneitem_t *item = nullptr;
    bool visible = false;
};

struct HideSceneMaskSourceContext {
    obs_source_t *source = nullptr;
    std::vector<HiddenSceneMaskItem> hidden_items;
};

static bool hide_scene_mask_source_item(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
    auto *ctx = static_cast<HideSceneMaskSourceContext *>(param);
    if (!ctx || !ctx->source || !item)
        return true;

    if (obs_sceneitem_is_group(item))
        obs_sceneitem_group_enum_items(item, hide_scene_mask_source_item, param);

    obs_source_t *item_source = obs_sceneitem_get_source(item);
    if (item_source == ctx->source && obs_sceneitem_visible(item)) {
        obs_sceneitem_addref(item);
        ctx->hidden_items.push_back({item, true});
        obs_sceneitem_set_visible(item, false);
    }

    return true;
}

static HideSceneMaskSourceContext hide_scene_mask_source_items(obs_source_t *scene_source, obs_source_t *source)
{
    HideSceneMaskSourceContext ctx;
    ctx.source = source;
    if (!scene_source || !source)
        return ctx;

    if (obs_scene_t *scene = obs_scene_from_source(scene_source))
        obs_scene_enum_items(scene, hide_scene_mask_source_item, &ctx);

    return ctx;
}

static void restore_scene_mask_source_items(HideSceneMaskSourceContext &ctx)
{
    for (auto &hidden : ctx.hidden_items) {
        if (hidden.item) {
            obs_sceneitem_set_visible(hidden.item, hidden.visible);
            obs_sceneitem_release(hidden.item);
        }
    }
    ctx.hidden_items.clear();
}

static constexpr const char *kSceneMaskEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform texture2d mask;

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

float4 PSMaskedScene(VertDataOut v_in) : TARGET
{
    float4 rgba = image.Sample(textureSampler, v_in.uv);
    float mask_alpha = mask.Sample(textureSampler, v_in.uv).a;
    rgba.rgb *= (rgba.a > 0.0) ? (1.0 / rgba.a) : 0.0;
    rgba.a *= mask_alpha;
    rgba.rgb *= rgba.a;
    return rgba;
}

technique Draw
{
    pass
    {
        vertex_shader = VSDefault(v_in);
        pixel_shader = PSMaskedScene(v_in);
    }
}
)";

static gs_effect_t *scene_mask_effect_for_data(TitleSourceData *data)
{
    if (!data)
        return nullptr;
    if (!data->scene_mask_effect)
        data->scene_mask_effect = gs_effect_create(kSceneMaskEffect, "obs-gsp-scene-mask.effect", nullptr);
    return data->scene_mask_effect;
}

static bool render_scene_mask_alpha_pixels(const Title &title, const Layer &layer,
                                           double title_time, int canvas_w, int canvas_h,
                                           std::vector<uint8_t> &pixels)
{
    if (canvas_w <= 0 || canvas_h <= 0)
        return false;

    const double lt = std::max(0.0, title_time - layer.in_time);
    const double w = eval_box_width(layer, lt);
    const double h = eval_box_height(layer, lt);
    if (w <= 0.0 || h <= 0.0)
        return false;

    pixels.assign((size_t)canvas_w * (size_t)canvas_h * 4, 0);
    CairoSurfacePtr surface(cairo_image_surface_create_for_data(
        pixels.data(), CAIRO_FORMAT_ARGB32, canvas_w, canvas_h, canvas_w * 4));
    auto cr = make_cairo_context(surface.get());
    if (!surface || cairo_surface_status(surface.get()) != CAIRO_STATUS_SUCCESS || !cr)
        return false;

    cairo_set_operator(cr.get(), CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr.get());
    cairo_set_operator(cr.get(), CAIRO_OPERATOR_OVER);

    if (layer.type == LayerType::SolidRect || layer.type == LayerType::Shape) {
        cairo_save(cr.get());
        apply_layer_world_transform(cr.get(), title, layer, title_time);
        cairo_translate(cr.get(), -eval_origin_x(layer, lt) * w, -eval_origin_y(layer, lt) * h);
        cairo_add_layer_shape(cr.get(), layer, w, h);
        cairo_set_source_rgba(cr.get(), 1.0, 1.0, 1.0,
                              std::clamp(layer_chain_opacity(title, layer, title_time), 0.0, 1.0));
        if (layer.type == LayerType::Shape && layer.shape_type == ShapeType::Line) {
            cairo_set_line_width(cr.get(), std::max(1.0, eval_outline_width(layer, lt)));
            cairo_set_line_cap(cr.get(), CAIRO_LINE_CAP_ROUND);
            cairo_stroke(cr.get());
        } else {
            cairo_fill(cr.get());
        }
        cairo_restore(cr.get());
    } else {
        render_layer_unmasked_with_stackable_effects(cr.get(), nullptr, title, layer, title_time, canvas_w, canvas_h);
    }

    cr.reset();
    cairo_surface_flush(surface.get());
    return true;
}

static bool ensure_scene_mask_alpha_texture(TitleSourceData *data, uint32_t w, uint32_t h)
{
    if (!data || w == 0 || h == 0)
        return false;

    if (data->scene_mask_alpha_texture &&
        (data->scene_mask_alpha_w != w || data->scene_mask_alpha_h != h)) {
        gs_texture_destroy(data->scene_mask_alpha_texture);
        data->scene_mask_alpha_texture = nullptr;
        data->scene_mask_alpha_w = 0;
        data->scene_mask_alpha_h = 0;
    }

    if (!data->scene_mask_alpha_texture) {
        data->scene_mask_alpha_texture = gs_texture_create(w, h, GS_BGRA, 1, nullptr, GS_DYNAMIC);
        if (!data->scene_mask_alpha_texture)
            return false;
        data->scene_mask_alpha_w = w;
        data->scene_mask_alpha_h = h;
    }

    return true;
}

static void render_scene_masks_gpu(TitleSourceData *data, const Title &title, double title_time)
{
    if (!data || data->scene_masks.empty())
        return;

    if (!data->scene_mask_scene_texrender)
        data->scene_mask_scene_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (!data->scene_mask_scene_texrender || !scene_mask_effect_for_data(data))
        return;

    for (const Layer *layer : scene_mask_layers(title)) {
        if (!layer || !layer_chain_visible(title, *layer, title_time))
            continue;
        const auto *cfg = scene_mask_config_for_layer(*data, layer->id);
        if (!cfg || cfg->scene_name.empty())
            continue;

        obs_source_t *scene = obs_get_source_by_name(cfg->scene_name.c_str());
        if (!scene)
            continue;

        const double lt = std::max(0.0, title_time - layer->in_time);
        const double w = eval_box_width(*layer, lt);
        const double h = eval_box_height(*layer, lt);
        if (w <= 0.0 || h <= 0.0) {
            obs_source_release(scene);
            continue;
        }
        const double zoom = std::clamp(cfg->zoom, 0.01, 100.0);

        std::vector<uint8_t> mask_pixels;
        if (!render_scene_mask_alpha_pixels(title, *layer, title_time, (int)data->tex_w, (int)data->tex_h, mask_pixels) ||
            !ensure_scene_mask_alpha_texture(data, data->tex_w, data->tex_h)) {
            obs_source_release(scene);
            continue;
        }
        gs_texture_set_image(data->scene_mask_alpha_texture, mask_pixels.data(), data->tex_w * 4, false);

        gs_viewport_push();
        gs_projection_push();
        gs_blend_state_push();
        gs_texrender_reset(data->scene_mask_scene_texrender);
        if (!gs_texrender_begin(data->scene_mask_scene_texrender, data->tex_w, data->tex_h)) {
            gs_blend_state_pop();
            gs_projection_pop();
            gs_viewport_pop();
            obs_source_release(scene);
            continue;
        }
        gs_ortho(0.0f, (float)data->tex_w, 0.0f, (float)data->tex_h, -100.0f, 100.0f);

        struct vec4 clear;
        vec4_zero(&clear);
        gs_clear(GS_CLEAR_COLOR, &clear, 1.0f, 0);

        gs_matrix_push();
        gs_matrix_identity();
        if (cfg->move_with_mask) {
            apply_layer_world_transform_gs(title, *layer, title_time);
            gs_matrix_translate3f((float)(-eval_origin_x(*layer, lt) * w + cfg->x),
                                  (float)(-eval_origin_y(*layer, lt) * h + cfg->y), 0.0f);
        } else {
            gs_matrix_translate3f((float)cfg->x, (float)cfg->y, 0.0f);
        }
        gs_matrix_scale3f((float)zoom, (float)zoom, 1.0f);
        auto hidden_items = hide_scene_mask_source_items(scene, data->source);
        obs_source_video_render(scene);
        restore_scene_mask_source_items(hidden_items);
        gs_matrix_pop();

        gs_texrender_end(data->scene_mask_scene_texrender);
        gs_blend_state_pop();
        gs_projection_pop();
        gs_viewport_pop();

        gs_texture_t *scene_texture = gs_texrender_get_texture(data->scene_mask_scene_texrender);
        gs_effect_t *effect = scene_mask_effect_for_data(data);
        if (scene_texture && effect) {
            gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
            gs_eparam_t *mask = gs_effect_get_param_by_name(effect, "mask");
            if (image && mask) {
                gs_effect_set_texture(image, scene_texture);
                gs_effect_set_texture(mask, data->scene_mask_alpha_texture);
                gs_blend_state_push();
                gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
                while (gs_effect_loop(effect, "Draw"))
                    gs_draw_sprite(scene_texture, 0, data->tex_w, data->tex_h);
                gs_blend_state_pop();
            }
        }

        obs_source_release(scene);
    }
}

static void source_video_render(void *priv, gs_effect_t * /*effect*/)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data) return;

    auto title = TitleDataStore::instance().get_title(data->title_id);
    const TitleGpuEffectUsage effect_usage = title ? title_gpu_effect_usage(*title) : TitleGpuEffectUsage{};

    {
        std::lock_guard<std::mutex> lock(data->texture_mutex);
        if (!data->texture) return;

        bool rendered_with_gpu_pipeline = false;
        if (TitlePreferences::use_gpu() && TitlePreferences::gpu_available()) {
            if (!data->gpu_filter_pipeline)
                data->gpu_filter_pipeline = std::make_unique<TitleGpuFilterPipeline>();
            rendered_with_gpu_pipeline = data->gpu_filter_pipeline->render(
                data->texture, data->tex_w, data->tex_h, effect_usage);
            if (rendered_with_gpu_pipeline) {
                TitlePreferences::set_gpu_available(true);
            } else {
                TitlePreferences::set_gpu_available(
                    false,
                    data->gpu_filter_pipeline ? data->gpu_filter_pipeline->last_error() : "GPU effects could not start.");
                data->effect_layer_cache.clear();
                data->dirty = true;
            }
        }

        if (!rendered_with_gpu_pipeline) {
            gs_effect_t *eff = obs_get_base_effect(OBS_EFFECT_DEFAULT);
            if (!eff) return;

            gs_eparam_t *image = gs_effect_get_param_by_name(eff, "image");
            if (!image) return;

            gs_effect_set_texture(image, data->texture);

            while (gs_effect_loop(eff, "Draw"))
                gs_draw_sprite(data->texture, 0, 0, 0);
        }
    }

    if (title)
        render_scene_masks_gpu(data, *title, data->playhead);
}


static void add_scene_list_items(obs_property_t *property)
{
    obs_property_list_add_string(property, "", "");
    obs_frontend_source_list scenes = {};
    obs_frontend_get_scenes(&scenes);
    for (size_t i = 0; i < scenes.sources.num; ++i) {
        obs_source_t *scene = scenes.sources.array[i];
        const char *name = obs_source_get_name(scene);
        if (name && *name)
            obs_property_list_add_string(property, name, name);
    }
    obs_frontend_source_list_free(&scenes);
}

static void add_scene_mask_properties_for_title(obs_properties_t *props, const std::string &title_id)
{
    obs_properties_remove_by_name(props, PROP_SCENE_MASKS_GROUP);

    auto title = TitleDataStore::instance().get_title(title_id);
    auto masks = scene_mask_layers(title);
    if (masks.empty())
        return;

    obs_properties_t *group = obs_properties_create();
    for (const auto &layer : masks) {
        if (!layer) continue;
        obs_properties_t *layer_group = obs_properties_create();
        obs_property_t *scene = obs_properties_add_list(
            layer_group, scene_mask_key(layer->id, "scene").c_str(), "Target OBS scene",
            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        add_scene_list_items(scene);
        obs_properties_add_float_slider(layer_group, scene_mask_key(layer->id, "zoom_percent").c_str(),
            "Scene Zoom Level (%)", 1.0, 800.0, 1.0);
        obs_properties_add_float_slider(layer_group, scene_mask_key(layer->id, "x").c_str(),
            "Horizontal Position", -9999.0, 9999.0, 1.0);
        obs_properties_add_float_slider(layer_group, scene_mask_key(layer->id, "y").c_str(),
            "Vertical Position", -9999.0, 9999.0, 1.0);
        obs_properties_add_bool(layer_group, scene_mask_key(layer->id, "move_with_mask").c_str(),
            "Move Scene With Mask");
        obs_properties_add_group(group, (std::string("scene_mask_group_") + layer->id).c_str(),
            layer->name.c_str(), OBS_GROUP_NORMAL, layer_group);
    }
    obs_properties_add_group(props, PROP_SCENE_MASKS_GROUP, "Scene Masks", OBS_GROUP_NORMAL, group);
}

static bool source_title_property_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
    const std::string title_id = obs_data_get_string(settings, PROP_TITLE_ID);
    set_scene_mask_property_defaults(settings, title_id);
    add_scene_mask_properties_for_title(props, title_id);
    return true;
}

/* ── Properties panel ─────────────────────────────────────────────── */
static obs_properties_t *source_get_properties(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    obs_properties_t *props = obs_properties_create();

    /* Title selector */
    obs_property_t *p = obs_properties_add_list(
        props, PROP_TITLE_ID, obsgs_tr_c("OBSTitles.TitleID"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

    obs_property_list_add_string(p, obsgs_tr_c("OBSTitles.NoTitle"), "");
    for (auto &t : TitleDataStore::instance().titles())
        obs_property_list_add_string(p, t->name.c_str(), t->id.c_str());
    obs_property_set_modified_callback(p, source_title_property_modified);

    obs_properties_add_bool(props,   PROP_LOOP,  obsgs_tr_c("OBSTitles.Loop"));
    obs_properties_add_float_slider(props, PROP_SPEED,
        obsgs_tr_c("OBSTitles.Speed"), 0.1, 4.0, 0.05);

    add_scene_mask_properties_for_title(props, data ? data->title_id : std::string());

    return props;
}

static void source_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, PROP_TITLE_ID, "");
    obs_data_set_default_bool(settings,   PROP_LOOP,     true);
    obs_data_set_default_double(settings, PROP_SPEED,    1.0);
}

/* ══════════════════════════════════════════════════════════════════
 *  Registration
 * ══════════════════════════════════════════════════════════════════ */
void title_source_register()
{
    static obs_source_info si = {};
    si.id             = "obs_graphics_studio_pro_source";
    si.type           = OBS_SOURCE_TYPE_INPUT;
    si.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    si.get_name       = source_get_name;
    si.create         = source_create;
    si.destroy        = source_destroy;
    si.update         = source_update;
    si.get_width      = source_get_width;
    si.get_height     = source_get_height;
    si.video_tick     = source_video_tick;
    si.video_render   = source_video_render;
    si.activate       = source_activate;
    si.deactivate     = source_deactivate;
    si.get_properties = source_get_properties;
    si.get_defaults   = source_get_defaults;

    obs_register_source(&si);
    blog(LOG_INFO, "[OBS Graphics Studio Pro] Source type registered.");
}
