/*
 * title-source.cpp
 *
 * Unified GPU compositor for OBS live output, editor preview and final cache
 * readback. Supported Text/Clock layers are composed from persistent SDF glyph
 * atlases and GPU quads; compatibility text transitions, vector/image adapters
 * and unsupported color fonts rasterize only when their content changes.
 * Transforms, masks, effects, blending and temporal motion blur remain
 * GPU-resident through presentation.
 *
 * Build dependency: cairo, pango, pangocairo (compatibility raster adapters)
 */

#include "cache-manager.h"
#include "cache-frame-payload.h"
#include "cache-tile-payload.h"
#include "title-cache-policy.h"
#include "title-source.h"
#include "style-presets.h"
#include "title-data.h"
#include "live-text-cue-utils.h"
#include "title-snapshot.h"
#include "plugin-main.h"
#include "title-localization.h"
#include "title-effect-registry.h"
#include "title-gpu-text-renderer.h"
#include "title-preferences.h"
#include "title-logger.h"
#include "ticker-runtime.h"
#include "image-layer-utils.h"
#include "title-text-layout.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
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
#include <QtGlobal>
#include <QFont>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QTextLayout>
#include <QTextDocument>
#include <QTextBlock>
#include <QAbstractTextDocumentLayout>
#include <QTextOption>
#include <QTextCursor>
#include <QTextBoundaryFinder>
#include <QDateTime>
#include <QFileInfo>
#include <QTransform>
#include <QColor>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QConicalGradient>
#include <QCryptographicHash>
#include <QByteArray>
#include <QRegularExpression>
#include <QMetaObject>
#include <QObject>

#include <memory>
#include <string>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>
#include <atomic>
#include <array>
#include <mutex>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <sstream>
#include <iomanip>
#include <functional>
#include <utility>
#include "path-geometry.h"

using bgs::live_text::exposed_text_layers;

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kCubicCircle = 0.5522847498307933984;
constexpr uint32_t kMaxSourceDimension = 16384;

enum class GpuReadbackContract { Immediate, FinalFrameOnly };
static thread_local GpuReadbackContract g_gpu_readback_contract = GpuReadbackContract::Immediate;

class ScopedGpuReadbackContract {
public:
    explicit ScopedGpuReadbackContract(GpuReadbackContract contract)
        : previous_(g_gpu_readback_contract)
    {
        g_gpu_readback_contract = contract;
    }
    ~ScopedGpuReadbackContract() { g_gpu_readback_contract = previous_; }

private:
    GpuReadbackContract previous_;
};

static bool final_frame_readback_only()
{
    return g_gpu_readback_contract == GpuReadbackContract::FinalFrameOnly;
}

static uint32_t clamped_source_dimension(int value)
{
    return static_cast<uint32_t>(std::clamp(value, 1, static_cast<int>(kMaxSourceDimension)));
}

static bool image_path_is_svg(const QString &path)
{
    return path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".svgz"), Qt::CaseInsensitive);
}

static QSize image_intrinsic_size(const QString &path)
{
    if (image_path_is_svg(path)) {
        QSvgRenderer renderer(path);
        if (!renderer.isValid()) return QSize();
        QSize size = renderer.defaultSize();
        if (!size.isValid() || size.isEmpty())
            size = renderer.viewBox().size();
        return size;
    }

    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    if (size.isValid() && !size.isEmpty())
        return size;

    const QImage image = reader.read();
    return image.isNull() ? QSize() : image.size();
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
    QSize decode_size;
    if (is_svg && fallback_size.isValid() && !fallback_size.isEmpty()) {
        /* Bucket one uniform SVG scale, not width and height independently.
         * Independent rounding changed the SVG aspect ratio and made Qt center
         * the viewBox inside the raster, producing an apparent bounding-box
         * offset. The GPU maps this aspect-correct raster to the exact logical
         * image-box dimensions. */
        QSvgRenderer size_renderer(path);
        QSize intrinsic = size_renderer.isValid() ? size_renderer.defaultSize() : QSize();
        if ((!intrinsic.isValid() || intrinsic.isEmpty()) && size_renderer.isValid())
            intrinsic = size_renderer.viewBox().size();
        if (!intrinsic.isValid() || intrinsic.isEmpty())
            intrinsic = QSize(256, 256);

        constexpr int kSvgRasterBucket = 64;
        const double requested_scale = std::max(
            static_cast<double>(fallback_size.width()) / std::max(1, intrinsic.width()),
            static_cast<double>(fallback_size.height()) / std::max(1, intrinsic.height()));
        const int intrinsic_long = std::max(intrinsic.width(), intrinsic.height());
        const int requested_long = std::max(1, static_cast<int>(std::ceil(
            intrinsic_long * std::max(0.001, requested_scale))));
        const int bucketed_long = std::clamp(
            ((requested_long + kSvgRasterBucket - 1) / kSvgRasterBucket) *
                kSvgRasterBucket,
            1, 4096);
        const double bucket_scale = static_cast<double>(bucketed_long) /
                                    std::max(1, intrinsic_long);
        decode_size = QSize(
            std::clamp(static_cast<int>(std::ceil(intrinsic.width() * bucket_scale)), 1, 4096),
            std::clamp(static_cast<int>(std::ceil(intrinsic.height() * bucket_scale)), 1, 4096));
    }
    /* Bitmap images are decoded once at source resolution. Repeated scaled
     * QImageReader decodes during canvas resize were one of the largest image
     * manipulation stalls; the cached source is reused by the layer rasterizer
     * and then remains GPU-resident for transforms/compositing. */
    const QString cache_key = is_svg
        ? QStringLiteral("%1|svg|%2x%3").arg(path).arg(decode_size.width()).arg(decode_size.height())
        : QStringLiteral("%1|bitmap-source").arg(path);

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

        QSize svg_size = decode_size.isValid() && !decode_size.isEmpty()
            ? decode_size
            : renderer.defaultSize();
        if (!svg_size.isValid() || svg_size.isEmpty())
            svg_size = renderer.viewBox().size();
        if (!svg_size.isValid() || svg_size.isEmpty())
            svg_size = QSize(256, 256);

        loaded = QImage(svg_size, QImage::Format_ARGB32_Premultiplied);
        loaded.fill(Qt::transparent);
        QPainter painter(&loaded);
        renderer.render(&painter, QRectF(0.0, 0.0, loaded.width(), loaded.height()));
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);
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

// Forward declaration must live in the same (global) scope as the definition below.
// Keeping it inside the anonymous namespace creates a second overload on MSVC.
static void box_blur_pixels(std::vector<uint8_t> &pixels, int w, int h, int radius);

/* ══════════════════════════════════════════════════════════════════
 *  Source private data
 * ══════════════════════════════════════════════════════════════════ */
static std::atomic<uint64_t> g_source_frontend_presentation_generation {1};
static std::atomic<bool> g_source_scene_collection_transition {false};

void title_source_invalidate_all_presentations()
{
    g_source_frontend_presentation_generation.fetch_add(
        1, std::memory_order_acq_rel);
}

void title_source_begin_scene_collection_transition()
{
    g_source_scene_collection_transition.store(true,
                                               std::memory_order_release);
    title_source_invalidate_all_presentations();
}

void title_source_end_scene_collection_transition()
{
    /* Invalidate while output is still blocked. Once transition=false becomes
     * visible, every source is already generation-stale and video_render will
     * remain transparent until its next tick rebuilds the new collection. */
    title_source_invalidate_all_presentations();
    g_source_scene_collection_transition.store(false,
                                                std::memory_order_release);
}

struct SourceCacheWakeState {
    std::mutex mutex;
    std::string title_id;
    std::unordered_set<int> ready_frames;
};

static void bind_source_cache_wake_title(
    const std::shared_ptr<SourceCacheWakeState> &state,
    const std::string &title_id)
{
    if (!state)
        return;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->title_id = title_id;
    state->ready_frames.clear();
}

static bool take_source_cache_wake_frame(
    const std::shared_ptr<SourceCacheWakeState> &state,
    const std::string &title_id, int frame)
{
    if (!state)
        return false;
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->title_id != title_id)
        return false;
    const auto it = state->ready_frames.find(frame);
    if (it == state->ready_frames.end())
        return false;
    state->ready_frames.erase(it);
    return true;
}

struct TitleSourceData {
    obs_source_t *source  = nullptr;

    /* Settings */
    std::string title_id;
    std::atomic<bool> output_visible {true};
    std::atomic<bool> shown_on_display {true};
    bool        manual_uncue = false;
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
    bool        force_cue_state_sync = false; /* title switch/startup may race an already-issued cue */
    bool        first_frame_pending = true;   /* source is not clean until video_render draws once */
    uint32_t    consecutive_draw_failures = 0;
    std::chrono::steady_clock::time_point last_cue_cache_check;
    std::string cue_cache_check_title_id;
    uint64_t    cue_cache_check_revision = std::numeric_limits<uint64_t>::max();
    int         cue_cache_check_row = -1;
    bool        cue_cache_check_ready = false;

    /* Unified GPU-only render graph shared by live/editor/cache. */
    TitleGpuRenderSession *gpu_render_session = nullptr;

    /* Scene-mask GPU resources. */
    std::mutex    texture_mutex;
    gs_texrender_t *scene_mask_scene_texrender = nullptr;
    gs_effect_t  *scene_mask_effect = nullptr;
    /* Logical source canvas used by the scene-mask GPU pass. */
    uint32_t      tex_w      = 0;
    uint32_t      tex_h      = 0;
    uint64_t      cache_hash_revision = std::numeric_limits<uint64_t>::max();
    QString       cached_content_hash;
    QString       visual_identity_hash;
    uint64_t      visual_model_revision = 1;
    std::atomic<uint64_t> requested_presentation_generation {1};
    std::atomic<uint64_t> applied_presentation_generation {0};
    std::atomic<uint64_t> applied_frontend_presentation_generation {0};
    bool          title_missing = false;
    std::shared_ptr<SourceCacheWakeState> cache_wake_state;
    QMetaObject::Connection cache_frame_ready_connection;

    /* Dirty flag – avoid re-uploading unchanged frames */
    bool dirty = true;
    uint64_t seen_store_revision = 0;

    struct CachedEffectLayer {
        std::string key;
        QImage image;
        QPointF origin;
        uint64_t last_used = 0;
    };
    std::unordered_map<std::string, CachedEffectLayer> effect_layer_cache;
    uint64_t effect_layer_cache_tick = 0;

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

static void release_active_scene_mask_scenes(TitleSourceData *data);

static bool source_presentation_generation_is_current(
    const TitleSourceData *data)
{
    if (!data)
        return false;
    return data->applied_presentation_generation.load(
               std::memory_order_acquire) ==
               data->requested_presentation_generation.load(
                   std::memory_order_acquire) &&
           data->applied_frontend_presentation_generation.load(
               std::memory_order_acquire) ==
               g_source_frontend_presentation_generation.load(
                   std::memory_order_acquire);
}

static uint64_t request_source_presentation_reset(TitleSourceData *data,
                                                  const char *reason)
{
    if (!data)
        return 0;
    const uint64_t generation =
        data->requested_presentation_generation.fetch_add(
            1, std::memory_order_acq_rel) + 1;
    if (TitlePreferences::cache_playback_logging_enabled()) {
        BGL_LOG_INFO(
            "CachePlayback",
            QStringLiteral("consumer=source action=request-presentation-reset title=%1 generation=%2 reason=%3")
                .arg(QString::fromStdString(data->title_id))
                .arg(generation)
                .arg(QString::fromUtf8(reason ? reason : "unspecified")));
    }
    return generation;
}

static void apply_source_presentation_reset(TitleSourceData *data,
                                            const char *reason)
{
    if (!data || !data->gpu_render_session)
        return;
    const uint64_t requested_generation =
        data->requested_presentation_generation.load(
            std::memory_order_acquire);
    const uint64_t frontend_generation =
        g_source_frontend_presentation_generation.load(
            std::memory_order_acquire);

    title_gpu_render_session_invalidate_presentation(
        data->gpu_render_session, true);
    /* Scene source references are collection-scoped even when their names are
     * identical. Release them at the same generation boundary; otherwise the
     * name-only configuration comparison can retain and render an outgoing
     * collection's scene through a mask after the switch. */
    release_active_scene_mask_scenes(data);
    data->applied_presentation_generation.store(
        requested_generation, std::memory_order_release);
    data->applied_frontend_presentation_generation.store(
        frontend_generation, std::memory_order_release);
    data->dirty = true;
    data->first_frame_pending = true;
    data->consecutive_draw_failures = 0;
    data->cache_hash_revision = std::numeric_limits<uint64_t>::max();
    data->cached_content_hash.clear();

    if (TitlePreferences::cache_playback_logging_enabled()) {
        BGL_LOG_INFO(
            "CachePlayback",
            QStringLiteral("consumer=source action=apply-presentation-reset title=%1 generation=%2 frontendGeneration=%3 reason=%4")
                .arg(QString::fromStdString(data->title_id))
                .arg(requested_generation)
                .arg(frontend_generation)
                .arg(QString::fromUtf8(reason ? reason : "unspecified")));
    }
}

static void set_source_output_visible(TitleSourceData *data, bool visible,
                                      const char *reason)
{
    if (!data || data->output_visible.load(std::memory_order_acquire) == visible)
        return;
    data->output_visible.store(visible, std::memory_order_release);
    request_source_presentation_reset(data, reason);
    /* This helper is called from video_tick. Apply immediately so a transition
     * from hidden to visible cannot draw the previously hidden poster frame on
     * the display callback that follows the same tick. */
    apply_source_presentation_reset(data, reason);
}


static bool effect_has_animation(const LayerEffect &effect)
{
    return effect.enabled_prop.is_animated() ||
           effect.opacity_prop.is_animated() ||
           effect.size_prop.is_animated() ||
           effect.distance_prop.is_animated() ||
           effect.angle_prop.is_animated() ||
           effect.spread_prop.is_animated() ||
           effect.falloff_prop.is_animated() ||
           effect.stroke_width_prop.is_animated() ||
           effect.stroke_opacity_prop.is_animated() ||
           effect.padding_left_prop.is_animated() ||
           effect.padding_right_prop.is_animated() ||
           effect.padding_top_prop.is_animated() ||
           effect.padding_bottom_prop.is_animated() ||
           effect.corner_radius_tl_prop.is_animated() ||
           effect.corner_radius_tr_prop.is_animated() ||
           effect.corner_radius_br_prop.is_animated() ||
           effect.corner_radius_bl_prop.is_animated() ||
           effect.color_a.is_animated() ||
           effect.color_r.is_animated() ||
           effect.color_g.is_animated() ||
           effect.color_b.is_animated() ||
           effect.stroke_color_a.is_animated() ||
           effect.stroke_color_r.is_animated() ||
           effect.stroke_color_g.is_animated() ||
           effect.stroke_color_b.is_animated() ||
           effect.amount_prop.is_animated() ||
           effect.scale_prop.is_animated() ||
           effect.softness_prop.is_animated() ||
           effect.roundness_prop.is_animated() ||
           effect.speed_prop.is_animated() ||
           effect.center_x_prop.is_animated() ||
           effect.center_y_prop.is_animated() ||
           effect.complexity_prop.is_animated() ||
           effect.evolution_prop.is_animated() ||
           effect.secondary_color_a.is_animated() ||
           effect.secondary_color_r.is_animated() ||
           effect.secondary_color_g.is_animated() ||
           effect.secondary_color_b.is_animated() ||
           (effect.type == LayerEffectType::Noise && effect.effect_animated);
}

static bool layer_has_effect_animation(const Layer &layer)
{
    return std::any_of(layer.effects.begin(), layer.effects.end(), effect_has_animation);
}

static bool layer_has_animation(const Layer &layer)
{
    const bool has_transition_animation = std::any_of(
        layer.transitions.begin(), layer.transitions.end(),
        [](const LayerTransition &transition) {
            return transition.enabled && transition.duration > 0.000001;
        });
    return has_transition_animation ||
           layer_has_effect_animation(layer) ||
           layer.position.is_animated() ||
           layer.scale.is_animated() ||
           layer.rotation.is_animated() ||
           layer.opacity.is_animated() ||
           layer.size.is_animated() ||
           layer.origin_prop.is_animated() ||
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

static bool include_property_bounds(const Layer &layer, const AnimatedVec2Property &prop,
                                    double &first_time, double &last_time)
{
    if (prop.keyframes.empty()) return false;
    first_time = std::min(first_time, layer.in_time + prop.keyframes.front().time);
    last_time = std::max(last_time, layer.in_time + prop.keyframes.back().time);
    return true;
}

static bool include_effect_property_bounds(const Layer &layer, const LayerEffect &effect,
                                           double &first_time, double &last_time)
{
    bool has_bounds = false;
    has_bounds |= include_property_bounds(layer, effect.enabled_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.opacity_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.size_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.distance_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.angle_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.spread_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.falloff_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.stroke_width_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.stroke_opacity_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.padding_left_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.padding_right_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.padding_top_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.padding_bottom_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.corner_radius_tl_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.corner_radius_tr_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.corner_radius_br_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.corner_radius_bl_prop, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.color_b, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.stroke_color_a, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.stroke_color_r, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.stroke_color_g, first_time, last_time);
    has_bounds |= include_property_bounds(layer, effect.stroke_color_b, first_time, last_time);
    return has_bounds;
}

static bool layer_animation_keyframe_bounds(const Layer &layer, double &first_time, double &last_time)
{
    bool has_bounds = false;
    has_bounds |= include_property_bounds(layer, layer.position, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.scale, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.rotation, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.opacity, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.size, first_time, last_time);
    has_bounds |= include_property_bounds(layer, layer.origin_prop, first_time, last_time);
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
    for (const LayerEffect &effect : layer.effects)
        has_bounds |= include_effect_property_bounds(layer, effect, first_time, last_time);
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
    const double duration = std::max(0.0, title->duration);
    return std::any_of(title->layers.begin(), title->layers.end(),
                       [duration](const std::shared_ptr<Layer> &layer) {
                           if (!layer)
                               return false;
                           /* Visibility windows are timeline animation too. A
                            * clock-only title can otherwise be classified as
                            * static and freeze at frame zero, preventing both
                            * transition presets and in/out timing from running
                            * during cue and uncue. */
                           const bool timed_visibility =
                               layer->in_time > 0.000001 ||
                               layer->out_time < duration - 0.000001;
                           return timed_visibility || layer_has_animation(*layer);
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
    BGL_LOG_DEBUG("Masks", QStringLiteral(
        "Refreshed scene-mask configuration title=%1 masks=%2")
        .arg(QString::fromStdString(data->title_id))
        .arg(static_cast<int>(data->scene_masks.size())));
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

static bool title_has_valid_scene_mask_cue(const TitleSourceData *data, const Title &title)
{
    const int row_count = static_cast<int>(title.live_text_rows.size());
    if (row_count <= 0)
        return false;

    auto valid_row = [row_count](int row) {
        return row >= 0 && row < row_count;
    };

    return valid_row(title.current_cue_row) ||
           valid_row(title.pending_cue_row) ||
           (data && valid_row(data->active_cue_row));
}

static bool scene_mask_active_scenes_match_configs(const TitleSourceData *data)
{
    if (!data)
        return false;

    std::unordered_set<std::string> configured_names;
    for (const auto &cfg : data->scene_masks) {
        if (!cfg.scene_name.empty())
            configured_names.insert(cfg.scene_name);
    }

    if (configured_names.size() != data->active_scene_mask_scenes.size())
        return false;

    for (const auto &active : data->active_scene_mask_scenes) {
        if (!active.source || configured_names.find(active.name) == configured_names.end())
            return false;
    }

    return true;
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

static void sync_scene_mask_scenes_for_cue(TitleSourceData *data, const std::shared_ptr<Title> &title)
{
    if (!data)
        return;

    const bool should_activate = data->scene_mask_foreground_active &&
        data->shown_on_display.load(std::memory_order_acquire) &&
        data->output_visible.load(std::memory_order_acquire) &&
        title && title_has_valid_scene_mask_cue(data, *title);

    if (!should_activate) {
        if (!data->active_scene_mask_scenes.empty())
            release_active_scene_mask_scenes(data);
        return;
    }

    if (!scene_mask_active_scenes_match_configs(data))
        activate_scene_mask_scenes(data);
}


static int live_text_playlist_row_count(const Title &title)
{
    return exposed_text_layers(title).empty()
        ? 1
        : static_cast<int>(title.live_text_rows.size());
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



static void replace_layer_text_preserving_rich_rules(const std::shared_ptr<Layer> &layer, const std::string &text)
{
    if (!layer)
        return;
    layer->text_content = text;
    if (layer->rich_text.empty())
        layer->rich_text = rich_text_document_from_layer_defaults(*layer);

    RichTextCharFormat insertion_format = rich_text_effective_typing_format(layer->rich_text);
    rich_text_document_replace_text(layer->rich_text, text, insertion_format,
                                    layer->rich_text.has_typing_format
                                        ? layer->rich_text.typing_format_mask : 0);
}

static void apply_live_cue_layer_value(const std::shared_ptr<Layer> &layer, const std::string &value)
{
    if (!layer)
        return;
    layer->live_cue_hidden_if_empty = layer->exposed_hide_if_empty && value.empty();
    if (layer->type == LayerType::Image) {
        bgs::apply_exposed_image_cue_value(*layer, value);
        return;
    }
    replace_layer_text_preserving_rich_rules(layer, value);
}

static void apply_live_text_row(const std::shared_ptr<Title> &title, int row)
{
    if (!title || row < 0 || row >= (int)title->live_text_rows.size()) return;
    auto exposed = exposed_text_layers(title);
    for (int col = 0; col < (int)exposed.size(); ++col) {
        const auto &target = exposed[col];
        const int value_row = target && target->exposed_single_value ? 0 : row;
        if (value_row < 0 || value_row >= (int)title->live_text_rows.size() ||
            col >= (int)title->live_text_rows[value_row].size())
            continue;
        apply_live_cue_layer_value(target, title->live_text_rows[value_row][col]);
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
static RichTextDocument rich_text_model_for_source_time(const Layer &layer,
                                                        double local_time);

static bool is_text_box_auto_size_layer(const Layer &layer)
{
    return layer.type == LayerType::Text || layer.type == LayerType::Clock;
}

static ImmutableTextLayout source_text_layout_for_metrics(const Layer &layer,
                                                           double local_time,
                                                           double width,
                                                           double height,
                                                           int overflow_mode)
{
    RichTextDocument model = rich_text_model_for_source_time(layer, local_time);
    if (overflow_mode == 2) {
        const QString fitted = overflow_layout_text(
            QString::fromStdString(model.plain_text), layer);
        const RichTextCharFormat insertion_format =
            rich_text_effective_typing_format(model);
        rich_text_document_replace_text(
            model, fitted.toStdString(), insertion_format,
            model.has_typing_format ? model.typing_format_mask : 0);
    }
    TextLayoutRequest request;
    request.document = std::move(model);
    request.max_width = static_cast<float>(std::max(0.0, width));
    request.max_height = static_cast<float>(std::max(0.0, height));
    request.device_scale = 1.0f;
    request.minimum_horizontal_fit =
        std::clamp(layer.text_fit_min_scale, 0.05f, 1.0f);
    request.overflow_mode = overflow_mode;
    return cached_text_layout(request);
}

static double natural_text_width(const Layer &layer, double local_time = 0.0)
{
    if (!is_text_box_auto_size_layer(layer)) return 1.0;
    const ImmutableTextLayout layout =
        source_text_layout_for_metrics(layer, local_time, 0.0, 0.0, 1);
    return layout && layout->valid
               ? std::ceil(std::max(1.0f, layout->natural_width))
               : 1.0;
}

static double natural_text_height(const Layer &layer, double width,
                                  double local_time = 0.0)
{
    if (!is_text_box_auto_size_layer(layer)) return 1.0;
    const ImmutableTextLayout layout = source_text_layout_for_metrics(
        layer, local_time, width, 0.0, layer.text_overflow_mode);
    return layout && layout->valid
               ? std::ceil(std::max(1.0f, layout->natural_height))
               : 1.0;
}

static double eval_box_width(const Layer &layer, double t)
{
    double width = layer.size.is_animated()
        ? layer.size.evaluate(t).x
        : static_cast<double>(layer.rect_width);
    if (layer.text_box_width_to_text && is_text_box_auto_size_layer(layer))
        width = std::min(natural_text_width(layer, t), std::max(1.0, (double)layer.max_text_box_width));
    return std::max(0.0, width);
}

static double eval_box_height(const Layer &layer, double t)
{
    double height = layer.size.is_animated()
        ? layer.size.evaluate(t).y
        : static_cast<double>(layer.rect_height);
    if (layer.text_box_height_to_text && is_text_box_auto_size_layer(layer)) {
        const double width = eval_box_width(layer, t);
        height = std::min(natural_text_height(layer, width, t), std::max(1.0, (double)layer.max_text_box_height));
    }
    return std::max(0.0, height);
}

static double eval_image_width(const Layer &layer, double t)
{
    const double width = layer.image_size.is_animated()
        ? layer.image_size.evaluate(t).x
        : static_cast<double>(layer.image_width);
    return std::max(0.0, width);
}

static double eval_image_height(const Layer &layer, double t)
{
    const double height = layer.image_size.is_animated()
        ? layer.image_size.evaluate(t).y
        : static_cast<double>(layer.image_height);
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
    if (depth > 64 || !layer.visible || layer.live_cue_hidden_if_empty ||
        title_time < layer.in_time || title_time > layer.out_time)
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
     * rendered into a cached GPU matte texture by the Phase 13 mask graph.
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
    const LayerTransitionVisualState transition = evaluate_layer_general_transitions(
        layer.transitions, layer.in_time, layer.out_time, title_time);
    cairo_translate(cr, layer.position.evaluate(lt).x + transition.translate_x,
                    layer.position.evaluate(lt).y + transition.translate_y);
    cairo_rotate(cr, layer.rotation.evaluate(lt) * kPi / 180.0);
    cairo_scale(cr, layer.scale.evaluate(lt).x * transition.scale,
                layer.scale.evaluate(lt).y * transition.scale);
}

static double layer_chain_opacity(const Title &title, const Layer &layer, double title_time, int depth = 0)
{
    if (depth > 64)
        return 1.0;
    const double lt = std::max(0.0, title_time - layer.in_time);
    const LayerTransitionVisualState transition = evaluate_layer_general_transitions(
        layer.transitions, layer.in_time, layer.out_time, title_time);
    double opacity = layer.opacity.evaluate(lt) * transition.opacity;
    if (!layer.parent_id.empty()) {
        if (const Layer *parent = find_layer_by_id(title, layer.parent_id))
            opacity *= layer_chain_opacity(title, *parent, title_time, depth + 1);
    }
    return opacity;
}

static void apply_layer_transition_clip(cairo_t *cr, const Layer &layer, double title_time,
                                        double x, double y, double width, double height)
{
    const LayerTransitionVisualState transition = evaluate_layer_general_transitions(
        layer.transitions, layer.in_time, layer.out_time, title_time);
    if (!transition.active || transition.wipe >= 0.999999 ||
        transition.wipe_softness > 0.000001 || width <= 0.0 || height <= 0.0)
        return;

    double clip_x = x;
    double clip_y = y;
    double clip_w = width;
    double clip_h = height;
    const double reveal = std::clamp(transition.wipe, 0.0, 1.0);
    switch (transition.wipe_direction) {
    case LayerTransitionDirection::Right:
        clip_w = width * reveal;
        clip_x = x + width - clip_w;
        break;
    case LayerTransitionDirection::Up:
        clip_h = height * reveal;
        clip_y = y + height - clip_h;
        break;
    case LayerTransitionDirection::Down:
        clip_h = height * reveal;
        break;
    case LayerTransitionDirection::Left:
    case LayerTransitionDirection::None:
    default:
        clip_w = width * reveal;
        break;
    }
    cairo_rectangle(cr, clip_x, clip_y, std::max(0.0, clip_w), std::max(0.0, clip_h));
    cairo_clip(cr);
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

static double legacy_corner_type_roundness(CornerType type)
{
    switch (type) {
    case CornerType::Straight:
    case CornerType::Cutout:
        return 0.0;
    case CornerType::Concave:
        return -100.0;
    case CornerType::Round:
    default:
        return 100.0;
    }
}

static void cairo_add_rounded_rect_corners(cairo_t *cr, double x, double y, double w, double h,
                                           double top_left, double top_right,
                                           double bottom_right, double bottom_left,
                                           double bevel_roundness = 100.0)
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
    const double roundness = std::clamp(bevel_roundness, -100.0, 100.0) / 100.0;
    auto add_corner = [&](double from_x, double from_y, double corner_x, double corner_y, double to_x, double to_y,
                          double cutout_x, double cutout_y, double radius) {
        if (radius <= 0.0) {
            cairo_line_to(cr, corner_x, corner_y);
            return;
        }
        if (std::abs(roundness) <= 1e-6) {
            cairo_line_to(cr, to_x, to_y);
        } else if (roundness < 0.0) {
            const double inverted = -roundness;
            const double flat_c1x = from_x + (to_x - from_x) / 3.0;
            const double flat_c1y = from_y + (to_y - from_y) / 3.0;
            const double flat_c2x = from_x + (to_x - from_x) * (2.0 / 3.0);
            const double flat_c2y = from_y + (to_y - from_y) * (2.0 / 3.0);
            const double arc_c1x = from_x + (cutout_x - to_x) * kCubicCircle;
            const double arc_c1y = from_y + (cutout_y - to_y) * kCubicCircle;
            const double arc_c2x = to_x - (corner_x - from_x) * kCubicCircle;
            const double arc_c2y = to_y - (corner_y - from_y) * kCubicCircle;
            cairo_curve_to(cr,
                           flat_c1x + (arc_c1x - flat_c1x) * inverted,
                           flat_c1y + (arc_c1y - flat_c1y) * inverted,
                           flat_c2x + (arc_c2x - flat_c2x) * inverted,
                           flat_c2y + (arc_c2y - flat_c2y) * inverted,
                           to_x, to_y);
        } else {
            const double c1x = corner_x + (cutout_x - corner_x) * (1.0 - roundness);
            const double c1y = corner_y + (cutout_y - corner_y) * (1.0 - roundness);
            cairo_curve_to(cr, c1x, c1y, c1x, c1y, to_x, to_y);
        }
    };
    cairo_new_sub_path(cr);
    cairo_move_to(cr, x + tl, y);
    cairo_line_to(cr, x + w - tr, y);
    if (tr > 0.0 && roundness >= 0.999)
        cairo_arc(cr, x + w - tr, y + tr, tr, 3 * kPi / 2, 2 * kPi);
    else if (tr > 0.0 && roundness <= -0.999)
        cairo_arc_negative(cr, x + w, y, tr, kPi, kPi / 2);
    else
        add_corner(x + w - tr, y, x + w, y, x + w, y + tr, x + w - tr, y + tr, tr);
    cairo_line_to(cr, x + w, y + h - br);
    if (br > 0.0 && roundness >= 0.999)
        cairo_arc(cr, x + w - br, y + h - br, br, 0, kPi / 2);
    else if (br > 0.0 && roundness <= -0.999)
        cairo_arc_negative(cr, x + w, y + h, br, -kPi / 2, -kPi);
    else
        add_corner(x + w, y + h - br, x + w, y + h, x + w - br, y + h, x + w - br, y + h - br, br);
    cairo_line_to(cr, x + bl, y + h);
    if (bl > 0.0 && roundness >= 0.999)
        cairo_arc(cr, x + bl, y + h - bl, bl, kPi / 2, kPi);
    else if (bl > 0.0 && roundness <= -0.999)
        cairo_arc_negative(cr, x, y + h, bl, 0, -kPi / 2);
    else
        add_corner(x + bl, y + h, x, y + h, x, y + h - bl, x + bl, y + h - bl, bl);
    cairo_line_to(cr, x, y + tl);
    if (tl > 0.0 && roundness >= 0.999)
        cairo_arc(cr, x + tl, y + tl, tl, kPi, 3 * kPi / 2);
    else if (tl > 0.0 && roundness <= -0.999)
        cairo_arc_negative(cr, x, y, tl, kPi / 2, 0);
    else
        add_corner(x, y + tl, x, y, x + tl, y, x + tl, y + tl, tl);
    cairo_close_path(cr);
}

static QPainterPath painter_rounded_rect_corners(const QRectF &rect, double top_left, double top_right,
                                                 double bottom_right, double bottom_left,
                                                 double bevel_roundness = 100.0)
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
    const double roundness = std::clamp(bevel_roundness, -100.0, 100.0) / 100.0;
    auto add_corner = [&](const QPointF &from, const QPointF &corner, const QPointF &to,
                          const QPointF &cutout, double radius) {
        if (radius <= 0.0) {
            path.lineTo(corner);
            return;
        }
        if (std::abs(roundness) <= 1e-6) {
            path.lineTo(to);
        } else if (roundness < 0.0) {
            const double inverted = -roundness;
            const QPointF flat_c1 = from + (to - from) / 3.0;
            const QPointF flat_c2 = from + (to - from) * (2.0 / 3.0);
            const QPointF arc_c1 = from + (cutout - to) * kCubicCircle;
            const QPointF arc_c2 = to - (corner - from) * kCubicCircle;
            path.cubicTo(flat_c1 + (arc_c1 - flat_c1) * inverted,
                         flat_c2 + (arc_c2 - flat_c2) * inverted,
                         to);
        } else {
            const QPointF control = corner + (cutout - corner) * (1.0 - roundness);
            path.cubicTo(control, control, to);
        }
    };
    path.moveTo(rect.left() + tl, rect.top());
    path.lineTo(rect.right() - tr, rect.top());
    if (tr > 0.0 && roundness >= 0.999)
        path.arcTo(QRectF(rect.right() - 2.0 * tr, rect.top(), 2.0 * tr, 2.0 * tr), 90.0, -90.0);
    else if (tr > 0.0 && roundness <= -0.999)
        path.arcTo(QRectF(rect.right() - tr, rect.top() - tr, 2.0 * tr, 2.0 * tr), 180.0, 90.0);
    else
        add_corner(QPointF(rect.right() - tr, rect.top()), rect.topRight(),
                   QPointF(rect.right(), rect.top() + tr), QPointF(rect.right() - tr, rect.top() + tr), tr);
    path.lineTo(rect.right(), rect.bottom() - br);
    if (br > 0.0 && roundness >= 0.999)
        path.arcTo(QRectF(rect.right() - 2.0 * br, rect.bottom() - 2.0 * br, 2.0 * br, 2.0 * br), 0.0, -90.0);
    else if (br > 0.0 && roundness <= -0.999)
        path.arcTo(QRectF(rect.right() - br, rect.bottom() - br, 2.0 * br, 2.0 * br), -90.0, 90.0);
    else
        add_corner(QPointF(rect.right(), rect.bottom() - br), rect.bottomRight(),
                   QPointF(rect.right() - br, rect.bottom()), QPointF(rect.right() - br, rect.bottom() - br), br);
    path.lineTo(rect.left() + bl, rect.bottom());
    if (bl > 0.0 && roundness >= 0.999)
        path.arcTo(QRectF(rect.left(), rect.bottom() - 2.0 * bl, 2.0 * bl, 2.0 * bl), 270.0, -90.0);
    else if (bl > 0.0 && roundness <= -0.999)
        path.arcTo(QRectF(rect.left() - bl, rect.bottom() - bl, 2.0 * bl, 2.0 * bl), 0.0, 90.0);
    else
        add_corner(QPointF(rect.left() + bl, rect.bottom()), rect.bottomLeft(),
                   QPointF(rect.left(), rect.bottom() - bl), QPointF(rect.left() + bl, rect.bottom() - bl), bl);
    path.lineTo(rect.left(), rect.top() + tl);
    if (tl > 0.0 && roundness >= 0.999)
        path.arcTo(QRectF(rect.left(), rect.top(), 2.0 * tl, 2.0 * tl), 180.0, -90.0);
    else if (tl > 0.0 && roundness <= -0.999)
        path.arcTo(QRectF(rect.left() - tl, rect.top() - tl, 2.0 * tl, 2.0 * tl), 90.0, 90.0);
    else
        add_corner(QPointF(rect.left(), rect.top() + tl), rect.topLeft(),
                   QPointF(rect.left() + tl, rect.top()), QPointF(rect.left() + tl, rect.top() + tl), tl);
    path.closeSubpath();
    return path;
}

static QPainterPath painter_layer_rounded_rect_path(const Layer &layer, const QRectF &rect)
{
    return painter_rounded_rect_corners(rect, layer.corner_radius_tl, layer.corner_radius_tr,
                                        layer.corner_radius_br, layer.corner_radius_bl,
                                        layer.corner_bevel_roundness);
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
    return bgs::layer_shape_path(layer, QRectF(0.0, 0.0, w, h));
}

static void cairo_append_qpainter_path(cairo_t *cr, const QPainterPath &path,
                                         bool close_subpaths)
{
    if (!cr || path.isEmpty())
        return;
    bool have_subpath = false;
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element element = path.elementAt(i);
        switch (element.type) {
        case QPainterPath::MoveToElement:
            if (have_subpath && close_subpaths)
                cairo_close_path(cr);
            cairo_move_to(cr, element.x, element.y);
            have_subpath = true;
            break;
        case QPainterPath::LineToElement:
            cairo_line_to(cr, element.x, element.y);
            break;
        case QPainterPath::CurveToElement:
            if (i + 2 < path.elementCount()) {
                const QPainterPath::Element control2 = path.elementAt(i + 1);
                const QPainterPath::Element endpoint = path.elementAt(i + 2);
                cairo_curve_to(cr, element.x, element.y,
                               control2.x, control2.y,
                               endpoint.x, endpoint.y);
                i += 2;
            }
            break;
        case QPainterPath::CurveToDataElement:
            /* Curve data is consumed together with CurveToElement above. */
            break;
        }
    }
    if (have_subpath && close_subpaths)
        cairo_close_path(cr);
}

static void cairo_add_layer_shape(cairo_t *cr, const Layer &layer, double w, double h)
{
    const QPainterPath path = painter_layer_shape_path(layer, w, h);
    const ShapeType shape_type = layer.type == LayerType::Shape
        ? layer.shape_type
        : ShapeType::RoundedRectangle;
    const bool closed = shape_type != ShapeType::Line &&
                        (shape_type != ShapeType::Path || layer.path_closed);
    cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_append_qpainter_path(cr, path, closed);
}

static double eval_origin_x(const Layer &layer, double t)
{
    return std::clamp(layer.origin_prop.is_animated()
                          ? layer.origin_prop.evaluate(t).x
                          : (double)layer.origin_x,
                      0.0, 1.0);
}

static double eval_origin_y(const Layer &layer, double t)
{
    return std::clamp(layer.origin_prop.is_animated()
                          ? layer.origin_prop.evaluate(t).y
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

static int normalized_gradient_type(int gradient_type)
{
    switch (std::clamp(gradient_type, 0, 4)) {
    case 1: return 1;
    case 2: return 2;
    case 4: return 1;
    case 0:
    case 3:
    default: return 0;
    }
}

static int normalized_gradient_spread(int gradient_spread, int gradient_type)
{
    if (gradient_spread == 1 || gradient_spread == 2)
        return gradient_spread;
    return std::clamp(gradient_type, 0, 4) == 3 ? 1 : 0;
}

static QGradient::Spread qt_gradient_spread(int gradient_spread, int gradient_type)
{
    switch (normalized_gradient_spread(gradient_spread, gradient_type)) {
    case 1: return QGradient::ReflectSpread;
    case 2: return QGradient::RepeatSpread;
    case 0:
    default: return QGradient::PadSpread;
    }
}

static cairo_extend_t cairo_gradient_extend(int gradient_spread, int gradient_type)
{
    switch (normalized_gradient_spread(gradient_spread, gradient_type)) {
    case 1: return CAIRO_EXTEND_REFLECT;
    case 2: return CAIRO_EXTEND_REPEAT;
    case 0:
    default: return CAIRO_EXTEND_PAD;
    }
}

static QBrush make_gradient_brush(int gradient_type,
                                  int gradient_spread,
                                  const QRectF &box,
                                  double opacity,
                                  double center_x, double center_y,
                                  double focal_x, double focal_y,
                                  double scale, double angle_degrees,
                                  double start_pos, uint32_t start_color, double start_opacity,
                                  double end_pos, uint32_t end_color, double end_opacity,
                                  const std::vector<GradientStop> &extra_stops)
{
    const int type = normalized_gradient_type(gradient_type);
    const QGradient::Spread spread = qt_gradient_spread(gradient_spread, gradient_type);
    const double cx = box.left() + center_x * box.width();
    const double cy = box.top() + center_y * box.height();
    const double safe_scale = std::clamp(scale, 0.01, 100.0);
    const double length = std::max(1.0, std::hypot(box.width(), box.height()) * 0.5 * safe_scale);
    const double angle = angle_degrees * kPi / 180.0;
    const double dx = std::cos(angle) * length;
    const double dy = std::sin(angle) * length;

    if (type == 1) {
        const double radius = std::max(box.width(), box.height()) * 0.5 * safe_scale;
        QRadialGradient gradient(QPointF(cx, cy), std::max(1.0, radius),
                                 QPointF(box.left() + focal_x * box.width(),
                                         box.top() + focal_y * box.height()));
        gradient.setSpread(spread);
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
    gradient.setSpread(spread);
    apply_base_gradient_stops(gradient, start_pos, start_color, start_opacity,
                              end_pos, end_color, end_opacity, extra_stops, opacity);
    return QBrush(gradient);
}

static QBrush gradient_fill_brush(const Layer &layer, const QRectF &box, double layer_opacity = 1.0)
{
    return make_gradient_brush(layer.gradient_type, layer.gradient_spread, box,
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

static const LayerEffect *find_layer_effect(const Layer &layer, LayerEffectType type);

static QBrush background_gradient_fill_brush(const Layer &layer, const QRectF &box, double layer_opacity = 1.0)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    if (!effect) return QBrush(Qt::NoBrush);
    return make_gradient_brush(effect->effect_gradient_type, effect->effect_gradient_spread, box,
                               std::clamp((double)effect->effect_gradient_opacity * layer_opacity, 0.0, 1.0),
                               effect->effect_gradient_center_x, effect->effect_gradient_center_y,
                               effect->effect_gradient_focal_x, effect->effect_gradient_focal_y,
                               effect->effect_gradient_scale, effect->effect_gradient_angle,
                               std::clamp((double)effect->effect_gradient_start_pos, 0.0, 1.0),
                               effect->effect_gradient_start_color, effect->effect_gradient_start_opacity,
                               std::clamp((double)effect->effect_gradient_end_pos, 0.0, 1.0),
                               effect->effect_gradient_end_color, effect->effect_gradient_end_opacity,
                               {});
}

static QBrush stroke_gradient_fill_brush(const Layer &layer, const QRectF &box, double layer_opacity = 1.0)
{
    return make_gradient_brush(layer.stroke_gradient_type, layer.stroke_gradient_spread, box,
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
    const int type = normalized_gradient_type(layer.gradient_type);
    if (type == 1) {
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
    cairo_pattern_set_extend(pattern, cairo_gradient_extend(layer.gradient_spread, layer.gradient_type));
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
    const int type = normalized_gradient_type(layer.stroke_gradient_type);
    if (type == 1) {
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
    cairo_pattern_set_extend(pattern, cairo_gradient_extend(layer.stroke_gradient_spread, layer.stroke_gradient_type));
    return pattern;
}

static cairo_pattern_t *create_background_gradient_pattern_for_effect(const LayerEffect &effect, double x, double y, double w, double h, double layer_alpha);

static cairo_pattern_t *create_background_gradient_pattern(const Layer &layer, double x, double y, double w, double h, double layer_alpha)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    if (!effect) return nullptr;
    return create_background_gradient_pattern_for_effect(*effect, x, y, w, h, layer_alpha);
}

static cairo_pattern_t *create_background_gradient_pattern_for_effect(const LayerEffect &effect, double x, double y, double w, double h, double layer_alpha)
{
    const double opacity = std::clamp((double)effect.effect_gradient_opacity * layer_alpha, 0.0, 1.0);
    const double cx = x + (double)effect.effect_gradient_center_x * w;
    const double cy = y + (double)effect.effect_gradient_center_y * h;
    const double scale = std::clamp((double)effect.effect_gradient_scale, 0.01, 100.0);
    const double start_pos = std::clamp((double)effect.effect_gradient_start_pos, 0.0, 1.0);
    const double end_pos = std::clamp((double)effect.effect_gradient_end_pos, 0.0, 1.0);
    cairo_pattern_t *pattern = nullptr;
    const int type = normalized_gradient_type(effect.effect_gradient_type);
    if (type == 1) {
        const double radius = std::max(w, h) * 0.5 * scale;
        const double fx = x + (double)effect.effect_gradient_focal_x * w;
        const double fy = y + (double)effect.effect_gradient_focal_y * h;
        pattern = cairo_pattern_create_radial(fx, fy, 0.0, cx, cy, std::max(1.0, radius));
    } else {
        const double length = std::hypot(w, h) * 0.5 * scale;
        const double angle = effect.effect_gradient_angle * kPi / 180.0;
        const double dx = std::cos(angle) * length;
        const double dy = std::sin(angle) * length;
        pattern = cairo_pattern_create_linear(cx - dx, cy - dy, cx + dx, cy + dy);
    }
    auto add_stop = [&](double pos, uint32_t argb, double stop_opacity) {
        QColor color = gradient_color_with_opacity(argb, opacity, stop_opacity);
        cairo_pattern_add_color_stop_rgba(pattern, pos, color.redF(), color.greenF(), color.blueF(), color.alphaF());
    };
    add_stop(start_pos, effect.effect_gradient_start_color, effect.effect_gradient_start_opacity);
    add_stop(end_pos, effect.effect_gradient_end_color, effect.effect_gradient_end_opacity);
    cairo_pattern_set_extend(pattern, cairo_gradient_extend(effect.effect_gradient_spread, effect.effect_gradient_type));
    return pattern;
}

static const LayerEffect *find_layer_effect(const Layer &layer, LayerEffectType type)
{
    for (const auto &effect : layer.effects) {
        if (effect.type == type)
            return &effect;
    }
    return nullptr;
}

static bool eval_effect_enabled(const LayerEffect &effect, double t)
{
    return effect.enabled && (!effect.enabled_prop.is_animated() || effect.enabled_prop.evaluate(t) >= 0.5);
}

static uint32_t eval_effect_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.color_a, (effect.effect_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.color_r, (effect.effect_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.color_g, (effect.effect_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.color_b, effect.effect_color & 0xFF, t);
}

static uint32_t eval_effect_secondary_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.secondary_color_a, (effect.effect_secondary_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.secondary_color_r, (effect.effect_secondary_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.secondary_color_g, (effect.effect_secondary_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.secondary_color_b, effect.effect_secondary_color & 0xFF, t);
}

static uint32_t eval_effect_stroke_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.stroke_color_a, (effect.effect_stroke_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.stroke_color_r, (effect.effect_stroke_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.stroke_color_g, (effect.effect_stroke_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.stroke_color_b, effect.effect_stroke_color & 0xFF, t);
}

static bool eval_outline_enabled(const Layer &layer, double)
{
    return layer.outline_enabled && layer.stroke_fill_type != 0 && layer.stroke_width > 0.0f;
}

static uint32_t eval_outline_color(const Layer &layer, double)
{
    return eval_outline_enabled(layer, 0.0) ? layer.stroke_color : 0x00000000;
}

static double eval_outline_width(const Layer &layer, double)
{
    return eval_outline_enabled(layer, 0.0) ? std::max(0.0f, layer.stroke_width) : 0.0;
}

static double eval_outline_opacity(const Layer &layer, double)
{
    return eval_outline_enabled(layer, 0.0) ? std::clamp((double)layer.outline_opacity, 0.0, 1.0) : 1.0;
}

static bool eval_outline_on_front(const Layer &layer, double)
{
    return layer.outline_on_front;
}

static int eval_outline_alignment(const Layer &layer, double)
{
    return std::clamp(layer.outline_alignment, 0, 2);
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
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect && eval_effect_enabled(*effect, t);
}

static bool eval_long_shadow_enabled(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::LongShadow);
    return effect && eval_effect_enabled(*effect, t);
}

static double eval_shadow_opacity(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect ? std::clamp(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(t) : (double)effect->effect_opacity, 0.0, 1.0) : 0.0;
}

static double eval_shadow_distance(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect ? std::max(0.0, effect->distance_prop.is_animated() ? effect->distance_prop.evaluate(t) : (double)effect->effect_distance) : 0.0;
}

static double eval_shadow_angle(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect ? (effect->angle_prop.is_animated() ? effect->angle_prop.evaluate(t) : (double)effect->effect_angle) : 0.0;
}

static double eval_shadow_blur(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect ? std::max(0.0, effect->size_prop.is_animated() ? effect->size_prop.evaluate(t) : (double)effect->effect_size) : 0.0;
}

static double eval_shadow_spread(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect ? std::max(0.0, effect->spread_prop.is_animated() ? effect->spread_prop.evaluate(t) : (double)effect->effect_spread) : 0.0;
}

static uint32_t eval_shadow_color(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect ? eval_effect_color(*effect, t) : 0x00000000;
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

struct CachedShadowEntry {
    CachedShadowImage image;
    uint64_t last_used = 0;
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
        if (const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow))
            p.blur_type = effect->effect_blur_type;
        p.color = eval_shadow_color(layer, t);
        p.opacity = eval_shadow_opacity(layer, t) * (((p.color >> 24) & 0xFF) / 255.0);
    }
    const auto *long_effect = find_layer_effect(layer, LayerEffectType::LongShadow);
    const double long_length = long_effect ? std::max(0.0, long_effect->distance_prop.is_animated() ? long_effect->distance_prop.evaluate(t) : (double)long_effect->effect_distance) : 0.0;
    const double long_opacity = long_effect ? std::clamp(long_effect->opacity_prop.is_animated() ? long_effect->opacity_prop.evaluate(t) : (double)long_effect->effect_opacity, 0.0, 1.0) : 0.0;
    p.long_enabled = long_effect && eval_effect_enabled(*long_effect, t) && long_length > 0.0 && long_opacity > 0.0;
    if (p.long_enabled) {
        p.long_color = eval_effect_color(*long_effect, t);
        p.long_opacity = long_opacity * (((p.long_color >> 24) & 0xFF) / 255.0);
        p.long_length = long_length;
        p.long_angle = long_effect->angle_prop.is_animated() ? long_effect->angle_prop.evaluate(t) : (double)long_effect->effect_angle;
        p.long_falloff = std::clamp(long_effect->falloff_prop.is_animated() ? long_effect->falloff_prop.evaluate(t) : (double)long_effect->effect_falloff, 0.0, 8.0);
        p.long_blur_type = long_effect->effect_blur_type;
        p.long_blur = p.long_blur_type == (int)LongShadowBlurType::None
                          ? 0.0
                          : std::max(0.0, long_effect->size_prop.is_animated() ? long_effect->size_prop.evaluate(t) : (double)long_effect->effect_size);
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
    static std::unordered_map<std::string, CachedShadowEntry> cache;
    static uint64_t cache_tick = 0;
    static qsizetype cache_bytes = 0;
    const QString key = shadow_param_key(layer, p, shape_key);
    const std::string skey = key.toStdString();
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(skey);
        if (it != cache.end()) {
            it->second.last_used = ++cache_tick;
            return it->second.image;
        }
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
    constexpr qsizetype kMaxShadowCacheBytes = 128 * 1024 * 1024;
    // A single pathological shadow surface must not become a permanent static
    // allocation. It can still be returned for the current frame, but is not
    // retained in the process-wide cache.
    if (out.image.sizeInBytes() > kMaxShadowCacheBytes)
        return out;
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        if (auto existing = cache.find(skey); existing != cache.end()) {
            cache_bytes -= existing->second.image.image.sizeInBytes();
            cache.erase(existing);
        }
        cache.emplace(skey, CachedShadowEntry{out, ++cache_tick});
        cache_bytes += out.image.sizeInBytes();

        constexpr size_t kMaxShadowCacheEntries = 128;
        while ((cache.size() > kMaxShadowCacheEntries ||
                cache_bytes > kMaxShadowCacheBytes) &&
               cache.size() > 1) {
            auto oldest = cache.end();
            for (auto it = cache.begin(); it != cache.end(); ++it) {
                if (it->first == skey)
                    continue;
                if (oldest == cache.end() || it->second.last_used < oldest->second.last_used)
                    oldest = it;
            }
            if (oldest == cache.end())
                break;
            cache_bytes -= oldest->second.image.image.sizeInBytes();
            cache.erase(oldest);
        }
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
    const bool distribute_vertical = layer.align_v == 3 && lines.size() > 1 && total_height < rect.height();
    const double distributed_gap = distribute_vertical
        ? (rect.height() - total_height) / (static_cast<double>(lines.size()) - 1.0)
        : 0.0;
    if (!distribute_vertical) {
        if (alignment & Qt::AlignVCenter) y = rect.top() + (rect.height() - total_height) / 2.0;
        else if (alignment & Qt::AlignBottom) y = rect.bottom() - total_height;
    }

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
        y += distributed_gap;
    }
    return path;
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
                                     const Layer &layer, const std::string &title_id,
                                     bool title_cued)
{
    struct TickerCache { QString key; QPainterPath path; QRectF bounds; };
    static TickerCache cache;
    QPainterPath path;
    QFontMetricsF metrics(font);
    const double speed = std::max(1.0, layer.ticker_speed);
    const TickerRuntimeSnapshot ticker_state =
        bgs::ticker_runtime::sample(title_id, layer, title_cued);
    const double now = ticker_state.time_seconds;

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
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect && eval_effect_enabled(*effect, t);
}

static double eval_background_opacity(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::clamp(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(t) : (double)effect->effect_opacity, 0.0, 1.0) : 0.0;
}

static double eval_background_padding_x(const Layer &layer, double t)
{
    return 0.0;
}

static double eval_background_padding_y(const Layer &layer, double t)
{
    return 0.0;
}

static double eval_background_padding_left(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? (effect->padding_left_prop.is_animated() ? effect->padding_left_prop.evaluate(t) : (double)effect->effect_padding_left) : 0.0;
}

static double eval_background_padding_right(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? (effect->padding_right_prop.is_animated() ? effect->padding_right_prop.evaluate(t) : (double)effect->effect_padding_right) : 0.0;
}

static double eval_background_padding_top(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? (effect->padding_top_prop.is_animated() ? effect->padding_top_prop.evaluate(t) : (double)effect->effect_padding_top) : 0.0;
}

static double eval_background_padding_bottom(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? (effect->padding_bottom_prop.is_animated() ? effect->padding_bottom_prop.evaluate(t) : (double)effect->effect_padding_bottom) : 0.0;
}

static double eval_background_corner_radius(const Layer &layer, double t)
{
    return 0.0;
}

static double eval_background_corner_radius_tl(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::max(0.0, effect->corner_radius_tl_prop.is_animated() ? effect->corner_radius_tl_prop.evaluate(t) : (double)effect->effect_corner_radius_tl) : 0.0;
}

static double eval_background_corner_radius_tr(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::max(0.0, effect->corner_radius_tr_prop.is_animated() ? effect->corner_radius_tr_prop.evaluate(t) : (double)effect->effect_corner_radius_tr) : 0.0;
}

static double eval_background_corner_radius_br(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::max(0.0, effect->corner_radius_br_prop.is_animated() ? effect->corner_radius_br_prop.evaluate(t) : (double)effect->effect_corner_radius_br) : 0.0;
}

static double eval_background_corner_radius_bl(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::max(0.0, effect->corner_radius_bl_prop.is_animated() ? effect->corner_radius_bl_prop.evaluate(t) : (double)effect->effect_corner_radius_bl) : 0.0;
}

static double eval_background_stroke_width(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::max(0.0, effect->stroke_width_prop.is_animated() ? effect->stroke_width_prop.evaluate(t) : (double)effect->effect_stroke_width) : 0.0;
}

static double eval_background_stroke_opacity(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::clamp(effect->stroke_opacity_prop.is_animated() ? effect->stroke_opacity_prop.evaluate(t) : (double)effect->effect_stroke_opacity, 0.0, 1.0) : 1.0;
}

static uint32_t eval_background_color(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? eval_effect_color(*effect, t) : 0x00000000;
}

static uint32_t eval_background_stroke_color(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? eval_effect_stroke_color(*effect, t) : 0x00000000;
}

static QColor evaluated_background_color(const Layer &layer, double t)
{
    QColor color = color_from_argb(eval_background_color(layer, t));
    color.setAlphaF(std::clamp((double)color.alphaF() * eval_background_opacity(layer, t), 0.0, 1.0));
    return color;
}






static void apply_rich_text_extended_font_properties(QFont &font, const RichTextCharFormat &format)
{
    if (!format.font_style.empty())
        font.setStyleName(QString::fromStdString(format.font_style));
    font.setKerning(format.kerning_mode != 2 && format.kerning);
    font.setLetterSpacing(QFont::AbsoluteSpacing,
                          format.tracking + (format.kerning_mode == 2 ? format.manual_kerning : 0.0f));
    const RichTextFontScaleMetrics scale = rich_text_font_scale_metrics(format.scale_x, format.scale_y);
    if (font.pixelSize() > 0)
        font.setPixelSize(std::max(1, (int)std::round(font.pixelSize() * scale.vertical_factor)));
    else if (font.pointSizeF() > 0.0)
        font.setPointSizeF(std::max(0.1, font.pointSizeF() * scale.vertical_factor));
    font.setStretch(scale.horizontal_stretch_percent);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    font.setFeature("kern", format.kerning_mode != 2 && format.kerning ? 1 : 0);
    font.setFeature("liga", format.ligatures ? 1 : 0);
    font.setFeature("clig", format.ligatures ? 1 : 0);
    font.setFeature("salt", format.stylistic_alternates ? 1 : 0);
    font.setFeature("frac", format.fractions ? 1 : 0);
    font.setFeature("calt", format.opentype_features ? 1 : 0);
#endif
    font.setCapitalization(QFont::MixedCase);
    if (format.text_style == 1)
        font.setCapitalization(QFont::AllUppercase);
    else if (format.text_style == 2)
        font.setCapitalization(QFont::SmallCaps);
    if (format.text_style == 3 || format.text_style == 4)
        font.setPixelSize(std::max(1, (int)std::round(font.pixelSize() * 0.65)));
    /* Preserve shaping for complex scripts. Ligature feature control moves to
     * the Phase 12 HarfBuzz shaping stage instead of disabling shaping. */
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

static void apply_rich_text_baseline_delta(QTextCharFormat &out,
                                           const RichTextCharFormat &format,
                                           float default_baseline_shift)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const auto scale_metrics = rich_text_font_scale_metrics(format.scale_x, format.scale_y);
    const double glyph_height = std::max<double>(
        1.0, static_cast<double>(format.font_size) *
                 static_cast<double>(scale_metrics.vertical_factor));
    out.setBaselineOffset(100.0 * (format.baseline_shift - default_baseline_shift) / glyph_height);
#else
    (void)out;
    (void)format;
    (void)default_baseline_shift;
#endif
}

static QBrush rich_text_fill_brush(const RichTextFill &fill, const QRectF &box)
{
    if (fill.type != 1)
        return QBrush(color_from_argb(fill.color));

    return make_gradient_brush(fill.gradient_type, fill.gradient_spread, box,
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


static int qtext_position_from_rich_byte_offset_source(const QString &text, size_t byte_offset)
{
    const QByteArray utf8 = text.toUtf8();
    const size_t clamped = std::min(byte_offset, (size_t)utf8.size());
    int units = 0;
    size_t bytes_seen = 0;
    for (int i = 0; i < text.size(); ++i) {
        const ushort u = text.at(i).unicode();
        const bool high = u >= 0xD800 && u <= 0xDBFF && i + 1 < text.size();
        const int char_units = high ? 2 : 1;
        const QString chunk = text.mid(i, char_units);
        const size_t chunk_bytes = (size_t)chunk.toUtf8().size();
        if (bytes_seen + chunk_bytes > clamped)
            break;
        bytes_seen += chunk_bytes;
        units += char_units;
        if (high) ++i;
    }
    return units;
}

static size_t qtext_byte_offset_from_position_source(const QString &text,
                                                       int position)
{
    position = std::clamp(position, 0, static_cast<int>(text.size()));
    return static_cast<size_t>(text.left(position).toUtf8().size());
}

static Qt::Alignment rich_text_qt_alignment_source(int align_h)
{
    if (align_h == 1 || align_h == 4) return Qt::AlignHCenter;
    if (align_h == 2 || align_h == 5) return Qt::AlignRight;
    if (align_h >= 3) return Qt::AlignJustify;
    return Qt::AlignLeft;
}

static QTextBlockFormat text_block_format_from_rich_format(
    const RichTextParagraphFormat &format)
{
    QTextBlockFormat out;
    out.setAlignment(rich_text_qt_alignment_source(format.align_h));
    out.setLeftMargin(std::max(0.0f, format.indent_left));
    out.setRightMargin(std::max(0.0f, format.indent_right));
    out.setTextIndent(format.indent_first_line);
    out.setTopMargin(std::max(0.0f, format.space_before));
    out.setBottomMargin(std::max(0.0f, format.space_after));
    if (std::abs(format.line_spacing) >= 0.0001f)
        out.setLineHeight(format.line_spacing, QTextBlockFormat::LineDistanceHeight);
    else
        out.setLineHeight(0.0, QTextBlockFormat::SingleHeight);
    return out;
}

static void apply_rich_text_paragraph_blocks(QTextDocument &doc,
                                             const RichTextDocument &model)
{
    const QString qplain = doc.toPlainText();
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        const size_t start = (size_t)qplain.left(block.position()).toUtf8().size();
        RichTextParagraphFormat effective = model.default_paragraph_format;
        for (const auto &rich_block : model.blocks) {
            if (rich_block.start != start)
                continue;
            rich_text_merge_paragraph_format(effective, rich_block.format,
                                             rich_block.mask);
            break;
        }
        QTextCursor cursor(block);
        cursor.setBlockFormat(text_block_format_from_rich_format(effective));
    }
}

static Qt::PenJoinStyle rich_text_stroke_join_style(int join_style)
{
    switch (join_style) {
    case 1: return Qt::RoundJoin;
    case 2: return Qt::BevelJoin;
    default: return Qt::MiterJoin;
    }
}

static QPen rich_text_stroke_pen(const RichTextStroke &stroke,
                                 const QRectF &text_rect)
{
    if (!stroke.enabled || stroke.width <= 0.0f)
        return QPen(Qt::NoPen);
    RichTextFill fill = stroke.fill;
    if (fill.type == 0) {
        QColor color = color_from_argb(fill.color);
        color.setAlphaF(std::clamp((double)color.alphaF() *
                                   (double)stroke.opacity, 0.0, 1.0));
        fill.color = ((uint32_t)color.alpha() << 24) |
                     ((uint32_t)color.red() << 16) |
                     ((uint32_t)color.green() << 8) |
                     (uint32_t)color.blue();
    } else {
        fill.gradient_opacity = (float)std::clamp(
            (double)fill.gradient_opacity * (double)stroke.opacity, 0.0, 1.0);
    }
    const qreal painted_width = stroke.alignment == 1
        ? std::max(0.0f, stroke.width)
        : std::max(0.0f, stroke.width) * 2.0;
    return QPen(rich_text_fill_brush(fill, text_rect), painted_width,
                Qt::SolidLine, Qt::RoundCap,
                rich_text_stroke_join_style(stroke.join_style));
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
    out.setTextOutline(rich_text_stroke_pen(format.stroke, text_rect));
    return out;
}

static bool resolve_auto_text_style_preset(const std::string &preset_id, RichTextCharFormat &format, uint32_t &mask)
{
    static obsbgs::StylePresetLibrary library;
    obsbgs::StylePreset preset;
    if (!library.findById(QString::fromStdString(preset_id), &preset) || preset.kind != obsbgs::StylePresetKind::Text)
        return false;
    if (!obsbgs::StylePresetLibrary::textPresetToCharFormat(preset, format))
        return false;
    mask = obsbgs::StylePresetLibrary::textPresetCharMask();
    return true;
}

static RichTextStroke rich_text_layer_stroke_for_source_time(
    const Layer &layer, double local_time)
{
    RichTextStroke stroke;
    stroke.enabled = eval_outline_enabled(layer, local_time);
    stroke.width = static_cast<float>(eval_outline_width(layer, local_time));
    stroke.opacity = static_cast<float>(eval_outline_opacity(layer, local_time));
    stroke.on_front = eval_outline_on_front(layer, local_time);
    stroke.alignment = eval_outline_alignment(layer, local_time);
    stroke.antialias = eval_outline_antialias(layer, local_time);
    stroke.join_style = layer.outline_join_style;
    stroke.fill.type = layer.stroke_fill_type == 2 ? 1 : 0;
    stroke.fill.color = eval_outline_color(layer, local_time);
    stroke.fill.gradient_type = layer.stroke_gradient_type;
    stroke.fill.gradient_spread = layer.stroke_gradient_spread;
    stroke.fill.gradient_start_color = layer.stroke_gradient_start_color;
    stroke.fill.gradient_end_color = layer.stroke_gradient_end_color;
    stroke.fill.gradient_start_pos = layer.stroke_gradient_start_pos;
    stroke.fill.gradient_end_pos = layer.stroke_gradient_end_pos;
    stroke.fill.gradient_start_opacity = layer.stroke_gradient_start_opacity;
    stroke.fill.gradient_end_opacity = layer.stroke_gradient_end_opacity;
    stroke.fill.gradient_opacity = layer.stroke_gradient_opacity;
    stroke.fill.gradient_angle = layer.stroke_gradient_angle;
    stroke.fill.gradient_center_x = layer.stroke_gradient_center_x;
    stroke.fill.gradient_center_y = layer.stroke_gradient_center_y;
    stroke.fill.gradient_scale = layer.stroke_gradient_scale;
    stroke.fill.gradient_focal_x = layer.stroke_gradient_focal_x;
    stroke.fill.gradient_focal_y = layer.stroke_gradient_focal_y;
    return stroke;
}

static RichTextDocument rich_text_model_for_source_time(const Layer &layer,
                                                        double local_time)
{
    Layer canonical = layer;
    rich_text_document_ensure_canonical(canonical);
    if (layer.type == LayerType::Clock || layer.type == LayerType::Ticker) {
        const std::string display_text =
            display_text_for_style(layer).toStdString();
        const RichTextCharFormat insertion_format =
            rich_text_effective_typing_format(canonical.rich_text);
        rich_text_document_replace_text(
            canonical.rich_text, display_text, insertion_format,
            canonical.rich_text.has_typing_format
                ? canonical.rich_text.typing_format_mask
                : 0);
    }

    RichTextEvaluatedDefaults defaults;
    defaults.font_size = std::max(
        1, static_cast<int>(std::round(eval_text_font_size(layer, local_time))));
    defaults.tracking =
        static_cast<float>(eval_char_tracking(layer, local_time));
    defaults.scale_x =
        static_cast<float>(eval_char_scale_x(layer, local_time));
    defaults.scale_y =
        static_cast<float>(eval_char_scale_y(layer, local_time));
    defaults.baseline_shift =
        static_cast<float>(eval_baseline_shift(layer, local_time));
    defaults.solid_fill_color = eval_text_color(layer, local_time);
    defaults.align_h = layer.align_h;
    defaults.align_v = layer.align_v;
    defaults.indent_left =
        static_cast<float>(eval_paragraph_indent_left(layer, local_time));
    defaults.indent_right =
        static_cast<float>(eval_paragraph_indent_right(layer, local_time));
    defaults.indent_first_line = static_cast<float>(
        eval_paragraph_indent_first_line(layer, local_time));
    defaults.line_spacing = layer.text_leading;
    defaults.space_before =
        static_cast<float>(eval_paragraph_space_before(layer, local_time));
    defaults.space_after =
        static_cast<float>(eval_paragraph_space_after(layer, local_time));
    defaults.hyphenate = layer.paragraph_hyphenate;
    RichTextDocument model = rich_text_document_with_evaluated_defaults(
        std::move(canonical.rich_text), defaults);
    /* Layer-wide text stroke is the fallback character stroke. Sparse
     * RichTextCharStroke ranges remain independent overrides, so changing the
     * object order/width/color no longer erases mixed inline stroke styles. */
    model.default_format.stroke =
        rich_text_layer_stroke_for_source_time(layer, local_time);
    return rich_text_document_with_auto_styles(
        model, resolve_auto_text_style_preset);
}

static double max_rich_text_stroke_width(const Layer &layer, double t)
{
    const RichTextDocument model = rich_text_model_for_source_time(layer, t);
    double maximum = model.default_format.stroke.enabled
        ? std::max(0.0f, model.default_format.stroke.width)
        : 0.0;
    for (const RichTextRange &range : model.ranges) {
        if (range.length == 0 || range.start >= model.plain_text.size())
            continue;
        const RichTextStroke &stroke =
            rich_text_format_at(model, range.start).stroke;
        if (stroke.enabled)
            maximum = std::max(maximum, (double)std::max(0.0f, stroke.width));
    }
    return maximum;
}

static void apply_rich_text_ranges(QTextDocument &doc, const Layer &layer, const QFont &font,
                                   const QRectF &text_rect, double t)
{
    const RichTextDocument model =
        rich_text_model_for_source_time(layer, t);
    QTextCursor all(&doc);
    all.select(QTextCursor::Document);
    all.mergeCharFormat(text_format_from_rich_format(model.default_format, font, text_rect));
    const QString qplain = QString::fromStdString(model.plain_text);
    for (const auto &range : model.ranges) {
        if (range.length == 0 || range.start >= model.plain_text.size()) continue;
        QTextCursor cursor(&doc);
        const int qstart = qtext_position_from_rich_byte_offset_source(qplain, range.start);
        const int qend = qtext_position_from_rich_byte_offset_source(qplain, range.start + range.length);
        cursor.setPosition(std::clamp(qstart, 0, static_cast<int>(qplain.size())));
        cursor.setPosition(std::clamp(qend, 0, static_cast<int>(qplain.size())), QTextCursor::KeepAnchor);
        const RichTextCharFormat effective = rich_text_format_at(model, range.start);
        QTextCharFormat format = text_format_from_rich_format(effective, font, text_rect);
        apply_rich_text_baseline_delta(format, effective, model.default_format.baseline_shift);
        cursor.mergeCharFormat(format);
    }
    apply_rich_text_paragraph_blocks(doc, model);
}

struct TextTransitionUnitRange {
    int start = 0;
    int length = 0;
};

static void hide_rich_text_outside_range(QTextDocument &doc,
                                         const TextTransitionUnitRange *visible_range)
{
    if (!visible_range)
        return;

    const int text_length = std::max(0, doc.characterCount() - 1);
    const int start = std::clamp(visible_range->start, 0, text_length);
    const int end = std::clamp(start + std::max(0, visible_range->length), start, text_length);

    QTextCharFormat hidden;
    hidden.setForeground(QBrush(Qt::transparent));
    hidden.setTextOutline(QPen(Qt::NoPen));
    hidden.setFontUnderline(false);
    hidden.setFontStrikeOut(false);

    auto hide_range = [&](int from, int to) {
        if (to <= from)
            return;
        QTextCursor cursor(&doc);
        cursor.setPosition(from);
        cursor.setPosition(to, QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(hidden);
    };

    hide_range(0, start);
    hide_range(end, text_length);
}

static int rich_text_document_visual_line_count(const QTextDocument &doc)
{
    int count = 0;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next()) {
        const QTextLayout *layout = block.layout();
        if (layout)
            count += layout->lineCount();
    }
    return count;
}

static void apply_rich_text_vertical_distribute(QTextDocument &doc, const Layer &layer, const QRectF &text_rect)
{
    if (layer.align_v != 3 || layer.text_overflow_mode == 2)
        return;

    const QSizeF natural_size = doc.size();
    const int line_count = rich_text_document_visual_line_count(doc);
    if (line_count <= 1 || natural_size.height() >= text_rect.height())
        return;

    const double extra_gap = (text_rect.height() - natural_size.height()) /
                             (static_cast<double>(line_count) - 1.0);
    if (extra_gap <= 0.0)
        return;

    QTextBlockFormat block_format;
    block_format.setLineHeight(extra_gap, QTextBlockFormat::LineDistanceHeight);
    QTextCursor cursor(&doc);
    cursor.select(QTextCursor::Document);
    cursor.mergeBlockFormat(block_format);
}

static std::unique_ptr<QTextDocument> rich_text_document_for_layer(const Layer &layer, const QFont &font,
                                                  const QRectF &text_rect, double t,
                                                  const TextTransitionUnitRange *visible_range = nullptr)
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
    const RichTextDocument render_model =
        rich_text_model_for_source_time(layer, t);
    doc->setPlainText(QString::fromStdString(render_model.plain_text));
    apply_rich_text_ranges(*doc, layer, font,
                           QRectF(0.0, 0.0, text_rect.width(), text_rect.height()), t);

    hide_rich_text_outside_range(*doc, visible_range);
    apply_rich_text_vertical_distribute(*doc, layer, text_rect);
    return doc;
}

static double rich_text_horizontal_fit_scale(const Layer &layer,
                                             const QRectF &text_rect,
                                             const QSizeF &document_size)
{
    if (layer.text_overflow_mode != 2 || document_size.width() <= text_rect.width())
        return 1.0;

    return std::clamp(text_rect.width() / std::max(1.0, document_size.width()),
                      std::clamp(static_cast<double>(layer.text_fit_min_scale), 0.05, 1.0),
                      1.0);
}

static QPointF rich_text_document_origin(const Layer &layer,
                                         const QRectF &text_rect,
                                         const QSizeF &document_size,
                                         double t,
                                         double horizontal_scale)
{
    QPointF origin = text_rect.topLeft();

    if (layer.text_overflow_mode == 2) {
        const double visual_width = document_size.width() * horizontal_scale;
        if (layer.align_h == 1 || layer.align_h == 4)
            origin.setX(text_rect.left() + (text_rect.width() - visual_width) / 2.0);
        else if (layer.align_h == 2 || layer.align_h == 5)
            origin.setX(text_rect.right() - visual_width);
    }

    if (layer.align_v == 1)
        origin.setY(text_rect.top() + (text_rect.height() - document_size.height()) / 2.0);
    else if (layer.align_v == 2)
        origin.setY(text_rect.bottom() - document_size.height());
    if (std::abs(eval_baseline_shift(layer, t)) > 0.0001)
        origin.setY(origin.y() - eval_baseline_shift(layer, t));

    return origin;
}

static QPointF rich_text_document_cursor_position(const QTextDocument &doc,
                                                  const Layer &layer,
                                                  const QRectF &text_rect,
                                                  double t,
                                                  int document_position)
{
    const QAbstractTextDocumentLayout *document_layout = doc.documentLayout();
    if (!document_layout)
        return text_rect.topLeft();

    const int text_length = std::max(0, doc.characterCount() - 1);
    const int clamped_position = std::clamp(document_position, 0, text_length);
    QTextBlock block = doc.findBlock(clamped_position);
    if (!block.isValid())
        block = doc.lastBlock();
    if (!block.isValid() || !block.layout())
        return text_rect.topLeft();

    const QTextLayout *layout = block.layout();
    const int relative_position = std::clamp(
        clamped_position - block.position(), 0, std::max(0, block.length() - 1));
    QTextLine line = layout->lineForTextPosition(relative_position);
    if (!line.isValid() && layout->lineCount() > 0)
        line = layout->lineAt(layout->lineCount() - 1);
    if (!line.isValid())
        return text_rect.topLeft();

    const QRectF block_rect = document_layout->blockBoundingRect(block);
    QPointF local(block_rect.left() + line.cursorToX(relative_position),
                  block_rect.top() + line.y());

    const QSizeF document_size = doc.size();
    const double horizontal_scale = rich_text_horizontal_fit_scale(layer, text_rect, document_size);
    const QPointF origin = rich_text_document_origin(layer, text_rect, document_size, t, horizontal_scale);
    local.setX(local.x() * horizontal_scale);
    return origin + local;
}

static QRectF rich_text_document_range_bounds(const QTextDocument &doc,
                                               const Layer &layer,
                                               const QRectF &text_rect,
                                               double t,
                                               const TextTransitionUnitRange &range)
{
    const QAbstractTextDocumentLayout *document_layout = doc.documentLayout();
    if (!document_layout)
        return {};

    const int text_length = std::max(0, doc.characterCount() - 1);
    const int range_start = std::clamp(range.start, 0, text_length);
    const int range_end = std::clamp(range_start + std::max(0, range.length),
                                     range_start, text_length);
    if (range_end <= range_start)
        return {};

    /* Phase 12B: transition units consume the same immutable glyph/cluster
     * layout that will feed the GPU renderer. QTextDocument remains only as
     * the temporary raster adapter until Phase 12C. */
    const ImmutableTextLayout shaped_layout = source_text_layout_for_metrics(
        layer, t, text_rect.width(), text_rect.height(),
        layer.text_overflow_mode);
    if (shaped_layout && shaped_layout->valid) {
        const QString plain_text = doc.toPlainText();
        const size_t byte_start = qtext_byte_offset_from_position_source(
            plain_text, range_start);
        const size_t byte_end = qtext_byte_offset_from_position_source(
            plain_text, range_end);
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        if (text_layout_range_bounds(*shaped_layout, byte_start,
                                     byte_end - byte_start, x, y,
                                     width, height)) {
            return QRectF(text_rect.left() + x, text_rect.top() + y,
                          std::max(1.0f, width),
                          std::max(1.0f, height));
        }
    }

    QRectF local_bounds;
    bool has_bounds = false;
    for (QTextBlock block = doc.findBlock(range_start);
         block.isValid() && block.position() < range_end;
         block = block.next()) {
        const QTextLayout *layout = block.layout();
        if (!layout)
            continue;
        const QRectF block_rect = document_layout->blockBoundingRect(block);
        for (int line_index = 0; line_index < layout->lineCount(); ++line_index) {
            const QTextLine line = layout->lineAt(line_index);
            if (!line.isValid())
                continue;
            const int line_start = block.position() + line.textStart();
            const int line_end = line_start + line.textLength();
            const int overlap_start = std::max(range_start, line_start);
            const int overlap_end = std::min(range_end, line_end);
            if (overlap_end <= overlap_start)
                continue;

            const int relative_start = overlap_start - block.position();
            const int relative_end = overlap_end - block.position();
            const qreal x1 = line.cursorToX(relative_start);
            const qreal x2 = line.cursorToX(relative_end);
            const QRectF line_bounds(
                block_rect.left() + std::min(x1, x2),
                block_rect.top() + line.y(),
                std::max<qreal>(1.0, std::abs(x2 - x1)),
                std::max<qreal>(1.0, line.height()));
            local_bounds = has_bounds ? local_bounds.united(line_bounds) : line_bounds;
            has_bounds = true;
        }
    }
    if (!has_bounds)
        return {};

    const QSizeF document_size = doc.size();
    const double horizontal_scale = rich_text_horizontal_fit_scale(layer, text_rect, document_size);
    const QPointF origin = rich_text_document_origin(layer, text_rect, document_size, t, horizontal_scale);
    const qreal scaled_left = local_bounds.left() * horizontal_scale;
    const qreal scaled_width = local_bounds.width() * horizontal_scale;
    local_bounds.setRect(scaled_left, local_bounds.top(),
                         scaled_width, local_bounds.height());
    return local_bounds.translated(origin);
}

static QRect text_transition_unit_render_bounds(const QTextDocument &shaped_document,
                                                const Layer &layer,
                                                const QRectF &text_rect,
                                                double t,
                                                const TextTransitionUnitRange &range,
                                                const ShadowRenderParams &shadow_params,
                                                bool render_shadow,
                                                const QRect &image_bounds)
{
    QRectF bounds = rich_text_document_range_bounds(
        shaped_document, layer, text_rect, t, range);
    if (bounds.isEmpty())
        bounds = text_rect;

    // QTextLine bounds describe advances, not all italic/outline/shadow pixel
    // overhang. Expand from the actual line height, then include the complete
    // shadow envelope. This remains much smaller than a full layer surface.
    double pad = std::max(8.0, bounds.height() * 0.5) +
                 std::max(std::max(0.0, eval_outline_width(layer, t)),
                          max_rich_text_stroke_width(layer, t)) * 2.0 + 3.0;
    if (render_shadow) {
        const double drop_extent = std::max(std::abs(shadow_params.dx),
                                            std::abs(shadow_params.dy)) +
                                   shadow_params.blur * 3.0 +
                                   std::max(0.0, shadow_params.spread);
        const double long_extent = std::max(0.0, shadow_params.long_length) +
                                   std::max(0.0, shadow_params.long_blur) * 3.0;
        pad += std::max(drop_extent, long_extent);
    }

    QRect result = bounds.adjusted(-pad, -pad, pad, pad).toAlignedRect();
    result = result.intersected(image_bounds);
    return result;
}

static QPointF rich_text_transition_unit_kerning_offset(const QTextDocument &shaped_document,
                                                        const QTextDocument &isolated_document,
                                                        const Layer &layer,
                                                        const QRectF &text_rect,
                                                        double t,
                                                        const TextTransitionUnitRange &range)
{
    // Hiding the surrounding characters creates QText format-run boundaries.
    // Qt can no longer apply kerning across those boundaries, even though the
    // hidden characters still reserve their normal advance. Compare the caret
    // position in the fully shaped document with the isolated document and
    // move the independently rendered unit back to its original shaped origin.
    const QPointF shaped = rich_text_document_cursor_position(
        shaped_document, layer, text_rect, t, range.start);
    const QPointF isolated = rich_text_document_cursor_position(
        isolated_document, layer, text_rect, t, range.start);
    return shaped - isolated;
}

static void clear_global_outline_on_local_stroke_ranges(
    QTextDocument &document, const Layer &layer, double t,
    const TextTransitionUnitRange *visible_range)
{
    const RichTextDocument model = rich_text_model_for_source_time(layer, t);
    const QString qplain = document.toPlainText();
    const int visible_start = visible_range ? std::max(0, visible_range->start) : 0;
    const int visible_end = visible_range
        ? visible_start + std::max(0, visible_range->length)
        : std::max(0, document.characterCount() - 1);
    for (const RichTextRange &range : model.ranges) {
        if ((range.mask & RichTextCharStroke) == 0 || range.length == 0)
            continue;
        const int start = std::max(
            visible_start, qtext_position_from_rich_byte_offset_source(qplain, range.start));
        const int end = std::min(
            visible_end, qtext_position_from_rich_byte_offset_source(
                             qplain, range.start + range.length));
        if (end <= start)
            continue;
        QTextCursor cursor(&document);
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        QTextCharFormat no_global_outline;
        no_global_outline.setTextOutline(QPen(Qt::NoPen));
        cursor.mergeCharFormat(no_global_outline);
    }
}

static void draw_rich_text_document(QPainter &painter, const Layer &layer, const QFont &font,
                                    const QRectF &text_rect, double t,
                                    const QPen *text_outline = nullptr,
                                    bool outline_only = false,
                                    bool alpha_mask_only = false,
                                    const TextTransitionUnitRange *visible_range = nullptr)
{
    auto doc = rich_text_document_for_layer(layer, font, text_rect, t, visible_range);
    if (text_outline || alpha_mask_only) {
        QTextCursor cursor(doc.get());
        if (visible_range) {
            const int text_length = std::max(0, doc->characterCount() - 1);
            const int start = std::clamp(visible_range->start, 0, text_length);
            const int end = std::clamp(start + std::max(0, visible_range->length), start, text_length);
            cursor.setPosition(start);
            cursor.setPosition(end, QTextCursor::KeepAnchor);
        } else {
            cursor.select(QTextCursor::Document);
        }
        QTextCharFormat format;
        if (text_outline)
            format.setTextOutline(*text_outline);
        if (outline_only)
            format.setForeground(QBrush(Qt::transparent));
        else if (alpha_mask_only) {
            // A stroke-alignment mask must not inherit inline fill colours,
            // gradients or opacity.  Use one opaque white glyph silhouette so
            // DestinationIn/DestinationOut isolate the exact inner/outer half.
            format.setForeground(QBrush(Qt::white));
            format.setTextOutline(QPen(Qt::NoPen));
        }
        cursor.mergeCharFormat(format);
        if (text_outline)
            clear_global_outline_on_local_stroke_ranges(*doc, layer, t, visible_range);
    }
    QSizeF doc_size = doc->size();
    const double scale = rich_text_horizontal_fit_scale(layer, text_rect, doc_size);
    const QPointF origin = rich_text_document_origin(layer, text_rect, doc_size, t, scale);
    if (std::abs(scale - 1.0) > 0.0001) {
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

static void draw_prepared_rich_text_document(QPainter &painter,
                                                const QTextDocument &source_document,
                                                const Layer &layer,
                                                const QRectF &text_rect,
                                                double t,
                                                const TextTransitionUnitRange &visible_range,
                                                const QPen *text_outline = nullptr,
                                                bool outline_only = false,
                                                bool alpha_mask_only = false)
{
    std::unique_ptr<QTextDocument> modified_document;
    QTextDocument *document = const_cast<QTextDocument *>(&source_document);
    if (text_outline || alpha_mask_only) {
        modified_document.reset(source_document.clone());
        document = modified_document.get();
        const int text_length = std::max(0, document->characterCount() - 1);
        const int start = std::clamp(visible_range.start, 0, text_length);
        const int end = std::clamp(start + std::max(0, visible_range.length), start, text_length);
        QTextCursor cursor(document);
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        QTextCharFormat format;
        if (text_outline)
            format.setTextOutline(*text_outline);
        if (outline_only)
            format.setForeground(QBrush(Qt::transparent));
        else if (alpha_mask_only) {
            format.setForeground(QBrush(Qt::white));
            format.setTextOutline(QPen(Qt::NoPen));
        }
        cursor.mergeCharFormat(format);
        if (text_outline)
            clear_global_outline_on_local_stroke_ranges(*document, layer, t, &visible_range);
    }

    const QSizeF document_size = document->size();
    const double scale = rich_text_horizontal_fit_scale(layer, text_rect, document_size);
    const QPointF origin = rich_text_document_origin(layer, text_rect, document_size, t, scale);
    if (std::abs(scale - 1.0) > 0.0001) {
        painter.save();
        painter.translate(origin);
        painter.scale(scale, 1.0);
        document->drawContents(&painter, QRectF(QPointF(0, 0), document_size));
        painter.restore();
        return;
    }

    painter.save();
    painter.translate(origin);
    document->drawContents(&painter, QRectF(QPointF(0, 0), document_size));
    painter.restore();
}


struct RichTextStrokeRenderGroup {
    bool on_front = false;
    int alignment = 0;
    bool antialias = true;
};

static bool same_rich_text_stroke_group(const RichTextStrokeRenderGroup &a,
                                        const RichTextStrokeRenderGroup &b)
{
    return a.on_front == b.on_front && a.alignment == b.alignment &&
           a.antialias == b.antialias;
}

static std::vector<RichTextStrokeRenderGroup>
rich_text_stroke_render_groups(const RichTextDocument &model)
{
    std::vector<RichTextStrokeRenderGroup> groups;
    for (const TextLayoutPaintRun &run : text_layout_paint_runs(model)) {
        const RichTextStroke &stroke = run.style.stroke;
        if (!stroke.enabled || stroke.width <= 0.0001f)
            continue;
        RichTextStrokeRenderGroup group;
        group.on_front = stroke.on_front;
        group.alignment = std::clamp(stroke.alignment, 0, 2);
        group.antialias = stroke.antialias;
        if (std::none_of(groups.begin(), groups.end(),
                         [&](const RichTextStrokeRenderGroup &existing) {
                             return same_rich_text_stroke_group(existing, group);
                         }))
            groups.push_back(group);
    }
    return groups;
}

static void draw_positioned_rich_text_document(QPainter &painter,
                                                QTextDocument &document,
                                                const Layer &layer,
                                                const QRectF &text_rect,
                                                double t)
{
    const QSizeF document_size = document.size();
    const double scale = rich_text_horizontal_fit_scale(
        layer, text_rect, document_size);
    const QPointF origin = rich_text_document_origin(
        layer, text_rect, document_size, t, scale);
    painter.save();
    painter.translate(origin);
    if (std::abs(scale - 1.0) > 0.0001)
        painter.scale(scale, 1.0);
    document.drawContents(&painter,
                          QRectF(QPointF(0, 0), document_size));
    painter.restore();
}

static std::unique_ptr<QTextDocument>
rich_text_fill_only_document(const QTextDocument &source)
{
    std::unique_ptr<QTextDocument> document(source.clone());
    QTextCursor cursor(document.get());
    cursor.select(QTextCursor::Document);
    QTextCharFormat format;
    format.setTextOutline(QPen(Qt::NoPen));
    cursor.mergeCharFormat(format);
    return document;
}

static std::unique_ptr<QTextDocument> rich_text_stroke_group_document(
    const QTextDocument &source, const RichTextDocument &model,
    const QRectF &text_rect, const RichTextStrokeRenderGroup &group,
    bool glyph_mask_only, const TextTransitionUnitRange *visible_range)
{
    std::unique_ptr<QTextDocument> document(source.clone());
    QTextCursor all(document.get());
    all.select(QTextCursor::Document);
    QTextCharFormat hidden;
    hidden.setForeground(QBrush(Qt::transparent));
    hidden.setTextOutline(QPen(Qt::NoPen));
    hidden.setFontUnderline(false);
    hidden.setFontStrikeOut(false);
    all.mergeCharFormat(hidden);

    const QString qplain = document->toPlainText();
    const int text_length = std::max(0, document->characterCount() - 1);
    const int visible_start = visible_range
        ? std::clamp(visible_range->start, 0, text_length)
        : 0;
    const int visible_end = visible_range
        ? std::clamp(visible_start + std::max(0, visible_range->length),
                     visible_start, text_length)
        : text_length;

    for (const TextLayoutPaintRun &run : text_layout_paint_runs(model)) {
        const RichTextStroke &stroke = run.style.stroke;
        if (!stroke.enabled || stroke.width <= 0.0001f ||
            stroke.on_front != group.on_front ||
            std::clamp(stroke.alignment, 0, 2) != group.alignment ||
            stroke.antialias != group.antialias)
            continue;

        int start = qtext_position_from_rich_byte_offset_source(
            qplain, run.byte_start);
        int end = qtext_position_from_rich_byte_offset_source(
            qplain, run.byte_start + run.byte_length);
        start = std::max(start, visible_start);
        end = std::min(end, visible_end);
        if (end <= start)
            continue;

        QTextCursor cursor(document.get());
        cursor.setPosition(std::clamp(start, 0, text_length));
        cursor.setPosition(std::clamp(end, 0, text_length),
                           QTextCursor::KeepAnchor);
        QTextCharFormat format;
        format.setFontUnderline(false);
        format.setFontStrikeOut(false);
        if (glyph_mask_only) {
            format.setForeground(QBrush(Qt::white));
            format.setTextOutline(QPen(Qt::NoPen));
        } else {
            format.setForeground(QBrush(Qt::transparent));
            format.setTextOutline(rich_text_stroke_pen(stroke, text_rect));
        }
        cursor.mergeCharFormat(format);
    }
    return document;
}

static QImage render_rich_text_stroke_group(
    const QTextDocument &source, const RichTextDocument &model,
    const Layer &layer, const QRectF &text_rect, double t,
    const QSize &surface_size, const QPointF &surface_origin,
    const RichTextStrokeRenderGroup &group,
    const TextTransitionUnitRange *visible_range = nullptr)
{
    if (surface_size.isEmpty())
        return {};

    QImage stroke_layer(surface_size,
                        QImage::Format_ARGB32_Premultiplied);
    stroke_layer.fill(Qt::transparent);
    {
        std::unique_ptr<QTextDocument> stroke_document =
            rich_text_stroke_group_document(
                source, model, text_rect, group, false, visible_range);
        QPainter stroke_painter(&stroke_layer);
        stroke_painter.setRenderHint(QPainter::Antialiasing,
                                     group.antialias);
        stroke_painter.setRenderHint(QPainter::TextAntialiasing,
                                     group.antialias);
        stroke_painter.translate(-surface_origin.x(), -surface_origin.y());
        draw_positioned_rich_text_document(
            stroke_painter, *stroke_document, layer, text_rect, t);
    }

    if (group.alignment != 1) {
        QImage glyph_mask(surface_size,
                          QImage::Format_ARGB32_Premultiplied);
        glyph_mask.fill(Qt::transparent);
        {
            std::unique_ptr<QTextDocument> mask_document =
                rich_text_stroke_group_document(
                    source, model, text_rect, group, true, visible_range);
            QPainter mask_painter(&glyph_mask);
            mask_painter.setRenderHint(QPainter::Antialiasing,
                                       group.antialias);
            mask_painter.setRenderHint(QPainter::TextAntialiasing,
                                       group.antialias);
            mask_painter.translate(-surface_origin.x(), -surface_origin.y());
            draw_positioned_rich_text_document(
                mask_painter, *mask_document, layer, text_rect, t);
        }

        QPainter isolate(&stroke_layer);
        isolate.setCompositionMode(
            group.alignment == 0
                ? QPainter::CompositionMode_DestinationOut
                : QPainter::CompositionMode_DestinationIn);
        isolate.drawImage(QPointF(0.0, 0.0), glyph_mask);
    }
    return stroke_layer;
}

static const LayerTransition *active_text_layer_transition(const Layer &layer, double title_time)
{
    const LayerTransition *active = nullptr;
    double lowest_progress = 2.0;
    for (const auto &transition : layer.transitions) {
        if (!transition.enabled || transition.kind != LayerTransitionKind::Text)
            continue;
        const bool within = transition.edge == LayerTransitionEdge::In
            ? title_time <= layer.in_time + transition.duration
            : title_time >= layer.out_time - transition.duration;
        if (!within)
            continue;
        const double progress = layer_transition_progress(
            transition, layer.in_time, layer.out_time, title_time);
        if (progress < lowest_progress) {
            lowest_progress = progress;
            active = &transition;
        }
    }
    return active;
}

static QVector<TextTransitionUnitRange> text_transition_unit_ranges(
    const QString &text, LayerTransitionUnit unit)
{
    QVector<TextTransitionUnitRange> ranges;

    auto append_range = [&](int start, int length) {
        if (length <= 0 || start < 0 || start >= static_cast<int>(text.size()))
            return;
        const int clamped_length = std::min(length, static_cast<int>(text.size()) - start);
        if (clamped_length <= 0 || text.mid(start, clamped_length).trimmed().isEmpty())
            return;
        ranges.push_back(TextTransitionUnitRange{start, clamped_length});
    };

    if (unit == LayerTransitionUnit::Character) {
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
        finder.toStart();
        int start = finder.position();
        while (start >= 0) {
            const int end = finder.toNextBoundary();
            if (end < 0)
                break;
            append_range(start, end - start);
            start = end;
        }
        return ranges;
    }

    if (unit == LayerTransitionUnit::Word) {
        static const QRegularExpression word_pattern(QStringLiteral("\\S+"));
        QRegularExpressionMatchIterator matches = word_pattern.globalMatch(text);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            append_range(static_cast<int>(match.capturedStart()),
                         static_cast<int>(match.capturedLength()));
        }
        return ranges;
    }

    static const QRegularExpression sentence_pattern(
        QStringLiteral("[^.!?\\n]+[.!?]*"));
    QRegularExpressionMatchIterator matches = sentence_pattern.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        append_range(static_cast<int>(match.capturedStart()),
                     static_cast<int>(match.capturedLength()));
    }
    if (ranges.isEmpty())
        append_range(0, static_cast<int>(text.size()));
    return ranges;
}

static QVector<QRectF> text_transition_unit_rects(const QString &text,
                                                   const QFont &font,
                                                   const QRectF &text_rect,
                                                   const Layer &layer,
                                                   LayerTransitionUnit unit)
{
    QVector<QRectF> result;
    const QFontMetricsF metrics(font);
    const QStringList lines = text.split(QLatin1Char('\n'));
    const qreal line_height = std::max<qreal>(1.0, metrics.height());
    const qreal total_height = line_height * std::max(1, static_cast<int>(lines.size()));
    qreal y = text_rect.top();
    if (layer.align_v == 1)
        y = text_rect.center().y() - total_height / 2.0;
    else if (layer.align_v == 2)
        y = text_rect.bottom() - total_height;

    for (const QString &line : lines) {
        const qreal line_width = metrics.horizontalAdvance(line);
        qreal line_x = text_rect.left();
        if (layer.align_h == 1)
            line_x = text_rect.center().x() - line_width / 2.0;
        else if (layer.align_h == 2)
            line_x = text_rect.right() - line_width;

        auto append_range = [&](int start, int length) {
            if (length <= 0)
                return;
            const QString fragment = line.mid(start, length);
            if (fragment.trimmed().isEmpty())
                return;
            const qreal x = line_x + metrics.horizontalAdvance(line.left(start));
            const qreal width = std::max<qreal>(1.0, metrics.horizontalAdvance(fragment));
            result.push_back(QRectF(x, y, width, line_height));
        };

        if (unit == LayerTransitionUnit::Character) {
            for (int i = 0; i < static_cast<int>(line.size()); ++i)
                append_range(i, 1);
        } else if (unit == LayerTransitionUnit::Word) {
            static const QRegularExpression word_pattern(QStringLiteral("\\S+"));
            QRegularExpressionMatchIterator matches = word_pattern.globalMatch(line);
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                append_range(static_cast<int>(match.capturedStart()),
                             static_cast<int>(match.capturedLength()));
            }
        } else {
            static const QRegularExpression sentence_pattern(
                QStringLiteral("[^.!?]+[.!?]*"));
            QRegularExpressionMatchIterator matches = sentence_pattern.globalMatch(line);
            bool matched = false;
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                append_range(static_cast<int>(match.capturedStart()),
                             static_cast<int>(match.capturedLength()));
                matched = true;
            }
            if (!matched)
                append_range(0, static_cast<int>(line.size()));
        }
        y += line_height;
    }
    return result;
}

static QImage blurred_transition_image(const QImage &source, int radius)
{
    if (source.isNull() || radius <= 0)
        return source;
    QImage blurred = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int width = blurred.width();
    const int height = blurred.height();
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y)
        std::memcpy(pixels.data() + static_cast<size_t>(y) * width * 4,
                    blurred.constScanLine(y), static_cast<size_t>(width) * 4);
    box_blur_pixels(pixels, width, height, std::clamp(radius, 1, 64));
    for (int y = 0; y < height; ++y)
        std::memcpy(blurred.scanLine(y),
                    pixels.data() + static_cast<size_t>(y) * width * 4,
                    static_cast<size_t>(width) * 4);
    return blurred;
}

static int animated_text_blur_radius(double maximum_radius, double unit_progress)
{
    const int max_radius = std::clamp(
        static_cast<int>(std::lround(std::max(0.0, maximum_radius))), 0, 64);
    if (max_radius <= 0)
        return 0;

    // A small blur pyramid avoids recalculating dozens of nearly identical
    // radii per glyph while still making the blur visibly contract to zero.
    constexpr int kBlurRadiusSteps = 12;
    const double hidden = std::clamp(1.0 - unit_progress, 0.0, 1.0);
    const int step = std::clamp(
        static_cast<int>(std::lround(hidden * kBlurRadiusSteps)),
        0, kBlurRadiusSteps);
    return std::clamp(
        static_cast<int>(std::lround(
            static_cast<double>(max_radius) * step / kBlurRadiusSteps)),
        0, max_radius);
}

static QRect transition_image_alpha_bounds(const QImage &image)
{
    if (image.isNull())
        return QRect();

    int min_x = image.width();
    int min_y = image.height();
    int max_x = -1;
    int max_y = -1;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) == 0)
                continue;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    }
    if (max_x < min_x || max_y < min_y)
        return QRect();
    return QRect(QPoint(min_x, min_y), QPoint(max_x, max_y));
}

static QImage render_rich_text_transition_unit(const QTextDocument &isolated_document,
                                               const Layer &layer,
                                               double t,
                                               const QFont &font,
                                               const QRectF &text_rect,
                                               const QRect &render_bounds,
                                               const ShadowRenderParams &shadow_params,
                                               bool render_shadow,
                                               const TextTransitionUnitRange &range)
{
    if (render_bounds.isEmpty())
        return {};

    QImage unit_image(render_bounds.size(), QImage::Format_ARGB32_Premultiplied);
    unit_image.fill(Qt::transparent);

    QPainter painter(&unit_image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);
    painter.translate(-render_bounds.x(), -render_bounds.y());

    const double text_outline_clip_pad =
        std::max(std::max(0.0, eval_outline_width(layer, t)),
                 max_rich_text_stroke_width(layer, t)) + 2.0;
    painter.save();
    painter.setClipRect(text_rect.adjusted(-text_outline_clip_pad,
                                           -text_outline_clip_pad,
                                           text_outline_clip_pad,
                                           text_outline_clip_pad));

    if (render_shadow) {
        QImage shadow_mask(render_bounds.size(),
                           QImage::Format_ARGB32_Premultiplied);
        shadow_mask.fill(Qt::transparent);
        QPainter mask_painter(&shadow_mask);
        mask_painter.setRenderHint(QPainter::Antialiasing, true);
        mask_painter.setRenderHint(QPainter::TextAntialiasing, true);
        mask_painter.setPen(Qt::NoPen);
        mask_painter.setBrush(Qt::white);
        mask_painter.translate(-render_bounds.x(), -render_bounds.y());
        draw_prepared_rich_text_document(mask_painter, isolated_document,
                                         layer, text_rect, t, range);
        mask_painter.end();
        const QString shape_key =
            QStringLiteral("text-transition|%1,%2,%3x%4|%5")
                .arg(render_bounds.x()).arg(render_bounds.y())
                .arg(render_bounds.width()).arg(render_bounds.height())
                .arg(image_content_hash(shadow_mask));
        CachedShadowImage shadow = build_shadow_image(
            layer, shadow_params, shape_key, shadow_mask);
        painter.save();
        painter.setClipping(false);
        painter.drawImage(QPointF(render_bounds.topLeft()) + shadow.origin,
                          shadow.image);
        painter.restore();
    }

    QColor fill = color_from_argb(eval_text_color(layer, t));
    fill.setAlphaF(std::clamp((double)fill.alphaF(), 0.0, 1.0));
    const RichTextDocument stroke_model =
        rich_text_model_for_source_time(layer, t);
    const std::vector<RichTextStrokeRenderGroup> stroke_groups =
        rich_text_stroke_render_groups(stroke_model);

    auto draw_text_fill = [&]() {
        std::unique_ptr<QTextDocument> fill_document =
            rich_text_fill_only_document(isolated_document);
        painter.save();
        painter.setOpacity(fill.alphaF());
        draw_positioned_rich_text_document(
            painter, *fill_document, layer, text_rect, t);
        painter.restore();
    };
    auto draw_stroke_phase = [&](bool on_front) {
        for (const RichTextStrokeRenderGroup &group : stroke_groups) {
            if (group.on_front != on_front)
                continue;
            const QImage stroke_layer = render_rich_text_stroke_group(
                isolated_document, stroke_model, layer, text_rect, t,
                render_bounds.size(), render_bounds.topLeft(), group, &range);
            if (!stroke_layer.isNull())
                painter.drawImage(QPointF(render_bounds.topLeft()),
                                  stroke_layer);
        }
    };

    draw_stroke_phase(false);
    draw_text_fill();
    draw_stroke_phase(true);

    painter.restore();
    painter.end();
    return unit_image;
}

static bool layer_has_non_transform_animation(const Layer &layer);

struct IsolatedTextUnitCache {
    struct BlurredVariant {
        int radius = 0;
        QImage crop;
        QRect bounds;
    };

    QString key;
    QString rejected_key;
    // Store only each unit's non-transparent crop. Keeping full layer-sized
    // images per character can retain hundreds of megabytes.
    QVector<TextTransitionUnitRange> ranges;
    QVector<QImage> source_crops;
    QVector<QRect> source_bounds;
    QVector<QPointF> placement_offsets;
    QVector<QVector<BlurredVariant>> blurred_variants;
    qsizetype source_bytes = 0;
    qsizetype blurred_bytes = 0;
    BlurredVariant scratch_variant;
    uint64_t last_used = 0;
};

static double source_frame_duration();

struct IsolatedTextUnitCachePool {
    std::unordered_map<std::string, IsolatedTextUnitCache> entries;
    uint64_t tick = 0;
};

static qsizetype isolated_text_unit_cache_bytes(const IsolatedTextUnitCache &cache)
{
    return cache.source_bytes + cache.blurred_bytes + cache.scratch_variant.crop.sizeInBytes();
}

static void evict_isolated_text_unit_caches(IsolatedTextUnitCachePool &pool,
                                             const std::string &protected_cache_id)
{
    constexpr size_t kMaxCachedTextLayers = 8;
    constexpr qsizetype kMaxTextTransitionCacheBytes = 128 * 1024 * 1024;
    qsizetype total_bytes = 0;
    for (const auto &entry : pool.entries)
        total_bytes += isolated_text_unit_cache_bytes(entry.second);

    while ((pool.entries.size() > kMaxCachedTextLayers ||
            total_bytes > kMaxTextTransitionCacheBytes) &&
           pool.entries.size() > 1) {
        auto oldest = pool.entries.end();
        for (auto it = pool.entries.begin(); it != pool.entries.end(); ++it) {
            if (it->first == protected_cache_id)
                continue;
            if (oldest == pool.entries.end() ||
                it->second.last_used < oldest->second.last_used)
                oldest = it;
        }
        if (oldest == pool.entries.end())
            break;
        total_bytes -= isolated_text_unit_cache_bytes(oldest->second);
        pool.entries.erase(oldest);
    }
}

static bool apply_isolated_text_layer_transition(QImage &image,
                                                 const QImage &static_background,
                                                 const Layer &layer,
                                                 double title_time,
                                                 const QString &text,
                                                 const QFont &font,
                                                 const QRectF &text_rect,
                                                 double t,
                                                 const ShadowRenderParams &shadow_params,
                                                 bool render_shadow)
{
    const LayerTransition *transition = active_text_layer_transition(layer, title_time);
    if (!transition || image.isNull() || layer.type == LayerType::Ticker)
        return false;

    // A single thread-local cache caused simultaneous text transitions on two
    // layers to evict each other every draw and rebuild all glyph surfaces on
    // every frame. Keep a small byte-bounded LRU pool keyed by layer id.
    static thread_local IsolatedTextUnitCachePool cache_pool;
    const std::string cache_id = layer.id.empty()
        ? std::string("__anonymous_text_transition") : layer.id;
    auto [cache_it, inserted] = cache_pool.entries.try_emplace(cache_id);
    (void)inserted;
    IsolatedTextUnitCache &cache = cache_it->second;
    cache.last_used = ++cache_pool.tick;
    // Oversized variants are scratch-only and must not remain resident after a
    // completed frame.
    cache.scratch_variant = {};
    evict_isolated_text_unit_caches(cache_pool, cache_id);

    // Hashing the complete layer-sized QImage on every transition frame was a
    // major memory-bandwidth regression (several MB per layer per frame). The
    // store revision invalidates static style changes, while the evaluated
    // time component covers animated text/style properties. Text and layout
    // values keep live-cue changes independent from unrelated store updates.
    const bool time_varying_source = layer_has_non_transform_animation(layer);
    const double frame_duration = std::max(1.0 / 240.0, source_frame_duration());
    const qlonglong evaluated_frame = time_varying_source
        ? static_cast<qlonglong>(std::llround(t / frame_duration)) : 0;
    const QByteArray text_digest = QCryptographicHash::hash(
        text.toUtf8(), QCryptographicHash::Sha1).toHex();
    const QString cache_key = QStringLiteral("%1|rev=%2|unit=%3|%4x%5|text=%6|font=%7|rect=%8,%9,%10,%11|shadow=%12|frame=%13")
        .arg(QString::fromStdString(layer.id))
        .arg(static_cast<qulonglong>(TitleDataStore::instance().revision()))
        .arg(static_cast<int>(transition->unit))
        .arg(image.width())
        .arg(image.height())
        .arg(QString::fromLatin1(text_digest))
        .arg(font.toString())
        .arg(text_rect.x(), 0, 'f', 3)
        .arg(text_rect.y(), 0, 'f', 3)
        .arg(text_rect.width(), 0, 'f', 3)
        .arg(text_rect.height(), 0, 'f', 3)
        .arg(render_shadow ? 1 : 0)
        .arg(evaluated_frame);

    constexpr qsizetype kMaxIsolatedTextUnits = 512;
    constexpr qsizetype kMaxSourceCropBytes = 48 * 1024 * 1024;
    if (cache.rejected_key == cache_key)
        return false;

    if (cache.key != cache_key || cache.source_crops.size() != cache.ranges.size()) {
        cache.key.clear();
        cache.ranges = text_transition_unit_ranges(text, transition->unit);
        cache.source_crops.clear();
        cache.source_bounds.clear();
        cache.placement_offsets.clear();
        cache.blurred_variants.clear();
        cache.source_bytes = 0;
        cache.blurred_bytes = 0;
        if (cache.ranges.isEmpty() || cache.ranges.size() > kMaxIsolatedTextUnits) {
            cache.ranges.clear();
            cache.rejected_key = cache_key;
            evict_isolated_text_unit_caches(cache_pool, cache_id);
            return false;
        }

        cache.source_crops.reserve(cache.ranges.size());
        cache.source_bounds.reserve(cache.ranges.size());
        cache.placement_offsets.reserve(cache.ranges.size());

        const std::unique_ptr<QTextDocument> shaped_document =
            rich_text_document_for_layer(layer, font, text_rect, t, nullptr);
        qsizetype source_crop_bytes = 0;
        for (const TextTransitionUnitRange &range : cache.ranges) {
            const std::unique_ptr<QTextDocument> isolated_document =
                rich_text_document_for_layer(layer, font, text_rect, t, &range);
            const QPointF placement_offset = shaped_document && isolated_document
                ? rich_text_transition_unit_kerning_offset(
                      *shaped_document, *isolated_document, layer, text_rect, t, range)
                : QPointF();
            const QRect render_bounds = shaped_document
                ? text_transition_unit_render_bounds(
                      *shaped_document, layer, text_rect, t, range,
                      shadow_params, render_shadow, image.rect())
                : image.rect();
            QImage unit_image = isolated_document
                ? render_rich_text_transition_unit(
                      *isolated_document, layer, t, font, text_rect, render_bounds,
                      shadow_params, render_shadow, range)
                : QImage();
            const QRect local_bounds = transition_image_alpha_bounds(unit_image)
                .intersected(unit_image.rect());
            const QRect bounds = local_bounds.isEmpty()
                ? QRect()
                : local_bounds.translated(render_bounds.topLeft());
            QImage source_crop = local_bounds.isEmpty()
                ? QImage() : unit_image.copy(local_bounds);
            source_crop_bytes += source_crop.sizeInBytes();
            if (source_crop_bytes > kMaxSourceCropBytes) {
                cache.key.clear();
                cache.rejected_key = cache_key;
                cache.ranges.clear();
                cache.source_crops.clear();
                cache.source_bounds.clear();
                cache.placement_offsets.clear();
                cache.blurred_variants.clear();
                cache.source_bytes = 0;
                cache.blurred_bytes = 0;
                evict_isolated_text_unit_caches(cache_pool, cache_id);
                return false;
            }
            cache.source_bounds.push_back(bounds);
            cache.placement_offsets.push_back(placement_offset);
            cache.source_crops.push_back(std::move(source_crop));
        }
        cache.source_bytes = source_crop_bytes;
        cache.key = cache_key;
        cache.rejected_key.clear();
        cache.blurred_variants.clear();
        cache.blurred_variants.resize(cache.ranges.size());
        cache.blurred_bytes = 0;
        cache.scratch_variant = {};
        evict_isolated_text_unit_caches(cache_pool, cache_id);
    }

    const QVector<TextTransitionUnitRange> &ranges = cache.ranges;
    if (ranges.isEmpty())
        return false;

    const bool blur_transition =
        transition->type == LayerTransitionType::TextBlur ||
        transition->type == LayerTransitionType::TextBlurSlide;

    auto blurred_variant_for = [&image, &cache, &cache_id](int index, int radius)
        -> const IsolatedTextUnitCache::BlurredVariant * {
        if (radius <= 0 || index < 0 || index >= cache.source_crops.size())
            return nullptr;

        QVector<IsolatedTextUnitCache::BlurredVariant> &variants =
            cache.blurred_variants[index];
        for (const auto &variant : variants) {
            if (variant.radius == radius)
                return &variant;
        }

        const QImage &source_crop = cache.source_crops[index];
        const QRect source_bounds = cache.source_bounds[index];
        if (source_crop.isNull() || source_bounds.isEmpty())
            return nullptr;

        // Blur only the current unit and expand its temporary image by the
        // current radius. The radius changes from blur_amount to zero as the
        // unit progresses, so the glyph itself becomes progressively sharper.
        const int blur_pad = radius * 3 + 2;
        QImage padded(source_crop.width() + blur_pad * 2,
                      source_crop.height() + blur_pad * 2,
                      QImage::Format_ARGB32_Premultiplied);
        padded.fill(Qt::transparent);
        {
            QPainter padded_painter(&padded);
            padded_painter.drawImage(QPoint(blur_pad, blur_pad), source_crop);
        }

        QImage blurred = blurred_transition_image(padded, radius);
        const QRect local_bounds = transition_image_alpha_bounds(blurred);
        if (local_bounds.isEmpty())
            return nullptr;

        const QPoint padded_origin = source_bounds.topLeft() - QPoint(blur_pad, blur_pad);
        const QRect global_bounds(padded_origin + local_bounds.topLeft(),
                                  local_bounds.size());
        const QRect clipped_bounds = global_bounds.intersected(image.rect());
        if (clipped_bounds.isEmpty())
            return nullptr;

        const QRect local_clip(clipped_bounds.topLeft() - global_bounds.topLeft(),
                               clipped_bounds.size());
        const QRect blurred_crop_rect(
            local_bounds.topLeft() + local_clip.topLeft(), local_clip.size());

        QImage cropped_blur = blurred.copy(blurred_crop_rect);
        const qsizetype variant_bytes = cropped_blur.sizeInBytes();

        // A per-unit count alone still allowed long strings to retain hundreds
        // of MB. Bound both the number of radii per unit and the total cache.
        constexpr int kMaxBlurVariantsPerUnit = 4;
        constexpr qsizetype kMaxBlurCacheBytes = 48 * 1024 * 1024;
        if (variant_bytes > kMaxBlurCacheBytes) {
            cache.scratch_variant = {radius, std::move(cropped_blur), clipped_bounds};
            return &cache.scratch_variant;
        }
        if (cache.blurred_bytes + variant_bytes > kMaxBlurCacheBytes) {
            for (auto &unit_variants : cache.blurred_variants)
                unit_variants.clear();
            cache.blurred_bytes = 0;
        }
        while (variants.size() >= kMaxBlurVariantsPerUnit) {
            cache.blurred_bytes -= variants.front().crop.sizeInBytes();
            variants.removeAt(0);
        }
        variants.push_back({radius, std::move(cropped_blur), clipped_bounds});
        cache.blurred_bytes += variants.back().crop.sizeInBytes();
        evict_isolated_text_unit_caches(cache_pool, cache_id);
        return &variants.back();
    };

    if (static_background.isNull())
        image.fill(Qt::transparent);
    else
        image = static_background.copy();
    const double global_progress = layer_transition_progress(
        *transition, layer.in_time, layer.out_time, title_time);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const int count = static_cast<int>(ranges.size());
    for (int index = 0; index < count; ++index) {
        const int order = transition->reverse_order ? count - 1 - index : index;
        const double delay = count <= 1 ? 0.0
            : transition->stagger * static_cast<double>(order) / static_cast<double>(count - 1);
        const double span = std::max(0.05, 1.0 - transition->stagger);
        double local = 1.0;
        if (transition->edge == LayerTransitionEdge::In)
            local = std::clamp((global_progress - delay) / span, 0.0, 1.0);
        else {
            const double out_phase = 1.0 - global_progress;
            local = 1.0 - std::clamp((out_phase - delay) / span, 0.0, 1.0);
        }
        local = layer_transition_ease(local, transition->easing);
        if (local <= 0.0001)
            continue;

        const QImage &unit_crop = cache.source_crops[index];
        const QRect source_bounds = cache.source_bounds[index];
        const QPointF placement_offset = cache.placement_offsets.value(index);
        if (source_bounds.isEmpty() || unit_crop.isNull())
            continue;

        const QPointF source_origin = QPointF(source_bounds.topLeft()) + placement_offset;
        const QRectF unit_bounds(source_origin, QSizeF(source_bounds.size()));
        const QPointF center = unit_bounds.center();

        double dx = 0.0;
        double dy = 0.0;
        if (transition->type == LayerTransitionType::TextSlide ||
            transition->type == LayerTransitionType::TextBlurSlide) {
            const double hidden = 1.0 - local;
            switch (transition->direction) {
            case LayerTransitionDirection::Right: dx = transition->offset * hidden; break;
            case LayerTransitionDirection::Up: dy = -transition->offset * hidden; break;
            case LayerTransitionDirection::Down: dy = transition->offset * hidden; break;
            case LayerTransitionDirection::Left:
            case LayerTransitionDirection::None:
            default: dx = -transition->offset * hidden; break;
            }
        }
        const double scale = transition->type == LayerTransitionType::TextScale
            ? transition->scale_from + (1.0 - transition->scale_from) * local : 1.0;

        QRectF visible = unit_bounds;
        if (transition->type == LayerTransitionType::TextWipe) {
            switch (transition->direction) {
            case LayerTransitionDirection::Right:
                visible.setLeft(unit_bounds.right() - unit_bounds.width() * local);
                break;
            case LayerTransitionDirection::Up:
                visible.setTop(unit_bounds.bottom() - unit_bounds.height() * local);
                break;
            case LayerTransitionDirection::Down:
                visible.setHeight(unit_bounds.height() * local);
                break;
            case LayerTransitionDirection::Left:
            case LayerTransitionDirection::None:
            default:
                visible.setWidth(unit_bounds.width() * local);
                break;
            }
        }

        painter.save();
        painter.translate(center.x() + dx, center.y() + dy);
        painter.scale(scale, scale);
        painter.translate(-center.x(), -center.y());
        if (transition->type == LayerTransitionType::TextWipe)
            painter.setClipRect(visible);
        const int current_blur_radius = blur_transition
            ? animated_text_blur_radius(transition->blur_amount, local)
            : 0;
        const IsolatedTextUnitCache::BlurredVariant *blurred_variant =
            blurred_variant_for(index, current_blur_radius);

        // Draw one representation of the glyph: blurred while radius > 0,
        // sharp only when the radius reaches zero. Drawing both simultaneously
        // produces a sharp core with a halo, i.e. a glow rather than blur.
        painter.setOpacity(local);
        if (blurred_variant && !blurred_variant->crop.isNull()) {
            painter.drawImage(QPointF(blurred_variant->bounds.topLeft()) + placement_offset,
                              blurred_variant->crop);
        } else {
            painter.drawImage(source_origin, unit_crop);
        }
        painter.restore();
    }

    painter.end();
    cache.scratch_variant = {};
    return true;
}

static void apply_text_layer_transition(QImage &image,
                                        const Layer &layer,
                                        double title_time,
                                        const QString &text,
                                        const QFont &font,
                                        const QRectF &text_rect)
{
    const LayerTransition *transition = active_text_layer_transition(layer, title_time);
    if (!transition || image.isNull())
        return;

    QVector<QRectF> raw_units = text_transition_unit_rects(
        text, font, text_rect, layer, transition->unit);
    // The precise isolated path is bounded to 512 units. When it declines a
    // pathological text block, progressively coarsen the fallback grouping
    // rather than issuing thousands of full-surface draw calls in one frame.
    constexpr int kMaxFlattenedTransitionUnits = 512;
    if (raw_units.size() > kMaxFlattenedTransitionUnits) {
        raw_units = text_transition_unit_rects(
            text, font, text_rect, layer, LayerTransitionUnit::Word);
        if (raw_units.size() > kMaxFlattenedTransitionUnits)
            raw_units = text_transition_unit_rects(
                text, font, text_rect, layer, LayerTransitionUnit::Sentence);
        if (raw_units.size() > kMaxFlattenedTransitionUnits)
            raw_units = {text_rect};
    }
    if (raw_units.isEmpty())
        return;

    const double global_progress = layer_transition_progress(
        *transition, layer.in_time, layer.out_time, title_time);
    const bool blur_transition =
        transition->type == LayerTransitionType::TextBlur ||
        transition->type == LayerTransitionType::TextBlurSlide;
    const QImage source = image.copy();
    QVector<int> cached_blur_radii;
    QVector<QImage> cached_blur_images;
    const qsizetype source_bytes = source.sizeInBytes();
    constexpr qsizetype kMaxFlattenedBlurWorkingSet = 64 * 1024 * 1024;
    const bool use_single_flattened_blur =
        blur_transition && source_bytes > kMaxFlattenedBlurWorkingSet / 4;
    const int single_flattened_blur_radius = use_single_flattened_blur
        ? animated_text_blur_radius(transition->blur_amount, global_progress) : 0;
    QImage single_flattened_blur;
    if (use_single_flattened_blur && source_bytes <= kMaxFlattenedBlurWorkingSet)
        single_flattened_blur = blurred_transition_image(source, single_flattened_blur_radius);

    auto image_for_blur_radius = [&source, &cached_blur_radii, &cached_blur_images,
                                  &single_flattened_blur, use_single_flattened_blur](int radius)
        -> const QImage & {
        if (use_single_flattened_blur)
            return single_flattened_blur.isNull() ? source : single_flattened_blur;
        for (int i = 0; i < cached_blur_radii.size(); ++i) {
            if (cached_blur_radii.at(i) == radius)
                return cached_blur_images.at(i);
        }
        // The precise fallback is used only for modest surfaces. Still cap the
        // number of retained full-size blur levels to avoid a 12x image cache.
        constexpr int kMaxFlattenedBlurLevels = 3;
        if (cached_blur_images.size() >= kMaxFlattenedBlurLevels) {
            cached_blur_radii.removeFirst();
            cached_blur_images.removeFirst();
        }
        cached_blur_radii.push_back(radius);
        cached_blur_images.push_back(blurred_transition_image(source, radius));
        return cached_blur_images.back();
    };
    const QRectF image_bounds(QPointF(0.0, 0.0), QSizeF(image.size()));
    // Keep a little glyph-bearing/antialiasing slack around each logical unit.
    // The clip itself is moved below together with slide transitions; keeping it
    // at the final unit position is what used to crop horizontally moving glyphs.
    const qreal margin = 1.5;

    QVector<QRectF> units;
    units.reserve(raw_units.size());
    for (const QRectF &unit : raw_units) {
        const QRectF expanded = unit.adjusted(-margin, -margin, margin, margin).intersected(image_bounds);
        if (!expanded.isEmpty())
            units.push_back(expanded);
    }
    if (units.isEmpty())
        return;

    QPainter clear_painter(&image);
    clear_painter.setCompositionMode(QPainter::CompositionMode_Clear);
    for (const QRectF &unit : units)
        clear_painter.fillRect(unit, Qt::transparent);
    clear_painter.end();

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const int count = static_cast<int>(units.size());
    for (int index = 0; index < count; ++index) {
        const int order = transition->reverse_order ? count - 1 - index : index;
        const double delay = count <= 1 ? 0.0
            : transition->stagger * static_cast<double>(order) / static_cast<double>(count - 1);
        const double span = std::max(0.05, 1.0 - transition->stagger);
        double local = 1.0;
        if (transition->edge == LayerTransitionEdge::In)
            local = std::clamp((global_progress - delay) / span, 0.0, 1.0);
        else {
            const double out_phase = 1.0 - global_progress;
            local = 1.0 - std::clamp((out_phase - delay) / span, 0.0, 1.0);
        }
        local = layer_transition_ease(local, transition->easing);
        if (local <= 0.0001)
            continue;

        const QRectF unit = units[index];
        double dx = 0.0;
        double dy = 0.0;
        if (transition->type == LayerTransitionType::TextSlide ||
            transition->type == LayerTransitionType::TextBlurSlide) {
            const double hidden = 1.0 - local;
            switch (transition->direction) {
            case LayerTransitionDirection::Right: dx = transition->offset * hidden; break;
            case LayerTransitionDirection::Up: dy = -transition->offset * hidden; break;
            case LayerTransitionDirection::Down: dy = transition->offset * hidden; break;
            case LayerTransitionDirection::Left:
            case LayerTransitionDirection::None:
            default: dx = -transition->offset * hidden; break;
            }
        }
        const double scale = transition->type == LayerTransitionType::TextScale
            ? transition->scale_from + (1.0 - transition->scale_from) * local : 1.0;

        QRectF visible = unit;
        if (transition->type == LayerTransitionType::TextWipe) {
            switch (transition->direction) {
            case LayerTransitionDirection::Right:
                visible.setLeft(unit.right() - unit.width() * local);
                break;
            case LayerTransitionDirection::Up:
                visible.setTop(unit.bottom() - unit.height() * local);
                break;
            case LayerTransitionDirection::Down:
                visible.setHeight(unit.height() * local);
                break;
            case LayerTransitionDirection::Left:
            case LayerTransitionDirection::None:
            default:
                visible.setWidth(unit.width() * local);
                break;
            }
        }

        painter.save();
        const QPointF center = unit.center();
        painter.translate(center.x() + dx, center.y() + dy);
        painter.scale(scale, scale);
        painter.translate(-center.x(), -center.y());
        // Establish the unit clip after the transition transform.  QPainter
        // resolves a clip using the world transform active at setClipRect(), so
        // this makes the clip travel/scale with the glyph instead of remaining
        // at its final position and slicing horizontal slide-ins.
        painter.setClipRect(visible.intersected(image_bounds));
        const int current_blur_radius = blur_transition
            ? animated_text_blur_radius(transition->blur_amount, local)
            : 0;
        painter.setOpacity(local);
        if (current_blur_radius > 0) {
            painter.drawImage(QPointF(0.0, 0.0),
                              image_for_blur_radius(current_blur_radius));
        } else {
            painter.drawImage(QPointF(0.0, 0.0), source);
        }
        painter.restore();
    }
    painter.end();
}

static void render_layer_text(cairo_t *cr, const Title &title, const Layer &layer, double title_time,
                               int canvas_w, int canvas_h)
{
    const double t = std::max(0.0, title_time - layer.in_time);
    const LayerTransition *active_text_transition =
        active_text_layer_transition(layer, title_time);
    const bool use_isolated_text_transition =
        active_text_transition && layer.type != LayerType::Ticker;
    double alpha = layer_chain_opacity(title, layer, title_time);
    double box_w = eval_box_width(layer, t);
    double box_h = eval_box_height(layer, t);
    if (box_w <= 0.0 || box_h <= 0.0) return;

    ShadowRenderParams shadow_params = evaluated_shadow_params(layer, t);
    const bool render_shadow = shadow_params.drop_enabled || shadow_params.long_enabled;
    int base_pad = render_shadow
        ? (int)std::ceil(std::max({std::abs(shadow_params.dx), std::abs(shadow_params.dy), shadow_params.long_length}) + shadow_params.blur * 3.0 + shadow_params.spread + 4.0)
        : 0;
    // Text outlines extend beyond the glyph and text box. Keep enough offscreen
    // padding so rich-text and plain-text strokes are not cropped at the layer edges.
    base_pad = std::max(base_pad, (int)std::ceil(
        std::max(std::max(0.0, eval_outline_width(layer, t)),
                 max_rich_text_stroke_width(layer, t)) + 2.0));
    if (eval_background_enabled(layer, t))
        base_pad += (int)std::ceil(std::max({std::abs(eval_background_padding_left(layer, t)),
                                             std::abs(eval_background_padding_right(layer, t)),
                                             std::abs(eval_background_padding_top(layer, t)),
                                             std::abs(eval_background_padding_bottom(layer, t)),
                                             eval_background_stroke_width(layer, t)}));

    int pad_left = base_pad;
    int pad_right = base_pad;
    int pad_top = base_pad;
    int pad_bottom = base_pad;
    if (active_text_transition) {
        // Blur is clamped to 64 px by the renderer; reserve only the pixels that
        // can actually be generated, rather than the unchecked preset value.
        const bool blur_transition =
            active_text_transition->type == LayerTransitionType::TextBlur ||
            active_text_transition->type == LayerTransitionType::TextBlurSlide;
        const int blur_pad = blur_transition
            ? std::clamp((int)std::ceil(std::max(0.0, active_text_transition->blur_amount)), 0, 64) * 3 + 3
            : 0;
        pad_left += blur_pad;
        pad_right += blur_pad;
        pad_top += blur_pad;
        pad_bottom += blur_pad;

        if (active_text_transition->type == LayerTransitionType::TextSlide ||
            active_text_transition->type == LayerTransitionType::TextBlurSlide) {
            // Travel beyond the canvas plus the layer size is guaranteed to be
            // invisible. Capping at that visible envelope prevents a 10,000 px
            // offset from creating a multi-gigabyte temporary QImage.
            const bool horizontal = active_text_transition->direction != LayerTransitionDirection::Up &&
                                    active_text_transition->direction != LayerTransitionDirection::Down;
            const double visible_limit = horizontal
                ? std::max(1.0, (double)canvas_w) + box_w
                : std::max(1.0, (double)canvas_h) + box_h;
            const int travel = (int)std::ceil(std::min(
                std::abs(active_text_transition->offset), visible_limit));
            switch (active_text_transition->direction) {
            case LayerTransitionDirection::Right: pad_right += travel; break;
            case LayerTransitionDirection::Up: pad_top += travel; break;
            case LayerTransitionDirection::Down: pad_bottom += travel; break;
            case LayerTransitionDirection::Left:
            case LayerTransitionDirection::None:
            default: pad_left += travel; break;
            }
        }

        if (active_text_transition->type == LayerTransitionType::TextScale) {
            const double maximum_scale = std::max(1.0, std::abs(active_text_transition->scale_from));
            const int extra_x = (int)std::ceil((maximum_scale - 1.0) * box_w * 0.5);
            const int extra_y = (int)std::ceil((maximum_scale - 1.0) * box_h * 0.5);
            const int visible_x = std::max(1, canvas_w) + (int)std::ceil(box_w);
            const int visible_y = std::max(1, canvas_h) + (int)std::ceil(box_h);
            pad_left += std::min(extra_x, visible_x);
            pad_right += std::min(extra_x, visible_x);
            pad_top += std::min(extra_y, visible_y);
            pad_bottom += std::min(extra_y, visible_y);
        }
    }

    const qint64 requested_width = std::max<qint64>(1, (qint64)std::ceil(box_w) + pad_left + pad_right);
    const qint64 requested_height = std::max<qint64>(1, (qint64)std::ceil(box_h) + pad_top + pad_bottom);
    if (requested_width > std::numeric_limits<int>::max() ||
        requested_height > std::numeric_limits<int>::max())
        return;
    int img_w = static_cast<int>(requested_width);
    int img_h = static_cast<int>(requested_height);
    QImage text_image(img_w, img_h, QImage::Format_ARGB32_Premultiplied);
    text_image.fill(Qt::transparent);

    QPainter painter(&text_image);
    const bool previous_shape_aa = painter.testRenderHint(QPainter::Antialiasing);
    const bool previous_text_aa = painter.testRenderHint(QPainter::TextAntialiasing);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font = font_for_layer(layer, t);
    painter.setFont(font);

    QRectF base_rect(pad_left, pad_top, box_w, box_h);
    if (eval_background_enabled(layer, t)) {
        const auto *background_effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
        const bool background_gradient = background_effect && background_effect->effect_fill_type == 1;
        const double bg_left = eval_background_padding_left(layer, t);
        const double bg_right = eval_background_padding_right(layer, t);
        const double bg_top = eval_background_padding_top(layer, t);
        const double bg_bottom = eval_background_padding_bottom(layer, t);
        QRectF bg_rect = base_rect.adjusted(-bg_left, -bg_top, bg_right, bg_bottom);
        if (bg_rect.width() <= 0.0 || bg_rect.height() <= 0.0) bg_rect = base_rect;
        QColor bg = evaluated_background_color(layer, t);
        if (bg.alpha() > 0 || background_gradient || eval_background_stroke_width(layer, t) > 0.0) {
            QPainterPath bg_path = painter_rounded_rect_corners(bg_rect,
                eval_background_corner_radius_tl(layer, t), eval_background_corner_radius_tr(layer, t),
                eval_background_corner_radius_br(layer, t), eval_background_corner_radius_bl(layer, t),
                legacy_corner_type_roundness((CornerType)(background_effect ? background_effect->effect_corner_type : 0)));
            painter.setPen(Qt::NoPen);
            painter.setBrush(background_gradient ? background_gradient_fill_brush(layer, bg_rect, eval_background_opacity(layer, t)) : QBrush(bg));
            if (bg.alpha() > 0 || background_gradient)
                painter.fillPath(bg_path, painter.brush());
            const double stroke_w = eval_background_stroke_width(layer, t);
            if (stroke_w > 0.0) {
                QColor stroke = color_from_argb(eval_background_stroke_color(layer, t));
                stroke.setAlphaF(std::clamp(stroke.alphaF() * eval_background_stroke_opacity(layer, t) * alpha, 0.0, 1.0));
                if (stroke.alpha() > 0) {
                    QPen pen(stroke, stroke_w);
                    pen.setJoinStyle(Qt::MiterJoin);
                    painter.setBrush(Qt::NoBrush);
                    painter.setPen(pen);
                    painter.drawPath(bg_path);
                }
            }
        }
    }

    // The isolated per-unit compositor needs a copy of the non-text content,
    // but a full QImage::copy() on every rendered text layer is prohibitively
    // expensive during cue changes and prerendering. Keep the normal path
    // allocation-free and take the snapshot only while a rich-text transition
    // is actually active.
    QImage static_text_background;
    // Without a text-box background the compositor can clear and reuse the
    // existing surface, avoiding another full layer-sized allocation per
    // transition frame. Preserve a snapshot only when there is actual static
    // content behind the animated glyph units.
    if (use_isolated_text_transition && eval_background_enabled(layer, t))
        static_text_background = text_image.copy();

    QRectF text_rect = text_rect_for_style(base_rect, layer);
    QString text = display_text_for_style(layer);
    painter.save();
    const double text_outline_clip_pad =
        std::max(std::max(0.0, eval_outline_width(layer, t)),
                 max_rich_text_stroke_width(layer, t)) + 2.0;
    painter.setClipRect(text_rect.adjusted(-text_outline_clip_pad, -text_outline_clip_pad,
                                           text_outline_clip_pad, text_outline_clip_pad));
    Qt::Alignment align = Qt::AlignVCenter | Qt::AlignHCenter;
    if (layer.align_h == 0) align = (align & ~Qt::AlignHorizontal_Mask) | Qt::AlignLeft;
    if (layer.align_h == 2) align = (align & ~Qt::AlignHorizontal_Mask) | Qt::AlignRight;
    if (layer.align_v == 0 || layer.align_v == 3) align = (align & ~Qt::AlignVertical_Mask) | Qt::AlignTop;
    if (layer.align_v == 2) align = (align & ~Qt::AlignVertical_Mask) | Qt::AlignBottom;
    const bool has_rich_text = layer.type != LayerType::Ticker;
    QPainterPath text_path;
    if (!has_rich_text) {
        text_path = layer.type == LayerType::Ticker
            ? ticker_text_path(font, text_rect, align, text, layer, title.id,
                               title.current_cue_row >= 0 || title.pending_cue_row >= 0)
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
    outline.setAlphaF(std::clamp((double)outline.alphaF() *
                                 eval_outline_opacity(layer, t),
                                 0.0, 1.0));
    QColor fill = color_from_argb(eval_text_color(layer, t));
    fill.setAlphaF(std::clamp((double)fill.alphaF(), 0.0, 1.0));

    std::unique_ptr<QTextDocument> rich_document;
    RichTextDocument rich_stroke_model;
    std::vector<RichTextStrokeRenderGroup> rich_stroke_groups;
    if (has_rich_text) {
        rich_document = rich_text_document_for_layer(
            layer, font, text_rect, t, nullptr);
        rich_stroke_model = rich_text_model_for_source_time(layer, t);
        rich_stroke_groups =
            rich_text_stroke_render_groups(rich_stroke_model);
    }

    auto draw_text_fill = [&]() {
        if (has_rich_text && rich_document) {
            std::unique_ptr<QTextDocument> fill_document =
                rich_text_fill_only_document(*rich_document);
            painter.save();
            painter.setOpacity(fill.alphaF());
            draw_positioned_rich_text_document(
                painter, *fill_document, layer, text_rect, t);
            painter.restore();
            return;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(layer.fill_type == 1
                             ? gradient_fill_brush(layer, text_rect)
                             : QBrush(fill));
        painter.drawPath(text_path);
    };

    auto draw_rich_stroke_phase = [&](bool on_front) {
        if (!has_rich_text || !rich_document)
            return;
        for (const RichTextStrokeRenderGroup &group : rich_stroke_groups) {
            if (group.on_front != on_front)
                continue;
            const QImage stroke_layer = render_rich_text_stroke_group(
                *rich_document, rich_stroke_model, layer, text_rect, t,
                QSize(img_w, img_h), QPointF(0.0, 0.0), group);
            if (!stroke_layer.isNull())
                painter.drawImage(QPointF(0.0, 0.0), stroke_layer);
        }
    };

    auto draw_plain_text_outline = [&]() {
        if (outline_width <= 0.0) return;
        if (layer.stroke_fill_type == 1 && outline.alpha() <= 0) return;

        const int alignment = eval_outline_alignment(layer, t);
        const qreal painted_width = alignment == 1
            ? outline_width : outline_width * 2.0;
        const QBrush stroke_brush = layer.stroke_fill_type == 2
            ? stroke_gradient_fill_brush(
                  layer, text_rect, eval_outline_opacity(layer, t))
            : QBrush(outline);
        QPen outline_pen(stroke_brush, painted_width, Qt::SolidLine,
                         Qt::RoundCap, outline_pen_join_style(layer));

        QImage stroke_layer(img_w, img_h,
                            QImage::Format_ARGB32_Premultiplied);
        stroke_layer.fill(Qt::transparent);
        QPainter stroke_painter(&stroke_layer);
        stroke_painter.setRenderHint(
            QPainter::Antialiasing, eval_outline_antialias(layer, t));
        stroke_painter.setRenderHint(QPainter::TextAntialiasing, true);
        stroke_painter.setClipRect(text_rect.adjusted(
            -text_outline_clip_pad, -text_outline_clip_pad,
            text_outline_clip_pad, text_outline_clip_pad));
        stroke_painter.setPen(outline_pen);
        stroke_painter.setBrush(Qt::NoBrush);
        stroke_painter.drawPath(text_path);
        stroke_painter.end();

        if (alignment != 1) {
            QImage glyph_mask(img_w, img_h,
                              QImage::Format_ARGB32_Premultiplied);
            glyph_mask.fill(Qt::transparent);
            QPainter mask_painter(&glyph_mask);
            mask_painter.setRenderHint(QPainter::Antialiasing, true);
            mask_painter.setRenderHint(QPainter::TextAntialiasing, true);
            mask_painter.setPen(Qt::NoPen);
            mask_painter.setBrush(Qt::white);
            mask_painter.drawPath(text_path);
            mask_painter.end();

            QPainter isolate(&stroke_layer);
            isolate.setCompositionMode(
                alignment == 0
                    ? QPainter::CompositionMode_DestinationOut
                    : QPainter::CompositionMode_DestinationIn);
            isolate.drawImage(QPointF(0.0, 0.0), glyph_mask);
            isolate.end();
        }
        painter.drawImage(QPointF(0.0, 0.0), stroke_layer);
    };

    if (has_rich_text) {
        draw_rich_stroke_phase(false);
        draw_text_fill();
        draw_rich_stroke_phase(true);
    } else {
        if (!eval_outline_on_front(layer, t))
            draw_plain_text_outline();
        draw_text_fill();
        if (eval_outline_on_front(layer, t))
            draw_plain_text_outline();
    }
    painter.setRenderHint(QPainter::TextAntialiasing, previous_text_aa);
    painter.setRenderHint(QPainter::Antialiasing, previous_shape_aa);
    painter.restore();
    painter.end();

    if (use_isolated_text_transition) {
        if (!apply_isolated_text_layer_transition(text_image, static_text_background,
                                                   layer, title_time, text, font, text_rect,
                                                   t, shadow_params, render_shadow)) {
            apply_text_layer_transition(text_image, layer, title_time, text, font, text_rect);
        }
    } else if (active_text_transition) {
        // Ticker text still uses the existing flattened transition path.
        apply_text_layer_transition(text_image, layer, title_time, text, font, text_rect);
    }

    auto text_surface = make_image_surface_for_qimage(text_image);
    if (!text_surface) return;

    cairo_save(cr);
    apply_layer_world_transform(cr, title, layer, title_time);
    const double local_x = -eval_origin_x(layer, t) * box_w - pad_left;
    const double local_y = -eval_origin_y(layer, t) * box_h - pad_top;
    apply_layer_transition_clip(cr, layer, title_time, local_x, local_y, img_w, img_h);
    cairo_set_source_surface(cr, text_surface.get(), local_x, local_y);
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
}

static cairo_pattern_t *set_layer_outline_source(cairo_t *cr, const Layer &layer,
                                                 double x, double y, double w, double h,
                                                 double alpha, double t);

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
    apply_layer_transition_clip(cr, layer, title_time, x, y, w, h);
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
        const QString shape_key = QStringLiteral("rect|%1|%2x%3|%4|%5|%6|%7|%8|%9|%10|%11|%12|%13|%14")
            .arg((int)layer.shape_type).arg(mw).arg(mh)
            .arg(layer.corner_radius_tl, 0, 'f', 2)
            .arg(layer.corner_radius_tr, 0, 'f', 2)
            .arg(layer.corner_radius_br, 0, 'f', 2)
            .arg(layer.corner_radius_bl, 0, 'f', 2)
            .arg(layer.corner_bevel_roundness, 0, 'f', 2)
            .arg(layer.shape_points).arg(layer.shape_sides)
            .arg(layer.shape_inner_radius, 0, 'f', 4)
            .arg(layer.shape_outer_radius, 0, 'f', 4)
            .arg(layer.shape_roundness, 0, 'f', 2)
            .arg(layer.shape_inner_roundness, 0, 'f', 2) + QStringLiteral("|") + bgs::path_geometry_signature(layer);
        CachedShadowImage shadow = build_shadow_image(layer, shadow_params, shape_key, mask);
        paint_qimage(cr, shadow.image, shadow.origin.x(), shadow.origin.y(), alpha);
    }

    double outline_width = eval_outline_width(layer, t);
    uint32_t outline_color = eval_outline_color(layer, t);
    bool has_outline = outline_width > 0.0 &&
                       (layer.stroke_fill_type == 2 || ((outline_color >> 24) & 0xFF) > 0);

    auto set_stroke_source = [&]() -> cairo_pattern_t * {
        return set_layer_outline_source(cr, layer, 0.0, 0.0, w, h, alpha, t);
    };

    auto stroke_outline = [&]() {
        const int alignment = eval_outline_alignment(layer, t);
        cairo_save(cr);
        cairo_set_antialias(cr, outline_cairo_antialias(layer));
        cairo_set_line_join(cr, outline_cairo_join_style(layer));

        if (alignment == 0) {
            // Outer stroke: render a double-width centered stroke into a group,
            // then remove the object's interior from the group.
            cairo_push_group(cr);
            cairo_add_layer_shape(cr, layer, w, h);
            cairo_set_line_width(cr, outline_width * 2.0);
            cairo_pattern_t *pattern = set_stroke_source();
            cairo_stroke(cr);
            if (pattern) cairo_pattern_destroy(pattern);
            cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OUT);
            cairo_add_layer_shape(cr, layer, w, h);
            cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
            cairo_fill(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            cairo_pop_group_to_source(cr);
            cairo_paint(cr);
        } else if (alignment == 2) {
            // Inner stroke: clip to the object's interior and use a double-width
            // centered stroke so the visible half has the requested width.
            cairo_add_layer_shape(cr, layer, w, h);
            cairo_clip(cr);
            cairo_add_layer_shape(cr, layer, w, h);
            cairo_set_line_width(cr, outline_width * 2.0);
            cairo_pattern_t *pattern = set_stroke_source();
            cairo_stroke(cr);
            if (pattern) cairo_pattern_destroy(pattern);
        } else {
            // Mid stroke: Cairo's native centered stroke.
            cairo_add_layer_shape(cr, layer, w, h);
            cairo_set_line_width(cr, outline_width);
            cairo_pattern_t *pattern = set_stroke_source();
            cairo_stroke(cr);
            if (pattern) cairo_pattern_destroy(pattern);
        }
        cairo_restore(cr);
    };

    if (has_outline && !eval_outline_on_front(layer, t))
        stroke_outline();

    cairo_add_layer_shape(cr, layer, w, h);
    cairo_pattern_t *gradient_pattern = nullptr;
    if (layer.fill_type == 1) {
        gradient_pattern = create_fill_gradient_pattern(layer, 0.0, 0.0, w, h, alpha);
        cairo_set_source(cr, gradient_pattern);
    } else {
        cairo_set_source_rgba(cr, fr, fg, fb, fa * alpha);
    }
    cairo_fill(cr);
    if (gradient_pattern) cairo_pattern_destroy(gradient_pattern);

    if (has_outline && eval_outline_on_front(layer, t))
        stroke_outline();
    cairo_restore(cr);
}

struct ImageBoxLayout {
    double x = 0.0;
    double y = 0.0;
    double w = 0.0;
    double h = 0.0;
};

static ImageBoxLayout image_box_layout_for_layer(const Layer &layer, double box_w, double box_h,
                                                  double image_w, double image_h)
{
    ImageBoxLayout out;
    const bgs::ImageDisplaySize display = bgs::calculate_image_display_size(
        layer.image_box_mode, layer.image_size_auto_fit, box_w, box_h, image_w, image_h);
    out.w = display.width;
    out.h = display.height;
    if (out.w <= 0.0 || out.h <= 0.0)
        return out;
    const double ax = std::clamp((double)layer.image_anchor_x, 0.0, 1.0);
    const double ay = std::clamp((double)layer.image_anchor_y, 0.0, 1.0);
    out.x = (box_w - out.w) * ax;
    out.y = (box_h - out.h) * ay;
    return out;
}

static QRectF image_visible_rect_for_layout(const ImageBoxLayout &layout, double box_w, double box_h,
                                            bool crop_to_box)
{
    QRectF image_rect(layout.x, layout.y, layout.w, layout.h);
    if (!crop_to_box)
        return image_rect;
    return image_rect.intersected(QRectF(0.0, 0.0, box_w, box_h));
}

static QImage image_box_shadow_mask(const QImage &argb, const Layer &layer, double box_w, double box_h,
                                    const ImageBoxLayout &layout)
{
    const int mask_w = std::max(1, (int)std::ceil(box_w));
    const int mask_h = std::max(1, (int)std::ceil(box_h));
    QImage mask(mask_w, mask_h, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    QPainter painter(&mask);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, layer.scale_filter != ImageScaleFilter::Disable);
    const QRectF visible = image_visible_rect_for_layout(layout, box_w, box_h, layer.image_crop_when_outside_box);
    if (visible.isEmpty())
        return mask;
    painter.setClipPath(painter_rounded_rect_corners(visible, layer.corner_radius_tl, layer.corner_radius_tr,
                                                     layer.corner_radius_br, layer.corner_radius_bl,
                                                     layer.corner_bevel_roundness));
    painter.drawImage(QRectF(layout.x, layout.y, layout.w, layout.h), argb);
    return mask;
}

static bool layer_has_visible_outline(const Layer &layer, double t)
{
    const double outline_width = eval_outline_width(layer, t);
    const uint32_t outline_color = eval_outline_color(layer, t);
    return outline_width > 0.0 &&
           (layer.stroke_fill_type == 2 || ((outline_color >> 24) & 0xFF) > 0);
}

static cairo_pattern_t *set_layer_outline_source(cairo_t *cr, const Layer &layer,
                                                 double x, double y, double w, double h,
                                                 double alpha, double t)
{
    if (layer.stroke_fill_type == 2) {
        cairo_pattern_t *pattern = create_stroke_gradient_pattern(
            layer, x, y, w, h, alpha * eval_outline_opacity(layer, t));
        cairo_set_source(cr, pattern);
        return pattern;
    }

    double sr, sg, sb, sa;
    unpack_color(eval_outline_color(layer, t), sr, sg, sb, sa);
    cairo_set_source_rgba(cr, sr, sg, sb, sa * alpha * eval_outline_opacity(layer, t));
    return nullptr;
}

static void stroke_image_visible_outline(cairo_t *cr, const Layer &layer, const QRectF &visible,
                                         double box_x, double box_y, double alpha, double t)
{
    const double outline_width = eval_outline_width(layer, t);
    if (outline_width <= 0.0 || visible.isEmpty())
        return;

    auto add_path = [&]() {
        cairo_add_rounded_rect_corners(cr, box_x + visible.x(), box_y + visible.y(),
                                       visible.width(), visible.height(),
                                       layer.corner_radius_tl, layer.corner_radius_tr,
                                       layer.corner_radius_br, layer.corner_radius_bl,
                                       layer.corner_bevel_roundness);
    };

    const int alignment = eval_outline_alignment(layer, t);
    cairo_save(cr);
    cairo_set_antialias(cr, outline_cairo_antialias(layer));
    cairo_set_line_join(cr, outline_cairo_join_style(layer));

    if (alignment == 0) {
        cairo_push_group(cr);
        add_path();
        cairo_set_line_width(cr, outline_width * 2.0);
        cairo_pattern_t *pattern = set_layer_outline_source(cr, layer, visible.x(), visible.y(),
                                                            visible.width(), visible.height(), alpha, t);
        cairo_stroke(cr);
        if (pattern) cairo_pattern_destroy(pattern);
        cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OUT);
        add_path();
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_fill(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_pop_group_to_source(cr);
        cairo_paint(cr);
    } else if (alignment == 2) {
        add_path();
        cairo_clip(cr);
        add_path();
        cairo_set_line_width(cr, outline_width * 2.0);
        cairo_pattern_t *pattern = set_layer_outline_source(cr, layer, visible.x(), visible.y(),
                                                            visible.width(), visible.height(), alpha, t);
        cairo_stroke(cr);
        if (pattern) cairo_pattern_destroy(pattern);
    } else {
        add_path();
        cairo_set_line_width(cr, outline_width);
        cairo_pattern_t *pattern = set_layer_outline_source(cr, layer, visible.x(), visible.y(),
                                                            visible.width(), visible.height(), alpha, t);
        cairo_stroke(cr);
        if (pattern) cairo_pattern_destroy(pattern);
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

    const QString image_path = QString::fromStdString(layer.image_path);
    double image_w = eval_image_width(layer, t);
    double image_h = eval_image_height(layer, t);
    if (image_w <= 0.0 || image_h <= 0.0) {
        QSize intrinsic_size = image_intrinsic_size(image_path);
        if (!intrinsic_size.isValid() || intrinsic_size.isEmpty())
            intrinsic_size = QSize(std::max(1, (int)std::ceil(w)), std::max(1, (int)std::ceil(h)));
        image_w = intrinsic_size.width();
        image_h = intrinsic_size.height();
    }
    ImageBoxLayout image_layout = image_box_layout_for_layer(layer, w, h,
                                                             image_w, image_h);
    const int max_sample_dim = std::clamp(std::max(title.width, title.height) * 2, 512, 4096);
    const QSize sample_size(std::clamp((int)std::ceil(image_layout.w), 1, max_sample_dim),
                            std::clamp((int)std::ceil(image_layout.h), 1, max_sample_dim));
    QImage argb = load_cached_layer_image(image_path, sample_size);
    if (argb.isNull() || argb.width() <= 0 || argb.height() <= 0) return;

    auto img_surface = make_image_surface_for_const_qimage(argb);
    if (!img_surface) return;

    cairo_save(cr);
    apply_layer_world_transform(cr, title, layer, title_time);
    const double origin_x = eval_origin_x(layer, t);
    const double origin_y = eval_origin_y(layer, t);
    const double box_x = -origin_x * w;
    const double box_y = -origin_y * h;
    apply_layer_transition_clip(cr, layer, title_time, box_x, box_y, w, h);
    ShadowRenderParams shadow_params = evaluated_shadow_params(layer, t);
    if (shadow_params.drop_enabled || shadow_params.long_enabled) {
        QImage mask = image_box_shadow_mask(argb, layer, w, h, image_layout);
        QFileInfo shadow_image_info(QString::fromStdString(layer.image_path));
        const QString shape_key = QStringLiteral("image|%1|%2x%3|%4|%5")
            .arg(QString::fromStdString(layer.image_path)).arg(mask.width()).arg(mask.height())
            .arg(shadow_image_info.exists() ? shadow_image_info.lastModified().toMSecsSinceEpoch() : 0)
            .arg(shadow_image_info.exists() ? shadow_image_info.size() : -1);
        CachedShadowImage shadow = build_shadow_image(layer, shadow_params, shape_key, mask);
        paint_qimage(cr, shadow.image, box_x + shadow.origin.x(), box_y + shadow.origin.y(), alpha);
    }
    if (eval_background_enabled(layer, t)) {
        const auto *background_effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
        const bool background_gradient = background_effect && background_effect->effect_fill_type == 1;
        const double bg_left = eval_background_padding_left(layer, t);
        const double bg_right = eval_background_padding_right(layer, t);
        const double bg_top = eval_background_padding_top(layer, t);
        const double bg_bottom = eval_background_padding_bottom(layer, t);
        QColor bg = evaluated_background_color(layer, t);
        double br, bgc, bb, ba;
        br = bg.redF(); bgc = bg.greenF(); bb = bg.blueF(); ba = bg.alphaF();
        const double stroke_w = eval_background_stroke_width(layer, t);
        if (ba > 0.0 || background_gradient || stroke_w > 0.0) {
            const double x = box_x - bg_left;
            const double y = box_y - bg_top;
            const double bw = std::max(1.0, w + bg_left + bg_right);
            const double bh = std::max(1.0, h + bg_top + bg_bottom);
            cairo_add_rounded_rect_corners(cr, x, y, bw, bh,
                eval_background_corner_radius_tl(layer, t), eval_background_corner_radius_tr(layer, t),
                eval_background_corner_radius_br(layer, t), eval_background_corner_radius_bl(layer, t),
                legacy_corner_type_roundness((CornerType)(background_effect ? background_effect->effect_corner_type : 0)));
            cairo_pattern_t *gradient_pattern = nullptr;
            if (ba > 0.0 || background_gradient) {
                if (background_gradient) {
                    gradient_pattern = create_background_gradient_pattern(layer, x, y, bw, bh, alpha * eval_background_opacity(layer, t));
                    cairo_set_source(cr, gradient_pattern);
                } else {
                    cairo_set_source_rgba(cr, br, bgc, bb, ba * alpha);
                }
                cairo_fill_preserve(cr);
                if (gradient_pattern) cairo_pattern_destroy(gradient_pattern);
            }
            if (stroke_w > 0.0) {
                QColor stroke = color_from_argb(eval_background_stroke_color(layer, t));
                cairo_set_line_width(cr, stroke_w);
                cairo_set_source_rgba(cr, stroke.redF(), stroke.greenF(), stroke.blueF(), stroke.alphaF() * alpha * eval_background_stroke_opacity(layer, t));
                cairo_stroke(cr);
            } else {
                cairo_new_path(cr);
            }
        }
    }
    const QRectF visible = image_visible_rect_for_layout(image_layout, w, h, layer.image_crop_when_outside_box);
    if (visible.isEmpty()) {
        cairo_restore(cr);
        return;
    }
    const bool has_outline = layer_has_visible_outline(layer, t);
    if (has_outline && !eval_outline_on_front(layer, t))
        stroke_image_visible_outline(cr, layer, visible, box_x, box_y, alpha, t);
    cairo_save(cr);
    cairo_add_rounded_rect_corners(cr, box_x + visible.x(), box_y + visible.y(),
                                   visible.width(), visible.height(),
                                   layer.corner_radius_tl, layer.corner_radius_tr,
                                   layer.corner_radius_br, layer.corner_radius_bl,
                                   layer.corner_bevel_roundness);
    cairo_clip(cr);
    cairo_translate(cr, box_x + image_layout.x, box_y + image_layout.y);
    cairo_scale(cr, image_layout.w / argb.width(), image_layout.h / argb.height());
    cairo_set_source_surface(cr, img_surface.get(),
                             0.0,
                             0.0);
    cairo_pattern_set_filter(cairo_get_source(cr),
                             cairo_filter_for_image_scale_filter(layer.scale_filter));
    cairo_paint_with_alpha(cr, alpha);
    cairo_restore(cr);
    if (has_outline && eval_outline_on_front(layer, t))
        stroke_image_visible_outline(cr, layer, visible, box_x, box_y, alpha, t);
    cairo_restore(cr);
}


static void render_layer_unmasked_raw(cairo_t *cr, const Title &title, const Layer &layer,
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
    case LayerType::ColorSolid:
        render_layer_rect(cr, title, layer, title_time);
        break;
    case LayerType::Image:
        render_layer_image(cr, title, layer, title_time);
        break;
    default:
        break;
    }
}

static void paint_soft_wipe_mask(cairo_t *cr, const Title &title, const Layer &layer,
                                 double title_time, int canvas_w, int canvas_h,
                                 const LayerTransitionVisualState &transition)
{
    const double t = std::max(0.0, title_time - layer.in_time);
    const double width = std::max(1.0, eval_box_width(layer, t));
    const double height = std::max(1.0, eval_box_height(layer, t));
    const double x = -eval_origin_x(layer, t) * width;
    const double y = -eval_origin_y(layer, t) * height;
    const double reveal = std::clamp(transition.wipe, 0.0, 1.0);
    if (reveal <= 0.000001)
        return;

    const double softness = std::clamp(transition.wipe_softness, 0.0, 1.0);
    const double extent = std::max({width, height, (double)std::max(1, canvas_w),
                                    (double)std::max(1, canvas_h)}) * 4.0 + 64.0;
    cairo_pattern_t *gradient = nullptr;
    switch (transition.wipe_direction) {
    case LayerTransitionDirection::Right: {
        const double boundary = x + width * (1.0 - reveal);
        const double feather = std::max(0.5, width * softness);
        gradient = cairo_pattern_create_linear(boundary, 0.0, boundary + feather, 0.0);
        cairo_pattern_add_color_stop_rgba(gradient, 0.0, 1.0, 1.0, 1.0, 0.0);
        cairo_pattern_add_color_stop_rgba(gradient, 1.0, 1.0, 1.0, 1.0, 1.0);
        break;
    }
    case LayerTransitionDirection::Up: {
        const double boundary = y + height * (1.0 - reveal);
        const double feather = std::max(0.5, height * softness);
        gradient = cairo_pattern_create_linear(0.0, boundary, 0.0, boundary + feather);
        cairo_pattern_add_color_stop_rgba(gradient, 0.0, 1.0, 1.0, 1.0, 0.0);
        cairo_pattern_add_color_stop_rgba(gradient, 1.0, 1.0, 1.0, 1.0, 1.0);
        break;
    }
    case LayerTransitionDirection::Down: {
        const double boundary = y + height * reveal;
        const double feather = std::max(0.5, height * softness);
        gradient = cairo_pattern_create_linear(0.0, boundary - feather, 0.0, boundary);
        cairo_pattern_add_color_stop_rgba(gradient, 0.0, 1.0, 1.0, 1.0, 1.0);
        cairo_pattern_add_color_stop_rgba(gradient, 1.0, 1.0, 1.0, 1.0, 0.0);
        break;
    }
    case LayerTransitionDirection::Left:
    case LayerTransitionDirection::None:
    default: {
        const double boundary = x + width * reveal;
        const double feather = std::max(0.5, width * softness);
        gradient = cairo_pattern_create_linear(boundary - feather, 0.0, boundary, 0.0);
        cairo_pattern_add_color_stop_rgba(gradient, 0.0, 1.0, 1.0, 1.0, 1.0);
        cairo_pattern_add_color_stop_rgba(gradient, 1.0, 1.0, 1.0, 1.0, 0.0);
        break;
    }
    }
    if (!gradient || cairo_pattern_status(gradient) != CAIRO_STATUS_SUCCESS) {
        if (gradient) cairo_pattern_destroy(gradient);
        return;
    }
    cairo_pattern_set_extend(gradient, CAIRO_EXTEND_PAD);
    cairo_save(cr);
    apply_layer_world_transform(cr, title, layer, title_time);
    cairo_rectangle(cr, x - extent, y - extent, width + extent * 2.0, height + extent * 2.0);
    cairo_set_source(cr, gradient);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_pattern_destroy(gradient);
}

static void render_layer_unmasked(cairo_t *cr, const Title &title, const Layer &layer,
                                  double title_time, int canvas_w, int canvas_h)
{
    const LayerTransitionVisualState transition = evaluate_layer_general_transitions(
        layer.transitions, layer.in_time, layer.out_time, title_time);
    const bool soft_wipe = transition.active && transition.wipe < 0.999999 &&
                           transition.wipe_softness > 0.000001;
    if (!soft_wipe) {
        render_layer_unmasked_raw(cr, title, layer, title_time, canvas_w, canvas_h);
        return;
    }

    cairo_push_group(cr);
    render_layer_unmasked_raw(cr, title, layer, title_time, canvas_w, canvas_h);
    cairo_pattern_t *content = cairo_pop_group(cr);

    cairo_push_group(cr);
    paint_soft_wipe_mask(cr, title, layer, title_time, canvas_w, canvas_h, transition);
    cairo_pattern_t *mask = cairo_pop_group(cr);

    if (content && mask && cairo_pattern_status(content) == CAIRO_STATUS_SUCCESS &&
        cairo_pattern_status(mask) == CAIRO_STATUS_SUCCESS) {
        cairo_set_source(cr, content);
        cairo_mask(cr, mask);
    }
    if (content) cairo_pattern_destroy(content);
    if (mask) cairo_pattern_destroy(mask);
}



static bool layer_effect_requires_stack_surface(const LayerEffect &effect)
{
    if (!effect.enabled) return false;
    switch (effect.type) {
    case LayerEffectType::BackgroundColor:
    case LayerEffectType::Outline:
    case LayerEffectType::Bloom:
    case LayerEffectType::Emboss:
    case LayerEffectType::DropShadow:
    case LayerEffectType::LongShadow:
    case LayerEffectType::ColorOverlay:
    case LayerEffectType::Glow:
    case LayerEffectType::InnerGlow:
    case LayerEffectType::InnerShadow:
    case LayerEffectType::Blur:
    case LayerEffectType::LensFlare:
    case LayerEffectType::Vignette:
    case LayerEffectType::Noise:
    case LayerEffectType::RoughenEdges:
        return true;
    case LayerEffectType::MotionBlur:
        /* Motion blur is temporal, not a post-render image-space filter.
         * Treating it as a stack-surface effect makes static bitmap/SVG
         * layers look like multiple overlaid copies even when they do not
         * move, because some code paths used a cached raster and applied a
         * directional smear.  Keep it out of the stack-surface pipeline and
         * let render_motion_blurred_layer() handle it consistently. */
        return false;
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
    return true;
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

static std::vector<uint8_t> render_background_effect_behind_surface(
    cairo_surface_t *surface, const Layer &layer, const LayerEffect &effect,
    double t, double local_x_offset, double local_y_offset)
{
    cairo_surface_flush(surface);
    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    if (width <= 0 || height <= 0)
        return {};

    const double box_w = std::max(1.0, eval_box_width(layer, t));
    const double box_h = std::max(1.0, eval_box_height(layer, t));
    const double bg_left = std::max(0.0, effect.padding_left_prop.is_animated()
                                             ? effect.padding_left_prop.evaluate(t)
                                             : (double)effect.effect_padding_left);
    const double bg_right = std::max(0.0, effect.padding_right_prop.is_animated()
                                              ? effect.padding_right_prop.evaluate(t)
                                              : (double)effect.effect_padding_right);
    const double bg_top = std::max(0.0, effect.padding_top_prop.is_animated()
                                            ? effect.padding_top_prop.evaluate(t)
                                            : (double)effect.effect_padding_top);
    const double bg_bottom = std::max(0.0, effect.padding_bottom_prop.is_animated()
                                               ? effect.padding_bottom_prop.evaluate(t)
                                               : (double)effect.effect_padding_bottom);
    const double x = -eval_origin_x(layer, t) * box_w - bg_left + local_x_offset;
    const double y = -eval_origin_y(layer, t) * box_h - bg_top + local_y_offset;
    const double bw = std::max(1.0, box_w + bg_left + bg_right);
    const double bh = std::max(1.0, box_h + bg_top + bg_bottom);

    std::vector<uint8_t> original = surface_pixels(surface);
    std::vector<uint8_t> under_pixels((size_t)width * (size_t)height * 4, 0);
    CairoSurfacePtr under(cairo_image_surface_create_for_data(
        under_pixels.data(), CAIRO_FORMAT_ARGB32, width, height, width * 4));
    auto under_cr = make_cairo_context(under.get());
    if (!under || cairo_surface_status(under.get()) != CAIRO_STATUS_SUCCESS || !under_cr)
        return {};

    const double tl = std::max(0.0, effect.corner_radius_tl_prop.is_animated()
                                        ? effect.corner_radius_tl_prop.evaluate(t)
                                        : (double)effect.effect_corner_radius_tl);
    const double tr = std::max(0.0, effect.corner_radius_tr_prop.is_animated()
                                        ? effect.corner_radius_tr_prop.evaluate(t)
                                        : (double)effect.effect_corner_radius_tr);
    const double br = std::max(0.0, effect.corner_radius_br_prop.is_animated()
                                        ? effect.corner_radius_br_prop.evaluate(t)
                                        : (double)effect.effect_corner_radius_br);
    const double bl = std::max(0.0, effect.corner_radius_bl_prop.is_animated()
                                        ? effect.corner_radius_bl_prop.evaluate(t)
                                        : (double)effect.effect_corner_radius_bl);

    cairo_add_rounded_rect_corners(under_cr.get(), x, y, bw, bh, tl, tr, br, bl,
                                   legacy_corner_type_roundness((CornerType)effect.effect_corner_type));
    const bool gradient = effect.effect_fill_type == 1;
    QColor fill = color_from_argb(eval_effect_color(effect, t));
    const double fill_opacity = std::clamp(effect.opacity_prop.is_animated()
                                               ? effect.opacity_prop.evaluate(t)
                                               : (double)effect.effect_opacity,
                                           0.0, 1.0);
    fill.setAlphaF(std::clamp(fill.alphaF() * fill_opacity, 0.0, 1.0));
    cairo_pattern_t *gradient_pattern = nullptr;
    if (fill.alpha() > 0 || gradient) {
        if (gradient) {
            gradient_pattern = create_background_gradient_pattern_for_effect(effect, x, y, bw, bh, fill_opacity);
            cairo_set_source(under_cr.get(), gradient_pattern);
        } else {
            cairo_set_source_rgba(under_cr.get(), fill.redF(), fill.greenF(), fill.blueF(), fill.alphaF());
        }
        cairo_fill_preserve(under_cr.get());
        if (gradient_pattern)
            cairo_pattern_destroy(gradient_pattern);
    }

    const double stroke_w = std::max(0.0, effect.stroke_width_prop.is_animated()
                                              ? effect.stroke_width_prop.evaluate(t)
                                              : (double)effect.effect_stroke_width);
    if (stroke_w > 0.0) {
        QColor stroke = color_from_argb(eval_effect_stroke_color(effect, t));
        const double stroke_opacity = std::clamp(effect.stroke_opacity_prop.is_animated()
                                                     ? effect.stroke_opacity_prop.evaluate(t)
                                                     : (double)effect.effect_stroke_opacity,
                                                 0.0, 1.0);
        stroke.setAlphaF(std::clamp(stroke.alphaF() * stroke_opacity, 0.0, 1.0));
        if (stroke.alpha() > 0) {
            cairo_set_line_width(under_cr.get(), stroke_w);
            cairo_set_source_rgba(under_cr.get(), stroke.redF(), stroke.greenF(), stroke.blueF(), stroke.alphaF());
            cairo_stroke(under_cr.get());
        } else {
            cairo_new_path(under_cr.get());
        }
    } else {
        cairo_new_path(under_cr.get());
    }

    under_cr.reset();
    cairo_surface_flush(under.get());
    std::vector<uint8_t> background_alpha = surface_alpha(under.get());
    composite_premultiplied_over(under_pixels, original);
    write_surface_pixels(surface, under_pixels);
    return background_alpha;
}


static std::vector<uint8_t> bloom_mask_from_surface(cairo_surface_t *surface, double threshold, double intensity)
{
    const int w = cairo_image_surface_get_width(surface);
    const int h = cairo_image_surface_get_height(surface);
    const auto px = surface_pixels(surface);
    std::vector<uint8_t> mask((size_t)std::max(0, w) * (size_t)std::max(0, h), 0);
    const double th = std::clamp(threshold, 0.0, 1.0);
    const double gain = std::max(0.0, intensity);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t *p = &px[((size_t)y * w + x) * 4];
            const double a = p[3] / 255.0;
            if (a <= 0.0) continue;
            const double r = (p[2] / 255.0) / a;
            const double g = (p[1] / 255.0) / a;
            const double b = (p[0] / 255.0) / a;
            const double luma = std::clamp(0.2126 * r + 0.7152 * g + 0.0722 * b, 0.0, 1.0);
            const double v = th >= 1.0 ? 0.0 : std::clamp((luma - th) / std::max(1e-6, 1.0 - th), 0.0, 1.0);
            mask[(size_t)y * w + x] = (uint8_t)std::lround(std::clamp(v * a * gain, 0.0, 1.0) * 255.0);
        }
    }
    return mask;
}

static void apply_emboss_to_surface(cairo_surface_t *surface, const LayerEffect &effect)
{
    const int w = cairo_image_surface_get_width(surface);
    const int h = cairo_image_surface_get_height(surface);
    if (w <= 0 || h <= 0) return;
    std::vector<uint8_t> px = surface_pixels(surface);
    std::vector<uint8_t> height((size_t)w * h, 0);
    for (size_t i = 0; i < height.size(); ++i) height[i] = px[i * 4 + 3];
    if (effect.effect_spread > 0.0f)
        blur_alpha_for_type(height, w, h, effect.effect_spread, (int)ShadowBlurType::StackFast);
    const double rad = effect.effect_angle * kPi / 180.0;
    const double lx = std::cos(rad), ly = std::sin(rad);
    const double depth = std::max(0.1f, effect.effect_size);
    const double relief = std::max(0.1f, effect.effect_distance);
    const double opacity = std::clamp((double)effect.effect_opacity, 0.0, 1.0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int xm = std::max(0, x - 1), xp = std::min(w - 1, x + 1);
            const int ym = std::max(0, y - 1), yp = std::min(h - 1, y + 1);
            const double gx = (height[(size_t)y * w + xp] - height[(size_t)y * w + xm]) / 255.0;
            const double gy = (height[(size_t)yp * w + x] - height[(size_t)ym * w + x]) / 255.0;
            const double shade = std::clamp((gx * lx + gy * ly) * depth * relief, -1.0, 1.0) * opacity;
            uint8_t *p = &px[((size_t)y * w + x) * 4];
            const double a = p[3] / 255.0;
            if (a <= 0.0) continue;
            for (int c = 0; c < 3; ++c) {
                double v = (p[c] / 255.0) / a;
                const double light = shade >= 0.0 ? 1.0 : 0.0;
                const double amount = std::abs(shade);
                v = blend_channel(v, light, effect.blend_mode) * amount + v * (1.0 - amount);
                p[c] = (uint8_t)std::lround(std::clamp(v, 0.0, 1.0) * a * 255.0);
            }
        }
    }
    write_surface_pixels(surface, px);
}

static bool effect_can_run_as_gpu_surface_pass(LayerEffectType type)
{
    switch (type) {
    case LayerEffectType::Outline:
    case LayerEffectType::Emboss:
    case LayerEffectType::BrightnessContrast:
    case LayerEffectType::Saturation:
    case LayerEffectType::ColorOverlay:
    case LayerEffectType::LensFlare:
    case LayerEffectType::Vignette:
    case LayerEffectType::Noise:
    case LayerEffectType::RoughenEdges:
        return true;
    /* These effects now require the shared two-pass Gaussian backend and a
     * second blurred texture. The old readback-oriented surface helper owns
     * only one pass/texture, so let its established CPU fallback handle these
     * rare legacy calls. Editor/live/cache composition stays GPU-only. */
    case LayerEffectType::Bloom:
    case LayerEffectType::Blur:
    case LayerEffectType::Glow:
    case LayerEffectType::InnerGlow:
    case LayerEffectType::InnerShadow:
    case LayerEffectType::DropShadow:
    case LayerEffectType::LongShadow:
    case LayerEffectType::BackgroundColor:
    case LayerEffectType::MotionBlur:
    default:
        return false;
    }
}

static bool layer_effects_can_run_as_gpu_surface_passes(const Layer &layer, double t)
{
    bool has_gpu_effect = false;
    for (const auto &effect : layer.effects) {
        if (!eval_effect_enabled(effect, t))
            continue;
        if (!effect_can_run_as_gpu_surface_pass(effect.type))
            return false;
        has_gpu_effect = true;
    }
    return has_gpu_effect;
}

static void set_effect_float_param(gs_effect_t *effect, const char *name, float value)
{
    if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, name))
        gs_effect_set_float(param, value);
}

static void set_effect_int_param(gs_effect_t *effect, const char *name, int value)
{
    if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, name))
        gs_effect_set_int(param, value);
}

static void set_effect_vec2_param(gs_effect_t *effect, const char *name, float x, float y)
{
    if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, name)) {
        struct vec2 value;
        vec2_set(&value, x, y);
        gs_effect_set_vec2(param, &value);
    }
}

static void set_effect_vec4_param(gs_effect_t *effect, const char *name,
                                  float x, float y, float z, float w)
{
    if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, name)) {
        struct vec4 value;
        vec4_set(&value, x, y, z, w);
        gs_effect_set_vec4(param, &value);
    }
}

static void set_effect_color_param(gs_effect_t *effect, const char *name, uint32_t argb)
{
    if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, name)) {
        struct vec4 value;
        value.x = ((argb >> 16) & 0xFF) / 255.0f;
        value.y = ((argb >> 8) & 0xFF) / 255.0f;
        value.z = (argb & 0xFF) / 255.0f;
        value.w = ((argb >> 24) & 0xFF) / 255.0f;
        gs_effect_set_vec4(param, &value);
    }
}

static void set_gpu_surface_effect_params(gs_effect_t *effect, const LayerEffect &resolved,
                                          int width, int height, double time = 0.0)
{
    set_effect_vec2_param(effect, "texelSize", 1.0f / std::max(1, width), 1.0f / std::max(1, height));
    set_effect_color_param(effect, "effectColor", resolved.effect_color);
    set_effect_color_param(effect, "secondaryColor", resolved.effect_secondary_color);
    set_effect_float_param(effect, "opacity", resolved.effect_opacity);
    set_effect_float_param(effect, "radius", resolved.effect_size);
    set_effect_float_param(effect, "width", resolved.effect_size);
    set_effect_float_param(effect, "blurRadius", resolved.effect_size);
    set_effect_float_param(effect, "spread", resolved.effect_spread);
    set_effect_float_param(effect, "falloff", resolved.effect_falloff);
    set_effect_float_param(effect, "brightness", resolved.brightness);
    set_effect_float_param(effect, "contrast", resolved.contrast);
    set_effect_float_param(effect, "saturation", resolved.saturation);
    set_effect_int_param(effect, "blendMode", static_cast<int>(resolved.blend_mode));
    if (resolved.type == LayerEffectType::LensFlare) {
        /* Keep the lens-flare ghost count on a dedicated uniform.  Reusing
         * the generic `samples` name made the effect depend on backend-specific
         * shader parsing even though no texture sampling loop is involved. */
        set_effect_float_param(effect, "ghostCount",
            std::clamp(resolved.effect_complexity, 2.0f, 12.0f));
    } else {
        set_effect_float_param(effect, "samples",
            static_cast<float>(std::clamp(resolved.effect_samples, 2, 64)));
    }
    set_effect_float_param(effect, "threshold", std::clamp(resolved.effect_spread, 0.0f, 1.0f));
    set_effect_float_param(effect, "intensity", std::max(0.0f, resolved.effect_falloff));
    set_effect_float_param(effect, "strength", std::max(0.0f, resolved.effect_size) / 32.0f);

    const float radians = resolved.effect_angle * static_cast<float>(kPi / 180.0);
    const float dx = std::cos(radians) * resolved.effect_distance;
    const float dy = std::sin(radians) * resolved.effect_distance;
    set_effect_vec2_param(effect, "offset", dx, dy);
    set_effect_vec2_param(effect, "direction", std::cos(radians), std::sin(radians));
    set_effect_float_param(effect, "shadowLength", resolved.effect_distance);
    if (resolved.type == LayerEffectType::LensFlare)
        set_effect_float_param(effect, "profile", static_cast<float>(resolved.effect_profile));
    else
        set_effect_int_param(effect, "profile", resolved.effect_profile);
    set_effect_int_param(effect, "animatedNoise", resolved.effect_animated ? 1 : 0);
    set_effect_int_param(effect, "monochrome", resolved.effect_monochrome ? 1 : 0);
    set_effect_int_param(effect, "invert", resolved.effect_invert ? 1 : 0);
    set_effect_float_param(effect, "seed", static_cast<float>(resolved.effect_seed));
    set_effect_float_param(effect, "amount", resolved.effect_amount);
    set_effect_float_param(effect, "scale", resolved.effect_scale);
    set_effect_float_param(effect, "softness", resolved.effect_softness);
    set_effect_float_param(effect, "roundness", resolved.effect_roundness);
    set_effect_float_param(effect, "speed", resolved.effect_speed);
    set_effect_vec2_param(effect, "center", resolved.effect_center_x, resolved.effect_center_y);
    set_effect_float_param(effect, "angle", resolved.effect_angle);
    set_effect_float_param(effect, "complexity", resolved.effect_complexity);
    set_effect_float_param(effect, "evolution", resolved.effect_evolution);
    set_effect_float_param(effect, "time", static_cast<float>(time));
}

static bool apply_gpu_surface_effects_to_surface(cairo_surface_t *surface, const Layer &layer, double t)
{
    if (final_frame_readback_only() || !TitlePreferences::gpu_available() ||
        !surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
        return false;

    const int width = cairo_image_surface_get_width(surface);
    const int height = cairo_image_surface_get_height(surface);
    if (width <= 0 || height <= 0 || !layer_effects_can_run_as_gpu_surface_passes(layer, t))
        return false;

    std::vector<uint8_t> pixels = surface_pixels(surface);
    if (pixels.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4)
        return false;

    static std::mutex gpu_effect_mutex;
    static TitleEffectRegistry registry;
    std::lock_guard<std::mutex> lock(gpu_effect_mutex);

    bool success = false;
    obs_enter_graphics();

    const uint8_t *initial_data[1] = {pixels.data()};
    gs_texture_t *source = gs_texture_create(width, height, GS_BGRA, 1, initial_data, 0);
    gs_texrender_t *ping = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    gs_texrender_t *pong = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    gs_stagesurf_t *stage = gs_stagesurface_create(width, height, GS_BGRA);
    gs_texture_t *current = source;
    bool use_ping = true;

    if (source && ping && pong && stage) {
        success = true;
        for (const auto &effect_config : layer.effects) {
            if (!eval_effect_enabled(effect_config, t))
                continue;

            LayerEffect resolved = effect_config;
            resolved.effect_color = eval_effect_color(effect_config, t);
            resolved.effect_opacity = (float)std::clamp(
                effect_config.opacity_prop.is_animated() ? effect_config.opacity_prop.evaluate(t)
                                                         : (double)effect_config.effect_opacity,
                0.0, 1.0);
            resolved.effect_size = (float)std::max(
                0.0, effect_config.size_prop.is_animated() ? effect_config.size_prop.evaluate(t)
                                                           : (double)effect_config.effect_size);
            resolved.effect_distance = (float)std::max(
                0.0, effect_config.distance_prop.is_animated() ? effect_config.distance_prop.evaluate(t)
                                                               : (double)effect_config.effect_distance);
            resolved.effect_angle = (float)(effect_config.angle_prop.is_animated()
                                                ? effect_config.angle_prop.evaluate(t)
                                                : (double)effect_config.effect_angle);
            resolved.effect_spread = (float)std::max(
                0.0, effect_config.spread_prop.is_animated() ? effect_config.spread_prop.evaluate(t)
                                                             : (double)effect_config.effect_spread);
            resolved.effect_falloff = (float)std::max(
                0.0, effect_config.falloff_prop.is_animated() ? effect_config.falloff_prop.evaluate(t)
                                                              : (double)effect_config.effect_falloff);

            gs_effect_t *gpu_effect = registry.compile(resolved.type);
            gs_texrender_t *target = use_ping ? ping : pong;
            if (!gpu_effect || !target || !current) {
                success = false;
                break;
            }

            gs_texrender_reset(target);
            if (!gs_texrender_begin(target, width, height)) {
                success = false;
                break;
            }

            gs_ortho(0.0f, static_cast<float>(width), 0.0f, static_cast<float>(height), -100.0f, 100.0f);
            struct vec4 clear;
            vec4_zero(&clear);
            gs_clear(GS_CLEAR_COLOR, &clear, 1.0f, 0);

            if (gs_eparam_t *image_param = gs_effect_get_param_by_name(gpu_effect, "image"))
                gs_effect_set_texture(image_param, current);
            set_gpu_surface_effect_params(gpu_effect, resolved, width, height);

            while (gs_effect_loop(gpu_effect, "Draw"))
                gs_draw_sprite(current, 0, width, height);

            gs_texrender_end(target);
            current = gs_texrender_get_texture(target);
            use_ping = !use_ping;
        }

        if (success && current) {
            gs_stage_texture(stage, current);
            uint8_t *mapped = nullptr;
            uint32_t linesize = 0;
            if (gs_stagesurface_map(stage, &mapped, &linesize) && mapped &&
                linesize >= static_cast<uint32_t>(width * 4)) {
                std::vector<uint8_t> out(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
                for (int y = 0; y < height; ++y) {
                    std::copy(mapped + static_cast<size_t>(y) * linesize,
                              mapped + static_cast<size_t>(y) * linesize + width * 4,
                              out.begin() + static_cast<size_t>(y) * width * 4);
                }
                gs_stagesurface_unmap(stage);
                if (source) gs_texture_destroy(source);
                if (ping) gs_texrender_destroy(ping);
                if (pong) gs_texrender_destroy(pong);
                if (stage) gs_stagesurface_destroy(stage);
                obs_leave_graphics();
                write_surface_pixels(surface, out);
                return true;
            }
            if (mapped)
                gs_stagesurface_unmap(stage);
            success = false;
        }
    }

    if (source) gs_texture_destroy(source);
    if (ping) gs_texrender_destroy(ping);
    if (pong) gs_texrender_destroy(pong);
    if (stage) gs_stagesurface_destroy(stage);
    obs_leave_graphics();
    return success;
}

static void apply_stackable_pixel_effects_to_surface(cairo_surface_t *surface, const Layer &layer, double t,
                                                     double local_x_offset = 0.0,
                                                     double local_y_offset = 0.0)
{
    /* Strict GPU-only effect contract. Cairo may generate immutable base
     * coverage, but it may not execute pixel effects or trigger a GPU-to-CPU
     * round trip. The unified compositor applies the complete effect stack. */
    (void)surface;
    (void)layer;
    (void)t;
    (void)local_x_offset;
    (void)local_y_offset;
}



static bool layer_has_non_transform_animation(const Layer &layer)
{
    return layer_has_effect_animation(layer) ||
           layer.size.is_animated() ||
           layer.image_size.is_animated() ||
           layer.origin_prop.is_animated() ||
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
           layer.text_color_a.is_animated() ||
           layer.text_color_r.is_animated() ||
           layer.text_color_g.is_animated() ||
           layer.text_color_b.is_animated() ||
           layer.fill_color_a.is_animated() ||
           layer.fill_color_r.is_animated() ||
           layer.fill_color_g.is_animated() ||
           layer.fill_color_b.is_animated() ||
           layer.background_enabled_prop.is_animated() ||
           layer.background_opacity_prop.is_animated() ||
           layer.background_padding_x_prop.is_animated() ||
           layer.background_padding_y_prop.is_animated() ||
           layer.background_padding_left_prop.is_animated() ||
           layer.background_padding_right_prop.is_animated() ||
           layer.background_padding_top_prop.is_animated() ||
           layer.background_padding_bottom_prop.is_animated() ||
           layer.background_corner_radius_prop.is_animated() ||
           layer.background_corner_radius_tl_prop.is_animated() ||
           layer.background_corner_radius_tr_prop.is_animated() ||
           layer.background_corner_radius_br_prop.is_animated() ||
           layer.background_corner_radius_bl_prop.is_animated() ||
           layer.background_stroke_width_prop.is_animated() ||
           layer.background_stroke_opacity_prop.is_animated() ||
           layer.background_color_a.is_animated() ||
           layer.background_color_r.is_animated() ||
           layer.background_color_g.is_animated() ||
           layer.background_color_b.is_animated() ||
           layer.background_stroke_color_a.is_animated() ||
           layer.background_stroke_color_r.is_animated() ||
           layer.background_stroke_color_g.is_animated() ||
           layer.background_stroke_color_b.is_animated() ||
           layer.shadow_enabled_prop.is_animated() ||
           layer.shadow_opacity_prop.is_animated() ||
           layer.shadow_distance_prop.is_animated() ||
           layer.shadow_angle_prop.is_animated() ||
           layer.shadow_blur_prop.is_animated() ||
           layer.shadow_spread_prop.is_animated() ||
           layer.shadow_color_a.is_animated() ||
           layer.shadow_color_r.is_animated() ||
           layer.shadow_color_g.is_animated() ||
           layer.shadow_color_b.is_animated();
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
    layer.position.static_value.x = anchor_x;
    layer.position.static_value.y = anchor_y;
    layer.position.keyframes.clear();
    layer.scale.static_value.x = 1.0;
    layer.scale.static_value.y = 1.0;
    layer.scale.keyframes.clear();
    layer.rotation.static_value = 0.0;
    layer.rotation.keyframes.clear();
    layer.opacity.static_value = opacity;
    layer.opacity.keyframes.clear();
}

static int gaussian_blur_downsample(double radius)
{
    const double clamped = std::clamp(radius, 0.0, 4096.0);
    int downsample = 1;
    while (downsample < 64 && clamped > 8.0 * downsample)
        downsample *= 2;
    return downsample;
}

static double gaussian_blur_support(double radius)
{
    if (radius <= 0.01)
        return 0.0;
    return 8.0 * gaussian_blur_downsample(radius);
}

static double quantized_effect_padding(double required)
{
    required = std::clamp(required, 2.0, 4096.0);
    double bucket = 8.0;
    while (bucket < required && bucket < 4096.0)
        bucket *= 2.0;
    return std::min(bucket, 4096.0);
}

static int general_transition_blur_padding(const Layer &layer)
{
    double max_blur = 0.0;
    for (const LayerTransition &transition : layer.transitions) {
        if (!transition.enabled || transition.kind != LayerTransitionKind::General)
            continue;
        if (transition.type == LayerTransitionType::OpacityBlur ||
            transition.type == LayerTransitionType::ZoomBlur ||
            transition.type == LayerTransitionType::BlurSlide) {
            max_blur = std::max(max_blur,
                                std::max(0.0, transition.blur_amount));
        }
    }
    if (max_blur <= 0.0)
        return 0;
    return static_cast<int>(std::ceil(quantized_effect_padding(
        gaussian_blur_support(std::clamp(max_blur, 0.0, 512.0)) + 4.0)));
}

static double stackable_effect_padding(const Layer &layer, double t)
{
    double required = 2.0;
    for (const auto &effect : layer.effects) {
        if (!eval_effect_enabled(effect, t))
            continue;
        const double size = std::max(0.0, effect.size_prop.is_animated()
                                              ? effect.size_prop.evaluate(t)
                                              : (double)effect.effect_size);
        const double distance = std::max(0.0, effect.distance_prop.is_animated()
                                                  ? effect.distance_prop.evaluate(t)
                                                  : (double)effect.effect_distance);
        const double spread = std::max(0.0, effect.spread_prop.is_animated()
                                                ? effect.spread_prop.evaluate(t)
                                                : (double)effect.effect_spread);
        switch (effect.type) {
        case LayerEffectType::DropShadow:
            required += distance + gaussian_blur_support(size + spread) + 4.0;
            break;
        case LayerEffectType::InnerShadow:
            /* Inner effects do not expand the layer alpha. */
            break;
        case LayerEffectType::LongShadow:
            required += distance +
                (effect.effect_blur_type == static_cast<int>(LongShadowBlurType::None)
                    ? 0.0 : gaussian_blur_support(size)) + 4.0;
            break;
        case LayerEffectType::Outline:
            required += size + distance + 4.0;
            break;
        case LayerEffectType::Glow:
        case LayerEffectType::Bloom:
            required += gaussian_blur_support(size) + 4.0;
            break;
        case LayerEffectType::InnerGlow:
            /* Inner glow never expands alpha outside the source. */
            break;
        case LayerEffectType::Blur:
            required += gaussian_blur_support(size) + 4.0;
            break;
        case LayerEffectType::MotionBlur:
            /* Temporal motion blur is produced by transformed GPU samples in
             * the full-frame target; it needs no local raster expansion. */
            break;
        default:
            break;
        }
    }
    /* A bucketed allocation means color/opacity/radius edits inside the same
     * capacity do not force text/vector/image rasterization on every slider
     * tick. The GPU effect cache still invalidates for every exact setting. */
    return std::ceil(quantized_effect_padding(required));
}

static QRectF layer_local_effect_bounds(const Layer &layer, double t)
{
    const double w = std::max(1.0, eval_box_width(layer, t));
    const double h = std::max(1.0, eval_box_height(layer, t));
    const double origin_x = eval_origin_x(layer, t);
    const double origin_y = eval_origin_y(layer, t);

    QRectF bounds(-origin_x * w, -origin_y * h, w, h);
    if (layer.type == LayerType::Image) {
        const ImageBoxLayout layout = image_box_layout_for_layer(layer, w, h,
                                                                 std::max(1.0, eval_image_width(layer, t)),
                                                                 std::max(1.0, eval_image_height(layer, t)));
        const QRectF visible = image_visible_rect_for_layout(layout, w, h,
                                                            layer.image_crop_when_outside_box);
        if (!visible.isEmpty())
            bounds = bounds.united(QRectF(-origin_x * w + visible.x(), -origin_y * h + visible.y(),
                                          visible.width(), visible.height()));
    }

    if (eval_background_enabled(layer, t)) {
        bounds = bounds.united(QRectF(-origin_x * w - eval_background_padding_left(layer, t),
                                      -origin_y * h - eval_background_padding_top(layer, t),
                                      w + eval_background_padding_left(layer, t) + eval_background_padding_right(layer, t),
                                      h + eval_background_padding_top(layer, t) + eval_background_padding_bottom(layer, t)));
    }

    const double outline = std::max({eval_outline_width(layer, t),
                                     eval_background_stroke_width(layer, t),
                                     0.0});
    if (outline > 0.0)
        bounds = bounds.adjusted(-outline, -outline, outline, outline);

    const double pad = stackable_effect_padding(layer, t);
    return bounds.adjusted(-pad, -pad, pad, pad);
}

static QRect clipped_effect_surface_rect(const Layer &layer, double t, int canvas_w, int canvas_h)
{
    QRectF bounds = layer_local_effect_bounds(layer, t);
    const int max_w = std::max(1, canvas_w);
    const int max_h = std::max(1, canvas_h);
    const int x = (int)std::floor(bounds.left());
    const int y = (int)std::floor(bounds.top());
    const int right = (int)std::ceil(bounds.right());
    const int bottom = (int)std::ceil(bounds.bottom());
    return QRect(QPoint(x, y), QPoint(std::max(x, right), std::max(y, bottom)))
        .adjusted(0, 0, 1, 1)
        .intersected(QRect(-max_w, -max_h, max_w * 3, max_h * 3));
}

static void evict_effect_layer_cache_if_needed(TitleSourceData *data,
                                                const std::string &protected_cache_id)
{
    if (!data)
        return;

    constexpr size_t kMaxEffectLayerCacheEntries = 128;
    constexpr qsizetype kMaxEffectLayerCacheBytes = 256 * 1024 * 1024;
    qsizetype cached_bytes = 0;
    for (const auto &entry : data->effect_layer_cache)
        cached_bytes += entry.second.image.sizeInBytes();

    while ((data->effect_layer_cache.size() > kMaxEffectLayerCacheEntries ||
            cached_bytes > kMaxEffectLayerCacheBytes) &&
           data->effect_layer_cache.size() > 1) {
        auto oldest = data->effect_layer_cache.end();
        for (auto it = data->effect_layer_cache.begin(); it != data->effect_layer_cache.end(); ++it) {
            if (it->first == protected_cache_id)
                continue;
            if (oldest == data->effect_layer_cache.end() ||
                it->second.last_used < oldest->second.last_used)
                oldest = it;
        }
        if (oldest == data->effect_layer_cache.end())
            break;
        cached_bytes -= oldest->second.image.sizeInBytes();
        data->effect_layer_cache.erase(oldest);
    }
}

static std::string effect_layer_cache_key(const TitleSourceData *data, const Title &title, const Layer &layer,
                                          double title_time, int canvas_w, int canvas_h,
                                          bool force_time_key = false,
                                          bool ignore_general_transitions = false)
{
    const double t = std::max(0.0, title_time - layer.in_time);
    std::ostringstream key;
    key << layer.id << "|rev=" << (data ? data->seen_store_revision : 0)
        << "|canvas=" << canvas_w << 'x' << canvas_h
        << "|type=" << (int)layer.type;
    if (!data)
        key << "|content=" << layer_render_fingerprint(layer);
    key << "|box=" << eval_box_width(layer, t) << 'x' << eval_box_height(layer, t)
        << "|origin=" << eval_origin_x(layer, t) << ',' << eval_origin_y(layer, t)
        << "|stack=" << layer.effects.size();

    const bool transition_active =
        (!ignore_general_transitions && evaluate_layer_general_transitions(
            layer.transitions, layer.in_time, layer.out_time, title_time).active) ||
        active_text_layer_transition(layer, title_time) != nullptr;
    if (force_time_key || layer_has_non_transform_animation(layer) || layer.type == LayerType::Ticker ||
        transition_active)
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
            << (info.exists() ? info.size() : -1)
            << "|imagebox=" << (int)layer.image_box_mode << ','
            << layer.image_size_auto_fit << ',' << layer.image_crop_when_outside_box << ','
            << layer.image_anchor_x << ',' << layer.image_anchor_y
            << "|imagesize=" << eval_image_width(layer, t) << 'x' << eval_image_height(layer, t)
            << "|imagefilter=" << (int)layer.scale_filter;
    }

    for (const auto &effect : layer.effects) {
        key << "|e=" << (int)effect.type << ',' << effect.enabled << ','
            << effect.brightness << ',' << effect.contrast << ',' << effect.saturation << ','
            << effect.tint_color << ',' << effect.tint_amount << ',' << effect.effect_color << ','
            << effect.effect_opacity << ',' << effect.effect_size << ',' << effect.effect_distance << ','
            << effect.effect_angle << ',' << effect.effect_spread << ',' << effect.effect_falloff << ','
            << effect.effect_blur_type << ',' << effect.effect_samples << ',' << effect.effect_centered << ','
            << (int)effect.blend_mode << ',' << effect.effect_profile << ','
            << effect.effect_animated << ',' << effect.effect_monochrome << ','
            << effect.effect_invert << ',' << effect.effect_seed << ','
            << effect.effect_amount << ',' << effect.effect_scale << ','
            << effect.effect_softness << ',' << effect.effect_roundness << ','
            << effect.effect_speed << ',' << effect.effect_center_x << ','
            << effect.effect_center_y << ',' << effect.effect_complexity << ','
            << effect.effect_evolution << ',' << effect.effect_secondary_color;
    }
    return key.str();
}

static const TitleSourceData::CachedEffectLayer *ensure_cached_effect_layer(
    TitleSourceData *data, const Title &title, const Layer &layer, double title_time,
    int canvas_w, int canvas_h, const std::string &cache_suffix = std::string(),
    bool force_time_key = false, bool allow_plain_layer = false,
    bool strip_general_transitions = false)
{
    if (!data || layer.type == LayerType::Clock || (!allow_plain_layer && !layer_has_stackable_pixel_effects(layer)))
        return nullptr;

    const std::string cache_id = (layer.id.empty() ? std::string("__anonymous_effect_layer") : layer.id) + cache_suffix;
    const std::string key = effect_layer_cache_key(
        data, title, layer, title_time, canvas_w, canvas_h,
        force_time_key, strip_general_transitions);
    auto it = data->effect_layer_cache.find(cache_id);
    if (it == data->effect_layer_cache.end() || it->second.key != key) {
        const double t = std::max(0.0, title_time - layer.in_time);
        const QRect surface_rect = clipped_effect_surface_rect(layer, t, canvas_w, canvas_h);
        if (!surface_rect.isValid() || surface_rect.isEmpty())
            return nullptr;

        QImage canvas(std::max(1, surface_rect.width()), std::max(1, surface_rect.height()),
                      QImage::Format_ARGB32_Premultiplied);
        canvas.fill(Qt::transparent);
        auto surface = make_image_surface_for_qimage(canvas);
        auto layer_cr = make_cairo_context(surface.get());
        if (!surface || !layer_cr)
            return nullptr;
        cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_CLEAR);
        cairo_paint(layer_cr.get());
        cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_OVER);
        cairo_translate(layer_cr.get(), -surface_rect.x(), -surface_rect.y());

        Layer base_layer = layer_without_stackable_pixel_effects(layer);
        if (strip_general_transitions) {
            base_layer.transitions.erase(
                std::remove_if(base_layer.transitions.begin(), base_layer.transitions.end(),
                               [](const LayerTransition &transition) {
                                   return transition.kind == LayerTransitionKind::General;
                               }),
                base_layer.transitions.end());
        }
        neutralize_layer_transform_for_effect_cache(base_layer, 1.0, 0.0, 0.0);

        render_layer_unmasked(layer_cr.get(), title, base_layer, title_time, canvas_w, canvas_h);
        layer_cr.reset();
        cairo_surface_flush(surface.get());

        apply_stackable_pixel_effects_to_surface(surface.get(), layer, t,
                                                 -surface_rect.x(), -surface_rect.y());
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
            cached.origin = QPointF(surface_rect.x() + bounds.x(), surface_rect.y() + bounds.y());
        }
        // Do not let a single pathological 8K/large-blur surface defeat the
        // byte cap by becoming the only protected entry in the per-source LRU.
        // Returning null makes the caller use the normal uncached render path
        // for that frame instead of retaining hundreds of megabytes.
        constexpr qsizetype kMaxRetainedEffectLayerBytes = 256 * 1024 * 1024;
        if (cached.image.sizeInBytes() > kMaxRetainedEffectLayerBytes)
            return nullptr;

        cached.last_used = ++data->effect_layer_cache_tick;
        data->effect_layer_cache.insert_or_assign(cache_id, std::move(cached));
        evict_effect_layer_cache_if_needed(data, cache_id);
        // Reacquire after eviction instead of assuming the selected entry
        // survived every future cache-policy change.
        it = data->effect_layer_cache.find(cache_id);
        if (it == data->effect_layer_cache.end())
            return nullptr;
    }
    it->second.last_used = ++data->effect_layer_cache_tick;

    return &it->second;
}

static bool paint_cached_effect_layer(cairo_t *cr,
                                      const Title &title,
                                      const Layer &paint_layer,
                                      double title_time,
                                      const TitleSourceData::CachedEffectLayer *cached)
{
    if (!cached)
        return false;
    if (cached->image.isNull())
        return true;

    auto cached_surface = make_image_surface_for_const_qimage(cached->image);
    if (!cached_surface)
        return false;
    cairo_save(cr);
    apply_layer_world_transform(cr, title, paint_layer, title_time);
    cairo_set_source_surface(cr, cached_surface.get(), cached->origin.x(), cached->origin.y());
    cairo_paint_with_alpha(cr, std::clamp(layer_chain_opacity(title, paint_layer, title_time), 0.0, 1.0));
    cairo_restore(cr);
    return true;
}

static bool render_cached_effect_layer(cairo_t *cr, TitleSourceData *data, const Title &title, const Layer &layer,
                                       double title_time, int canvas_w, int canvas_h)
{
    if (!data || !layer_has_stackable_pixel_effects(layer) || layer.type == LayerType::Clock)
        return false;

    const auto *cached = ensure_cached_effect_layer(data, title, layer, title_time, canvas_w, canvas_h);
    return paint_cached_effect_layer(cr, title, layer, title_time, cached);
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

static QTransform layer_world_transform_qt(const Title &title, const Layer &layer,
                                           double title_time, int depth = 0)
{
    if (depth > 64)
        return QTransform();

    QTransform transform;
    if (!layer.parent_id.empty()) {
        if (const Layer *parent = find_layer_by_id(title, layer.parent_id))
            transform = layer_world_transform_qt(title, *parent, title_time, depth + 1);
    }

    const double local_time = std::max(0.0, title_time - layer.in_time);
    const LayerTransitionVisualState transition = evaluate_layer_general_transitions(
        layer.transitions, layer.in_time, layer.out_time, title_time);
    const auto position = layer.position.evaluate(local_time);
    const auto scale = layer.scale.evaluate(local_time);
    transform.translate(position.x + transition.translate_x,
                        position.y + transition.translate_y);
    transform.rotate(layer.rotation.evaluate(local_time));
    transform.scale(scale.x * transition.scale,
                    scale.y * transition.scale);
    return transform;
}

static double layer_shutter_travel_pixels(const Title &title, const Layer &layer,
                                          double start_time, double end_time)
{
    const std::array<double, 3> times = {
        start_time, (start_time + end_time) * 0.5, end_time};
    std::array<std::array<QPointF, 5>, 3> world_points;
    for (std::size_t sample = 0; sample < times.size(); ++sample) {
        const double local_time = std::max(0.0, times[sample] - layer.in_time);
        const QRectF bounds = layer_local_effect_bounds(layer, local_time);
        const QTransform transform = layer_world_transform_qt(title, layer, times[sample]);
        const std::array<QPointF, 5> local_points = {
            bounds.topLeft(), bounds.topRight(), bounds.bottomRight(),
            bounds.bottomLeft(), bounds.center()};
        for (std::size_t point = 0; point < local_points.size(); ++point)
            world_points[sample][point] = transform.map(local_points[point]);
    }

    double max_distance = 0.0;
    for (std::size_t a_sample = 0; a_sample < world_points.size(); ++a_sample) {
        for (std::size_t b_sample = a_sample + 1; b_sample < world_points.size(); ++b_sample) {
            for (std::size_t point = 0; point < world_points[a_sample].size(); ++point) {
                const QPointF &a = world_points[a_sample][point];
                const QPointF &b = world_points[b_sample][point];
                max_distance = std::max(max_distance,
                                        std::hypot(b.x() - a.x(), b.y() - a.y()));
            }
        }
    }
    return max_distance;
}

static bool layer_has_non_rigid_transition_during_shutter(
    const Layer &layer, double start_time, double end_time)
{
    const std::array<double, 3> times = {
        start_time, (start_time + end_time) * 0.5, end_time};
    for (double time : times) {
        const LayerTransitionVisualState state = evaluate_layer_general_transitions(
            layer.transitions, layer.in_time, layer.out_time, time);
        if ((state.active && state.blur > 0.01) ||
            (state.active && state.wipe < 0.999999) ||
            active_text_layer_transition(layer, time) != nullptr) {
            return true;
        }
    }
    return false;
}

/* The editor/cache renderer is still Cairo-based, but temporal samples no
 * longer need to be composited in Cairo.  A transform-only layer is uploaded
 * once, drawn at every shutter time into one GPU accumulation target, and
 * staged back once after the complete exposure.  Keeping the accumulation in
 * premultiplied-alpha space prevents the bright fringes and separated copies
 * that were especially visible on bitmap/SVG image layers. */
static void apply_layer_world_transform_gs(const Title &title, const Layer &layer,
                                           double title_time, int depth);

static constexpr const char *kTemporalAccumulateEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform float weight;

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

float4 PSTemporalAccumulate(VertDataOut v_in) : TARGET
{
    return image.Sample(textureSampler, v_in.uv) * weight;
}

technique Draw
{
    pass
    {
        vertex_shader = VSDefault(v_in);
        pixel_shader = PSTemporalAccumulate(v_in);
    }
}
)";

struct TemporalGpuResources {
    gs_texture_t *source = nullptr;
    gs_texrender_t *target = nullptr;
    gs_stagesurf_t *stage = nullptr;
    gs_effect_t *effect = nullptr;
    uint32_t source_w = 0;
    uint32_t source_h = 0;
    uint32_t stage_w = 0;
    uint32_t stage_h = 0;
};

static TemporalGpuResources g_temporal_gpu;
static std::mutex g_temporal_gpu_mutex;
static std::mutex g_gpu_readback_sessions_mutex;
struct GpuReadbackSessionEntry {
    TitleGpuRenderSession *session = nullptr;
    uint64_t revision = 0;
};
static std::unordered_map<std::thread::id, GpuReadbackSessionEntry>
    g_gpu_readback_sessions;

static void destroy_temporal_gpu_resources_locked()
{
    if (g_temporal_gpu.source)
        gs_texture_destroy(g_temporal_gpu.source);
    if (g_temporal_gpu.target)
        gs_texrender_destroy(g_temporal_gpu.target);
    if (g_temporal_gpu.stage)
        gs_stagesurface_destroy(g_temporal_gpu.stage);
    if (g_temporal_gpu.effect)
        gs_effect_destroy(g_temporal_gpu.effect);
    g_temporal_gpu = {};
}

static bool prepare_temporal_gpu_resources(const QImage &upload,
                                           uint32_t canvas_w, uint32_t canvas_h)
{
    const uint32_t source_w = static_cast<uint32_t>(upload.width());
    const uint32_t source_h = static_cast<uint32_t>(upload.height());
    if (!g_temporal_gpu.source || g_temporal_gpu.source_w != source_w ||
        g_temporal_gpu.source_h != source_h) {
        if (g_temporal_gpu.source)
            gs_texture_destroy(g_temporal_gpu.source);
        g_temporal_gpu.source = gs_texture_create(source_w, source_h, GS_BGRA, 1,
                                                  nullptr, GS_DYNAMIC);
        g_temporal_gpu.source_w = g_temporal_gpu.source ? source_w : 0;
        g_temporal_gpu.source_h = g_temporal_gpu.source ? source_h : 0;
    }
    if (!g_temporal_gpu.target)
        g_temporal_gpu.target = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    if (!g_temporal_gpu.stage || g_temporal_gpu.stage_w != canvas_w ||
        g_temporal_gpu.stage_h != canvas_h) {
        if (g_temporal_gpu.stage)
            gs_stagesurface_destroy(g_temporal_gpu.stage);
        g_temporal_gpu.stage = gs_stagesurface_create(canvas_w, canvas_h, GS_BGRA);
        g_temporal_gpu.stage_w = g_temporal_gpu.stage ? canvas_w : 0;
        g_temporal_gpu.stage_h = g_temporal_gpu.stage ? canvas_h : 0;
    }
    if (!g_temporal_gpu.effect) {
        g_temporal_gpu.effect = gs_effect_create(
            kTemporalAccumulateEffect, "obs-bgs-temporal-accumulate.effect", nullptr);
    }
    if (!g_temporal_gpu.source || !g_temporal_gpu.target ||
        !g_temporal_gpu.stage || !g_temporal_gpu.effect)
        return false;

    gs_texture_set_image(g_temporal_gpu.source, upload.constBits(),
                         static_cast<uint32_t>(upload.bytesPerLine()), false);
    return true;
}

static bool gpu_accumulate_motion_raster(cairo_t *cr, const Title &title,
                                         const Layer &layer,
                                         const QImage &raster,
                                         const QPointF &origin,
                                         const std::vector<double> &sample_times,
                                         double title_time, double mix,
                                         int canvas_w, int canvas_h)
{
    if (final_frame_readback_only() || !cr || raster.isNull() || sample_times.empty() ||
        canvas_w <= 0 || canvas_h <= 0 || !TitlePreferences::gpu_available())
        return false;

    QImage upload = raster.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (upload.isNull() || upload.bytesPerLine() != upload.width() * 4)
        upload = upload.copy();
    if (upload.isNull() || upload.bytesPerLine() != upload.width() * 4)
        return false;

    std::lock_guard<std::mutex> lock(g_temporal_gpu_mutex);

    bool mapped = false;
    QImage exposure;

    obs_enter_graphics();
    const uint32_t output_w = static_cast<uint32_t>(canvas_w);
    const uint32_t output_h = static_cast<uint32_t>(canvas_h);
    bool ok = prepare_temporal_gpu_resources(upload, output_w, output_h);
    gs_texture_t *source = g_temporal_gpu.source;
    gs_texrender_t *target = g_temporal_gpu.target;
    gs_stagesurf_t *stage = g_temporal_gpu.stage;
    gs_effect_t *effect = g_temporal_gpu.effect;
    if (ok) {
        gs_viewport_push();
        gs_projection_push();
        gs_blend_state_push();
        gs_texrender_reset(target);
        ok = gs_texrender_begin(target, static_cast<uint32_t>(canvas_w),
                                static_cast<uint32_t>(canvas_h));
        if (ok) {
            gs_ortho(0.0f, static_cast<float>(canvas_w), 0.0f,
                     static_cast<float>(canvas_h), -100.0f, 100.0f);
            struct vec4 clear;
            vec4_zero(&clear);
            gs_clear(GS_CLEAR_COLOR, &clear, 1.0f, 0);
            gs_enable_blending(true);
            gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);

            gs_eparam_t *image_param = gs_effect_get_param_by_name(effect, "image");
            gs_eparam_t *weight_param = gs_effect_get_param_by_name(effect, "weight");
            if (!image_param || !weight_param) {
                ok = false;
            } else {
                gs_effect_set_texture(image_param, source);
                const double current_opacity = layer_chain_visible(title, layer, title_time)
                    ? std::clamp(layer_chain_opacity(title, layer, title_time), 0.0, 1.0)
                    : 0.0;
                const double sample_weight = mix / static_cast<double>(sample_times.size());
                auto draw_at_time = [&](double sample_time, double weight) {
                    /* Temporal blur integrates geometry over the shutter interval,
                     * but opacity is evaluated once at the displayed frame. Sampling
                     * opacity over time creates a final opacity-only tail after motion
                     * has stopped and is especially visible at cue/outro boundaries. */
                    if (current_opacity <= 0.0)
                        return;
                    const float resolved_weight = static_cast<float>(weight * current_opacity);
                    if (resolved_weight <= 0.0f)
                        return;
                    gs_effect_set_float(weight_param, resolved_weight);
                    gs_matrix_push();
                    gs_matrix_identity();
                    apply_layer_world_transform_gs(title, layer, sample_time, 0);
                    gs_matrix_translate3f(static_cast<float>(origin.x()),
                                          static_cast<float>(origin.y()), 0.0f);
                    while (gs_effect_loop(effect, "Draw"))
                        gs_draw_sprite(source, 0,
                                       static_cast<uint32_t>(upload.width()),
                                       static_cast<uint32_t>(upload.height()));
                    gs_matrix_pop();
                };

                if (mix < 1.0)
                    draw_at_time(title_time, 1.0 - mix);
                for (double sample_time : sample_times)
                    draw_at_time(sample_time, sample_weight);
            }
            gs_texrender_end(target);
        }
        gs_blend_state_pop();
        gs_projection_pop();
        gs_viewport_pop();
    }

    if (ok) {
        gs_texture_t *result = gs_texrender_get_texture(target);
        ok = result != nullptr;
        if (ok) {
            gs_stage_texture(stage, result);
            uint8_t *mapped_data = nullptr;
            uint32_t linesize = 0;
            mapped = gs_stagesurface_map(stage, &mapped_data, &linesize);
            ok = mapped && mapped_data && linesize >= static_cast<uint32_t>(canvas_w * 4);
            if (ok) {
                exposure = QImage(canvas_w, canvas_h, QImage::Format_ARGB32_Premultiplied);
                for (int y = 0; y < canvas_h; ++y) {
                    std::memcpy(exposure.scanLine(y),
                                mapped_data + static_cast<size_t>(y) * linesize,
                                static_cast<size_t>(canvas_w) * 4);
                }
            }
        }
    }

    if (mapped)
        gs_stagesurface_unmap(stage);
    obs_leave_graphics();

    if (!ok || exposure.isNull())
        return false;

    auto exposure_surface = make_image_surface_for_const_qimage(exposure);
    if (!exposure_surface)
        return false;
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_surface(cr, exposure_surface.get(), 0.0, 0.0);
    cairo_paint(cr);
    cairo_restore(cr);
    return true;
}

struct MotionBaseRaster {
    QImage image;
    QPointF origin;
    double logical_width = 0.0;
    double logical_height = 0.0;
    QRectF layer_box_rect;
    QRectF image_clip_rect;
};

static MotionBaseRaster render_motion_base_raster(const Title &title,
                                                   const Layer &base_layer,
                                                   double title_time,
                                                   int canvas_w, int canvas_h)
{
    MotionBaseRaster result;
    const double local_time = std::max(0.0, title_time - base_layer.in_time);
    const QRect surface_rect = clipped_effect_surface_rect(base_layer, local_time,
                                                           canvas_w, canvas_h);
    if (!surface_rect.isValid() || surface_rect.isEmpty())
        return result;

    QImage canvas(std::max(1, surface_rect.width()),
                  std::max(1, surface_rect.height()),
                  QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    auto surface = make_image_surface_for_qimage(canvas);
    auto layer_cr = make_cairo_context(surface.get());
    if (!surface || !layer_cr)
        return result;

    cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_CLEAR);
    cairo_paint(layer_cr.get());
    cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_OVER);
    cairo_translate(layer_cr.get(), -surface_rect.x(), -surface_rect.y());

    Layer local_layer = base_layer;
    local_layer.transitions.erase(
        std::remove_if(local_layer.transitions.begin(), local_layer.transitions.end(),
                       [](const LayerTransition &transition) {
                           return transition.kind == LayerTransitionKind::General;
                       }),
        local_layer.transitions.end());
    neutralize_layer_transform_for_effect_cache(local_layer, 1.0, 0.0, 0.0);
    {
        ScopedGpuReadbackContract no_intermediate_readbacks(
            GpuReadbackContract::FinalFrameOnly);
        render_layer_unmasked_with_stackable_effects(layer_cr.get(), nullptr, title,
                                                     local_layer, title_time,
                                                     canvas_w, canvas_h);
    }
    layer_cr.reset();
    cairo_surface_flush(surface.get());
    surface.reset();

    const QRect bounds = image_alpha_bounds(canvas);
    if (!bounds.isValid() || bounds.isEmpty())
        return result;
    const bool needs_effect_padding = std::any_of(
        base_layer.effects.begin(), base_layer.effects.end(),
        [local_time](const LayerEffect &effect) {
            return effect.type != LayerEffectType::MotionBlur &&
                   eval_effect_enabled(effect, local_time);
        });
    if (needs_effect_padding) {
        /* Preserve the transparent border calculated by
         * clipped_effect_surface_rect(). GPU glow/blur/shadow shaders need this
         * room or their output is clipped to the source alpha bounds. */
        result.image = canvas;
        result.origin = QPointF(surface_rect.x(), surface_rect.y());
    } else {
        result.image = canvas.copy(bounds);
        result.origin = QPointF(surface_rect.x() + bounds.x(),
                                surface_rect.y() + bounds.y());
    }
    return result;
}

static bool render_motion_blurred_layer(cairo_t *cr, TitleSourceData *data, const Title &title, const Layer &layer,
                                        double title_time, int canvas_w, int canvas_h)
{
    const LayerEffect *motion = layer_motion_blur_effect(layer);
    if (!motion || layer.type == LayerType::Clock)
        return false;

    const int configured_samples = std::clamp(motion->effect_samples, 2, 64);
    const double amount = std::clamp((double)motion->effect_opacity, 0.0, 1.0);
    const double shutter_angle = std::clamp((double)motion->effect_size, 0.0, 720.0);
    Layer base_layer = layer_without_motion_blur_effects(layer);
    const double frame_seconds = std::max(1.0 / 240.0, source_frame_duration());
    const double shutter_seconds = frame_seconds * shutter_angle / 360.0;
    const double title_duration = std::max(0.0, title.duration);
    const double shutter_start = std::clamp(
        title_time + shutter_seconds * (motion->effect_centered ? -0.5 : -1.0),
        0.0, title_duration);
    const double shutter_end = std::clamp(
        title_time + shutter_seconds * (motion->effect_centered ? 0.5 : 0.0),
        0.0, title_duration);
    const bool non_rigid_transition = layer_has_non_rigid_transition_during_shutter(
        base_layer, shutter_start, shutter_end);
    const bool reusable_transform_raster =
        !layer_has_non_transform_animation(base_layer) &&
        !non_rigid_transition && base_layer.type != LayerType::Ticker;

    auto paint_fast_motion = [&](auto render_sample, auto render_original,
                                 const std::function<bool(const std::vector<double> &, double)> &gpu_accumulator) -> bool {
        const double travel_px = layer_shutter_travel_pixels(
            title, base_layer, shutter_start, shutter_end);
        const double shutter_mid = (shutter_start + shutter_end) * 0.5;
        const bool visible_start = layer_chain_visible(title, layer, shutter_start);
        const bool visible_mid = layer_chain_visible(title, layer, shutter_mid);
        const bool visible_end = layer_chain_visible(title, layer, shutter_end);
        const bool visibility_changed = visible_start != visible_mid || visible_mid != visible_end;
        /* A zero-motion exposure must be pixel-identical to the sharp layer.
         * This early-out is particularly important for image layers: repeatedly
         * accumulating the same alpha edge can expose tiny premultiplication or
         * sampling differences as a stationary horizontal halo. */
        if (travel_px < 0.01 && !visibility_changed &&
            !layer_has_non_transform_animation(base_layer) &&
            !non_rigid_transition) {
            render_original(cr);
            return true;
        }

        /* Sharp-edged bitmap/SVG layers expose low temporal sample counts as
         * separated horizontal copies.  Scale the exposure density with the
         * maximum transformed corner travel over the shutter interval, so
         * rotation and scale animation receive the same treatment as position.
         * Roughly two samples
         * per travelled pixel gives a continuous trail while retaining the
         * user setting as the minimum quality level. */
        const bool sharp_image_layer = layer.type == LayerType::Image;
        const double samples_per_pixel = sharp_image_layer ? 1.5 : 1.0;
        const int max_temporal_samples = sharp_image_layer ? 64 : 48;
        const int adaptive_samples = (int)std::ceil(travel_px * samples_per_pixel) + 1;
        const int samples = std::clamp(std::max(configured_samples, adaptive_samples), 2, max_temporal_samples);

        std::vector<double> sample_times;
        sample_times.reserve((size_t)samples);
        for (int i = 0; i < samples; ++i) {
            // Midpoint sampling avoids overweighting both ends of the exposure.
            const double f = ((double)i + 0.5) / (double)samples;
            const double sample_time = shutter_start + (shutter_end - shutter_start) * f;
            if (!sample_times.empty() && std::abs(sample_times.back() - sample_time) < 1e-8)
                continue;
            sample_times.push_back(sample_time);
        }

        if (sample_times.empty())
            return true;

        /* Motion blur must replace the sharp layer, not sit on top of it.
         * Build an averaged temporal exposure first, then mix that exposure
         * with the current sharp sample only when the effect opacity is below
         * 100%.  The old path painted the current layer at full opacity and
         * then added blurred samples over it, which looked like a smear effect
         * attached to an otherwise sharp object.
         */
        const double mix = std::clamp(amount, 0.0, 1.0);
        if (gpu_accumulator && gpu_accumulator(sample_times, mix))
            return true;

        cairo_save(cr);
        cairo_push_group(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_ADD);

        if (mix < 1.0) {
            cairo_push_group(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            render_original(cr);
            cairo_pop_group_to_source(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
            cairo_paint_with_alpha(cr, 1.0 - mix);
        }

        cairo_push_group(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
        const double sample_alpha = 1.0 / (double)sample_times.size();
        for (double sample_time : sample_times) {
            cairo_push_group(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
            if (layer_chain_visible(title, layer, sample_time))
                render_sample(cr, sample_time);
            cairo_pop_group_to_source(cr);
            cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
            cairo_paint_with_alpha(cr, sample_alpha);
        }
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, mix);

        cairo_pop_group_to_source(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
        cairo_paint(cr);
        cairo_restore(cr);

        return true;
    };

    if (!data) {
        if (amount <= 0.0 || shutter_angle <= 0.0) {
            render_layer_unmasked_with_stackable_effects(cr, nullptr, title, base_layer, title_time, canvas_w, canvas_h);
            return true;
        }

        const MotionBaseRaster motion_raster = reusable_transform_raster
            ? render_motion_base_raster(title, base_layer, title_time, canvas_w, canvas_h)
            : MotionBaseRaster{};
        return paint_fast_motion(
            [&](cairo_t *sample_cr, double sample_time) {
                render_layer_unmasked_with_stackable_effects(sample_cr, nullptr, title, base_layer,
                                                             sample_time, canvas_w, canvas_h);
            },
            [&](cairo_t *sample_cr) {
                render_layer_unmasked_with_stackable_effects(sample_cr, nullptr, title, base_layer,
                                                             title_time, canvas_w, canvas_h);
            },
            [&](const std::vector<double> &sample_times, double mix) {
                if (motion_raster.image.isNull())
                    return false;
                return gpu_accumulate_motion_raster(
                    cr, title, base_layer, motion_raster.image, motion_raster.origin,
                    sample_times, title_time, mix, canvas_w, canvas_h);
            });
    }

    if (amount <= 0.0 || shutter_angle <= 0.0) {
        render_layer_unmasked_with_stackable_effects(cr, nullptr, title, base_layer,
                                                     title_time, canvas_w, canvas_h);
        return true;
    }

    if (reusable_transform_raster) {
        const TitleSourceData::CachedEffectLayer *cached = nullptr;
        {
            ScopedGpuReadbackContract no_intermediate_readbacks(
                GpuReadbackContract::FinalFrameOnly);
            cached = ensure_cached_effect_layer(data, title, base_layer, title_time,
                                                canvas_w, canvas_h, "|motion-base",
                                                false, true, true);
        }
        if (cached) {
            if (cached->image.isNull())
                return true;

            auto cached_surface = make_image_surface_for_const_qimage(cached->image);
            if (cached_surface) {
                auto paint_cached_sample = [&](double sample_time, double alpha) {
                    if (!layer_chain_visible(title, base_layer, sample_time))
                        return;
                    cairo_save(cr);
                    apply_layer_world_transform(cr, title, base_layer, sample_time);
                    cairo_set_source_surface(cr, cached_surface.get(),
                                             cached->origin.x(), cached->origin.y());
                    cairo_paint_with_alpha(cr, alpha * std::clamp(layer_chain_opacity(title, base_layer, sample_time),
                                                                  0.0, 1.0));
                    cairo_restore(cr);
                };

                return paint_fast_motion(
                    [&](cairo_t *sample_cr, double sample_time) {
                        (void)sample_cr;
                        paint_cached_sample(sample_time, 1.0);
                    },
                    [&](cairo_t *sample_cr) {
                        (void)sample_cr;
                        paint_cached_sample(title_time, 1.0);
                    },
                    [&](const std::vector<double> &sample_times, double mix) {
                        return gpu_accumulate_motion_raster(
                            cr, title, base_layer, cached->image, cached->origin,
                            sample_times, title_time, mix, canvas_w, canvas_h);
                    });
            }
        }
    }

    /* Keep the OBS source path pixel-identical to the editor/prerender path.
     * Reusing one cached raster for every shutter sample only samples the
     * transform; it freezes animated size, opacity, effects, SVG raster size,
     * image alpha and other time-dependent properties.  Rendering the base
     * layer at each sample time fixes bitmap, bitmap-with-alpha and SVG motion
     * blur and removes the editor/OBS mismatch.  Image decoding remains cached
     * by load_cached_layer_image(), so this does not repeatedly read the file. */
    return paint_fast_motion(
        [&](cairo_t *sample_cr, double sample_time) {
            render_layer_unmasked_with_stackable_effects(sample_cr, nullptr, title, base_layer,
                                                         sample_time, canvas_w, canvas_h);
        },
        [&](cairo_t *sample_cr) {
            render_layer_unmasked_with_stackable_effects(sample_cr, nullptr, title, base_layer,
                                                         title_time, canvas_w, canvas_h);
        }, {});
}

static void render_layer_unmasked_with_stackable_effects(cairo_t *cr, TitleSourceData *data, const Title &title, const Layer &layer,
                                                         double title_time, int canvas_w, int canvas_h)
{
    const LayerTransitionVisualState transition_state = evaluate_layer_general_transitions(
        layer.transitions, layer.in_time, layer.out_time, title_time);
    Layer transition_layer;
    const Layer *effective_layer = &layer;
    if (transition_state.active && transition_state.blur > 0.01) {
        transition_layer = layer;
        LayerEffect blur;
        blur.type = LayerEffectType::Blur;
        blur.enabled = true;
        blur.effect_size = static_cast<float>(transition_state.blur);
        blur.effect_opacity = 1.0f;
        blur.effect_blur_type = static_cast<int>(ShadowBlurType::StackFast);
        transition_layer.effects.push_back(std::move(blur));
        effective_layer = &transition_layer;
    }
    const Layer &render_layer = *effective_layer;

    if (layer_has_motion_blur(render_layer) &&
        render_motion_blurred_layer(cr, data, title, render_layer, title_time, canvas_w, canvas_h))
        return;

    const bool transition_requires_dynamic_surface = transition_state.active &&
        (transition_state.blur > 0.01 || transition_state.wipe < 0.999999);

    if (!transition_state.active &&
        render_cached_effect_layer(cr, data, title, render_layer, title_time, canvas_w, canvas_h))
        return;

    // Opacity, scale and slide transitions only affect the final transform or
    // alpha. Cache the expensive pixel-effect result without general
    // transitions, then apply the active transition while painting it. The old
    // path disabled the cache for every transition and reran the full CPU
    // effect stack on every frame.
    if (transition_state.active && !transition_requires_dynamic_surface && data &&
        layer_has_stackable_pixel_effects(render_layer) && render_layer.type != LayerType::Clock) {
        // Do not deep-copy the complete layer graph on every transition frame.
        // The cache helper strips general transitions only when a cache miss
        // actually requires a base raster to be rendered.
        const auto *cached = ensure_cached_effect_layer(
            data, title, render_layer, title_time, canvas_w, canvas_h,
            "|general-transition-base", false, false, true);
        if (paint_cached_effect_layer(cr, title, render_layer, title_time, cached))
            return;
    }

    if (!layer_has_stackable_pixel_effects(render_layer)) {
        render_layer_unmasked(cr, title, render_layer, title_time, canvas_w, canvas_h);
        return;
    }

    Layer base_layer = layer_without_stackable_pixel_effects(render_layer);
    base_layer.transitions.erase(
        std::remove_if(base_layer.transitions.begin(), base_layer.transitions.end(),
                       [](const LayerTransition &transition) {
                           return transition.kind == LayerTransitionKind::General &&
                                  transition.type != LayerTransitionType::Wipe;
                       }),
        base_layer.transitions.end());
    const double t = std::max(0.0, title_time - render_layer.in_time);
    const QRect surface_rect = clipped_effect_surface_rect(render_layer, t, canvas_w, canvas_h);
    if (!surface_rect.isValid() || surface_rect.isEmpty())
        return;

    CairoSurfacePtr layer_surface(cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, std::max(1, surface_rect.width()), std::max(1, surface_rect.height())));
    auto layer_cr = make_cairo_context(layer_surface.get());
    if (!layer_surface || cairo_surface_status(layer_surface.get()) != CAIRO_STATUS_SUCCESS || !layer_cr)
        return;
    cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_CLEAR);
    cairo_paint(layer_cr.get());
    cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_OVER);
    cairo_translate(layer_cr.get(), -surface_rect.x(), -surface_rect.y());
    neutralize_layer_transform_for_effect_cache(base_layer, 1.0, 0.0, 0.0);
    render_layer_unmasked(layer_cr.get(), title, base_layer, title_time, canvas_w, canvas_h);
    layer_cr.reset();

    apply_stackable_pixel_effects_to_surface(layer_surface.get(), render_layer, t,
                                             -surface_rect.x(), -surface_rect.y());
    cairo_save(cr);
    apply_layer_world_transform(cr, title, render_layer, title_time);
    cairo_set_source_surface(cr, layer_surface.get(), surface_rect.x(), surface_rect.y());
    cairo_paint_with_alpha(cr, std::clamp(layer_chain_opacity(title, render_layer, title_time), 0.0, 1.0));
    cairo_restore(cr);
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

static bool render_layer_with_local_blend_surface(cairo_t *cr, TitleSourceData *data, const Title &title,
                                                  const Layer &layer, double title_time,
                                                  int canvas_w, int canvas_h)
{
    if (!cr || layer.mask_mode != MaskMode::None || layer.use_as_scene_mask)
        return false;

    /* Motion blur is temporal and must sample the layer with its real animated
     * world transform.  The compact local blend surface intentionally
     * neutralizes transforms, which made motion blur disappear (or differ) for
     * bitmap/SVG layers using a non-Normal blend mode.  Let the full-canvas
     * blend fallback handle these layers instead. */
    if (layer_has_motion_blur(layer))
        return false;

    const double t = std::max(0.0, title_time - layer.in_time);
    const QRect surface_rect = clipped_effect_surface_rect(layer, t, canvas_w, canvas_h);
    if (!surface_rect.isValid() || surface_rect.isEmpty())
        return false;

    CairoSurfacePtr layer_surface(cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, std::max(1, surface_rect.width()), std::max(1, surface_rect.height())));
    auto layer_cr = make_cairo_context(layer_surface.get());
    if (!layer_surface || cairo_surface_status(layer_surface.get()) != CAIRO_STATUS_SUCCESS || !layer_cr)
        return false;

    cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_CLEAR);
    cairo_paint(layer_cr.get());
    cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_OVER);
    cairo_translate(layer_cr.get(), -surface_rect.x(), -surface_rect.y());

    Layer local_layer = layer;
    neutralize_layer_transform_for_effect_cache(local_layer, 1.0, 0.0, 0.0);
    render_layer_unmasked_with_stackable_effects(layer_cr.get(), data, title, local_layer,
                                                 title_time, canvas_w, canvas_h);
    layer_cr.reset();

    cairo_save(cr);
    cairo_set_operator(cr, cairo_operator_for_layer_blend_mode(layer.blend_mode));
    apply_layer_world_transform(cr, title, layer, title_time);
    cairo_set_source_surface(cr, layer_surface.get(), surface_rect.x(), surface_rect.y());
    cairo_paint_with_alpha(cr, std::clamp(layer_chain_opacity(title, layer, title_time), 0.0, 1.0));
    cairo_restore(cr);
    return true;
}


static bool mask_mode_is_inverted(MaskMode mode)
{
    return mode == MaskMode::InvertedAlpha || mode == MaskMode::InvertedLuma;
}

/* Phase 13 intentionally has no Cairo/QPainter mask compositor.  Mask source
 * coverage remains a GPU texture (or GPU-rendered vector/text geometry), and
 * alpha/luma/inversion are resolved by kGpuMaskEffect. */

/* Full-frame CPU composition and texture-upload pipeline removed.
 * Layer-local source rasterization remains only as input generation for the
 * unified GPU compositor. */

static std::pair<TitleGpuRenderSession *, uint64_t>
acquire_gpu_readback_session(uint64_t requested_revision)
{
    std::lock_guard<std::mutex> lock(g_gpu_readback_sessions_mutex);
    auto &entry = g_gpu_readback_sessions[std::this_thread::get_id()];
    if (!entry.session)
        entry.session = title_gpu_render_session_create();
    const uint64_t revision = requested_revision != 0
        ? requested_revision
        : ++entry.revision;
    return {entry.session, revision};
}

static bool is_transparent_sparse_cache_payload(const QImage &image)
{
    if (image.isNull() || image.width() != 1 || image.height() != 1)
        return false;
    const QImage argb = image.format() == QImage::Format_ARGB32_Premultiplied
        ? image : image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    return argb.isNull() || qAlpha(argb.pixel(0, 0)) == 0;
}

static QImage render_title_gpu_layer_range_readback(
    const Title &title, double t, std::size_t first_layer,
    std::size_t last_layer, uint64_t model_revision)
{
    const auto acquired = acquire_gpu_readback_session(model_revision);
    title_gpu_render_session_update_range(acquired.first, title, t,
                                          acquired.second, first_layer,
                                          last_layer);
    QImage image = title_gpu_render_session_readback(acquired.first);

    /* A transiently incomplete GPU session used to be published as the sparse
     * 1x1 transparent marker. Once inserted into RAM/disk cache that marker was
     * treated as a valid prerender frame forever. Validate suspicious empty
     * payloads with a completely fresh render session before publication. A
     * genuinely transparent frame remains transparent after the second pass. */
    if (is_transparent_sparse_cache_payload(image)) {
        TitleGpuRenderSession *verification = title_gpu_render_session_create();
        if (verification) {
            const uint64_t verification_revision = model_revision != 0
                ? model_revision + 1 : acquired.second + 1;
            title_gpu_render_session_update_range(verification, title, t,
                                                  verification_revision,
                                                  first_layer, last_layer);
            QImage verified = title_gpu_render_session_readback(verification);
            title_gpu_render_session_destroy(verification);
            if (!verified.isNull())
                image = std::move(verified);
        }
    }
    return image;
}

QImage render_title_region_to_image(const Title &title, double t,
                                    const QRect &region,
                                    uint64_t model_revision)
{
    const QImage full = render_title_gpu_frame_readback(title, t, model_revision);
    if (full.isNull())
        return QImage();
    const QRect clipped = region.intersected(full.rect());
    return clipped.isEmpty() ? QImage() : full.copy(clipped);
}

QImage render_title_to_image(const Title &title, double t,
                             uint64_t model_revision)
{
    return render_title_gpu_frame_readback(title, t, model_revision);
}

QImage render_title_cache_region_to_image(const Title &title, double t,
                                          const QRect &region,
                                          uint64_t model_revision)
{
    const QImage full = render_title_cache_to_image(title, t, model_revision);
    if (full.isNull())
        return QImage();
    const QRect clipped = region.intersected(full.rect());
    return clipped.isEmpty() ? QImage() : full.copy(clipped);
}

QImage render_title_cache_to_image(const Title &title, double t,
                                   uint64_t model_revision)
{
    const TitleDynamicLayerAnalysis analysis = analyze_title_dynamic_layers(title);
    if (analysis.has_dynamic_layers && !analysis.has_cacheable_prefix)
        return QImage();
    const std::size_t cache_end = analysis.has_dynamic_layers
        ? analysis.first_dynamic_layer
        : title.layers.size();
    return render_title_gpu_layer_range_readback(title, t, 0, cache_end,
                                                 model_revision);
}

QImage render_title_over_cached_frame(const Title &title, double t,
                                      const QImage &cached_prefix,
                                      uint64_t model_revision)
{
    const TitleDynamicLayerAnalysis analysis = analyze_title_dynamic_layers(title);
    if (!analysis.has_dynamic_layers)
        return cached_prefix;
    if (!analysis.has_cacheable_prefix)
        return render_title_gpu_frame_readback(title, t, model_revision);

    const auto acquired = acquire_gpu_readback_session(model_revision);
    if (!title_gpu_render_session_submit_cached_prefix(
            acquired.first, title, cached_prefix, t,
            analysis.first_dynamic_layer, acquired.second))
        return render_title_gpu_frame_readback(title, t, model_revision);
    return title_gpu_render_session_readback(acquired.first);
}

void release_title_gpu_render_resources()
{
    title_gpu_frame_cache_clear();
    {
        std::lock_guard<std::mutex> lock(g_gpu_readback_sessions_mutex);
        for (auto &entry : g_gpu_readback_sessions)
            title_gpu_render_session_destroy(entry.second.session);
        g_gpu_readback_sessions.clear();
    }
    std::lock_guard<std::mutex> lock(g_temporal_gpu_mutex);
    obs_enter_graphics();
    destroy_temporal_gpu_resources_locked();
    obs_leave_graphics();
}

QImage render_title_to_image_scaled(const Title &title, double t, double scale,
                                    bool /*editor_draft*/)
{
    const double clamped_scale = std::clamp(scale, 0.125, 1.0);
    QImage image = render_title_gpu_frame_readback(title, t);
    if (image.isNull())
        return image;
    if (clamped_scale < 0.999) {
        image = image.scaled(std::max(1, qRound(title.width * clamped_scale)),
                             std::max(1, qRound(title.height * clamped_scale)),
                             Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    image.setText(QStringLiteral("obs_bgs_preview_scale"),
                  QString::number(clamped_scale, 'f', 6));
    bgs::cache_frame_payload::set_placement(
        image, 0, 0, std::max(1, title.width), std::max(1, title.height));
    return image;
}

static void title_gpu_render_session_prepare_auxiliary_layers(
    TitleGpuRenderSession *session, const Title &title, double time,
    uint64_t model_revision, const std::vector<std::string> &layer_ids);
static gs_texture_t *title_gpu_render_session_render_auxiliary_layer(
    TitleGpuRenderSession *session, const std::string &layer_id,
    double title_time);

/* ══════════════════════════════════════════════════════════════════
 *  OBS source callbacks
 * ══════════════════════════════════════════════════════════════════ */
static const char *source_get_name(void *)
{
    return bgl_tr_c("OBSTitles.SourceName");
}

static void *source_create(obs_data_t *settings, obs_source_t *source)
{
    auto *data = new TitleSourceData();
    data->source = source;
    data->gpu_render_session = title_gpu_render_session_create();
    if (settings) {
        data->title_id = obs_data_get_string(settings, PROP_TITLE_ID);
        refresh_scene_mask_configs(data, settings);
    }
    data->cache_wake_state = std::make_shared<SourceCacheWakeState>();
    bind_source_cache_wake_title(data->cache_wake_state, data->title_id);
    const std::weak_ptr<SourceCacheWakeState> weak_wake_state =
        data->cache_wake_state;
    data->cache_frame_ready_connection = QObject::connect(
        &CacheManager::instance(), &CacheManager::frameReady,
        [weak_wake_state](const QString &title_id, int frame) {
            const auto state = weak_wake_state.lock();
            if (!state)
                return;
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->title_id != title_id.toStdString())
                return;
            state->ready_frames.insert(frame);
            /* A source normally consumes only the current frame. Bound stale
             * notifications so a long background prerender cannot grow this
             * per-source wake set without limit. Extra wakeups are harmless:
             * the source always revalidates the exact content hash. */
            while (state->ready_frames.size() > 512)
                state->ready_frames.erase(state->ready_frames.begin());
        });
    data->last_tick = std::chrono::steady_clock::now();
    data->last_clock_refresh = data->last_tick;
    if (auto title = TitleDataStore::instance().get_title(data->title_id)) {
        data->seen_cue_revision = title->cue_revision;
        data->force_cue_state_sync = title->current_cue_row >= 0 ||
            title->pending_cue_row >= 0 || title->cue_uncue_requested;
    }
    /* Force one complete store/session reconciliation after startup. Source
     * creation can run while the title store and cache index are still being
     * restored; adopting the current revision here can otherwise make a blank
     * GPU session look permanently clean until another cue changes runtime
     * state. */
    data->seen_store_revision = std::numeric_limits<uint64_t>::max();
    data->playing = false;
    data->waiting_for_cue = true;
    data->active_cue_row = -1;
    data->output_visible.store(true, std::memory_order_release);
    data->dirty = true;
    data->first_frame_pending = true;
    data->consecutive_draw_failures = 0;
    BGL_LOG_INFO("Source", QStringLiteral(
        "Created OBS source=%1 title=%2 session=%3")
        .arg(reinterpret_cast<quintptr>(source), 0, 16)
        .arg(QString::fromStdString(data->title_id))
        .arg(reinterpret_cast<quintptr>(data->gpu_render_session), 0, 16));
    return data;
}

static void source_destroy(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data) return;
    BGL_LOG_INFO("Source", QStringLiteral(
        "Destroying OBS source title=%1 session=%2")
        .arg(QString::fromStdString(data->title_id))
        .arg(reinterpret_cast<quintptr>(data->gpu_render_session), 0, 16));
    QObject::disconnect(data->cache_frame_ready_connection);
    data->cache_wake_state.reset();
    release_active_scene_mask_scenes(data);
    title_gpu_render_session_destroy(data->gpu_render_session);
    data->gpu_render_session = nullptr;
    {
        std::lock_guard<std::mutex> lock(data->texture_mutex);
        obs_enter_graphics();
        if (data->scene_mask_scene_texrender) gs_texrender_destroy(data->scene_mask_scene_texrender);
        if (data->scene_mask_effect) gs_effect_destroy(data->scene_mask_effect);
        obs_leave_graphics();
        data->scene_mask_scene_texrender = nullptr;
        data->scene_mask_effect = nullptr;
        data->tex_w = 0;
        data->tex_h = 0;
    }
    delete data;
}

static void source_update(void *priv, obs_data_t *settings)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data || !settings) return;
    const std::string previous_title_id = data->title_id;
    const bool keep_scene_masks_foreground = data->scene_mask_foreground_active;
    if (keep_scene_masks_foreground)
        release_active_scene_mask_scenes(data);

    data->title_id = obs_data_get_string(settings, PROP_TITLE_ID);
    const bool title_changed = data->title_id != previous_title_id;
    if (title_changed) {
        request_source_presentation_reset(data, "source-title-binding-changed");
        data->visual_identity_hash.clear();
        ++data->visual_model_revision;
        data->title_missing = false;
    } else {
        request_source_presentation_reset(data, "source-settings-updated");
    }
    bind_source_cache_wake_title(data->cache_wake_state, data->title_id);
    refresh_scene_mask_configs(data, settings);
    data->playhead = 0.0;
    data->playback_reverse = false;
    data->cue_phase = TitleSourceData::CuePhase::FreeRun;
    data->playing = false;
    data->output_visible.store(true, std::memory_order_release);
    data->waiting_for_cue = true;
    data->active_cue_row = -1;
    if (auto title = TitleDataStore::instance().get_title(data->title_id)) {
        data->seen_cue_revision = title->cue_revision;
        /* The dock selection and OBS source-settings update are asynchronous.
         * A cue can therefore be issued for the newly selected title before
         * this callback runs. Merely adopting cue_revision here marks that cue
         * as consumed, leaving the source stopped on cached frame zero. Force
         * one state-machine pass whenever the newly bound title already has an
         * active or pending cue. */
        data->force_cue_state_sync = title_changed &&
            (title->current_cue_row >= 0 || title->pending_cue_row >= 0);
        if (data->force_cue_state_sync) {
            BGL_LOG_INFO("LiveCue", QStringLiteral("Rebinding active cue after title switch old=%1 new=%2 current=%3 pending=%4 revision=%5")
                         .arg(QString::fromStdString(previous_title_id))
                         .arg(QString::fromStdString(data->title_id))
                         .arg(title->current_cue_row)
                         .arg(title->pending_cue_row)
                         .arg(static_cast<qulonglong>(title->cue_revision)));
        }
    } else {
        data->seen_cue_revision = 0;
        data->force_cue_state_sync = false;
    }
    data->last_clock_refresh = std::chrono::steady_clock::now();
    data->seen_store_revision = std::numeric_limits<uint64_t>::max();
    data->cache_hash_revision = std::numeric_limits<uint64_t>::max();
    data->cached_content_hash.clear();
    data->effect_layer_cache.clear();
    data->dirty = true;
    data->first_frame_pending = true;
    data->consecutive_draw_failures = 0;
    if (keep_scene_masks_foreground)
        sync_scene_mask_scenes_for_cue(data, TitleDataStore::instance().get_title(data->title_id));
    BGL_LOG_INFO("Source", QStringLiteral(
        "Updated source binding old=%1 new=%2 changed=%3")
        .arg(QString::fromStdString(previous_title_id),
             QString::fromStdString(data->title_id))
        .arg(title_changed ? 1 : 0));
}

static void source_activate(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data)
        return;
    data->scene_mask_foreground_active = true;
    /* OBS can deactivate/reactivate sources while graphics resources or cache
     * payloads are being restored. Never trust the previous clean flag across
     * activation; require a fresh successful draw. */
    data->dirty = true;
    data->first_frame_pending = true;
    data->consecutive_draw_failures = 0;
    BGL_LOG_DEBUG("Source", QStringLiteral("Activated source title=%1")
        .arg(QString::fromStdString(data->title_id)));

    auto title = TitleDataStore::instance().get_title(data->title_id);
    sync_scene_mask_scenes_for_cue(data, title);
    if (!title || !title->playlist_restart_on_source_active)
        return;

    const int row_count = live_text_playlist_row_count(*title);
    if (row_count <= 0)
        return;

    title->playlist_active = true;
    title->playlist_next_row = title->playlist_reverse ? row_count - 1 : 0;
    title->playlist_next_due_ms = 1;
    title->playlist_stop_after_due = false;
    title->current_cue_row = -1;
    title->pending_cue_row = -1;
    title->cue_uncue_requested = false;
    title->cue_persistence_transition = false;
    title->cue_persistent_text_columns.clear();
    ++title->cue_revision;
    TitleDataStore::instance().touch_runtime_change();
}

static void source_deactivate(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data)
        return;
    data->scene_mask_foreground_active = false;
    BGL_LOG_DEBUG("Source", QStringLiteral("Deactivated source title=%1")
        .arg(QString::fromStdString(data->title_id)));
    release_active_scene_mask_scenes(data);
    /* A deactivated source may remain alive across a scene or collection
     * change. Do not allow its old poster texture to become visible again
     * before the reactivated source has reconciled its current title. */
    request_source_presentation_reset(data, "source-deactivated");

    auto title = TitleDataStore::instance().get_title(data->title_id);
    if (!title || !title->playlist_stop_on_source_inactive || !title->playlist_active)
        return;

    title->playlist_active = false;
    title->playlist_next_due_ms = 0;
    title->playlist_stop_after_due = false;
    TitleDataStore::instance().touch_runtime_change();
}

static void source_show(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data)
        return;
    /* show/hide covers projector and Studio-preview visibility as well as the
     * main output. Reconcile the current model before the source becomes
     * eligible to present a poster retained while it was not shown anywhere. */
    data->shown_on_display.store(true, std::memory_order_release);
    BGL_LOG_TRACE("Source", QStringLiteral("Source shown title=%1")
        .arg(QString::fromStdString(data->title_id)));
    request_source_presentation_reset(data, "source-shown");
    data->dirty = true;
    data->first_frame_pending = true;
}

static void source_hide(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data)
        return;
    data->shown_on_display.store(false, std::memory_order_release);
    BGL_LOG_TRACE("Source", QStringLiteral("Source hidden title=%1")
        .arg(QString::fromStdString(data->title_id)));
    release_active_scene_mask_scenes(data);
    request_source_presentation_reset(data, "source-hidden");
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
    if (!data)
        return;

    if (!source_presentation_generation_is_current(data))
        apply_source_presentation_reset(data, "generation-boundary");

    if (g_source_scene_collection_transition.load(
            std::memory_order_acquire))
        return;

    if (data->title_id.empty())
        return;

    auto title = TitleDataStore::instance().get_title(data->title_id);
    if (!title) {
        /* The title store may finish restoring after OBS creates its sources.
         * Keep retrying, but invalidate the old GPU poster immediately. The
         * previous implementation left final_texture valid here, so a source
         * rebound during project/scene-collection loading could retain artwork
         * from a title that no longer existed. */
        if (!data->title_missing) {
            data->title_missing = true;
            request_source_presentation_reset(data, "title-missing");
            apply_source_presentation_reset(data, "title-missing");
            data->visual_identity_hash.clear();
            ++data->visual_model_revision;
        }
        data->dirty = true;
        data->first_frame_pending = true;
        return;
    }
    if (data->title_missing) {
        data->title_missing = false;
        request_source_presentation_reset(data, "title-restored");
        apply_source_presentation_reset(data, "title-restored");
        data->visual_identity_hash.clear();
        ++data->visual_model_revision;
    }

    if (data->force_cue_state_sync || title->cue_revision != data->seen_cue_revision) {
        const int requested_cue_row = title->pending_cue_row >= 0
            ? title->pending_cue_row
            : title->current_cue_row;
        const bool is_uncue_request = title->cue_uncue_requested &&
            title->pending_cue_row < 0 && title->current_cue_row >= 0;
        bool cache_gate_ready = true;
        if (!is_uncue_request && requested_cue_row >= 0 &&
            requested_cue_row < static_cast<int>(title->live_text_rows.size()) &&
            CacheManager::instance().cacheEnabled() &&
            CacheManager::instance().titleCacheability(title) != TitleCacheability::NonCacheable) {
            const auto now = std::chrono::steady_clock::now();
            const bool target_changed = data->cue_cache_check_title_id != title->id ||
                data->cue_cache_check_revision != title->cue_revision ||
                data->cue_cache_check_row != requested_cue_row;
            const bool retry_due = data->last_cue_cache_check.time_since_epoch().count() == 0 ||
                now - data->last_cue_cache_check >= std::chrono::milliseconds(100);
            if (target_changed || retry_due) {
                data->cue_cache_check_ready =
                    CacheManager::instance().prepareLiveCueForPlayback(title, requested_cue_row);
                data->cue_cache_check_title_id = title->id;
                data->cue_cache_check_revision = title->cue_revision;
                data->cue_cache_check_row = requested_cue_row;
                data->last_cue_cache_check = now;
            }
            cache_gate_ready = data->cue_cache_check_ready;
        }

        if (!cache_gate_ready) {
            /* Keep the current cue/frame running, but do not consume the new cue
             * revision. The request is retried after the required prefix/transition
             * is resident in either RAM or SSD. */
            data->waiting_for_cue = true;
            data->dirty = true;
        } else {
            data->force_cue_state_sync = false;
            double loop_end = std::clamp(title->loop_end, title->loop_start, title->duration);
            double pause_time = std::clamp(title->pause_time, 0.0, title->duration);
            bool has_pending = title->pending_cue_row >= 0 &&
                               title->pending_cue_row < (int)title->live_text_rows.size();
            bool has_current = title->current_cue_row >= 0 &&
                               title->current_cue_row < (int)title->live_text_rows.size();
            bool is_uncue = title->cue_uncue_requested && !has_pending && has_current;
            data->manual_uncue = is_uncue;
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
            if (!data->manual_uncue)
                set_source_output_visible(data, true, "cue-started");
            data->dirty = true;
        }
    }

    const bool has_clock_layer = title_has_clock_layer(title);
    const bool has_ticker_layer = title_has_ticker_layer(title);
    const bool has_timeline_animation = title_has_animation(title);
    const bool static_clock_title = has_clock_layer && !has_timeline_animation;

    if (data->playing && (!static_clock_title ||
                          data->cue_phase != TitleSourceData::CuePhase::FreeRun)) {
        double dt = (double)seconds;
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
                /* The row remains cued until this exact point.  Once the
                 * outro has completed, clear the runtime cue status and apply
                 * the title's configured end behavior instead of forcing every
                 * manual uncue to hide the source. */
                title->current_cue_row = -1;
                title->pending_cue_row = -1;
                title->cue_uncue_requested = false;
                title->cue_persistence_transition = false;
                title->cue_persistent_text_columns.clear();
                data->active_cue_row = -1;
                if (title->cue_end_behavior == 1) {
                    set_source_output_visible(data, false,
                                              "cue-ended-show-nothing");
                } else {
                    set_source_output_visible(data, true,
                                              "cue-ended-hold-frame");
                    if (title->cue_end_behavior == 2)
                        data->playhead = 0.0;
                }
                data->manual_uncue = false;
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
                    if (title->cue_end_behavior == 1) {
                        title->current_cue_row = -1;
                        title->pending_cue_row = -1;
                        title->cue_persistence_transition = false;
                        title->cue_persistent_text_columns.clear();
                        data->active_cue_row = -1;
                        set_source_output_visible(data, false,
                                                  "play-once-show-nothing");
                    } else if (title->cue_end_behavior == 2) {
                        title->current_cue_row = -1;
                        title->pending_cue_row = -1;
                        title->cue_persistence_transition = false;
                        title->cue_persistent_text_columns.clear();
                        data->active_cue_row = -1;
                        data->playhead = 0.0;
                        set_source_output_visible(data, true,
                                                  "play-once-show-first-frame");
                    } else {
                        title->pending_cue_row = -1;
                        title->cue_persistence_transition = false;
                        title->cue_persistent_text_columns.clear();
                        data->active_cue_row = title->current_cue_row;
                        set_source_output_visible(data, true,
                                                  "play-once-hold-last-frame");
                    }
                    title->cue_uncue_requested = false;
                    data->manual_uncue = false;
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
        const QString next_visual_identity =
            CacheManager::instance().contentHashForTitle(*title);
        const bool visual_identity_changed =
            !data->visual_identity_hash.isEmpty() &&
            data->visual_identity_hash != next_visual_identity;
        if (visual_identity_changed) {
            ++data->visual_model_revision;
            request_source_presentation_reset(data,
                                              "title-visual-identity-changed");
            apply_source_presentation_reset(data,
                                            "title-visual-identity-changed");
        }
        data->visual_identity_hash = next_visual_identity;
        data->cached_content_hash = next_visual_identity;
        data->cache_hash_revision = revision;
        if (data->source) {
            obs_data_t *settings = obs_source_get_settings(data->source);
            if (settings) {
                const bool keep_scene_masks_foreground = data->scene_mask_foreground_active;
                if (keep_scene_masks_foreground)
                    release_active_scene_mask_scenes(data);
                refresh_scene_mask_configs(data, settings);
                if (keep_scene_masks_foreground)
                    sync_scene_mask_scenes_for_cue(data, title);
                obs_data_release(settings);
            }
        }
        data->effect_layer_cache.clear();
        data->dirty = true;
    }

    sync_scene_mask_scenes_for_cue(data, title);

    if (!data->output_visible.load(std::memory_order_acquire)) {
        data->dirty = false;
        return;
    }

    /* A static source may have rendered a live fallback and become clean while
     * the cache worker was still producing the same frame. Wake it when that
     * exact frame arrives so prerendered output is adopted without waiting for
     * an unrelated model or cue change. */
    const int current_cache_frame =
        CacheManager::instance().frameIndexForTitleTime(*title,
                                                        data->playhead);
    if (take_source_cache_wake_frame(data->cache_wake_state, data->title_id,
                                     current_cache_frame)) {
        data->dirty = true;
        if (TitlePreferences::cache_playback_logging_enabled()) {
            BGL_LOG_INFO(
                "CachePlayback",
                QStringLiteral("consumer=source action=frame-ready-wakeup title=%1 time=%2 frame=%3")
                    .arg(QString::fromStdString(data->title_id))
                    .arg(data->playhead, 0, 'f', 6)
                    .arg(current_cache_frame));
        }
    }

    /* Static titles keep their last GPU graph result. Avoid cache lookups,
     * title snapshots and layer-key walks on every OBS video tick when neither
     * the timeline, clock/ticker content nor the model changed. Scene masks are
     * rendered independently in video_render and therefore remain live. */
    if (!data->dirty && !data->first_frame_pending)
        return;

    /* Live rendering stays inside one GPU session. A completed prerender frame
     * may be submitted as the session's final texture; cache misses fall back
     * only to the real-time GPU graph, never to the retired CPU compositor. */
    data->tex_w = clamped_source_dimension(title->width);
    data->tex_h = clamped_source_dimension(title->height);
    if (data->gpu_render_session) {
        bool submitted_cached_frame = false;
        CacheManager &cache = CacheManager::instance();
        const CachePlaybackSettings playback = cache.playbackSettings();
        const TitleCacheability cacheability = cache.titleCacheability(title);
        if (cache.cacheEnabled() && cacheability != TitleCacheability::NonCacheable) {
            if (data->cached_content_hash.isEmpty() ||
                data->cache_hash_revision != revision) {
                data->cached_content_hash = cache.contentHashForTitle(*title);
                data->cache_hash_revision = revision;
            }

            int cached_cue_row = title->current_cue_row;
            if (cached_cue_row < 0 && data->manual_uncue &&
                data->cue_phase == TitleSourceData::CuePhase::OutroOnly)
                cached_cue_row = data->active_cue_row;

            /* Live-cue prerenders use a cue-applied title snapshot and
             * therefore a different content-addressed GPU key from the base
             * title. Asking for the base key here could successfully return a
             * resident frame with the wrong text/persistence state, so choose
             * the same cue-aware key family as the QImage fallback. */
            const QString gpu_cache_token = cached_cue_row >= 0 &&
                    cached_cue_row < static_cast<int>(title->live_text_rows.size())
                ? cache.requestLiveCueFrameGpuToken(
                      title, cached_cue_row, data->playhead, false)
                : cache.requestFrameGpuToken(
                      title, data->playhead, false, data->cached_content_hash);
            QImage cached;
            if (gpu_cache_token.isEmpty()) {
                if (cached_cue_row >= 0 &&
                    cached_cue_row < static_cast<int>(title->live_text_rows.size())) {
                    cached = cache.requestLiveCueFrameRealtime(
                        title, cached_cue_row, data->playhead,
                        data->cached_content_hash);
                } else {
                    cached = cache.requestFrameRealtime(
                        title, data->playhead, data->cached_content_hash);
                }
            }

            if (TitlePreferences::cache_playback_logging_enabled()) {
                BGL_LOG_INFO("CachePlayback", QStringLiteral("consumer=source title=%1 time=%2 cueRow=%3 payload=%4 cacheKey=%5 size=%6x%7 cachedOnly=%8")
                    .arg(QString::fromStdString(title->id)).arg(data->playhead, 0, 'f', 6)
                    .arg(cached_cue_row).arg(cached.isNull() ? QStringLiteral("null") : QStringLiteral("ready"))
                    .arg(cached.isNull() ? 0 : cached.cacheKey()).arg(cached.width()).arg(cached.height())
                    .arg(playback.cached_frames_only));
            }

            if (!gpu_cache_token.isEmpty()) {
                if (cacheability == TitleCacheability::PartiallyCacheable) {
                    const TitleDynamicLayerAnalysis analysis =
                        analyze_title_dynamic_layers(*title);
                    if (analysis.has_cacheable_prefix) {
                        submitted_cached_frame =
                            title_gpu_render_session_submit_gpu_cached_prefix(
                                data->gpu_render_session, *title,
                                gpu_cache_token.toStdString(), data->playhead,
                                analysis.first_dynamic_layer,
                                data->visual_model_revision);
                    }
                } else {
                    submitted_cached_frame =
                        title_gpu_render_session_submit_gpu_cached_frame(
                            data->gpu_render_session, *title,
                            gpu_cache_token.toStdString(),
                            data->visual_model_revision);
                }
                if (TitlePreferences::cache_playback_logging_enabled()) {
                    BGL_LOG_INFO("CachePlayback", QStringLiteral(
                        "consumer=source action=%1 title=%2 time=%3 gpuToken=%4")
                        .arg(submitted_cached_frame
                                 ? QStringLiteral("submit-gpu-ram")
                                 : QStringLiteral("reject-gpu-ram"))
                        .arg(QString::fromStdString(title->id))
                        .arg(data->playhead, 0, 'f', 6)
                        .arg(gpu_cache_token));
                }
            } else if (!cached.isNull()) {
                if (cacheability == TitleCacheability::PartiallyCacheable) {
                    const TitleDynamicLayerAnalysis analysis =
                        analyze_title_dynamic_layers(*title);
                    if (analysis.has_cacheable_prefix) {
                        submitted_cached_frame =
                            title_gpu_render_session_submit_cached_prefix(
                                data->gpu_render_session, *title, cached,
                                data->playhead, analysis.first_dynamic_layer,
                                data->visual_model_revision);
                        if (TitlePreferences::cache_playback_logging_enabled())
                            BGL_LOG_INFO("CachePlayback", QStringLiteral("consumer=source action=%1 title=%2 time=%3 firstDynamicLayer=%4 cacheKey=%5")
                                .arg(submitted_cached_frame ? QStringLiteral("submit-prefix")
                                                           : QStringLiteral("reject-prefix"))
                                .arg(QString::fromStdString(title->id)).arg(data->playhead, 0, 'f', 6)
                                .arg(analysis.first_dynamic_layer).arg(cached.cacheKey()));
                    }
                } else {
                    submitted_cached_frame =
                        title_gpu_render_session_submit_final_frame(
                            data->gpu_render_session, *title, cached,
                            data->visual_model_revision);
                    if (TitlePreferences::cache_playback_logging_enabled())
                        BGL_LOG_INFO("CachePlayback", QStringLiteral("consumer=source action=%1 title=%2 time=%3 cacheKey=%4")
                            .arg(submitted_cached_frame ? QStringLiteral("submit-final")
                                                       : QStringLiteral("reject-final"))
                            .arg(QString::fromStdString(title->id)).arg(data->playhead, 0, 'f', 6).arg(cached.cacheKey()));
                }
                if (!submitted_cached_frame) {
                    cache.rejectFramePayload(title, data->playhead,
                                             data->cached_content_hash);
                    if (playback.cached_frames_only &&
                        !data->first_frame_pending) {
                        /* Once a valid frame has been published, cached-only
                         * playback holds it until the repaired payload arrives. */
                        data->dirty = true;
                        return;
                    }
                    /* A source with no published texture must never remain
                     * permanently transparent. Bootstrap one live poster frame;
                     * subsequent cache misses still hold the published result. */
                }
            } else if (playback.cached_frames_only &&
                       !data->first_frame_pending) {
                /* Keep the last valid GPU texture while the worker prepares the
                 * requested frame. Do not synchronously render on video_tick. */
                data->dirty = true;
                return;
            } else if (playback.cached_frames_only &&
                       data->first_frame_pending &&
                       TitlePreferences::cache_playback_logging_enabled()) {
                BGL_LOG_WARNING("CachePlayback", QStringLiteral(
                    "consumer=source action=bootstrap-live-poster title=%1 time=%2 reason=no-published-cache-frame")
                    .arg(QString::fromStdString(title->id))
                    .arg(data->playhead, 0, 'f', 6));
            }
        }

        if (!submitted_cached_frame) {
            title_gpu_render_session_update(data->gpu_render_session, *title,
                                            data->playhead,
                                            data->visual_model_revision);
        }

        /* Scene-mask layers are auxiliary GPU inputs. They must remain
         * available even when the visible title is supplied by a completed
         * cached frame, but they never force the cached artwork itself to be
         * recomposited. */
        if (!data->scene_masks.empty()) {
            std::vector<std::string> mask_layer_ids;
            mask_layer_ids.reserve(data->scene_masks.size());
            for (const auto &cfg : data->scene_masks) {
                if (!cfg.layer_id.empty())
                    mask_layer_ids.push_back(cfg.layer_id);
            }
            title_gpu_render_session_prepare_auxiliary_layers(
                data->gpu_render_session, *title, data->playhead,
                data->visual_model_revision,
                mask_layer_ids);
        }
        /* video_tick only prepares the GPU graph. The source becomes clean
         * after video_render proves that the session actually produced and
         * drew a texture. This closes the startup/cache race where a failed
         * first render was marked clean forever. */
        data->dirty = data->first_frame_pending;
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
    const LayerTransitionVisualState transition = evaluate_layer_general_transitions(
        layer.transitions, layer.in_time, layer.out_time, title_time);
    gs_matrix_translate3f((float)(layer.position.evaluate(lt).x + transition.translate_x),
                          (float)(layer.position.evaluate(lt).y + transition.translate_y), 0.0f);
    gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, (float)(layer.rotation.evaluate(lt) * kPi / 180.0));
    gs_matrix_scale3f((float)(layer.scale.evaluate(lt).x * transition.scale),
                      (float)(layer.scale.evaluate(lt).y * transition.scale), 1.0f);
}


/* ══════════════════════════════════════════════════════════════════
 *  GPU-only title compositor
 *
 *  CPU work is limited to producing reusable, transform-neutral layer
 *  rasters (text glyph coverage, vector tessellation and decoded images).
 *  Full-frame composition, transforms, masks, blend modes, stack effects and
 *  temporal accumulation remain in OBS graphics resources.  Editor and live
 *  output share this exact session implementation.
 * ══════════════════════════════════════════════════════════════════ */

static constexpr const char *kGpuFrameBlitEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
sampler_state textureSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };
struct VertDataIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VertDataOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertDataOut VSDefault(VertDataIn v)
{
    VertDataOut o;
    o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj);
    o.uv = v.uv;
    return o;
}
float4 PSBlit(VertDataOut v) : TARGET
{
    return image.Sample(textureSampler, v.uv);
}
technique Draw
{
    pass
    {
        vertex_shader = VSDefault(v);
        pixel_shader = PSBlit(v);
    }
}
)";

static constexpr const char *kGpuLayerCopyEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform texture2d transitionMatte;
uniform float weight;
uniform float wipeProgress;
uniform float wipeSoftness;
uniform int wipeDirection;
uniform int transitionType;
uniform int blocksColumns;
uniform int blocksRows;
uniform float randomSeed;
uniform int transitionMatteEnabled;
uniform int imageChannel;
uniform int transitionInvert;
uniform int transitionClockwise;
uniform float2 transitionCenter;
uniform float transitionRotation;
uniform float transitionAspect;
uniform int transitionProfile;
uniform int imageClipEnabled;
uniform float4 imageClipRect;
uniform float4 imageCornerRadii;
uniform float2 imageLogicalSize;
sampler_state textureSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };
struct VertDataIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VertDataOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertDataOut VSDefault(VertDataIn v) { VertDataOut o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }
float hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32 + randomSeed);
    return frac(p.x * p.y);
}
float directional_wipe(float2 uv, float reveal, float feather)
{
    if (wipeDirection == 2) {
        float edge = 1.0 - reveal;
        return wipeSoftness > 0.000001 ? smoothstep(edge, edge + feather, uv.x) : step(edge, uv.x);
    }
    if (wipeDirection == 3) {
        float edge = 1.0 - reveal;
        return wipeSoftness > 0.000001 ? smoothstep(edge, edge + feather, uv.y) : step(edge, uv.y);
    }
    if (wipeDirection == 4)
        return wipeSoftness > 0.000001 ? 1.0 - smoothstep(reveal - feather, reveal, uv.y) : 1.0 - step(reveal, uv.y);
    return wipeSoftness > 0.000001 ? 1.0 - smoothstep(reveal - feather, reveal, uv.x) : 1.0 - step(reveal, uv.x);
}
float transition_alpha(float2 uv)
{
    float reveal = clamp(wipeProgress, 0.0, 1.0);
    if (reveal >= 0.999999)
        return 1.0;
    float feather = max(wipeSoftness, 0.000001);
    float alpha = 1.0;
    if (transitionType == 14) {
        float2 grid = float2(max(blocksColumns, 1), max(blocksRows, 1));
        float2 cell = floor(uv * grid);
        float threshold = hash21(cell);
        if (transitionInvert != 0) threshold = 1.0 - threshold;
        alpha = wipeSoftness > 0.000001 ? smoothstep(threshold - feather, threshold + feather, reveal) : step(threshold, reveal);
    } else if (transitionType == 15) {
        float matte = uv.x;
        if (transitionMatteEnabled != 0) {
            float4 m = transitionMatte.Sample(textureSampler, uv);
            if (imageChannel == 1) matte = m.a;
            else if (imageChannel == 2) matte = m.r;
            else if (imageChannel == 3) matte = m.g;
            else if (imageChannel == 4) matte = m.b;
            else matte = dot(m.rgb, float3(0.2126, 0.7152, 0.0722));
        }
        if (transitionInvert != 0) matte = 1.0 - matte;
        alpha = smoothstep(matte - feather, matte + feather, reveal);
    } else if (transitionType == 16) {
        float2 p = uv - transitionCenter;
        float angle = atan2(p.y, p.x) / 6.28318530718 + 0.5 + transitionRotation / 360.0;
        angle = frac(angle + 1.0);
        if (transitionClockwise == 0) angle = 1.0 - angle;
        if (transitionInvert != 0) angle = 1.0 - angle;
        alpha = smoothstep(angle - feather, angle + feather, reveal);
    } else if (transitionType == 17) {
        float2 p = (uv - transitionCenter) * 2.0;
        p.x *= max(transitionAspect, 0.01);
        float distanceValue = length(p) / 1.41421356;
        if (transitionProfile == 1) distanceValue = max(abs(p.x), abs(p.y));
        else if (transitionProfile == 2) distanceValue = (abs(p.x) + abs(p.y)) * 0.5;
        distanceValue = clamp(distanceValue, 0.0, 1.0);
        if (transitionInvert != 0) distanceValue = 1.0 - distanceValue;
        alpha = smoothstep(distanceValue - feather, distanceValue + feather, reveal);
    } else if (transitionType == 18) {
        float2 p = uv - transitionCenter;
        float radians = transitionRotation * 0.01745329252;
        float2 q = float2(p.x * cos(radians) - p.y * sin(radians), p.x * sin(radians) + p.y * cos(radians));
        q.x *= max(transitionAspect, 0.01);
        float matte = q.x + 0.5;
        if (transitionProfile == 1) matte = length(q) * 1.41421356;
        else if (transitionProfile == 2) matte = (abs(q.x) + abs(q.y));
        matte = clamp(matte, 0.0, 1.0);
        if (transitionInvert != 0) matte = 1.0 - matte;
        alpha = smoothstep(matte - feather, matte + feather, reveal);
    } else if (wipeDirection != 0) {
        alpha = directional_wipe(uv, reveal, feather);
    }
    return clamp(alpha, 0.0, 1.0);
}
float rounded_box_alpha(float2 p, float4 rect, float4 radii)
{
    float2 rectMin = rect.xy;
    float2 rectMax = rect.xy + rect.zw;
    if (p.x < rectMin.x || p.y < rectMin.y || p.x > rectMax.x || p.y > rectMax.y)
        return 0.0;
    float2 local = p - rectMin;
    float2 size = max(rect.zw, float2(0.0001, 0.0001));
    float maxRadius = min(size.x, size.y) * 0.5;
    float4 clampedRadii = clamp(radii, 0.0, maxRadius);
    float radius = 0.0;
    float2 center = float2(0.0, 0.0);
    if (local.x <= clampedRadii.x && local.y <= clampedRadii.x) { radius = clampedRadii.x; center = float2(radius, radius); }
    else if (local.x >= size.x - clampedRadii.y && local.y <= clampedRadii.y) { radius = clampedRadii.y; center = float2(size.x - radius, radius); }
    else if (local.x >= size.x - clampedRadii.z && local.y >= size.y - clampedRadii.z) { radius = clampedRadii.z; center = float2(size.x - radius, size.y - radius); }
    else if (local.x <= clampedRadii.w && local.y >= size.y - clampedRadii.w) { radius = clampedRadii.w; center = float2(radius, size.y - radius); }
    else return 1.0;
    if (radius <= 0.0001) return 1.0;
    return 1.0 - smoothstep(max(0.0, radius - 1.0), radius + 1.0, length(local - center));
}
float4 PSLayer(VertDataOut v) : TARGET
{
    float clipAlpha = 1.0;
    if (imageClipEnabled != 0) {
        float2 logicalPoint = v.uv * imageLogicalSize;
        clipAlpha = rounded_box_alpha(logicalPoint, imageClipRect, imageCornerRadii);
    }
    return image.Sample(textureSampler, v.uv) * (weight * transition_alpha(v.uv) * clipAlpha);
}
technique Draw { pass { vertex_shader = VSDefault(v); pixel_shader = PSLayer(v); } }
)";


static constexpr const char *kGpuAdjustmentMixEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d originalImage;
uniform texture2d effectedImage;
uniform texture2d transitionMatte;
uniform float amount;
uniform float wipeProgress;
uniform float wipeSoftness;
uniform int wipeDirection;
uniform int transitionType;
uniform int blocksColumns;
uniform int blocksRows;
uniform float randomSeed;
uniform int transitionMatteEnabled;
uniform int imageChannel;
uniform int transitionInvert;
uniform int transitionClockwise;
uniform float2 transitionCenter;
uniform float transitionRotation;
uniform float transitionAspect;
uniform int transitionProfile;
sampler_state textureSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };
struct VertDataIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VertDataOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertDataOut VSDefault(VertDataIn v){VertDataOut o;o.pos=mul(float4(v.pos.xyz,1.0),ViewProj);o.uv=v.uv;return o;}
float hash21(float2 p){p=frac(p*float2(123.34,456.21));p+=dot(p,p+45.32+randomSeed);return frac(p.x*p.y);}
float transition_alpha(float2 uv)
{
 float r=clamp(wipeProgress,0.0,1.0);if(r>=0.999999)return 1.0;float f=max(wipeSoftness,0.000001);float a=1.0;
 if(transitionType==14){float2 c=floor(uv*float2(max(blocksColumns,1),max(blocksRows,1)));float t=hash21(c);if(transitionInvert!=0)t=1.0-t;a=smoothstep(t-f,t+f,r);}
 else if(transitionType==15){float m=uv.x;if(transitionMatteEnabled!=0){float4 x=transitionMatte.Sample(textureSampler,uv);m=imageChannel==1?x.a:imageChannel==2?x.r:imageChannel==3?x.g:imageChannel==4?x.b:dot(x.rgb,float3(0.2126,0.7152,0.0722));}if(transitionInvert!=0)m=1.0-m;a=smoothstep(m-f,m+f,r);}
 else if(transitionType==16){float2 p=uv-transitionCenter;float x=frac(atan2(p.y,p.x)/6.28318530718+0.5+transitionRotation/360.0+1.0);if(transitionClockwise==0)x=1.0-x;if(transitionInvert!=0)x=1.0-x;a=smoothstep(x-f,x+f,r);}
 else if(transitionType==17){float2 p=(uv-transitionCenter)*2.0;p.x*=max(transitionAspect,0.01);float d=length(p)/1.41421356;if(transitionProfile==1)d=max(abs(p.x),abs(p.y));else if(transitionProfile==2)d=(abs(p.x)+abs(p.y))*0.5;d=clamp(d,0.0,1.0);if(transitionInvert!=0)d=1.0-d;a=smoothstep(d-f,d+f,r);}
 else if(transitionType==18){float2 p=uv-transitionCenter;float z=transitionRotation*0.01745329252;float2 q=float2(p.x*cos(z)-p.y*sin(z),p.x*sin(z)+p.y*cos(z));q.x*=max(transitionAspect,0.01);float m=q.x+0.5;if(transitionProfile==1)m=length(q)*1.41421356;else if(transitionProfile==2)m=abs(q.x)+abs(q.y);m=clamp(m,0.0,1.0);if(transitionInvert!=0)m=1.0-m;a=smoothstep(m-f,m+f,r);}
 else if(wipeDirection!=0){if(wipeDirection==2){float e=1.0-r;a=smoothstep(e,e+f,uv.x);}else if(wipeDirection==3){float e=1.0-r;a=smoothstep(e,e+f,uv.y);}else if(wipeDirection==4)a=1.0-smoothstep(r-f,r,uv.y);else a=1.0-smoothstep(r-f,r,uv.x);}
 return clamp(a,0.0,1.0);
}
float4 PSMix(VertDataOut v):TARGET{float4 a=originalImage.Sample(textureSampler,v.uv);float4 b=effectedImage.Sample(textureSampler,v.uv);return lerp(a,b,clamp(amount*transition_alpha(v.uv),0.0,1.0));}
technique Draw{pass{vertex_shader=VSDefault(v);pixel_shader=PSMix(v);}}
)";

static constexpr const char *kGpuPrimitiveShapeEffect = R"(
uniform float4x4 ViewProj;
uniform int shapeType;
uniform float2 surfaceSize;
uniform float2 shapeSize;
uniform float2 shapeOffset;
uniform float4 cornerRadii;
uniform float4 fillColor;
uniform float4 strokeColor;
uniform float strokeWidth;
uniform int strokeAlignment;
uniform int strokeOnFront;
uniform float starInnerRadius;
uniform float starOuterRadius;
uniform int vertexCount;
struct VertDataIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VertDataOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertDataOut VSDefault(VertDataIn v) { VertDataOut o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }
float coverage(float distanceValue, float aa) { return 1.0 - smoothstep(-aa, aa, distanceValue); }
float sdRoundBox(float2 p, float2 b, float4 radii) {
    float radius = p.x < 0.0
        ? (p.y < 0.0 ? radii.x : radii.w)
        : (p.y < 0.0 ? radii.y : radii.z);
    radius = min(max(radius, 0.0), min(b.x, b.y));
    float2 q = abs(p) - b + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}
float sdEllipse(float2 p, float2 ab) {
    float2 a = max(ab, float2(0.0001, 0.0001));
    return (length(p / a) - 1.0) * min(a.x, a.y);
}
float sdPolygonUnit(float2 p, int n) {
    float halfSector = 3.14159265359 / max((float)n, 3.0);
    float fullSector = halfSector * 2.0;
    float angleFromTop = atan2(p.y, p.x) + 1.57079632679;
    angleFromTop -= floor(angleFromTop / fullSector) * fullSector;
    float sideNormalDelta = angleFromTop - halfSector;
    return length(p) * cos(sideNormalDelta) - cos(halfSector);
}
float sdStarUnit(float2 p, float innerRadius, float outerRadius, int points) {
    float count = max((float)points, 2.0);
    float sector = 3.14159265359 / count;
    float a = atan2(p.y, p.x) + 1.57079632679;
    float k = floor(0.5 + a / sector);
    float local = k * sector - a;
    float radius = fmod(abs(k), 2.0) < 0.5 ? outerRadius : innerRadius;
    return cos(local) * length(p) - radius;
}
float shapeDistance(float2 p, float2 halfSize) {
    if (shapeType == 2)
        return sdEllipse(p, halfSize);
    float minHalf = max(min(halfSize.x, halfSize.y), 0.0001);
    float2 normalized = p / max(halfSize, float2(0.0001, 0.0001));
    if (shapeType == 3)
        return sdPolygonUnit(normalized, 3) * minHalf;
    if (shapeType == 4)
        return sdStarUnit(normalized,
                          clamp(starInnerRadius, 0.0, 1.0),
                          clamp(starOuterRadius, 0.0001, 1.0),
                          max(vertexCount, 2)) * minHalf;
    if (shapeType == 5)
        return sdPolygonUnit(normalized, max(vertexCount, 3)) * minHalf;
    if (shapeType == 6)
        return sdPolygonUnit(normalized, 4) * minHalf;
    return sdRoundBox(p, halfSize, cornerRadii);
}
float4 premultiplied(float4 color, float alphaCoverage) {
    float alpha = color.a * alphaCoverage;
    return float4(color.rgb * alpha, alpha);
}
float4 over(float4 foreground, float4 background) {
    return foreground + background * (1.0 - foreground.a);
}
float4 PSShape(VertDataOut v) : TARGET {
    float2 surfacePoint = v.uv * surfaceSize;
    float2 p = surfacePoint - shapeOffset - shapeSize * 0.5;
    float2 halfSize = max(shapeSize * 0.5, float2(0.0001, 0.0001));
    float d = shapeDistance(p, halfSize);
    float aa = max(fwidth(d), 0.75);
    float fillA = coverage(d, aa);
    float strokeA = 0.0;
    if (strokeWidth > 0.0001) {
        if (strokeAlignment == 0) {
            strokeA = clamp(coverage(d - strokeWidth, aa) - coverage(d, aa), 0.0, 1.0);
        } else if (strokeAlignment == 2) {
            strokeA = clamp(coverage(d, aa) - coverage(d + strokeWidth, aa), 0.0, 1.0);
        } else {
            strokeA = clamp(coverage(d - strokeWidth * 0.5, aa) -
                            coverage(d + strokeWidth * 0.5, aa), 0.0, 1.0);
        }
    }

    float4 fill = premultiplied(fillColor, fillA);
    float4 stroke = premultiplied(strokeColor, strokeA);
    return strokeOnFront != 0 ? over(stroke, fill) : over(fill, stroke);
}
technique Draw { pass { vertex_shader = VSDefault(v); pixel_shader = PSShape(v); } }
)";

static constexpr const char *kGpuFrameBlendEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d background;
uniform texture2d foreground;
uniform int blendMode;
sampler_state textureSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };
struct VertDataIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VertDataOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertDataOut VSDefault(VertDataIn v) { VertDataOut o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }
float blend_lum(float3 c) { return dot(c, float3(0.30, 0.59, 0.11)); }
float blend_sat(float3 c) { return max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)); }
float3 blend_clip_color(float3 c)
{
    float l = blend_lum(c);
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    if (n < 0.0) c = l + ((c - l) * l) / max(l - n, 0.000001);
    if (x > 1.0) c = l + ((c - l) * (1.0 - l)) / max(x - l, 0.000001);
    return clamp(c, 0.0, 1.0);
}
float3 blend_set_lum(float3 c, float l)
{
    return blend_clip_color(c + (l - blend_lum(c)));
}
float3 blend_set_sat(float3 c, float s)
{
    float n = min(c.r, min(c.g, c.b));
    float x = max(c.r, max(c.g, c.b));
    return x > n ? (c - n) * (s / (x - n)) : float3(0.0, 0.0, 0.0);
}
float3 blend_color(float3 cb, float3 cs, int mode)
{
    if (mode == 1) return cb * cs;
    if (mode == 2) return min(cb + cs, 1.0);
    if (mode == 3) return 1.0 - (1.0 - cb) * (1.0 - cs);
    if (mode == 4) return lerp(2.0 * cb * cs, 1.0 - 2.0 * (1.0 - cb) * (1.0 - cs), step(0.5, cb));
    if (mode == 5) return blend_set_lum(blend_set_sat(cs, blend_sat(cb)), blend_lum(cb));
    return cs;
}
float4 PSBlend(VertDataOut v) : TARGET
{
    float4 dst = background.Sample(textureSampler, v.uv);
    float4 src = foreground.Sample(textureSampler, v.uv);
    float da = clamp(dst.a, 0.0, 1.0);
    float sa = clamp(src.a, 0.0, 1.0);
    float3 cb = da > 0.000001 ? dst.rgb / da : float3(0.0, 0.0, 0.0);
    float3 cs = sa > 0.000001 ? src.rgb / sa : float3(0.0, 0.0, 0.0);
    float3 blended = blend_color(cb, cs, blendMode);
    float outA = sa + da * (1.0 - sa);
    float3 outRGB = dst.rgb * (1.0 - sa) + src.rgb * (1.0 - da) + blended * (sa * da);
    return float4(clamp(outRGB, 0.0, 1.0), clamp(outA, 0.0, 1.0));
}
technique Draw { pass { vertex_shader = VSDefault(v); pixel_shader = PSBlend(v); } }
)";

static constexpr const char *kGpuMaskEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d image;
uniform texture2d maskImage;
uniform int maskMode;
sampler_state textureSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };
struct VertDataIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VertDataOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertDataOut VSDefault(VertDataIn v) { VertDataOut o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }
float4 PSMask(VertDataOut v) : TARGET
{
    float4 src = image.Sample(textureSampler, v.uv);
    float4 m = maskImage.Sample(textureSampler, v.uv);
    float maskValue = m.a;
    if (maskMode == 3 || maskMode == 4) {
        float3 straight = m.a > 0.000001 ? m.rgb / m.a : float3(0.0, 0.0, 0.0);
        maskValue = dot(straight, float3(0.2126, 0.7152, 0.0722)) * m.a;
    }
    if (maskMode == 2 || maskMode == 4)
        maskValue = 1.0 - maskValue;
    return src * clamp(maskValue, 0.0, 1.0);
}
technique Draw { pass { vertex_shader = VSDefault(v); pixel_shader = PSMask(v); } }
)";

struct TitleGpuRenderSession {
    struct LayerRaster {
        std::string key;
        std::string effect_cache_key;
        QImage pending_image;
        QPointF origin;
        double logical_width = 0.0;
        double logical_height = 0.0;
        double base_box_width = 1.0;
        double base_box_height = 1.0;
        QRectF layer_box_rect;
        QRectF image_clip_rect;
        bool pending_upload = false;
        bool gpu_text = false;
        std::unique_ptr<bgs::gpu_text::Layer> text_layer;
        bool gpu_primitive = false;
        int primitive_shape_type = 0;
        int primitive_vertex_count = 0;
        float primitive_inner_radius = 0.2f;
        float primitive_outer_radius = 0.5f;
        float primitive_stroke_width = 0.0f;
        int primitive_stroke_alignment = 1;
        bool primitive_stroke_on_front = true;
        double primitive_shape_width = 0.0;
        double primitive_shape_height = 0.0;
        double primitive_padding = 0.0;
        uint32_t primitive_fill_color = 0xFFFFFFFFu;
        uint32_t primitive_stroke_color = 0x00000000u;
        std::array<float, 4> primitive_corner_radii {0.0f, 0.0f, 0.0f, 0.0f};
        gs_texture_t *texture = nullptr;
        gs_texrender_t *primitive_targets[2] = {nullptr, nullptr};
        int primitive_active_target = -1;
        gs_texrender_t *effect_cache = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct CachedFrameTexture {
        gs_texture_t *texture = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        quint64 bytes = 0;
        uint64_t last_used = 0;
    };

    struct MaskTextureCacheEntry {
        std::string key;
        gs_texrender_t *targets[2] = {nullptr, nullptr};
        int active_target = -1;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t last_used = 0;
    };

    struct TransitionMatteTexture {
        gs_texture_t *texture = nullptr;
        qint64 modified_msecs = 0;
        qint64 file_size = -1;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct AsyncReadbackSlot {
        gs_stagesurf_t *stage = nullptr;
        gs_texrender_t *crop_target = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        QRect region;
        uint64_t serial = 0;
        bool pending = false;
    };

    std::mutex mutex;
    std::atomic<bool> destroying {false};
    /* Prefix submission is a two-stage CPU transaction (update dynamic range,
     * then attach the cached base). Draw/readback must never observe the
     * intermediate range-only state. */
    std::atomic<bool> state_transaction_pending {false};
    /* The primitive shader is optional. A compile failure permanently routes
     * subsequent shape updates through the stable CPU base-raster generator
     * instead of leaving the first frame blank or retrying forever. */
    bool primitive_backend_unavailable = false;
    bool text_backend_unavailable = false;
    Title title;
    bool has_title = false;
    double time = 0.0;
    uint64_t model_revision = std::numeric_limits<uint64_t>::max();
    std::size_t first_layer = 0;
    std::size_t last_layer = std::numeric_limits<std::size_t>::max();
    bool frame_dirty = true;
    std::unordered_map<std::string, LayerRaster> layers;

    gs_texrender_t *frame_a = nullptr;
    gs_texrender_t *frame_b = nullptr;
    gs_texrender_t *presentation_targets[2] = {nullptr, nullptr};
    int active_presentation_target = -1;
    gs_texrender_t *layer_target = nullptr;
    gs_texrender_t *mask_target = nullptr;
    gs_texrender_t *masked_target = nullptr;
    gs_texrender_t *effect_a = nullptr;
    gs_texrender_t *effect_b = nullptr;
    gs_texrender_t *blur_a = nullptr;
    gs_texrender_t *blur_b = nullptr;
    /* Immediate readback remains for compatibility callers. Phase 14 prerender
     * uses the independent triple-buffered slots below. */
    gs_stagesurf_t *stage = nullptr;
    std::array<AsyncReadbackSlot, 3> readback_slots;
    uint64_t next_readback_serial = 0;
    std::size_t next_readback_slot = 0;
    gs_texture_t *submitted_final_texture = nullptr;
    QImage pending_submitted_final;
    QRect submitted_final_rect;
    bool submitted_final_pending = false;
    bool use_submitted_final = false;
    bool use_gpu_cached_final = false;
    std::string submitted_gpu_cache_key;
    qint64 submitted_final_image_key = 0;
    uint32_t submitted_final_width = 0;
    uint32_t submitted_final_height = 0;
    uint64_t submitted_final_serial = 0;
    uint64_t uploaded_final_serial = 0;
    uint64_t published_final_serial = 0;
    uint64_t draw_serial = 0;
    bool lens_flare_pass_logged = false;

    gs_texture_t *base_frame_texture = nullptr;
    QImage pending_base_frame;
    QRect base_frame_rect;
    bool base_frame_pending = false;
    bool use_base_frame = false;
    bool use_gpu_cached_base = false;
    std::string base_gpu_cache_key;
    qint64 base_frame_image_key = 0;
    uint32_t base_frame_width = 0;
    uint32_t base_frame_height = 0;

    /* GPU-resident RAM-cache presentation pool. QImage remains the disk/cache
     * transport payload, but once a payload reaches a render session its
     * texture is retained and reused when playback revisits that frame. */
    std::unordered_map<qint64, CachedFrameTexture> cached_frame_textures;
    quint64 cached_frame_texture_bytes = 0;
    /* Compatibility-only QImage upload cache. Normal playback uses the
     * process-wide sparse GPU tile cache; keep this fallback deliberately
     * small so each source/editor session cannot reserve hundreds of MiB. */
    quint64 cached_frame_texture_budget = 32ull * 1024ull * 1024ull;
    uint64_t cached_frame_texture_tick = 0;

    /* Phase 13: transformed/effected track mattes are retained as full-canvas
     * GPU textures.  The cache is shared by alpha/luma variants because the
     * mode conversion happens in the final mask shader, not in the source
     * matte render.  Double buffering prevents a cache refresh from clearing a
     * texture that is still sampled by the currently published frame. */
    std::unordered_map<std::string, MaskTextureCacheEntry> mask_texture_cache;
    uint64_t mask_texture_cache_tick = 0;
    std::unordered_map<std::string, TransitionMatteTexture> transition_matte_textures;

    /* Keep presentation/cache blits isolated from layer-copy uniforms.
     * Reusing copy_effect here leaked the last layer's wipe/crop state into the
     * whole frame and could make Preview/Program output disappear. */
    gs_effect_t *blit_effect = nullptr;
    gs_effect_t *copy_effect = nullptr;
    gs_effect_t *adjustment_mix_effect = nullptr;
    gs_effect_t *primitive_shape_effect = nullptr;
    std::unique_ptr<bgs::gpu_text::Renderer> text_renderer;
    gs_effect_t *blend_effect = nullptr;
    gs_effect_t *mask_effect = nullptr;
    std::unique_ptr<TitleEffectRegistry> effect_registry;
    gs_texture_t *final_texture = nullptr;
    /* A published texture is valid only for the exact model that produced it.
     * Keeping this identity beside the pointer makes cross-title/project
     * retention impossible even if a caller forgets an outer lifecycle reset. */
    std::string published_title_id;
    uint64_t published_model_revision =
        std::numeric_limits<uint64_t>::max();
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stage_width = 0;
    uint32_t stage_height = 0;
    float preview_quality_scale = 1.0f;
    bool editor_draft = false;
    const char *last_error = nullptr;
};

/* Phase 14 RAM tier, corrected to use sparse content-addressed GPU tiles.
 * A full-canvas render target per frame makes a 1920x1080 frame cost ~8 MiB
 * even when the title contains only a small logo or line of text. Frames now
 * reference shared 128x128 tile textures; transparent tiles are omitted and
 * identical pixels across animation frames are uploaded only once. The live
 * and editor paths still present directly from GPU textures. */
static constexpr int kGpuRamTileSize = 128;

struct GpuRamTileEntry {
    gs_texture_t *texture = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t bytes = 0;
    uint64_t references = 0;
    uint64_t last_used = 0;
};

struct GpuRamTileRef {
    std::string digest;
    QRect destination;
};

struct GpuRamFrameEntry {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<GpuRamTileRef> tiles;
    uint64_t metadata_bytes = 0;
    uint64_t last_used = 0;
};

static std::mutex g_gpu_frame_cache_mutex;
static std::unordered_map<std::string, GpuRamFrameEntry> g_gpu_frame_cache;
static std::unordered_map<std::string, GpuRamTileEntry> g_gpu_ram_tiles;
static uint64_t g_gpu_frame_cache_bytes = 0;
static uint64_t g_gpu_frame_cache_budget = 512ull * 1024ull * 1024ull;
static uint64_t g_gpu_frame_cache_tick = 0;

static std::string gpu_session_layer_id(const Layer &layer)
{
    return layer.id.empty()
        ? std::string("__gpu_layer_") + std::to_string((uintptr_t)&layer)
        : layer.id;
}

static double gpu_layer_render_time(const Title &title, const Layer &layer,
                                    double title_time)
{
    const bool persistence = title.cue_background_persistence &&
        title.cue_persistence_transition && title.current_cue_row >= 0 &&
        !title.live_text_rows.empty() && !layer.ignore_persistence;
    if (!persistence)
        return title_time;

    const auto exposed = exposed_text_layers(title);
    int exposed_index = -1;
    for (std::size_t i = 0; i < exposed.size(); ++i) {
        if (exposed[i] && (exposed[i].get() == &layer ||
            (!layer.id.empty() && exposed[i]->id == layer.id))) {
            exposed_index = static_cast<int>(i);
            break;
        }
    }
    const bool persistent_text = exposed_index >= 0 && title.cue_text_persistence &&
        exposed_index < (int)title.cue_persistent_text_columns.size() &&
        title.cue_persistent_text_columns[exposed_index];
    return (exposed_index < 0 || persistent_text)
        ? cue_persistence_hold_time(title)
        : title_time;
}

static LayerEffect resolve_gpu_layer_effect(const LayerEffect &effect, double t)
{
    LayerEffect resolved = effect;
    resolved.effect_color = eval_effect_color(effect, t);
    resolved.effect_opacity = (float)std::clamp(
        effect.opacity_prop.is_animated() ? effect.opacity_prop.evaluate(t)
                                          : (double)effect.effect_opacity,
        0.0, 1.0);
    resolved.effect_size = (float)std::max(
        0.0, effect.size_prop.is_animated() ? effect.size_prop.evaluate(t)
                                            : (double)effect.effect_size);
    resolved.effect_distance = (float)std::max(
        0.0, effect.distance_prop.is_animated() ? effect.distance_prop.evaluate(t)
                                                : (double)effect.effect_distance);
    resolved.effect_angle = (float)(effect.angle_prop.is_animated()
        ? effect.angle_prop.evaluate(t) : (double)effect.effect_angle);
    resolved.effect_spread = (float)std::max(
        0.0, effect.spread_prop.is_animated() ? effect.spread_prop.evaluate(t)
                                              : (double)effect.effect_spread);
    resolved.effect_falloff = (float)std::max(
        0.0, effect.falloff_prop.is_animated() ? effect.falloff_prop.evaluate(t)
                                               : (double)effect.effect_falloff);
    resolved.effect_stroke_width = (float)std::max(0.0,
        effect.stroke_width_prop.is_animated() ? effect.stroke_width_prop.evaluate(t)
                                                : (double)effect.effect_stroke_width);
    resolved.effect_stroke_opacity = (float)std::clamp(
        effect.stroke_opacity_prop.is_animated() ? effect.stroke_opacity_prop.evaluate(t)
                                                  : (double)effect.effect_stroke_opacity,
        0.0, 1.0);
    resolved.effect_padding_left = (float)std::max(0.0,
        effect.padding_left_prop.is_animated() ? effect.padding_left_prop.evaluate(t)
                                                : (double)effect.effect_padding_left);
    resolved.effect_padding_right = (float)std::max(0.0,
        effect.padding_right_prop.is_animated() ? effect.padding_right_prop.evaluate(t)
                                                 : (double)effect.effect_padding_right);
    resolved.effect_padding_top = (float)std::max(0.0,
        effect.padding_top_prop.is_animated() ? effect.padding_top_prop.evaluate(t)
                                               : (double)effect.effect_padding_top);
    resolved.effect_padding_bottom = (float)std::max(0.0,
        effect.padding_bottom_prop.is_animated() ? effect.padding_bottom_prop.evaluate(t)
                                                  : (double)effect.effect_padding_bottom);
    resolved.effect_corner_radius_tl = (float)std::max(0.0,
        effect.corner_radius_tl_prop.is_animated() ? effect.corner_radius_tl_prop.evaluate(t)
                                                    : (double)effect.effect_corner_radius_tl);
    resolved.effect_corner_radius_tr = (float)std::max(0.0,
        effect.corner_radius_tr_prop.is_animated() ? effect.corner_radius_tr_prop.evaluate(t)
                                                    : (double)effect.effect_corner_radius_tr);
    resolved.effect_corner_radius_br = (float)std::max(0.0,
        effect.corner_radius_br_prop.is_animated() ? effect.corner_radius_br_prop.evaluate(t)
                                                    : (double)effect.effect_corner_radius_br);
    resolved.effect_corner_radius_bl = (float)std::max(0.0,
        effect.corner_radius_bl_prop.is_animated() ? effect.corner_radius_bl_prop.evaluate(t)
                                                    : (double)effect.effect_corner_radius_bl);
    resolved.effect_stroke_color = eval_effect_stroke_color(effect, t);
    resolved.effect_secondary_color = eval_effect_secondary_color(effect, t);
    resolved.effect_amount = (float)std::max(0.0, effect.amount_prop.is_animated() ? effect.amount_prop.evaluate(t) : (double)effect.effect_amount);
    resolved.effect_scale = (float)std::max(0.001, effect.scale_prop.is_animated() ? effect.scale_prop.evaluate(t) : (double)effect.effect_scale);
    resolved.effect_softness = (float)std::clamp(effect.softness_prop.is_animated() ? effect.softness_prop.evaluate(t) : (double)effect.effect_softness, 0.0, 1.0);
    resolved.effect_roundness = (float)std::clamp(effect.roundness_prop.is_animated() ? effect.roundness_prop.evaluate(t) : (double)effect.effect_roundness, -1.0, 1.0);
    resolved.effect_speed = (float)(effect.speed_prop.is_animated() ? effect.speed_prop.evaluate(t) : (double)effect.effect_speed);
    resolved.effect_center_x = (float)(effect.center_x_prop.is_animated() ? effect.center_x_prop.evaluate(t) : (double)effect.effect_center_x);
    resolved.effect_center_y = (float)(effect.center_y_prop.is_animated() ? effect.center_y_prop.evaluate(t) : (double)effect.effect_center_y);
    resolved.effect_complexity = (float)std::clamp(effect.complexity_prop.is_animated() ? effect.complexity_prop.evaluate(t) : (double)effect.effect_complexity, 1.0, 12.0);
    resolved.effect_evolution = (float)(effect.evolution_prop.is_animated() ? effect.evolution_prop.evaluate(t) : (double)effect.effect_evolution);
    return resolved;
}

static bool layer_can_use_direct_gpu_image_raster(const Layer &layer,
                                                   double title_time,
                                                   int transition_blur_padding)
{
    if (layer.type != LayerType::Image || layer.image_path.empty())
        return false;
    if (transition_blur_padding > 0 || layer_has_stackable_pixel_effects(layer))
        return false;

    const double t = std::max(0.0, title_time - layer.in_time);
    const bool has_rounded_corners =
        layer.corner_radius_tl > 0.01f || layer.corner_radius_tr > 0.01f ||
        layer.corner_radius_br > 0.01f || layer.corner_radius_bl > 0.01f;
    /* The direct shader implements the normal circular corner contract. Keep
     * bevel, chamfer and inverted-corner variants on the exact path until the
     * same curve family is implemented analytically on the GPU. */
    if (has_rounded_corners &&
        std::abs(layer.corner_bevel_roundness - 100.0f) > 0.001f)
        return false;
    if (eval_background_enabled(layer, t) ||
        layer_has_visible_outline(layer, t))
        return false;

    const ShadowRenderParams shadow_params = evaluated_shadow_params(layer, t);
    if (shadow_params.drop_enabled || shadow_params.long_enabled)
        return false;

    return true;
}

static uint32_t argb_with_multiplied_alpha(uint32_t argb, double opacity)
{
    const uint32_t alpha = static_cast<uint32_t>(std::clamp(
        std::round(((argb >> 24) & 0xFF) * std::clamp(opacity, 0.0, 1.0)),
        0.0, 255.0));
    return (argb & 0x00FFFFFFu) | (alpha << 24);
}

static bool layer_can_use_gpu_primitive_raster(const Layer &layer,
                                                double title_time,
                                                bool backend_available)
{
    if (!backend_available ||
        (layer.type != LayerType::SolidRect && layer.type != LayerType::Shape &&
         layer.type != LayerType::ColorSolid))
        return false;

    const ShapeType shape_type = (layer.type == LayerType::SolidRect ||
                                  layer.type == LayerType::ColorSolid)
        ? ShapeType::RoundedRectangle : layer.shape_type;
    /* Line is intentionally excluded from the Phase 11 shape contract. The
     * current line-shaped primitive is no longer exposed as a shape tool and
     * legacy line layers keep using the exact path renderer until the planned
     * dedicated line tool defines its own rendering semantics. */
    if (shape_type == ShapeType::Path || shape_type == ShapeType::Star ||
        shape_type == ShapeType::Line)
        return false;

    const double local_time = std::max(0.0, title_time - layer.in_time);
    if (layer.fill_type != 0)
        return false;

    const bool has_stroke = layer_has_visible_outline(layer, local_time);
    if (has_stroke) {
        if (layer.stroke_fill_type != 1 || !eval_outline_antialias(layer, local_time))
            return false;
        /* Polygonal SDF coverage currently has a fixed analytic join. Keep
         * outlined polygon/star/triangle/diamond layers on the exact path
         * renderer until join-style parity is implemented. */
        if (shape_type == ShapeType::Triangle || shape_type == ShapeType::Star ||
            shape_type == ShapeType::Polygon || shape_type == ShapeType::Diamond)
            return false;
    }

    if ((shape_type == ShapeType::Rectangle ||
         shape_type == ShapeType::RoundedRectangle) &&
        std::abs(layer.corner_bevel_roundness - 100.0f) > 0.001f)
        return false;
    /* Triangle, diamond and polygon roundness use the editable-path corner
     * contract (edge-distance radii plus bevel roundness). Until that exact
     * contract is implemented analytically in the shader, rounded variants
     * must stay on the exact renderer rather than silently drawing sharp. */
    if ((shape_type == ShapeType::Triangle || shape_type == ShapeType::Diamond ||
         shape_type == ShapeType::Polygon) &&
        std::abs(layer.shape_roundness) > 0.001f)
        return false;

    return eval_box_width(layer, local_time) > 0.0 &&
           eval_box_height(layer, local_time) > 0.0;
}

static double gpu_primitive_padding(const Layer &layer, double local_time)
{
    const double stroke_width = layer_has_visible_outline(layer, local_time)
        ? eval_outline_width(layer, local_time) : 0.0;
    switch (eval_outline_alignment(layer, local_time)) {
    case 0: return std::ceil(stroke_width) + 2.0;
    case 2: return 2.0;
    case 1:
    default: return std::ceil(stroke_width * 0.5) + 2.0;
    }
}

static bool direct_gpu_image_geometry(const Layer &layer, double title_time,
                                      QPointF &origin, double &logical_width,
                                      double &logical_height)
{
    const double t = std::max(0.0, title_time - layer.in_time);
    const double box_w = eval_box_width(layer, t);
    const double box_h = eval_box_height(layer, t);
    if (box_w <= 0.0 || box_h <= 0.0)
        return false;

    const QString image_path = QString::fromStdString(layer.image_path);
    double image_w = eval_image_width(layer, t);
    double image_h = eval_image_height(layer, t);
    if (image_w <= 0.0 || image_h <= 0.0) {
        const QSize intrinsic_size = image_intrinsic_size(image_path);
        if (!intrinsic_size.isValid() || intrinsic_size.isEmpty())
            return false;
        image_w = intrinsic_size.width();
        image_h = intrinsic_size.height();
    }

    const ImageBoxLayout layout = image_box_layout_for_layer(
        layer, box_w, box_h, image_w, image_h);
    if (layout.w <= 0.0 || layout.h <= 0.0)
        return false;
    origin = QPointF(-eval_origin_x(layer, t) * box_w + layout.x,
                     -eval_origin_y(layer, t) * box_h + layout.y);
    logical_width = layout.w;
    logical_height = layout.h;
    return true;
}

static MotionBaseRaster render_gpu_image_layer_base_raster_direct(
    const Title &title, const Layer &layer, double title_time,
    double raster_scale = 1.0)
{
    MotionBaseRaster result;
    const double t = std::max(0.0, title_time - layer.in_time);
    const double box_w = eval_box_width(layer, t);
    const double box_h = eval_box_height(layer, t);
    if (box_w <= 0.0 || box_h <= 0.0)
        return result;

    QPointF logical_origin;
    double logical_width = 0.0;
    double logical_height = 0.0;
    if (!direct_gpu_image_geometry(layer, title_time, logical_origin,
                                   logical_width, logical_height))
        return result;

    const QString image_path = QString::fromStdString(layer.image_path);
    const int max_sample_dim = std::clamp(std::max(title.width, title.height) * 2,
                                          512, 4096);
    const double resolved_scale = std::clamp(raster_scale, 0.25, 1.0);
    const QSize sample_size(
        std::clamp(static_cast<int>(std::ceil(logical_width * resolved_scale)), 1, max_sample_dim),
        std::clamp(static_cast<int>(std::ceil(logical_height * resolved_scale)), 1, max_sample_dim));
    QImage raster = load_cached_layer_image(image_path, sample_size);
    if (raster.isNull() || raster.width() <= 0 || raster.height() <= 0)
        return result;

    if (raster.format() != QImage::Format_ARGB32_Premultiplied)
        raster = raster.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (!image_path_is_svg(image_path) && resolved_scale < 0.999) {
        raster = raster.scaled(sample_size, Qt::IgnoreAspectRatio,
                               Qt::FastTransformation);
    }
    result.image = std::move(raster);
    result.origin = logical_origin;
    result.logical_width = logical_width;
    result.logical_height = logical_height;
    result.layer_box_rect = QRectF(
        -eval_origin_x(layer, t) * box_w - logical_origin.x(),
        -eval_origin_y(layer, t) * box_h - logical_origin.y(),
        box_w, box_h);
    const QRectF source_rect(0.0, 0.0, logical_width, logical_height);
    result.image_clip_rect = layer.image_crop_when_outside_box
        ? source_rect.intersected(result.layer_box_rect)
        : source_rect;
    return result;
}

static MotionBaseRaster render_gpu_layer_base_raster(const Title &title,
                                                       const Layer &layer,
                                                       double title_time,
                                                       double raster_scale = 1.0)
{
    MotionBaseRaster result;
    const int canvas_w = std::max(1, title.width);
    const int canvas_h = std::max(1, title.height);
    const double local_time = std::max(0.0, title_time - layer.in_time);
    const int transition_blur_padding = general_transition_blur_padding(layer);
    if (layer_can_use_direct_gpu_image_raster(layer, title_time,
                                              transition_blur_padding)) {
        return render_gpu_image_layer_base_raster_direct(title, layer,
                                                         title_time, raster_scale);
    }
    QRect surface_rect = clipped_effect_surface_rect(layer, local_time,
                                                      canvas_w, canvas_h);
    if (transition_blur_padding > 0) {
        surface_rect = surface_rect.adjusted(-transition_blur_padding,
                                             -transition_blur_padding,
                                             transition_blur_padding,
                                             transition_blur_padding);
    }
    if (!surface_rect.isValid() || surface_rect.isEmpty())
        return result;

    const double resolved_scale = std::clamp(raster_scale, 0.25, 1.0);
    QImage canvas(std::max(1, static_cast<int>(std::ceil(surface_rect.width() * resolved_scale))),
                  std::max(1, static_cast<int>(std::ceil(surface_rect.height() * resolved_scale))),
                  QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    auto surface = make_image_surface_for_qimage(canvas);
    auto layer_cr = make_cairo_context(surface.get());
    if (!surface || !layer_cr)
        return result;

    cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_CLEAR);
    cairo_paint(layer_cr.get());
    cairo_set_operator(layer_cr.get(), CAIRO_OPERATOR_OVER);
    cairo_scale(layer_cr.get(), resolved_scale, resolved_scale);
    cairo_translate(layer_cr.get(), -surface_rect.x(), -surface_rect.y());

    Layer base_layer = layer_without_stackable_pixel_effects(layer);
    base_layer.transitions.erase(
        std::remove_if(base_layer.transitions.begin(), base_layer.transitions.end(),
                       [](const LayerTransition &transition) {
                           return transition.kind == LayerTransitionKind::General;
                       }),
        base_layer.transitions.end());
    neutralize_layer_transform_for_effect_cache(base_layer, 1.0, 0.0, 0.0);
    ScopedGpuReadbackContract no_intermediate_readback(GpuReadbackContract::FinalFrameOnly);
    render_layer_unmasked(layer_cr.get(), title, base_layer, title_time,
                          canvas_w, canvas_h);
    layer_cr.reset();
    cairo_surface_flush(surface.get());
    surface.reset();

    result.layer_box_rect = QRectF(
        -eval_origin_x(layer, local_time) * eval_box_width(layer, local_time) - surface_rect.x(),
        -eval_origin_y(layer, local_time) * eval_box_height(layer, local_time) - surface_rect.y(),
        eval_box_width(layer, local_time), eval_box_height(layer, local_time));
    const QRect bounds = image_alpha_bounds(canvas);
    if (!bounds.isValid() || bounds.isEmpty())
        return result;
    const bool needs_effect_padding = transition_blur_padding > 0 || std::any_of(
        layer.effects.begin(), layer.effects.end(),
        [local_time](const LayerEffect &effect) {
            return effect.type != LayerEffectType::MotionBlur &&
                   eval_effect_enabled(effect, local_time);
        });
    if (needs_effect_padding || resolved_scale < 0.999) {
        result.image = canvas;
        result.origin = QPointF(surface_rect.x(), surface_rect.y());
        result.logical_width = surface_rect.width();
        result.logical_height = surface_rect.height();
    } else {
        result.image = canvas.copy(bounds);
        result.origin = QPointF(surface_rect.x() + bounds.x(),
                                surface_rect.y() + bounds.y());
        result.logical_width = result.image.width();
        result.logical_height = result.image.height();
    }
    return result;
}

static constexpr const char *kGpuSceneMaskRasterPrefix = "__gpu_scene_mask__";

static std::string gpu_scene_mask_raster_id(const std::string &layer_id)
{
    return std::string(kGpuSceneMaskRasterPrefix) + layer_id;
}

static bool is_gpu_scene_mask_raster_id(const std::string &id)
{
    return id.rfind(kGpuSceneMaskRasterPrefix, 0) == 0;
}

static bool begin_gpu_target(gs_texrender_t *target, uint32_t width, uint32_t height,
                             const struct vec4 &clear_color)
{
    if (!target)
        return false;
    gs_texrender_reset(target);
    if (!gs_texrender_begin(target, width, height))
        return false;
    gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
    gs_clear(GS_CLEAR_COLOR, &clear_color, 1.0f, 0);
    return true;
}


static QRect gpu_text_surface_rect(const Title &title, const Layer &layer,
                                   double local_time)
{
    QRect surface = clipped_effect_surface_rect(
        layer, local_time, std::max(1, title.width), std::max(1, title.height));
    const int inline_stroke_pad = static_cast<int>(std::ceil(
        std::max(0.0, max_rich_text_stroke_width(layer, local_time)))) + 2;
    if (inline_stroke_pad > 2) {
        surface = surface.adjusted(-inline_stroke_pad, -inline_stroke_pad,
                                   inline_stroke_pad, inline_stroke_pad);
        const int max_w = std::max(1, title.width);
        const int max_h = std::max(1, title.height);
        surface = surface.intersected(
            QRect(-max_w, -max_h, max_w * 3, max_h * 3));
    }
    return surface;
}

static bool layer_can_use_gpu_text_raster(const Layer &layer,
                                          double title_time,
                                          bool backend_available)
{
    if (!backend_available)
        return false;
    if (layer.type != LayerType::Text && layer.type != LayerType::Clock)
        return false;
    /* Phase 12C keeps per-character transition isolation on the exact legacy
     * adapter until transition units are emitted directly as GPU instances.
     * General layer transitions already remain in the unified compositor. */
    return active_text_layer_transition(layer, title_time) == nullptr;
}

static bool prepare_gpu_text_raster(TitleGpuRenderSession *session,
                                    const Layer &layer, double layer_time,
                                    const QRect &surface_rect,
                                    TitleGpuRenderSession::LayerRaster &entry,
                                    std::string *failure_reason)
{
    if (!session || !surface_rect.isValid() || surface_rect.isEmpty())
        return false;
    const double local_time = std::max(0.0, layer_time - layer.in_time);
    const double box_width = eval_box_width(layer, local_time);
    const double box_height = eval_box_height(layer, local_time);
    if (box_width <= 0.0 || box_height <= 0.0)
        return false;

    const QRectF style_rect = text_rect_for_style(
        QRectF(0.0, 0.0, box_width, box_height), layer);
    RichTextDocument model = rich_text_model_for_source_time(layer, local_time);
    if (layer.text_overflow_mode == 2) {
        const QString fitted = overflow_layout_text(
            QString::fromStdString(model.plain_text), layer);
        const RichTextCharFormat insertion_format =
            rich_text_effective_typing_format(model);
        rich_text_document_replace_text(
            model, fitted.toStdString(), insertion_format,
            model.has_typing_format ? model.typing_format_mask : 0);
    }

    TextLayoutRequest request;
    request.document = model;
    request.max_width = static_cast<float>(std::max(1.0, style_rect.width()));
    request.max_height = static_cast<float>(std::max(1.0, style_rect.height()));
    request.device_scale = 1.0f;
    request.minimum_horizontal_fit =
        std::clamp(layer.text_fit_min_scale, 0.05f, 1.0f);
    request.overflow_mode = layer.text_overflow_mode;
    const ImmutableTextLayout layout = cached_text_layout(request);
    if (!layout || !layout->valid)
        return false;

    if (!session->text_renderer)
        session->text_renderer =
            std::make_unique<bgs::gpu_text::Renderer>();
    if (!entry.text_layer)
        entry.text_layer = std::make_unique<bgs::gpu_text::Layer>();

    const double box_local_x =
        -eval_origin_x(layer, local_time) * box_width - surface_rect.x();
    const double box_local_y =
        -eval_origin_y(layer, local_time) * box_height - surface_rect.y();
    const double text_offset_x = box_local_x + style_rect.x();
    const double text_offset_y = box_local_y + style_rect.y();
    const double clip_pad =
        std::max(0.0, max_rich_text_stroke_width(layer, local_time)) + 2.0;
    const QRectF target_bounds(0.0, 0.0, surface_rect.width(),
                               surface_rect.height());
    const QRectF text_clip = QRectF(text_offset_x, text_offset_y,
                                    style_rect.width(), style_rect.height())
                                 .adjusted(-clip_pad, -clip_pad,
                                           clip_pad, clip_pad)
                                 .intersected(target_bounds);

    bgs::gpu_text::PrepareOptions options;
    options.logical_width = static_cast<float>(surface_rect.width());
    options.logical_height = static_cast<float>(surface_rect.height());
    options.text_offset_x = static_cast<float>(text_offset_x);
    options.text_offset_y = static_cast<float>(text_offset_y);
    options.text_width = static_cast<float>(style_rect.width());
    options.text_height = static_cast<float>(style_rect.height());
    options.clip_x = static_cast<float>(text_clip.x());
    options.clip_y = static_cast<float>(text_clip.y());
    options.clip_width = static_cast<float>(std::max(0.0, text_clip.width()));
    options.clip_height = static_cast<float>(std::max(0.0, text_clip.height()));
    options.raster_scale = session->editor_draft
        ? std::clamp(session->preview_quality_scale, 0.25f, 1.0f)
        : 1.0f;

    if (!session->text_renderer->prepare(
            *entry.text_layer, layout,
            text_layout_paint_runs(request.document), options,
            failure_reason))
        return false;

    entry.gpu_text = true;
    entry.gpu_primitive = false;
    entry.pending_image = QImage();
    entry.origin = QPointF(surface_rect.x(), surface_rect.y());
    entry.logical_width = surface_rect.width();
    entry.logical_height = surface_rect.height();
    entry.layer_box_rect = QRectF(box_local_x, box_local_y,
                                  box_width, box_height);
    entry.image_clip_rect = QRectF();
    return true;
}

static bool ensure_gpu_session_objects(TitleGpuRenderSession *session,
                                       uint32_t width, uint32_t height)
{
    if (!session)
        return false;
    auto create_target = [](gs_texrender_t *&target) {
        if (!target)
            target = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
        return target != nullptr;
    };
    if (!create_target(session->frame_a) || !create_target(session->frame_b) ||
        !create_target(session->presentation_targets[0]) ||
        !create_target(session->presentation_targets[1]) ||
        !create_target(session->layer_target) || !create_target(session->mask_target) ||
        !create_target(session->masked_target) || !create_target(session->effect_a) ||
        !create_target(session->effect_b) || !create_target(session->blur_a) ||
        !create_target(session->blur_b)) {
        session->last_error = "Could not allocate GPU render targets.";
        return false;
    }
    if (!session->blit_effect)
        session->blit_effect = gs_effect_create(kGpuFrameBlitEffect,
                                                "obs-bgs-gpu-frame-blit.effect", nullptr);
    if (!session->blit_effect) {
        session->last_error = "Could not compile mandatory GPU frame-blit shader.";
        return false;
    }
    if (!session->effect_registry)
        session->effect_registry = std::make_unique<TitleEffectRegistry>();
    /* Staging is intentionally lazy. Interactive editor/live rendering never
     * allocates or maps a CPU-readable surface; only explicit cache/export
     * readback creates one. */
    session->width = width;
    session->height = height;
    return true;
}

static void set_gpu_effect_argb(gs_effect_t *effect, const char *name, uint32_t argb)
{
    gs_eparam_t *param = effect ? gs_effect_get_param_by_name(effect, name) : nullptr;
    if (!param) return;
    struct vec4 color;
    vec4_set(&color, ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
             (argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f);
    gs_effect_set_vec4(param, &color);
}


static void release_gpu_text_layer(
    TitleGpuRenderSession *session,
    TitleGpuRenderSession::LayerRaster &entry)
{
    if (!entry.text_layer)
        return;
    const bool owns_current = session && session->text_renderer &&
        session->text_renderer->owns_texture(*entry.text_layer, entry.texture);
    if (session && session->text_renderer)
        session->text_renderer->release_layer(*entry.text_layer);
    entry.text_layer.reset();
    if (owns_current) {
        entry.texture = nullptr;
        entry.width = 0;
        entry.height = 0;
    }
}

static bool render_gpu_text_raster(
    TitleGpuRenderSession *session,
    TitleGpuRenderSession::LayerRaster &entry)
{
    if (!session || !session->text_renderer || !entry.text_layer)
        return false;
    if (session->text_backend_unavailable) {
        entry.key.clear();
        return false;
    }

    gs_texture_t *old_texture = entry.texture;
    const bool old_text_texture =
        session->text_renderer->owns_texture(*entry.text_layer, old_texture);
    const bool old_primitive_targets =
        entry.primitive_targets[0] || entry.primitive_targets[1];
    if (!session->text_renderer->render(*entry.text_layer)) {
        if (!session->text_renderer->backend_available())
            session->text_backend_unavailable = true;
        entry.key.clear();
        entry.effect_cache_key.clear();
        if (const char *error = session->text_renderer->last_error())
            session->last_error = error;
        return false;
    }

    gs_texture_t *rendered =
        session->text_renderer->texture(*entry.text_layer);
    if (!rendered)
        return false;

    if (old_primitive_targets) {
        for (gs_texrender_t *&target : entry.primitive_targets) {
            if (target)
                gs_texrender_destroy(target);
            target = nullptr;
        }
        entry.primitive_active_target = -1;
    } else if (old_texture && old_texture != rendered && !old_text_texture) {
        gs_texture_destroy(old_texture);
    }

    entry.texture = rendered;
    entry.width = session->text_renderer->texture_width(*entry.text_layer);
    entry.height = session->text_renderer->texture_height(*entry.text_layer);
    entry.gpu_text = true;
    entry.gpu_primitive = false;
    entry.pending_image = QImage();
    entry.pending_upload = false;
    entry.effect_cache_key.clear();
    return true;
}

static bool render_gpu_primitive_raster(TitleGpuRenderSession *session,
                                        TitleGpuRenderSession::LayerRaster &entry)
{
    if (!session)
        return false;
    if (session->primitive_backend_unavailable) {
        entry.key.clear();
        return false;
    }
    if (entry.text_layer)
        release_gpu_text_layer(session, entry);
    /* Phase 11 remains lazy and optional: cache-only presentation does not
     * depend on this shader, and a backend compile failure is converted into a
     * per-layer CPU base-raster fallback on the next model update. */
    if (!session->primitive_shape_effect)
        session->primitive_shape_effect = gs_effect_create(
            kGpuPrimitiveShapeEffect,
            "obs-bgs-gpu-primitive-shape.effect", nullptr);
    if (!session->primitive_shape_effect) {
        session->primitive_backend_unavailable = true;
        entry.key.clear();
        entry.effect_cache_key.clear();
        session->last_error = "Could not compile optional GPU primitive-shape shader; falling back to the stable base raster.";
        return false;
    }
    const bool replacing_owned_cpu_texture = entry.texture &&
        !entry.primitive_targets[0] && !entry.primitive_targets[1];
    gs_texture_t *owned_cpu_texture = replacing_owned_cpu_texture
        ? entry.texture : nullptr;
    const float raster_scale = session->editor_draft
        ? std::clamp(session->preview_quality_scale, 0.25f, 1.0f) : 1.0f;
    const uint32_t width = clamped_source_dimension(static_cast<int>(
        std::ceil(entry.logical_width * raster_scale)));
    const uint32_t height = clamped_source_dimension(static_cast<int>(
        std::ceil(entry.logical_height * raster_scale)));

    /* Never reset the target currently sampled by the last valid frame.
     * Render into the inactive target and publish it only after a successful
     * draw. This prevents a transient cleared/black texture during shape
     * creation and interactive scaling. */
    const int render_index = entry.primitive_active_target == 0 ? 1 : 0;
    gs_texrender_t *&render_target = entry.primitive_targets[render_index];
    if (!render_target)
        render_target = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    if (!render_target)
        return false;

    struct vec4 clear;
    vec4_zero(&clear);
    if (!begin_gpu_target(render_target, width, height, clear))
        return false;
    gs_effect_t *effect = session->primitive_shape_effect;
    if (auto *p = gs_effect_get_param_by_name(effect, "shapeType")) gs_effect_set_int(p, entry.primitive_shape_type);
    if (auto *p = gs_effect_get_param_by_name(effect, "vertexCount")) gs_effect_set_int(p, entry.primitive_vertex_count);
    if (auto *p = gs_effect_get_param_by_name(effect, "starInnerRadius")) gs_effect_set_float(p, entry.primitive_inner_radius);
    if (auto *p = gs_effect_get_param_by_name(effect, "starOuterRadius")) gs_effect_set_float(p, entry.primitive_outer_radius);
    if (auto *p = gs_effect_get_param_by_name(effect, "strokeWidth")) gs_effect_set_float(p, entry.primitive_stroke_width);
    if (auto *p = gs_effect_get_param_by_name(effect, "strokeAlignment")) gs_effect_set_int(p, entry.primitive_stroke_alignment);
    if (auto *p = gs_effect_get_param_by_name(effect, "strokeOnFront")) gs_effect_set_int(p, entry.primitive_stroke_on_front ? 1 : 0);
    if (auto *p = gs_effect_get_param_by_name(effect, "surfaceSize")) { struct vec2 v; vec2_set(&v,(float)entry.logical_width,(float)entry.logical_height); gs_effect_set_vec2(p,&v); }
    if (auto *p = gs_effect_get_param_by_name(effect, "shapeSize")) { struct vec2 v; vec2_set(&v,(float)entry.primitive_shape_width,(float)entry.primitive_shape_height); gs_effect_set_vec2(p,&v); }
    if (auto *p = gs_effect_get_param_by_name(effect, "shapeOffset")) { struct vec2 v; vec2_set(&v,(float)entry.primitive_padding,(float)entry.primitive_padding); gs_effect_set_vec2(p,&v); }
    if (auto *p = gs_effect_get_param_by_name(effect, "cornerRadii")) { struct vec4 v; vec4_set(&v,entry.primitive_corner_radii[0],entry.primitive_corner_radii[1],entry.primitive_corner_radii[2],entry.primitive_corner_radii[3]); gs_effect_set_vec4(p,&v); }
    set_gpu_effect_argb(effect, "fillColor", entry.primitive_fill_color);
    set_gpu_effect_argb(effect, "strokeColor", entry.primitive_stroke_color);
    gs_enable_blending(false);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(nullptr, 0, width, height);
    gs_texrender_end(render_target);

    gs_texture_t *rendered = gs_texrender_get_texture(render_target);
    if (!rendered)
        return false;
    entry.primitive_active_target = render_index;
    entry.texture = rendered;
    if (owned_cpu_texture)
        gs_texture_destroy(owned_cpu_texture);
    entry.width = width;
    entry.height = height;
    /* The effected variant is tied to the previous immutable primitive
     * generation even when its dimensions are unchanged. */
    entry.effect_cache_key.clear();
    entry.pending_upload = false;
    return true;
}

static bool upload_gpu_layer_raster(TitleGpuRenderSession *session,
                                    TitleGpuRenderSession::LayerRaster &entry)
{
    if (entry.gpu_text && entry.pending_upload)
        return render_gpu_text_raster(session, entry);
    if (entry.gpu_text && !entry.pending_upload)
        return entry.texture != nullptr;
    if (entry.text_layer)
        release_gpu_text_layer(session, entry);
    if (entry.gpu_primitive && entry.pending_upload)
        return render_gpu_primitive_raster(session, entry);
    if (!entry.pending_upload)
        return entry.texture != nullptr;
    if (!entry.gpu_primitive &&
        (entry.primitive_targets[0] || entry.primitive_targets[1])) {
        for (gs_texrender_t *&target : entry.primitive_targets) {
            if (target)
                gs_texrender_destroy(target);
            target = nullptr;
        }
        entry.primitive_active_target = -1;
        entry.texture = nullptr;
        entry.width = entry.height = 0;
    }
    if (entry.pending_image.isNull()) {
        if (entry.primitive_targets[0] || entry.primitive_targets[1]) {
            for (gs_texrender_t *&target : entry.primitive_targets) {
                if (target)
                    gs_texrender_destroy(target);
                target = nullptr;
            }
            entry.primitive_active_target = -1;
            entry.texture = nullptr;
        } else if (entry.texture) {
            gs_texture_destroy(entry.texture);
        }
        entry.texture = nullptr;
        if (entry.effect_cache)
            gs_texrender_destroy(entry.effect_cache);
        entry.effect_cache = nullptr;
        entry.effect_cache_key.clear();
        entry.width = entry.height = 0;
        entry.pending_upload = false;
        return false;
    }
    QImage upload = entry.pending_image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (upload.bytesPerLine() != upload.width() * 4)
        upload = upload.copy();
    const uint32_t width = (uint32_t)upload.width();
    const uint32_t height = (uint32_t)upload.height();
    if (!entry.texture || entry.width != width || entry.height != height) {
        if (entry.texture)
            gs_texture_destroy(entry.texture);
        const uint8_t *planes[1] = {upload.constBits()};
        entry.texture = gs_texture_create(width, height, GS_BGRA, 1, planes, GS_DYNAMIC);
        entry.width = entry.texture ? width : 0;
        entry.height = entry.texture ? height : 0;
    } else {
        gs_texture_set_image(entry.texture, upload.constBits(),
                             (uint32_t)upload.bytesPerLine(), false);
    }
    /* A new base raster invalidates any GPU-resident effected variant, even
     * when the texture dimensions happened to stay the same. */
    entry.effect_cache_key.clear();
    entry.pending_image = QImage();
    entry.pending_upload = false;
    return entry.texture != nullptr;
}

struct PackedGaussianKernel {
    float center_weight = 1.0f;
    std::array<float, 4> offsets {1.0f, 3.0f, 5.0f, 7.0f};
    std::array<float, 4> weights {0.0f, 0.0f, 0.0f, 0.0f};
    int downsample = 1;
};

static PackedGaussianKernel packed_gaussian_kernel(double radius)
{
    PackedGaussianKernel kernel;
    kernel.downsample = gaussian_blur_downsample(radius);
    const double sigma = std::max(0.35, radius /
        (3.0 * static_cast<double>(kernel.downsample)));
    std::array<double, 9> discrete {};
    for (int i = 0; i <= 8; ++i) {
        const double x = static_cast<double>(i);
        discrete[static_cast<std::size_t>(i)] =
            std::exp(-(x * x) / (2.0 * sigma * sigma));
    }
    double normalization = discrete[0];
    for (int i = 1; i <= 8; ++i)
        normalization += 2.0 * discrete[static_cast<std::size_t>(i)];
    normalization = std::max(normalization, 1e-12);
    kernel.center_weight = static_cast<float>(discrete[0] / normalization);
    for (int pair = 0; pair < 4; ++pair) {
        const int first = pair * 2 + 1;
        const int second = first + 1;
        const double first_weight = discrete[static_cast<std::size_t>(first)];
        const double second_weight = discrete[static_cast<std::size_t>(second)];
        const double combined = std::max(first_weight + second_weight, 1e-12);
        kernel.offsets[static_cast<std::size_t>(pair)] = static_cast<float>(
            (first * first_weight + second * second_weight) / combined);
        kernel.weights[static_cast<std::size_t>(pair)] = static_cast<float>(
            combined / normalization);
    }
    return kernel;
}

static bool effect_uses_separable_gaussian(LayerEffectType type)
{
    switch (type) {
    case LayerEffectType::DropShadow:
    case LayerEffectType::Glow:
    case LayerEffectType::InnerGlow:
    case LayerEffectType::InnerShadow:
    case LayerEffectType::Blur:
    case LayerEffectType::Bloom:
        return true;
    default:
        return false;
    }
}

static double effect_gaussian_radius(const LayerEffect &effect)
{
    switch (effect.type) {
    case LayerEffectType::DropShadow:
    case LayerEffectType::InnerShadow:
        return std::max(0.0, static_cast<double>(effect.effect_size) +
                              static_cast<double>(effect.effect_spread));
    default:
        return std::max(0.0, static_cast<double>(effect.effect_size));
    }
}

static gs_texture_t *render_separable_gaussian(
    TitleGpuRenderSession *session, gs_texture_t *source,
    uint32_t source_width, uint32_t source_height, double radius,
    int prefilter_mode, float threshold)
{
    if (!session || !source || !session->effect_registry)
        return source;
    if (radius <= 0.01 && prefilter_mode == 0)
        return source;

    gs_effect_t *blur = session->effect_registry->compile(LayerEffectType::Blur);
    if (!blur) {
        session->last_error = session->effect_registry->last_error();
        return source;
    }

    const PackedGaussianKernel kernel = packed_gaussian_kernel(radius);
    struct vec4 clear;
    vec4_zero(&clear);

    gs_texture_t *current = source;
    uint32_t current_width = std::max<uint32_t>(1, source_width);
    uint32_t current_height = std::max<uint32_t>(1, source_height);
    const gs_texture_t *blur_a_texture = gs_texrender_get_texture(session->blur_a);
    const gs_texture_t *blur_b_texture = gs_texrender_get_texture(session->blur_b);
    bool next_is_a = current != blur_a_texture;
    if (current == blur_b_texture)
        next_is_a = true;
    int applied_prefilter = 0;

    /* Build a proper low-pass pyramid instead of jumping directly from the
     * source to a tiny target. Each 2x step averages four bilinear samples,
     * eliminating the sparse/ringing look that large single-pass kernels had. */
    for (int scale = 1; scale < kernel.downsample; scale *= 2) {
        const uint32_t next_width = std::max<uint32_t>(1, (current_width + 1) / 2);
        const uint32_t next_height = std::max<uint32_t>(1, (current_height + 1) / 2);
        gs_texrender_t *target = next_is_a ? session->blur_a : session->blur_b;
        if (!begin_gpu_target(target, next_width, next_height, clear))
            return source;
        if (gs_eparam_t *image = gs_effect_get_param_by_name(blur, "image"))
            gs_effect_set_texture(image, current);
        set_effect_vec2_param(blur, "texelSize",
                              1.0f / static_cast<float>(current_width),
                              1.0f / static_cast<float>(current_height));
        set_effect_int_param(blur, "prefilterMode",
                             applied_prefilter ? 0 : prefilter_mode);
        set_effect_float_param(blur, "threshold", threshold);
        gs_enable_blending(false);
        while (gs_effect_loop(blur, "Downsample"))
            gs_draw_sprite(current, 0, next_width, next_height);
        gs_texrender_end(target);
        current = gs_texrender_get_texture(target);
        if (!current)
            return source;
        current_width = next_width;
        current_height = next_height;
        next_is_a = !next_is_a;
        applied_prefilter = prefilter_mode != 0;
    }

    auto configure_gaussian = [&](gs_texture_t *input, float dx, float dy,
                                  int active_prefilter) {
        if (gs_eparam_t *image = gs_effect_get_param_by_name(blur, "image"))
            gs_effect_set_texture(image, input);
        set_effect_vec2_param(blur, "texelSize",
                              1.0f / static_cast<float>(current_width),
                              1.0f / static_cast<float>(current_height));
        set_effect_vec2_param(blur, "direction", dx, dy);
        set_effect_float_param(blur, "centerWeight", kernel.center_weight);
        set_effect_vec4_param(blur, "pairOffsets",
                              kernel.offsets[0], kernel.offsets[1],
                              kernel.offsets[2], kernel.offsets[3]);
        set_effect_vec4_param(blur, "pairWeights",
                              kernel.weights[0], kernel.weights[1],
                              kernel.weights[2], kernel.weights[3]);
        set_effect_int_param(blur, "prefilterMode", active_prefilter);
        set_effect_float_param(blur, "threshold", threshold);
    };

    gs_texrender_t *horizontal_target = next_is_a ? session->blur_a : session->blur_b;
    if (!begin_gpu_target(horizontal_target, current_width, current_height, clear))
        return source;
    configure_gaussian(current, 1.0f, 0.0f,
                       applied_prefilter ? 0 : prefilter_mode);
    gs_enable_blending(false);
    while (gs_effect_loop(blur, "Gaussian"))
        gs_draw_sprite(current, 0, current_width, current_height);
    gs_texrender_end(horizontal_target);
    gs_texture_t *horizontal = gs_texrender_get_texture(horizontal_target);
    if (!horizontal)
        return source;
    next_is_a = !next_is_a;

    gs_texrender_t *vertical_target = next_is_a ? session->blur_a : session->blur_b;
    if (!begin_gpu_target(vertical_target, current_width, current_height, clear))
        return source;
    configure_gaussian(horizontal, 0.0f, 1.0f, 0);
    gs_enable_blending(false);
    while (gs_effect_loop(blur, "Gaussian"))
        gs_draw_sprite(horizontal, 0, current_width, current_height);
    gs_texrender_end(vertical_target);
    gs_texture_t *result = gs_texrender_get_texture(vertical_target);
    return result ? result : source;
}

static gs_texture_t *render_long_shadow_mask(
    TitleGpuRenderSession *session, gs_effect_t *effect,
    const LayerEffect &resolved, gs_texture_t *source,
    uint32_t width, uint32_t height)
{
    if (!session || !effect || !source)
        return source;
    struct vec4 clear;
    vec4_zero(&clear);
    if (!begin_gpu_target(session->blur_a, width, height, clear))
        return source;
    if (gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image"))
        gs_effect_set_texture(image, source);
    set_gpu_surface_effect_params(effect, resolved,
                                  static_cast<int>(width),
                                  static_cast<int>(height));
    gs_enable_blending(false);
    while (gs_effect_loop(effect, "ShadowMask"))
        gs_draw_sprite(source, 0, width, height);
    gs_texrender_end(session->blur_a);
    gs_texture_t *mask = gs_texrender_get_texture(session->blur_a);
    return mask ? mask : source;
}

static void set_gpu_background_geometry_params(
    gs_effect_t *effect, const LayerEffect &resolved, const Layer &layer,
    double title_time, const TitleGpuRenderSession::LayerRaster *entry,
    uint32_t texture_width, uint32_t texture_height)
{
    if (!effect || !entry || texture_width == 0 || texture_height == 0)
        return;
    (void)layer;
    (void)title_time;
    const double logical_w = std::max(1.0, entry->logical_width);
    const double logical_h = std::max(1.0, entry->logical_height);
    const QRectF box = entry->layer_box_rect.isValid()
        ? entry->layer_box_rect
        : QRectF(0.0, 0.0, entry->base_box_width, entry->base_box_height);
    /* Keep background geometry in logical layer-raster coordinates. The shader
     * maps UVs into this space, so adaptive-resolution texture dimensions,
     * SVG buckets and cropped rasters cannot introduce pixel-rounding offsets. */
    const double x = box.x() - resolved.effect_padding_left;
    const double y = box.y() - resolved.effect_padding_top;
    const double w = box.width() + resolved.effect_padding_left +
                     resolved.effect_padding_right;
    const double h = box.height() + resolved.effect_padding_top +
                     resolved.effect_padding_bottom;
    set_effect_vec4_param(effect, "backgroundRect",
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(w), static_cast<float>(h));
    set_effect_vec2_param(effect, "textureSize", static_cast<float>(logical_w),
                          static_cast<float>(logical_h));
    set_effect_vec4_param(effect, "cornerRadii",
                          resolved.effect_corner_radius_tl,
                          resolved.effect_corner_radius_tr,
                          resolved.effect_corner_radius_br,
                          resolved.effect_corner_radius_bl);
    set_effect_color_param(effect, "strokeColor", resolved.effect_stroke_color);
    set_effect_float_param(effect, "strokeWidth", resolved.effect_stroke_width);
    set_effect_float_param(effect, "strokeOpacity", resolved.effect_stroke_opacity);
}

static gs_texture_t *apply_gpu_layer_effect_stack(TitleGpuRenderSession *session,
                                                   const Layer &layer,
                                                   double title_time,
                                                   gs_texture_t *input,
                                                   uint32_t width,
                                                   uint32_t height,
                                                   TitleGpuRenderSession::LayerRaster *cache_entry = nullptr)
{
    if (!session || !input)
        return input;
    const double local_time = std::max(0.0, title_time - layer.in_time);
    struct vec4 clear;
    vec4_zero(&clear);

    struct GpuEffectPass {
        LayerEffect resolved;
        gs_effect_t *effect = nullptr;
    };
    std::vector<GpuEffectPass> passes;
    passes.reserve(layer.effects.size() + 1);

    auto append_resolved_effect = [&](const LayerEffect &resolved) {
        /* Every effect whose visual construction depends on blurring uses the
         * same general separable-Gaussian shader.  Only the final composite
         * technique differs (shadow/glow/inner/bloom/blur). */
        const LayerEffectType shader_type = effect_uses_separable_gaussian(resolved.type)
            ? LayerEffectType::Blur
            : resolved.type;
        gs_effect_t *effect = session->effect_registry->compile(shader_type);
        if (!effect) {
            /* GPU-only means there is no CPU compositor fallback. Keep the
             * layer visible if an optional shader asset is unavailable and
             * expose the error through the GPU status instead of dropping the
             * complete layer from editor/live output. */
            session->last_error = session->effect_registry->last_error();
            return;
        }
        passes.push_back({resolved, effect});
    };

    for (const LayerEffect &effect_config : layer.effects) {
        if (!eval_effect_enabled(effect_config, local_time) ||
            effect_config.type == LayerEffectType::MotionBlur)
            continue;
        LayerEffect resolved = resolve_gpu_layer_effect(effect_config, local_time);
        if (session->editor_draft) {
            switch (resolved.type) {
            case LayerEffectType::Bloom:
            case LayerEffectType::Glow:
            case LayerEffectType::InnerGlow:
            case LayerEffectType::InnerShadow:
            case LayerEffectType::DropShadow:
            case LayerEffectType::LongShadow:
            case LayerEffectType::Emboss:
                continue;
            case LayerEffectType::Blur:
                resolved.effect_size *= session->preview_quality_scale;
                break;
            default:
                break;
            }
        }
        append_resolved_effect(resolved);
    }

    const LayerTransitionVisualState transition = evaluate_layer_general_transitions(
        layer.transitions, layer.in_time, layer.out_time, title_time);
    if (transition.active && transition.blur > 0.01) {
        LayerEffect transition_blur;
        transition_blur.type = LayerEffectType::Blur;
        transition_blur.enabled = true;
        transition_blur.effect_size = static_cast<float>(transition.blur);
        transition_blur.effect_opacity = 1.0f;
        transition_blur.effect_blur_type = static_cast<int>(ShadowBlurType::StackFast);
        append_resolved_effect(transition_blur);
    }

    if (passes.empty()) {
        if (cache_entry && cache_entry->effect_cache) {
            gs_texrender_destroy(cache_entry->effect_cache);
            cache_entry->effect_cache = nullptr;
            cache_entry->effect_cache_key.clear();
        }
        return input;
    }

    const bool can_cache = cache_entry && input == cache_entry->texture &&
                           !cache_entry->key.empty();
    const std::string desired_cache_key = can_cache
        ? cache_entry->key + "|gpu-effects-v8-lens-flare-dx11-keyword-fix|" +
              effect_layer_cache_key(nullptr, session->title, layer, title_time,
                                     static_cast<int>(width), static_cast<int>(height),
                                     false, false)
        : std::string();
    if (can_cache && cache_entry->effect_cache &&
        cache_entry->effect_cache_key == desired_cache_key) {
        if (gs_texture_t *cached = gs_texrender_get_texture(cache_entry->effect_cache))
            return cached;
    }

    if (can_cache && !cache_entry->effect_cache)
        cache_entry->effect_cache = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    if (can_cache)
        cache_entry->effect_cache_key.clear();

    gs_texture_t *current = input;
    bool use_a = true;
    for (std::size_t index = 0; index < passes.size(); ++index) {
        const bool last = index + 1 == passes.size();
        GpuEffectPass &pass = passes[index];
        gs_texture_t *blurred = nullptr;
        gs_texture_t *long_shadow = nullptr;
        if (effect_uses_separable_gaussian(pass.resolved.type)) {
            const int prefilter_mode = pass.resolved.type == LayerEffectType::Bloom ? 1 : 0;
            blurred = render_separable_gaussian(
                session, current, width, height,
                effect_gaussian_radius(pass.resolved), prefilter_mode,
                std::clamp(pass.resolved.effect_spread, 0.0f, 1.0f));
        } else if (pass.resolved.type == LayerEffectType::LongShadow) {
            long_shadow = render_long_shadow_mask(
                session, pass.effect, pass.resolved, current, width, height);
            if (pass.resolved.effect_blur_type !=
                    static_cast<int>(LongShadowBlurType::None) &&
                pass.resolved.effect_size > 0.01f) {
                long_shadow = render_separable_gaussian(
                    session, long_shadow, width, height,
                    pass.resolved.effect_size, 0, 0.0f);
            }
        }
        gs_texrender_t *target = last && can_cache && cache_entry->effect_cache
            ? cache_entry->effect_cache
            : (use_a ? session->effect_a : session->effect_b);
        if (!target || !begin_gpu_target(target, width, height, clear)) {
            session->last_error = "Could not allocate an effect render target.";
            return current;
        }
        if (gs_eparam_t *image = gs_effect_get_param_by_name(pass.effect, "image"))
            gs_effect_set_texture(image, current);
        if (blurred) {
            if (gs_eparam_t *blurred_image =
                    gs_effect_get_param_by_name(pass.effect, "blurredImage"))
                gs_effect_set_texture(blurred_image, blurred);
        }
        if (long_shadow) {
            if (gs_eparam_t *shadow_image =
                    gs_effect_get_param_by_name(pass.effect, "shadowImage"))
                gs_effect_set_texture(shadow_image, long_shadow);
        }
        set_gpu_surface_effect_params(pass.effect, pass.resolved,
                                      (int)width, (int)height, local_time);
        if (pass.resolved.type == LayerEffectType::BackgroundColor)
            set_gpu_background_geometry_params(pass.effect, pass.resolved, layer,
                                               title_time, cache_entry, width, height);
        gs_enable_blending(false);
        const char *technique = "Draw";
        if (blurred) {
            switch (pass.resolved.type) {
            case LayerEffectType::Blur: technique = "Composite"; break;
            case LayerEffectType::DropShadow: technique = "DropShadowComposite"; break;
            case LayerEffectType::Glow: technique = "GlowComposite"; break;
            case LayerEffectType::InnerGlow: technique = "InnerGlowComposite"; break;
            case LayerEffectType::InnerShadow: technique = "InnerShadowComposite"; break;
            case LayerEffectType::Bloom: technique = "BloomComposite"; break;
            default: break;
            }
        }
        int executed_passes = 0;
        while (gs_effect_loop(pass.effect, technique)) {
            ++executed_passes;
            gs_draw_sprite(current, 0, width, height);
        }
        if (pass.resolved.type == LayerEffectType::LensFlare) {
            if (!session->lens_flare_pass_logged) {
                BGL_LOG_INFO("Effects", QStringLiteral(
                    "Lens flare GPU pass active passes=%1 size=%2x%3 profile=%4 radius=%5 amount=%6 opacity=%7")
                    .arg(executed_passes).arg(width).arg(height)
                    .arg(pass.resolved.effect_profile)
                    .arg(pass.resolved.effect_size)
                    .arg(pass.resolved.effect_amount)
                    .arg(pass.resolved.effect_opacity));
                session->lens_flare_pass_logged = true;
            }
            if (executed_passes == 0) {
                session->last_error = "Lens flare effect compiled but Draw technique executed no passes.";
                BGL_LOG_WARNING("Effects", QStringLiteral(
                    "Lens flare Draw technique executed no passes"));
            }
        }
        gs_texrender_end(target);
        current = gs_texrender_get_texture(target);
        if (!current)
            return input;
        if (!(last && can_cache))
            use_a = !use_a;
    }

    if (can_cache && cache_entry->effect_cache &&
        current == gs_texrender_get_texture(cache_entry->effect_cache))
        cache_entry->effect_cache_key = desired_cache_key;
    return current;
}

static gs_texture_t *gpu_transition_matte_texture(
    TitleGpuRenderSession *session, const std::string &path_value)
{
    if (!session || path_value.empty())
        return nullptr;
    const QString path = QString::fromStdString(path_value);
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile() || !info.isReadable())
        return nullptr;
    const std::string key = info.absoluteFilePath().toStdString();
    auto &entry = session->transition_matte_textures[key];
    const qint64 modified = info.lastModified().toMSecsSinceEpoch();
    const qint64 size = info.size();
    if (entry.texture && entry.modified_msecs == modified && entry.file_size == size)
        return entry.texture;

    if (entry.texture) {
        gs_texture_destroy(entry.texture);
        entry.texture = nullptr;
    }
    QImage upload = load_cached_layer_image(info.absoluteFilePath());
    if (upload.isNull())
        return nullptr;
    if (upload.format() != QImage::Format_ARGB32_Premultiplied)
        upload = upload.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    upload.detach();
    unpremultiply_bgra_for_obs(upload.bits(),
        static_cast<size_t>(upload.width()) * static_cast<size_t>(upload.height()));
    const uint8_t *data[1] = {upload.constBits()};
    entry.texture = gs_texture_create(
        static_cast<uint32_t>(upload.width()),
        static_cast<uint32_t>(upload.height()), GS_BGRA, 1, data, 0);
    if (!entry.texture)
        return nullptr;
    entry.modified_msecs = modified;
    entry.file_size = size;
    entry.width = static_cast<uint32_t>(upload.width());
    entry.height = static_cast<uint32_t>(upload.height());
    return entry.texture;
}

static void set_gpu_transition_effect_params(
    TitleGpuRenderSession *session, gs_effect_t *effect,
    const LayerTransitionVisualState &transition)
{
    if (!effect)
        return;
    set_effect_float_param(effect, "wipeProgress", static_cast<float>(transition.wipe));
    set_effect_float_param(effect, "wipeSoftness", static_cast<float>(transition.wipe_softness));
    set_effect_int_param(effect, "wipeDirection",
        transition.active && transition.wipe < 0.999999
            ? static_cast<int>(transition.wipe_direction) : 0);
    set_effect_int_param(effect, "transitionType",
        transition.active ? static_cast<int>(transition.type) : 0);
    set_effect_int_param(effect, "blocksColumns", std::max(1, transition.blocks_columns));
    set_effect_int_param(effect, "blocksRows", std::max(1, transition.blocks_rows));
    set_effect_float_param(effect, "randomSeed", static_cast<float>(transition.random_seed));
    set_effect_int_param(effect, "imageChannel", std::clamp(transition.image_channel, 0, 4));
    set_effect_int_param(effect, "transitionInvert", transition.invert ? 1 : 0);
    set_effect_int_param(effect, "transitionClockwise", transition.clockwise ? 1 : 0);
    set_effect_vec2_param(effect, "transitionCenter",
        static_cast<float>(transition.center_x), static_cast<float>(transition.center_y));
    set_effect_float_param(effect, "transitionRotation", static_cast<float>(transition.rotation));
    set_effect_float_param(effect, "transitionAspect", static_cast<float>(transition.aspect));
    set_effect_int_param(effect, "transitionProfile", transition.profile);

    gs_texture_t *matte = transition.type == LayerTransitionType::ImageWipe
        ? gpu_transition_matte_texture(session, transition.image_path) : nullptr;
    set_effect_int_param(effect, "transitionMatteEnabled", matte ? 1 : 0);
    if (matte) {
        if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, "transitionMatte"))
            gs_effect_set_texture(param, matte);
    }
}

static gs_texture_t *mix_gpu_adjustment_layer(
    TitleGpuRenderSession *session, const Title &title, const Layer &layer,
    double title_time, gs_texture_t *original, gs_texture_t *effected,
    gs_texrender_t *target)
{
    if (!session || !original || !effected || !target)
        return original;
    if (!session->adjustment_mix_effect)
        session->adjustment_mix_effect = gs_effect_create(
            kGpuAdjustmentMixEffect, "obs-bgs-adjustment-mix.effect", nullptr);
    if (!session->adjustment_mix_effect) {
        session->last_error = "Could not compile adjustment-layer mix shader.";
        return original;
    }
    struct vec4 clear;
    vec4_zero(&clear);
    if (!begin_gpu_target(target, session->width, session->height, clear))
        return original;
    gs_effect_t *effect = session->adjustment_mix_effect;
    if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, "originalImage"))
        gs_effect_set_texture(param, original);
    if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, "effectedImage"))
        gs_effect_set_texture(param, effected);
    set_effect_float_param(effect, "amount", static_cast<float>(std::clamp(
        layer_chain_opacity(title, layer, title_time), 0.0, 1.0)));
    const LayerTransitionVisualState transition = evaluate_layer_general_transitions(
        layer.transitions, layer.in_time, layer.out_time, title_time);
    set_gpu_transition_effect_params(session, effect, transition);
    gs_enable_blending(false);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(original, 0, session->width, session->height);
    gs_texrender_end(target);
    gs_texture_t *result = gs_texrender_get_texture(target);
    return result ? result : original;
}

static bool draw_gpu_layer_texture(TitleGpuRenderSession *session,
                                   const Title &title,
                                   const Layer &layer,
                                   gs_texture_t *texture,
                                   const QPointF &origin,
                                   uint32_t texture_width,
                                   uint32_t texture_height,
                                   double logical_width,
                                   double logical_height,
                                   const QRectF &image_clip_rect,
                                   double base_box_width,
                                   double base_box_height,
                                   double title_time,
                                   float weight)
{
    if (!session || !texture || weight <= 0.0f)
        return false;
    if (!session->copy_effect)
        session->copy_effect = gs_effect_create(
            kGpuLayerCopyEffect, "obs-bgs-gpu-layer-copy.effect", nullptr);
    if (!session->copy_effect) {
        session->last_error = "Could not compile GPU layer-copy shader.";
        return false;
    }
    gs_eparam_t *image = gs_effect_get_param_by_name(session->copy_effect, "image");
    gs_eparam_t *weight_param = gs_effect_get_param_by_name(session->copy_effect, "weight");
    gs_eparam_t *clip_enabled = gs_effect_get_param_by_name(session->copy_effect, "imageClipEnabled");
    gs_eparam_t *clip_rect = gs_effect_get_param_by_name(session->copy_effect, "imageClipRect");
    gs_eparam_t *corner_radii = gs_effect_get_param_by_name(session->copy_effect, "imageCornerRadii");
    gs_eparam_t *logical_size = gs_effect_get_param_by_name(session->copy_effect, "imageLogicalSize");
    if (!image || !weight_param || !clip_enabled || !clip_rect || !corner_radii || !logical_size)
        return false;
    gs_effect_set_texture(image, texture);
    gs_effect_set_float(weight_param, weight);
    const LayerTransitionVisualState transition = evaluate_layer_general_transitions(
        layer.transitions, layer.in_time, layer.out_time, title_time);
    set_gpu_transition_effect_params(session, session->copy_effect, transition);
    const bool gpu_image_clip = layer.type == LayerType::Image &&
        image_clip_rect.isValid() && !image_clip_rect.isEmpty() &&
        (layer.image_crop_when_outside_box ||
         layer.corner_radius_tl > 0.01f || layer.corner_radius_tr > 0.01f ||
         layer.corner_radius_br > 0.01f || layer.corner_radius_bl > 0.01f);
    gs_effect_set_int(clip_enabled, gpu_image_clip ? 1 : 0);
    struct vec4 clip_value;
    vec4_set(&clip_value,
             static_cast<float>(image_clip_rect.x()),
             static_cast<float>(image_clip_rect.y()),
             static_cast<float>(image_clip_rect.width()),
             static_cast<float>(image_clip_rect.height()));
    gs_effect_set_vec4(clip_rect, &clip_value);
    struct vec4 radii_value;
    vec4_set(&radii_value, layer.corner_radius_tl, layer.corner_radius_tr,
             layer.corner_radius_br, layer.corner_radius_bl);
    gs_effect_set_vec4(corner_radii, &radii_value);
    struct vec2 logical_value;
    vec2_set(&logical_value, static_cast<float>(std::max(0.0001, logical_width)),
             static_cast<float>(std::max(0.0001, logical_height)));
    gs_effect_set_vec2(logical_size, &logical_value);
    gs_matrix_push();
    gs_matrix_identity();
    const float frame_scale = session->editor_draft
        ? std::clamp(session->preview_quality_scale, 0.25f, 1.0f) : 1.0f;
    gs_matrix_scale3f(frame_scale, frame_scale, 1.0f);
    apply_layer_world_transform_gs(title, layer, title_time, 0);
    const double local_time = std::max(0.0, title_time - layer.in_time);
    const double current_box_width = std::max(0.0001, eval_box_width(layer, local_time));
    const double current_box_height = std::max(0.0001, eval_box_height(layer, local_time));
    const double sx = current_box_width / std::max(0.0001, base_box_width);
    const double sy = current_box_height / std::max(0.0001, base_box_height);
    const double raster_sx = logical_width > 0.0
        ? logical_width / std::max(1.0, static_cast<double>(texture_width)) : 1.0;
    const double raster_sy = logical_height > 0.0
        ? logical_height / std::max(1.0, static_cast<double>(texture_height)) : 1.0;
    /* The source texture can have a different pixel resolution from its
     * logical image-box size (native bitmap pixels or bucketed SVG pixels).
     * Fold that ratio into the local scale and express the translation back in
     * texture units so the logical origin is not scaled twice. */
    gs_matrix_scale3f(static_cast<float>(sx * raster_sx),
                      static_cast<float>(sy * raster_sy), 1.0f);
    gs_matrix_translate3f(
        static_cast<float>(origin.x() / std::max(0.000001, raster_sx)),
        static_cast<float>(origin.y() / std::max(0.000001, raster_sy)), 0.0f);
    while (gs_effect_loop(session->copy_effect, "Draw"))
        gs_draw_sprite(texture, 0, texture_width, texture_height);
    gs_matrix_pop();
    return true;
}

static bool render_gpu_layer_to_target(TitleGpuRenderSession *session,
                                       const Title &title,
                                       const Layer &layer,
                                       double title_time,
                                       gs_texrender_t *target,
                                       bool apply_pixel_effects = true,
                                       const std::string &raster_id = std::string())
{
    const std::string resolved_raster_id = raster_id.empty()
        ? gpu_session_layer_id(layer) : raster_id;
    auto found = session->layers.find(resolved_raster_id);
    if (found == session->layers.end() || !found->second.texture)
        return false;
    auto &entry = found->second;
    gs_texture_t *local_texture = apply_pixel_effects
        ? apply_gpu_layer_effect_stack(session, layer, title_time,
                                       entry.texture, entry.width, entry.height,
                                       &entry)
        : entry.texture;
    if (!local_texture)
        return false;

    struct vec4 clear;
    vec4_zero(&clear);
    if (!begin_gpu_target(target, session->width, session->height, clear))
        return false;

    const double current_opacity = layer_chain_visible(title, layer, title_time)
        ? std::clamp(layer_chain_opacity(title, layer, title_time), 0.0, 1.0)
        : 0.0;
    const LayerEffect *motion_config = layer_motion_blur_effect(layer);
    const LayerEffect motion = motion_config
        ? resolve_gpu_layer_effect(*motion_config,
                                   std::max(0.0, title_time - layer.in_time))
        : LayerEffect{};
    if (current_opacity > 0.0 && motion_config && motion.effect_opacity > 0.0f &&
        motion.effect_size > 0.0f && layer.type != LayerType::Clock) {
        const double frame_seconds = std::max(1.0 / 240.0, source_frame_duration());
        const double shutter_seconds = frame_seconds *
            std::clamp((double)motion.effect_size, 0.0, 720.0) / 360.0;
        const double start = std::clamp(
            title_time + shutter_seconds * (motion.effect_centered ? -0.5 : -1.0),
            0.0, std::max(0.0, title.duration));
        const double end = std::clamp(
            title_time + shutter_seconds * (motion.effect_centered ? 0.5 : 0.0),
            0.0, std::max(0.0, title.duration));
        const double travel = layer_shutter_travel_pixels(title, layer, start, end);
        /* Temporal supersampling stays fully on the GPU, but unbounded sample
         * counts turn one blurred layer into dozens of full-frame draws. Use a
         * perceptual density and a strict real-time budget; the current sharp
         * sample is preserved separately, so long moves remain readable. */
        const int sample_cap = layer.type == LayerType::Image ? 12 : 10;
        const int configured = std::clamp(motion.effect_samples, 2, sample_cap);
        const double density = layer.type == LayerType::Image ? 0.32 : 0.25;
        const int adaptive = static_cast<int>(std::ceil(travel * density)) + 1;
        const int samples = std::clamp(std::max(configured, adaptive), 2,
                                       sample_cap);
        const double mix = std::clamp(static_cast<double>(motion.effect_opacity),
                                      0.0, 1.0);
        gs_enable_blending(true);
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
        if (travel < 0.01) {
            draw_gpu_layer_texture(session, title, layer, local_texture, entry.origin,
                                   entry.width, entry.height,
                                   entry.logical_width, entry.logical_height,
                                   entry.image_clip_rect, entry.base_box_width, entry.base_box_height,
                                   title_time, static_cast<float>(current_opacity));
        } else {
            if (mix < 1.0)
                draw_gpu_layer_texture(session, title, layer, local_texture,
                                       entry.origin, entry.width, entry.height,
                                       entry.logical_width, entry.logical_height,
                                       entry.image_clip_rect, entry.base_box_width, entry.base_box_height,
                                       title_time,
                                       static_cast<float>(current_opacity *
                                                          (1.0 - mix)));

            std::vector<double> visible_samples;
            visible_samples.reserve(static_cast<std::size_t>(samples));
            for (int i = 0; i < samples; ++i) {
                const double f = (static_cast<double>(i) + 0.5) /
                                 static_cast<double>(samples);
                const double sample_time = start + (end - start) * f;
                if (layer_chain_visible(title, layer, sample_time))
                    visible_samples.push_back(sample_time);
            }
            /* Shutter intervals outside the layer's visibility range are
             * transparent samples. Divide by the requested sample count, not
             * by visible_samples.size(); renormalizing only the visible samples
             * caused the final blur frame to pop to a different opacity. */
            const float sample_weight = static_cast<float>(
                current_opacity * mix / static_cast<double>(samples));
            for (double sample_time : visible_samples) {
                draw_gpu_layer_texture(session, title, layer, local_texture,
                                       entry.origin, entry.width,
                                       entry.height, entry.logical_width,
                                       entry.logical_height, entry.image_clip_rect,
                                       entry.base_box_width, entry.base_box_height, sample_time,
                                       sample_weight);
            }
        }
    } else if (current_opacity > 0.0) {
        gs_enable_blending(true);
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
        draw_gpu_layer_texture(session, title, layer, local_texture, entry.origin,
                               entry.width, entry.height, entry.logical_width,
                               entry.logical_height, entry.image_clip_rect,
                               entry.base_box_width, entry.base_box_height, title_time,
                               (float)current_opacity);
    }
    gs_texrender_end(target);
    return true;
}

static gs_texture_t *apply_gpu_mask(TitleGpuRenderSession *session,
                                    gs_texture_t *layer_texture,
                                    gs_texture_t *mask_texture,
                                    MaskMode mode)
{
    if (!session || !layer_texture || !mask_texture || mode == MaskMode::None)
        return layer_texture;
    if (!session->mask_effect)
        session->mask_effect = gs_effect_create(
            kGpuMaskEffect, "obs-bgs-gpu-mask.effect", nullptr);
    if (!session->mask_effect) {
        session->last_error = "Could not compile GPU mask shader.";
        return nullptr;
    }
    struct vec4 clear;
    vec4_zero(&clear);
    if (!begin_gpu_target(session->masked_target, session->width, session->height, clear))
        return nullptr;
    gs_eparam_t *image = gs_effect_get_param_by_name(session->mask_effect, "image");
    gs_eparam_t *mask = gs_effect_get_param_by_name(session->mask_effect, "maskImage");
    gs_eparam_t *mask_mode = gs_effect_get_param_by_name(session->mask_effect, "maskMode");
    if (!image || !mask || !mask_mode) {
        gs_texrender_end(session->masked_target);
        return nullptr;
    }
    gs_effect_set_texture(image, layer_texture);
    gs_effect_set_texture(mask, mask_texture);
    gs_effect_set_int(mask_mode, (int)mode);
    gs_enable_blending(false);
    while (gs_effect_loop(session->mask_effect, "Draw"))
        gs_draw_sprite(layer_texture, 0, session->width, session->height);
    gs_texrender_end(session->masked_target);
    return gs_texrender_get_texture(session->masked_target);
}

static bool copy_full_canvas_gpu_texture(TitleGpuRenderSession *session,
                                         gs_texture_t *texture,
                                         gs_texrender_t *target)
{
    if (!session || !target)
        return false;
    struct vec4 clear;
    vec4_zero(&clear);
    if (!begin_gpu_target(target, session->width, session->height, clear))
        return false;
    if (texture) {
        gs_eparam_t *image = session->blit_effect
            ? gs_effect_get_param_by_name(session->blit_effect, "image")
            : nullptr;
        if (!session->blit_effect || !image) {
            gs_texrender_end(target);
            return false;
        }
        gs_effect_set_texture(image, texture);
        gs_enable_blending(false);
        while (gs_effect_loop(session->blit_effect, "Draw"))
            gs_draw_sprite(texture, 0, session->width, session->height);
    }
    gs_texrender_end(target);
    return true;
}

static std::string gpu_mask_texture_key(
    const TitleGpuRenderSession *session, const Layer &layer,
    double title_time, const std::string &raster_id,
    const TitleGpuRenderSession::LayerRaster &raster)
{
    std::ostringstream key;
    key << "phase13-mask-v1|title=" << session->title.id
        << "|model=" << session->model_revision
        << "|layer=" << layer.id
        << "|raster=" << raster_id
        << "|raster-key=" << raster.key
        << "|time=" << std::fixed << std::setprecision(9) << title_time
        << "|surface=" << session->width << 'x' << session->height
        << "|draft=" << (session->editor_draft ? 1 : 0)
        << "|scale=" << std::setprecision(6)
        << session->preview_quality_scale;
    return key.str();
}

static void destroy_gpu_mask_cache_entry(
    TitleGpuRenderSession::MaskTextureCacheEntry &entry)
{
    for (gs_texrender_t *&target : entry.targets) {
        if (target)
            gs_texrender_destroy(target);
        target = nullptr;
    }
    entry.active_target = -1;
}

static void prune_gpu_mask_texture_cache(TitleGpuRenderSession *session)
{
    constexpr std::size_t kMaximumMaskTextures = 64;
    if (!session || session->mask_texture_cache.size() <= kMaximumMaskTextures)
        return;

    const uint64_t keep_after = session->mask_texture_cache_tick > 120
        ? session->mask_texture_cache_tick - 120 : 0;
    for (auto it = session->mask_texture_cache.begin();
         it != session->mask_texture_cache.end() &&
         session->mask_texture_cache.size() > kMaximumMaskTextures;) {
        if (it->second.last_used >= keep_after) {
            ++it;
            continue;
        }
        destroy_gpu_mask_cache_entry(it->second);
        it = session->mask_texture_cache.erase(it);
    }

    /* A project can legitimately use more than 64 masks in the same frame, so
     * age-based pruning alone is not a hard bound.  Evict the least-recently
     * used entries until the per-session GPU mask cache is bounded. */
    while (session->mask_texture_cache.size() > kMaximumMaskTextures) {
        auto victim = session->mask_texture_cache.end();
        for (auto it = session->mask_texture_cache.begin();
             it != session->mask_texture_cache.end(); ++it) {
            if (victim == session->mask_texture_cache.end() ||
                it->second.last_used < victim->second.last_used)
                victim = it;
        }
        if (victim == session->mask_texture_cache.end())
            break;
        destroy_gpu_mask_cache_entry(victim->second);
        session->mask_texture_cache.erase(victim);
    }
}

static gs_texture_t *render_gpu_mask_graph_texture(
    TitleGpuRenderSession *session, const Title &title, const Layer &layer,
    double title_time, const std::string &raster_id,
    std::unordered_set<std::string> &visiting)
{
    if (!session)
        return nullptr;
    const std::string resolved_raster_id = raster_id.empty()
        ? gpu_session_layer_id(layer) : raster_id;
    auto raster_found = session->layers.find(resolved_raster_id);
    if (raster_found == session->layers.end())
        return nullptr;
    if (!upload_gpu_layer_raster(
            session, raster_found->second) ||
        !raster_found->second.texture)
        return nullptr;

    const std::string visit_id = layer.id + '|' + resolved_raster_id;
    if (!visiting.insert(visit_id).second) {
        session->last_error = "GPU mask graph contains a cyclic track-matte dependency.";
        return nullptr;
    }

    const double resolved_time = gpu_layer_render_time(title, layer, title_time);
    const bool has_nested_mask = layer.mask_mode != MaskMode::None &&
                                 !layer.mask_source_id.empty();
    const bool effects_after_nested_mask = has_nested_mask &&
                                           layer.effect_stack_respects_masks;
    gs_texture_t *nested_mask_texture = nullptr;
    bool nested_mask_available = false;
    if (has_nested_mask) {
        const Layer *nested = find_layer_by_id(title, layer.mask_source_id);
        const double nested_time = nested
            ? gpu_layer_render_time(title, *nested, title_time)
            : title_time;
        if (nested && layer_chain_visible(title, *nested, nested_time)) {
            const std::string nested_raster_id =
                is_gpu_scene_mask_raster_id(resolved_raster_id)
                    ? gpu_scene_mask_raster_id(nested->id)
                    : gpu_session_layer_id(*nested);
            nested_mask_texture = render_gpu_mask_graph_texture(
                session, title, *nested, title_time,
                nested_raster_id, visiting);
            nested_mask_available = nested_mask_texture != nullptr;
        }
    }

    const std::string cache_slot = resolved_raster_id;
    std::string desired_key = gpu_mask_texture_key(
        session, layer, resolved_time, resolved_raster_id,
        raster_found->second);
    desired_key += "|mode=" + std::to_string(static_cast<int>(layer.mask_mode));
    if (has_nested_mask) {
        const std::string nested_slot =
            is_gpu_scene_mask_raster_id(resolved_raster_id)
                ? gpu_scene_mask_raster_id(layer.mask_source_id)
                : layer.mask_source_id;
        const auto nested_cache = session->mask_texture_cache.find(nested_slot);
        desired_key += "|nested=";
        if (nested_cache != session->mask_texture_cache.end())
            desired_key += nested_cache->second.key;
        else
            desired_key += nested_mask_available ? "uncached" : "missing";
    }
    auto existing = session->mask_texture_cache.find(cache_slot);
    if (existing != session->mask_texture_cache.end() &&
        existing->second.key == desired_key &&
        existing->second.active_target >= 0 &&
        existing->second.width == session->width &&
        existing->second.height == session->height) {
        existing->second.last_used = ++session->mask_texture_cache_tick;
        visiting.erase(visit_id);
        return gs_texrender_get_texture(
            existing->second.targets[existing->second.active_target]);
    }

    gs_texture_t *matte_texture = nullptr;
    const bool nested_hides_layer = has_nested_mask &&
        !nested_mask_available && !mask_mode_is_inverted(layer.mask_mode);
    if (!nested_hides_layer) {
        if (!render_gpu_layer_to_target(session, title, layer, resolved_time,
                                        session->mask_target,
                                        !effects_after_nested_mask,
                                        resolved_raster_id)) {
            visiting.erase(visit_id);
            return nullptr;
        }
        matte_texture = gs_texrender_get_texture(session->mask_target);
        if (has_nested_mask && nested_mask_available) {
            matte_texture = apply_gpu_mask(session, matte_texture,
                                            nested_mask_texture,
                                            layer.mask_mode);
        }
        if (effects_after_nested_mask && matte_texture) {
            matte_texture = apply_gpu_layer_effect_stack(
                session, layer, resolved_time, matte_texture,
                session->width, session->height);
        }
    }

    auto &cache = session->mask_texture_cache[cache_slot];
    const int render_index = cache.active_target == 0 ? 1 : 0;
    if (!cache.targets[render_index])
        cache.targets[render_index] = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    if (!cache.targets[render_index] ||
        !copy_full_canvas_gpu_texture(session, matte_texture,
                                      cache.targets[render_index])) {
        visiting.erase(visit_id);
        return nullptr;
    }
    cache.active_target = render_index;
    cache.width = session->width;
    cache.height = session->height;
    cache.key = desired_key;
    cache.last_used = ++session->mask_texture_cache_tick;
    gs_texture_t *published = gs_texrender_get_texture(
        cache.targets[cache.active_target]);
    visiting.erase(visit_id);
    return published;
}

static gs_texture_t *render_gpu_mask_graph_texture(
    TitleGpuRenderSession *session, const Title &title, const Layer &layer,
    double title_time, const std::string &raster_id = std::string())
{
    std::unordered_set<std::string> visiting;
    return render_gpu_mask_graph_texture(session, title, layer, title_time,
                                         raster_id, visiting);
}

static bool composite_gpu_frame_layer(TitleGpuRenderSession *session,
                                      gs_texture_t *background,
                                      gs_texture_t *foreground,
                                      EffectBlendMode mode,
                                      gs_texrender_t *target)
{
    if (!session || !background || !foreground || !target)
        return false;
    if (!session->blend_effect)
        session->blend_effect = gs_effect_create(
            kGpuFrameBlendEffect, "obs-bgs-gpu-frame-blend.effect", nullptr);
    if (!session->blend_effect) {
        session->last_error = "Could not compile GPU frame-blend shader.";
        return false;
    }
    struct vec4 clear;
    vec4_zero(&clear);
    if (!begin_gpu_target(target, session->width, session->height, clear))
        return false;
    gs_eparam_t *bg = gs_effect_get_param_by_name(session->blend_effect, "background");
    gs_eparam_t *fg = gs_effect_get_param_by_name(session->blend_effect, "foreground");
    gs_eparam_t *blend_mode = gs_effect_get_param_by_name(session->blend_effect, "blendMode");
    if (!bg || !fg || !blend_mode) {
        gs_texrender_end(target);
        return false;
    }
    gs_effect_set_texture(bg, background);
    gs_effect_set_texture(fg, foreground);
    gs_effect_set_int(blend_mode, (int)mode);
    gs_enable_blending(false);
    while (gs_effect_loop(session->blend_effect, "Draw"))
        gs_draw_sprite(background, 0, session->width, session->height);
    gs_texrender_end(target);
    return gs_texrender_get_texture(target) != nullptr;
}

static bool gpu_cached_image_rect(const QImage &image, const Title &title,
                                  QRect &destination)
{
    return bgs::cache_frame_payload::resolve_placement(
        image, std::max(1, title.width), std::max(1, title.height),
        destination);
}

static void evict_gpu_cached_frame_textures(TitleGpuRenderSession *session)
{
    if (!session)
        return;
    while (session->cached_frame_texture_bytes >
               session->cached_frame_texture_budget &&
           !session->cached_frame_textures.empty()) {
        auto victim = session->cached_frame_textures.end();
        for (auto it = session->cached_frame_textures.begin();
             it != session->cached_frame_textures.end(); ++it) {
            if (it->first == session->submitted_final_image_key ||
                it->first == session->base_frame_image_key)
                continue;
            if (victim == session->cached_frame_textures.end() ||
                it->second.last_used < victim->second.last_used)
                victim = it;
        }
        if (victim == session->cached_frame_textures.end())
            break;
        if (victim->second.texture)
            gs_texture_destroy(victim->second.texture);
        session->cached_frame_texture_bytes -= std::min(
            session->cached_frame_texture_bytes, victim->second.bytes);
        session->cached_frame_textures.erase(victim);
    }
}

static bool upload_gpu_cached_image(TitleGpuRenderSession *session,
                                    QImage &pending_image, bool &pending,
                                    qint64 image_key,
                                    gs_texture_t *&texture,
                                    uint32_t &texture_width,
                                    uint32_t &texture_height)
{
    if (!session)
        return false;

    if (image_key != 0) {
        auto cached = session->cached_frame_textures.find(image_key);
        if (cached != session->cached_frame_textures.end()) {
            cached->second.last_used = ++session->cached_frame_texture_tick;
            texture = cached->second.texture;
            texture_width = cached->second.width;
            texture_height = cached->second.height;
            pending_image = QImage();
            pending = false;
            return texture != nullptr;
        }
    }

    if (pending) {
        QImage upload = pending_image;
        if (upload.isNull())
            return false;
        if (upload.format() != QImage::Format_ARGB32_Premultiplied)
            upload = upload.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (upload.bytesPerLine() != upload.width() * 4)
            upload = upload.copy();
        if (upload.isNull() || upload.bytesPerLine() != upload.width() * 4)
            return false;

        const uint32_t width = static_cast<uint32_t>(upload.width());
        const uint32_t height = static_cast<uint32_t>(upload.height());
        const uint8_t *planes[1] = {upload.constBits()};
        gs_texture_t *created = gs_texture_create(width, height, GS_BGRA, 1,
                                                  planes, GS_DYNAMIC);
        if (!created)
            return false;

        if (image_key != 0) {
            TitleGpuRenderSession::CachedFrameTexture entry;
            entry.texture = created;
            entry.width = width;
            entry.height = height;
            entry.bytes = static_cast<quint64>(width) *
                          static_cast<quint64>(height) * 4ull;
            entry.last_used = ++session->cached_frame_texture_tick;
            session->cached_frame_texture_bytes += entry.bytes;
            session->cached_frame_textures.insert_or_assign(image_key, entry);
            texture = created; // borrowed from cached_frame_textures
            evict_gpu_cached_frame_textures(session);
        } else {
            /* Unkeyed payloads are rare (temporary screenshots). Preserve the
             * old single-texture behavior for them. */
            if (texture)
                gs_texture_destroy(texture);
            texture = created;
        }
        texture_width = width;
        texture_height = height;
        pending_image = QImage();
        pending = false;
    }
    return texture != nullptr;
}

static bool draw_gpu_cached_image(TitleGpuRenderSession *session,
                                  gs_texture_t *texture,
                                  const QRect &destination,
                                  gs_texrender_t *target)
{
    if (!session || !texture || !destination.isValid() ||
        destination.isEmpty() || !target)
        return false;
    struct vec4 clear;
    vec4_zero(&clear);
    if (!begin_gpu_target(target, session->width, session->height, clear))
        return false;

    gs_eparam_t *image = gs_effect_get_param_by_name(session->blit_effect,
                                                     "image");
    if (!image) {
        gs_texrender_end(target);
        return false;
    }
    gs_effect_set_texture(image, texture);
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    gs_matrix_push();
    gs_matrix_identity();
    gs_matrix_translate3f(static_cast<float>(destination.x()),
                          static_cast<float>(destination.y()), 0.0f);
    while (gs_effect_loop(session->blit_effect, "Draw"))
        gs_draw_sprite(texture, 0,
                       static_cast<uint32_t>(destination.width()),
                       static_cast<uint32_t>(destination.height()));
    gs_matrix_pop();
    gs_texrender_end(target);
    return gs_texrender_get_texture(target) != nullptr;
}

static uint64_t gpu_ram_frame_metadata_bytes(std::size_t tile_count)
{
    return static_cast<uint64_t>(sizeof(GpuRamFrameEntry)) + 128ull +
           static_cast<uint64_t>(tile_count) *
               (static_cast<uint64_t>(sizeof(GpuRamTileRef)) + 64ull);
}

static void release_gpu_ram_frame_locked(GpuRamFrameEntry &frame)
{
    g_gpu_frame_cache_bytes -= std::min(
        g_gpu_frame_cache_bytes, frame.metadata_bytes);
    for (const GpuRamTileRef &reference : frame.tiles) {
        auto tile = g_gpu_ram_tiles.find(reference.digest);
        if (tile == g_gpu_ram_tiles.end())
            continue;
        if (tile->second.references > 0)
            --tile->second.references;
        if (tile->second.references != 0)
            continue;
        if (tile->second.texture)
            gs_texture_destroy(tile->second.texture);
        g_gpu_frame_cache_bytes -= std::min(
            g_gpu_frame_cache_bytes, tile->second.bytes);
        g_gpu_ram_tiles.erase(tile);
    }
    frame = {};
}

static void evict_global_gpu_frame_cache_locked()
{
    while (g_gpu_frame_cache_bytes > g_gpu_frame_cache_budget &&
           !g_gpu_frame_cache.empty()) {
        auto victim = g_gpu_frame_cache.end();
        for (auto it = g_gpu_frame_cache.begin();
             it != g_gpu_frame_cache.end(); ++it) {
            if (victim == g_gpu_frame_cache.end() ||
                it->second.last_used < victim->second.last_used)
                victim = it;
        }
        if (victim == g_gpu_frame_cache.end())
            break;
        release_gpu_ram_frame_locked(victim->second);
        g_gpu_frame_cache.erase(victim);
    }
}

static std::string gpu_ram_tile_digest_key(
    const bgs::cache_tile_payload::Tile &tile)
{
    if (tile.digest.isEmpty())
        return std::string();
    return tile.digest.toHex().toStdString();
}

static bool acquire_gpu_ram_tile_locked(
    const bgs::cache_tile_payload::Tile &tile, std::string &digest_key)
{
    digest_key = gpu_ram_tile_digest_key(tile);
    if (digest_key.empty() || tile.image.isNull() ||
        tile.rect.size() != tile.image.size())
        return false;

    auto found = g_gpu_ram_tiles.find(digest_key);
    if (found != g_gpu_ram_tiles.end()) {
        ++found->second.references;
        found->second.last_used = ++g_gpu_frame_cache_tick;
        return true;
    }

    QImage upload = tile.image.format() == QImage::Format_ARGB32_Premultiplied
        ? tile.image
        : tile.image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (upload.bytesPerLine() != upload.width() * 4)
        upload = upload.copy();
    if (upload.isNull() || upload.bytesPerLine() != upload.width() * 4)
        return false;

    const uint8_t *planes[1] = {upload.constBits()};
    gs_texture_t *texture = gs_texture_create(
        static_cast<uint32_t>(upload.width()),
        static_cast<uint32_t>(upload.height()), GS_BGRA, 1, planes, 0);
    if (!texture)
        return false;

    GpuRamTileEntry entry;
    entry.texture = texture;
    entry.width = static_cast<uint32_t>(upload.width());
    entry.height = static_cast<uint32_t>(upload.height());
    entry.bytes = static_cast<uint64_t>(entry.width) *
                      static_cast<uint64_t>(entry.height) * 4ull +
                  static_cast<uint64_t>(sizeof(GpuRamTileEntry)) + 96ull;
    entry.references = 1;
    entry.last_used = ++g_gpu_frame_cache_tick;
    g_gpu_frame_cache_bytes += entry.bytes;
    g_gpu_ram_tiles.emplace(digest_key, entry);
    return true;
}

static void release_acquired_gpu_ram_tiles_locked(
    const std::vector<GpuRamTileRef> &references)
{
    for (const GpuRamTileRef &reference : references) {
        auto tile = g_gpu_ram_tiles.find(reference.digest);
        if (tile == g_gpu_ram_tiles.end())
            continue;
        if (tile->second.references > 0)
            --tile->second.references;
        if (tile->second.references != 0)
            continue;
        if (tile->second.texture)
            gs_texture_destroy(tile->second.texture);
        g_gpu_frame_cache_bytes -= std::min(
            g_gpu_frame_cache_bytes, tile->second.bytes);
        g_gpu_ram_tiles.erase(tile);
    }
}

static bool store_global_gpu_frame_tiles_locked(
    const std::string &cache_key,
    const QVector<bgs::cache_tile_payload::Tile> &extracted,
    uint32_t canvas_width, uint32_t canvas_height)
{
    if (cache_key.empty() || canvas_width == 0 || canvas_height == 0)
        return false;

    std::vector<GpuRamTileRef> acquired;
    acquired.reserve(static_cast<std::size_t>(extracted.size()));
    for (const auto &tile : extracted) {
        std::string digest;
        if (!acquire_gpu_ram_tile_locked(tile, digest)) {
            release_acquired_gpu_ram_tiles_locked(acquired);
            return false;
        }
        GpuRamTileRef reference;
        reference.digest = std::move(digest);
        reference.destination = tile.rect;
        acquired.push_back(std::move(reference));
    }

    auto existing = g_gpu_frame_cache.find(cache_key);
    if (existing != g_gpu_frame_cache.end()) {
        release_gpu_ram_frame_locked(existing->second);
        g_gpu_frame_cache.erase(existing);
    }

    GpuRamFrameEntry frame;
    frame.width = canvas_width;
    frame.height = canvas_height;
    frame.tiles = std::move(acquired);
    frame.metadata_bytes = gpu_ram_frame_metadata_bytes(frame.tiles.size());
    frame.last_used = ++g_gpu_frame_cache_tick;
    g_gpu_frame_cache_bytes += frame.metadata_bytes;
    g_gpu_frame_cache.emplace(cache_key, std::move(frame));
    evict_global_gpu_frame_cache_locked();
    return g_gpu_frame_cache.find(cache_key) != g_gpu_frame_cache.end();
}

static bool draw_global_gpu_frame_locked(TitleGpuRenderSession *session,
                                         const std::string &cache_key,
                                         gs_texrender_t *target)
{
    if (!session || cache_key.empty() || !target || !session->blit_effect)
        return false;
    std::lock_guard<std::mutex> cache_lock(g_gpu_frame_cache_mutex);
    auto found = g_gpu_frame_cache.find(cache_key);
    if (found == g_gpu_frame_cache.end() ||
        found->second.width != session->width ||
        found->second.height != session->height)
        return false;

    struct vec4 clear;
    vec4_zero(&clear);
    if (!begin_gpu_target(target, session->width, session->height, clear))
        return false;

    gs_eparam_t *image = gs_effect_get_param_by_name(session->blit_effect,
                                                     "image");
    if (!image) {
        gs_texrender_end(target);
        return false;
    }

    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    bool complete = true;
    for (const GpuRamTileRef &reference : found->second.tiles) {
        auto tile = g_gpu_ram_tiles.find(reference.digest);
        if (tile == g_gpu_ram_tiles.end() || !tile->second.texture) {
            complete = false;
            break;
        }
        gs_effect_set_texture(image, tile->second.texture);
        gs_matrix_push();
        gs_matrix_identity();
        gs_matrix_translate3f(
            static_cast<float>(reference.destination.x()),
            static_cast<float>(reference.destination.y()), 0.0f);
        while (gs_effect_loop(session->blit_effect, "Draw"))
            gs_draw_sprite(
                tile->second.texture, 0,
                static_cast<uint32_t>(reference.destination.width()),
                static_cast<uint32_t>(reference.destination.height()));
        gs_matrix_pop();
        tile->second.last_used = ++g_gpu_frame_cache_tick;
    }
    gs_texrender_end(target);
    if (!complete)
        return false;
    found->second.last_used = ++g_gpu_frame_cache_tick;
    return gs_texrender_get_texture(target) != nullptr;
}

static bool gpu_session_final_matches_model(
    const TitleGpuRenderSession *session)
{
    return session && session->final_texture && session->has_title &&
           session->published_model_revision == session->model_revision &&
           session->published_title_id == session->title.id;
}

static bool gpu_session_has_published_frame_for_current_title(
    const TitleGpuRenderSession *session)
{
    /* Interactive edits advance model_revision before the replacement raster
     * reaches the graphics thread. The previously published frame is still a
     * safe fallback when it belongs to the same non-empty title identity; it
     * must not be rejected merely because its revision is one edit behind.
     * Lifecycle invalidation clears final_texture/published_title_id before a
     * title, project or scene-collection generation can be replaced. */
    return session && session->final_texture && session->has_title &&
           !session->title.id.empty() &&
           session->published_title_id == session->title.id;
}

static gs_texture_t *publish_stable_gpu_frame(TitleGpuRenderSession *session,
                                              gs_texture_t *frame)
{
    if (!session || !frame)
        return nullptr;
    const int publish_index = session->active_presentation_target == 0 ? 1 : 0;
    gs_texrender_t *target = session->presentation_targets[publish_index];
    if (!target ||
        !draw_gpu_cached_image(session, frame,
                               QRect(0, 0, static_cast<int>(session->width),
                                     static_cast<int>(session->height)),
                               target)) {
        return nullptr;
    }
    gs_texture_t *published = gs_texrender_get_texture(target);
    if (!published)
        return nullptr;
    session->active_presentation_target = publish_index;
    session->final_texture = published;
    session->published_title_id = session->title.id;
    session->published_model_revision = session->model_revision;
    return published;
}

class ScopedGpuCompositorState {
public:
    ScopedGpuCompositorState()
    {
        gs_viewport_push();
        gs_projection_push();
        gs_blend_state_push();
        gs_matrix_push();
        gs_matrix_identity();
    }
    ~ScopedGpuCompositorState()
    {
        gs_matrix_pop();
        gs_blend_state_pop();
        gs_projection_pop();
        gs_viewport_pop();
    }
};

static gs_texture_t *render_gpu_session_locked(TitleGpuRenderSession *session)
{
    if (!session || !session->has_title)
        return nullptr;
    if (session->state_transaction_pending.load(std::memory_order_acquire))
        return gpu_session_has_published_frame_for_current_title(session)
            ? session->final_texture : nullptr;
    const double render_scale = session->editor_draft
        ? std::clamp(static_cast<double>(session->preview_quality_scale), 0.25, 1.0)
        : 1.0;
    const uint32_t width = clamped_source_dimension(
        std::max(1, static_cast<int>(std::ceil(session->title.width * render_scale))));
    const uint32_t height = clamped_source_dimension(
        std::max(1, static_cast<int>(std::ceil(session->title.height * render_scale))));
    session->width = width;
    session->height = height;
    if (!ensure_gpu_session_objects(session, width, height))
        return nullptr;

    /* Raster uploads can execute shader draws (notably Phase 11 primitives).
     * Enter the isolated GPU state before either the complete-frame presentation
     * path or the live compositor path so no caller state leaks into texrender
     * operations. */
    ScopedGpuCompositorState state;

    /* A submitted final cache frame is already the complete compositor output.
     * It must be presented before inspecting or uploading live layer rasters.
     * The previous ordering validated every stale/pending text and shape raster
     * first, so a perfectly valid RAM/disk prerender was rejected with
     * "GPU raster set is incomplete". Fresh editor sessions hit this reliably;
     * OBS sources hit it nondeterministically depending on whether the live
     * first-frame raster happened to finish before the cache token arrived. */
    if (!session->frame_dirty && gpu_session_final_matches_model(session))
        return session->final_texture;

    if (session->use_gpu_cached_final) {
        if (!draw_global_gpu_frame_locked(
                session, session->submitted_gpu_cache_key,
                session->frame_a)) {
            session->last_error = "GPU RAM cache frame is no longer resident.";
            return nullptr;
        }
        gs_texture_t *rendered = gs_texrender_get_texture(session->frame_a);
        gs_texture_t *published = publish_stable_gpu_frame(session, rendered);
        if (!published) {
            session->last_error = "Could not publish GPU RAM cache frame.";
            return gpu_session_has_published_frame_for_current_title(session)
                ? session->final_texture : nullptr;
        }
        session->frame_dirty = false;
        session->last_error = nullptr;
        if (TitlePreferences::cache_playback_logging_enabled()) {
            BGL_LOG_INFO("CachePlayback", QStringLiteral(
                "stage=gpu-publish-ram-final title=%1 revision=%2 key=%3 published=%4 presentationTarget=%5")
                .arg(QString::fromStdString(session->title.id))
                .arg(session->model_revision)
                .arg(QString::fromStdString(session->submitted_gpu_cache_key))
                .arg(reinterpret_cast<quintptr>(published), 0, 16)
                .arg(session->active_presentation_target));
        }
        return published;
    }

    if (session->use_submitted_final) {
        if (!upload_gpu_cached_image(session,
                                     session->pending_submitted_final,
                                     session->submitted_final_pending,
                                     session->submitted_final_image_key,
                                     session->submitted_final_texture,
                                     session->submitted_final_width,
                                     session->submitted_final_height) ||
            !draw_gpu_cached_image(session, session->submitted_final_texture,
                                   session->submitted_final_rect,
                                   session->frame_a)) {
            session->last_error = "Could not upload cached GPU frame.";
            return nullptr;
        }
        session->uploaded_final_serial = session->submitted_final_serial;
        if (TitlePreferences::cache_playback_logging_enabled()) {
            BGL_LOG_INFO("CachePlayback", QStringLiteral(
                "stage=gpu-upload-final serial=%1 texture=%2 textureSize=%3x%4 rect=%5,%6,%7x%8")
                .arg(session->uploaded_final_serial)
                .arg(reinterpret_cast<quintptr>(session->submitted_final_texture), 0, 16)
                .arg(session->submitted_final_width).arg(session->submitted_final_height)
                .arg(session->submitted_final_rect.x()).arg(session->submitted_final_rect.y())
                .arg(session->submitted_final_rect.width()).arg(session->submitted_final_rect.height()));
        }
        gs_texture_t *rendered = gs_texrender_get_texture(session->frame_a);
        gs_texture_t *published = publish_stable_gpu_frame(session, rendered);
        if (!published) {
            session->last_error = "Could not publish cached GPU frame.";
            return gpu_session_has_published_frame_for_current_title(session)
                ? session->final_texture : nullptr;
        }
        session->published_final_serial = session->uploaded_final_serial;
        if (TitlePreferences::cache_playback_logging_enabled()) {
            BGL_LOG_INFO("CachePlayback", QStringLiteral(
                "stage=gpu-publish-final serial=%1 rendered=%2 published=%3 presentationTarget=%4")
                .arg(session->published_final_serial)
                .arg(reinterpret_cast<quintptr>(rendered), 0, 16)
                .arg(reinterpret_cast<quintptr>(published), 0, 16)
                .arg(session->active_presentation_target));
        }
        session->frame_dirty = false;
        session->last_error = nullptr;
        return published;
    }

    /* Only live composition and cached-prefix composition depend on layer
     * rasters. Validate them transactionally after the full-frame cache paths
     * have had the opportunity to publish their self-contained output. */
    bool all_required_rasters_ready = true;
    for (auto &pair : session->layers) {
        /* Scene-mask rasters are an auxiliary output family. They are uploaded
         * on demand by title_gpu_render_session_render_auxiliary_layer() and
         * must never gate the main title frame in OBS. */
        if (is_gpu_scene_mask_raster_id(pair.first))
            continue;
        auto &entry = pair.second;
        const bool was_pending = entry.pending_upload;
        /* A null pending image with no primitive is an intentional retirement,
         * not a failed replacement. The upload helper destroys the old texture
         * and returns false because there is no new raster. Treating that as a
         * missing required raster preserved the previous complete compositor
         * frame for one extra draw, visibly retaining deleted/hidden content. */
        const bool retiring = was_pending && !entry.gpu_text &&
                              !entry.gpu_primitive &&
                              entry.pending_image.isNull();
        const bool ready = upload_gpu_layer_raster(session, entry);
        if (was_pending && !ready && !retiring)
            all_required_rasters_ready = false;
    }

    /* Live frame publication remains transactional for the first frame as well
     * as later replacements. A same-title complete frame may remain visible
     * while live rasters retry; a new session/title returns no texture. */
    if (!all_required_rasters_ready) {
        session->frame_dirty = true;
        session->last_error =
            "GPU raster set is incomplete; frame publication deferred.";
        BGL_LOG_DEBUG("GpuPipeline", QStringLiteral(
            "stage=transactional-publication action=defer title=%1 revision=%2 hasPrevious=%3 mode=%4")
            .arg(QString::fromStdString(session->title.id))
            .arg(session->model_revision)
            .arg(gpu_session_has_published_frame_for_current_title(session) ? 1 : 0)
            .arg(session->use_base_frame ? QStringLiteral("cached-prefix")
                                         : QStringLiteral("live")));
        return gpu_session_has_published_frame_for_current_title(session)
            ? session->final_texture : nullptr;
    }

    if (session->use_base_frame && !session->use_gpu_cached_base &&
        !upload_gpu_cached_image(session,
                                 session->pending_base_frame,
                                 session->base_frame_pending,
                                 session->base_frame_image_key,
                                 session->base_frame_texture,
                                 session->base_frame_width,
                                 session->base_frame_height)) {
        session->last_error = "Could not upload cached GPU prefix.";
        return nullptr;
    }

    gs_texture_t *frame = nullptr;
    if (session->use_base_frame && session->use_gpu_cached_base) {
        if (!draw_global_gpu_frame_locked(
                session, session->base_gpu_cache_key,
                session->frame_a)) {
            session->last_error = "GPU RAM cache prefix is no longer resident.";
            return nullptr;
        }
        frame = gs_texrender_get_texture(session->frame_a);
    } else if (session->use_base_frame && session->base_frame_texture) {
        if (!draw_gpu_cached_image(session, session->base_frame_texture,
                                   session->base_frame_rect,
                                   session->frame_a)) {
            session->last_error = "Could not initialize GPU frame from cached prefix.";
            return nullptr;
        }
        frame = gs_texrender_get_texture(session->frame_a);
    } else {
        struct vec4 background;
        vec4_zero(&background);
        if (session->first_layer == 0) {
            double r, g, b, a;
            unpack_color(session->title.bg_color, r, g, b, a);
            vec4_set(&background, static_cast<float>(r * a),
                     static_cast<float>(g * a), static_cast<float>(b * a),
                     static_cast<float>(a));
        }
        if (!begin_gpu_target(session->frame_a, width, height, background))
            return nullptr;
        gs_texrender_end(session->frame_a);
        frame = gs_texrender_get_texture(session->frame_a);
    }

    bool current_is_a = true;
    const std::size_t layer_count = session->title.layers.size();
    const std::size_t first = std::min(session->first_layer, layer_count);
    const std::size_t last = std::min(session->last_layer, layer_count);
    for (std::size_t index = first; index < last; ++index) {
        const auto &layer_ptr = session->title.layers[index];
        if (!layer_ptr)
            continue;
        const Layer &layer = *layer_ptr;
        const double layer_time = gpu_layer_render_time(
            session->title, layer, session->time);
        if (!layer_chain_visible(session->title, layer, layer_time) ||
            !layer_should_render_as_visible_content(session->title, layer))
            continue;
        if (layer.type == LayerType::Adjustment) {
            gs_texture_t *adjusted = apply_gpu_layer_effect_stack(
                session, layer, layer_time, frame, width, height);
            if (adjusted && adjusted != frame) {
                gs_texrender_t *next = current_is_a ? session->frame_b
                                                    : session->frame_a;
                gs_texture_t *mixed = mix_gpu_adjustment_layer(
                    session, session->title, layer, layer_time,
                    frame, adjusted, next);
                if (mixed && mixed != frame) {
                    frame = mixed;
                    current_is_a = !current_is_a;
                }
            }
            continue;
        }
        const bool has_mask = layer.mask_mode != MaskMode::None &&
                              !layer.mask_source_id.empty();
        const bool effects_after_mask = has_mask &&
                                        layer.effect_stack_respects_masks;
        if (!render_gpu_layer_to_target(session, session->title, layer,
                                        layer_time, session->layer_target,
                                        !effects_after_mask))
            continue;
        gs_texture_t *foreground = gs_texrender_get_texture(session->layer_target);
        if (!foreground)
            continue;

        if (has_mask) {
            const Layer *mask_layer = find_layer_by_id(session->title,
                                                       layer.mask_source_id);
            const double mask_time = mask_layer
                ? gpu_layer_render_time(session->title, *mask_layer,
                                        session->time)
                : session->time;
            if (mask_layer &&
                layer_chain_visible(session->title, *mask_layer, mask_time)) {
                gs_texture_t *mask_texture = render_gpu_mask_graph_texture(
                    session, session->title, *mask_layer, session->time);
                if (mask_texture) {
                    gs_texture_t *masked = apply_gpu_mask(
                        session, foreground, mask_texture, layer.mask_mode);
                    if (masked)
                        foreground = masked;
                    else if (!mask_mode_is_inverted(layer.mask_mode))
                        continue;
                } else if (!mask_mode_is_inverted(layer.mask_mode)) {
                    continue;
                }
            } else if (!mask_mode_is_inverted(layer.mask_mode)) {
                continue;
            }
        }

        if (effects_after_mask) {
            foreground = apply_gpu_layer_effect_stack(
                session, layer, layer_time, foreground,
                session->width, session->height);
            if (!foreground)
                continue;
        }

        gs_texrender_t *next = current_is_a ? session->frame_b
                                            : session->frame_a;
        if (!composite_gpu_frame_layer(session, frame, foreground,
                                       layer.blend_mode, next))
            continue;
        frame = gs_texrender_get_texture(next);
        current_is_a = !current_is_a;
    }

    gs_texture_t *published = publish_stable_gpu_frame(session, frame);
    if (!published) {
        session->last_error = "Could not publish stable GPU frame.";
        return gpu_session_has_published_frame_for_current_title(session)
            ? session->final_texture : nullptr;
    }
    session->frame_dirty = false;
    prune_gpu_mask_texture_cache(session);
    session->last_error = nullptr;
    return published;
}

TitleGpuRenderSession *title_gpu_render_session_create()
{
    return new TitleGpuRenderSession();
}

void title_gpu_render_session_invalidate_presentation(
    TitleGpuRenderSession *session, bool discard_model)
{
    if (!session || session->destroying.load())
        return;

    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->destroying.load())
        return;

    /* Do not destroy texrender resources here: lifecycle invalidation can be
     * requested from the OBS frontend thread while video_render owns the
     * graphics context. Clearing only the published references is enough to
     * guarantee that the next draw cannot sample a texture from the previous
     * source/title/scene-collection generation. The resources are safely
     * reused or destroyed later on the graphics thread. */
    session->final_texture = nullptr;
    session->published_title_id.clear();
    session->published_model_revision =
        std::numeric_limits<uint64_t>::max();
    session->active_presentation_target = -1;
    session->published_final_serial = 0;

    session->use_submitted_final = false;
    session->use_gpu_cached_final = false;
    session->submitted_gpu_cache_key.clear();
    session->pending_submitted_final = QImage();
    session->submitted_final_pending = false;
    session->submitted_final_image_key = 0;
    session->submitted_final_rect = QRect();

    session->use_base_frame = false;
    session->use_gpu_cached_base = false;
    session->base_gpu_cache_key.clear();
    session->pending_base_frame = QImage();
    session->base_frame_pending = false;
    session->base_frame_image_key = 0;
    session->base_frame_rect = QRect();

    session->frame_dirty = true;
    session->last_error = nullptr;

    if (discard_model) {
        session->has_title = false;
        session->model_revision = std::numeric_limits<uint64_t>::max();
        session->time = 0.0;
        session->first_layer = 0;
        session->last_layer = std::numeric_limits<std::size_t>::max();
        /* Force every layer that survives by ID into a fresh raster-key
         * comparison. Stale non-live entries are retired by update_range and
         * destroyed from render_gpu_session_locked under the graphics lock. */
        for (auto &pair : session->layers) {
            pair.second.key.clear();
            pair.second.effect_cache_key.clear();
        }
        for (auto &pair : session->mask_texture_cache)
            pair.second.key.clear();
    }
}

void title_gpu_render_session_destroy(TitleGpuRenderSession *session)
{
    if (!session)
        return;
    bool expected = false;
    if (!session->destroying.compare_exchange_strong(expected, true))
        return;
    /* OBS display callbacks already execute under the graphics lock and call
     * title_gpu_render_session_draw(), which then takes session->mutex. Taking
     * the mutex first here creates the inverse order (session -> graphics) and
     * deadlocks shutdown against an in-flight canvas callback
     * (graphics -> session). Mark destruction first, wait for the graphics
     * callback to drain, and only then take the session mutex. */
    obs_enter_graphics();
    std::unique_lock<std::mutex> lock(session->mutex);
    for (auto &pair : session->layers) {
        if (pair.second.text_layer)
            release_gpu_text_layer(session, pair.second);
        if (pair.second.primitive_targets[0] || pair.second.primitive_targets[1]) {
            for (gs_texrender_t *target : pair.second.primitive_targets) {
                if (target)
                    gs_texrender_destroy(target);
            }
        } else if (pair.second.texture) {
            gs_texture_destroy(pair.second.texture);
        }
        if (pair.second.effect_cache)
            gs_texrender_destroy(pair.second.effect_cache);
    }
    if (session->text_renderer)
        session->text_renderer->reset();
    auto destroy_target = [](gs_texrender_t *&target) {
        if (target)
            gs_texrender_destroy(target);
        target = nullptr;
    };
    destroy_target(session->frame_a);
    destroy_target(session->frame_b);
    destroy_target(session->presentation_targets[0]);
    destroy_target(session->presentation_targets[1]);
    destroy_target(session->layer_target);
    destroy_target(session->mask_target);
    destroy_target(session->masked_target);
    destroy_target(session->effect_a);
    destroy_target(session->effect_b);
    destroy_target(session->blur_a);
    destroy_target(session->blur_b);
    if (session->stage)
        gs_stagesurface_destroy(session->stage);
    for (auto &slot : session->readback_slots) {
        if (slot.stage)
            gs_stagesurface_destroy(slot.stage);
        if (slot.crop_target)
            gs_texrender_destroy(slot.crop_target);
        slot = {};
    }
    for (auto &cached : session->cached_frame_textures) {
        if (cached.second.texture)
            gs_texture_destroy(cached.second.texture);
    }
    session->cached_frame_textures.clear();
    session->cached_frame_texture_bytes = 0;
    for (auto &cached : session->mask_texture_cache) {
        for (gs_texrender_t *target : cached.second.targets) {
            if (target)
                gs_texrender_destroy(target);
        }
    }
    session->mask_texture_cache.clear();
    for (auto &cached : session->transition_matte_textures) {
        if (cached.second.texture)
            gs_texture_destroy(cached.second.texture);
    }
    session->transition_matte_textures.clear();
    /* submitted_final_texture/base_frame_texture are borrowed from the pool
     * whenever they have a cacheKey. Unkeyed temporary textures are owned
     * directly by the corresponding field. */
    if (session->submitted_final_texture &&
        session->submitted_final_image_key == 0)
        gs_texture_destroy(session->submitted_final_texture);
    if (session->base_frame_texture && session->base_frame_image_key == 0)
        gs_texture_destroy(session->base_frame_texture);
    if (session->blit_effect)
        gs_effect_destroy(session->blit_effect);
    if (session->copy_effect)
        gs_effect_destroy(session->copy_effect);
    if (session->adjustment_mix_effect)
        gs_effect_destroy(session->adjustment_mix_effect);
    if (session->primitive_shape_effect)
        gs_effect_destroy(session->primitive_shape_effect);
    if (session->blend_effect)
        gs_effect_destroy(session->blend_effect);
    if (session->mask_effect)
        gs_effect_destroy(session->mask_effect);
    if (session->effect_registry)
        session->effect_registry->reset();
    lock.unlock();
    obs_leave_graphics();
    delete session;
}

void title_gpu_render_session_update_range(TitleGpuRenderSession *session,
                                           const Title &title,
                                           double time,
                                           uint64_t model_revision,
                                           std::size_t first_layer,
                                           std::size_t last_layer,
                                           bool transform_only_update)
{
    if (!session || session->destroying.load())
        return;
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->destroying.load())
        return;
    const bool was_submitted = session->use_submitted_final ||
                               session->use_gpu_cached_final;
    const bool had_base = session->use_base_frame;
    session->use_submitted_final = false;
    session->use_gpu_cached_final = false;
    session->submitted_gpu_cache_key.clear();
    session->pending_submitted_final = QImage();
    session->submitted_final_pending = false;
    session->use_base_frame = false;
    session->use_gpu_cached_base = false;
    session->base_gpu_cache_key.clear();
    session->pending_base_frame = QImage();
    session->base_frame_pending = false;

    const bool model_changed = !session->has_title ||
        session->model_revision != model_revision ||
        session->title.id != title.id;
    if (model_changed) {
        bool updated_transform_snapshot = false;
        if (transform_only_update && session->has_title &&
            session->title.id == title.id &&
            session->title.layers.size() == title.layers.size()) {
            updated_transform_snapshot = true;
            for (std::size_t i = 0; i < title.layers.size(); ++i) {
                const auto &source_layer = title.layers[i];
                const auto &snapshot_layer = session->title.layers[i];
                if (!source_layer || !snapshot_layer ||
                    source_layer->id != snapshot_layer->id ||
                    source_layer->type != snapshot_layer->type) {
                    updated_transform_snapshot = false;
                    break;
                }
            }
            if (updated_transform_snapshot) {
                for (std::size_t i = 0; i < title.layers.size(); ++i) {
                    const Layer &source_layer = *title.layers[i];
                    Layer &snapshot_layer = *session->title.layers[i];
                    snapshot_layer.position = source_layer.position;
                    snapshot_layer.scale = source_layer.scale;
                    snapshot_layer.rotation = source_layer.rotation;
                    snapshot_layer.parent_id = source_layer.parent_id;
                    /* During direct resize, present the old raster through a
                     * temporary GPU local-space scale. The settled edit will
                     * replace it with an exact new raster after mouse release. */
                    snapshot_layer.size = source_layer.size;
                    snapshot_layer.origin_prop = source_layer.origin_prop;
                    snapshot_layer.origin_x = source_layer.origin_x;
                    snapshot_layer.origin_y = source_layer.origin_y;
                    snapshot_layer.rect_width = source_layer.rect_width;
                    snapshot_layer.rect_height = source_layer.rect_height;
                    snapshot_layer.image_size = source_layer.image_size;
                    snapshot_layer.image_width = source_layer.image_width;
                    snapshot_layer.image_height = source_layer.image_height;
                }
            }
        }
        if (!updated_transform_snapshot) {
            session->title = clone_title_snapshot(title);
        }
        session->has_title = true;
        session->model_revision = model_revision;
    }

    const std::size_t layer_count = session->title.layers.size();
    const std::size_t resolved_first = std::min(first_layer, layer_count);
    const std::size_t resolved_last = std::max(
        resolved_first, std::min(last_layer, layer_count));
    const bool range_changed = session->first_layer != resolved_first ||
                               session->last_layer != resolved_last;
    session->first_layer = resolved_first;
    session->last_layer = resolved_last;

    const double clamped_time = std::clamp(
        time, 0.0, std::max(0.0, session->title.duration));
    const bool time_changed = std::abs(session->time - clamped_time) > 1e-9;
    session->time = clamped_time;

    /* Only rasterize the layer range that this graph will composite, plus any
     * track-matte sources it references. Parent layers are evaluated as
     * transforms and do not need their own raster texture. */
    std::unordered_set<std::string> needed_ids;
    needed_ids.reserve((resolved_last - resolved_first) * 2 + 4);
    for (std::size_t index = resolved_first; index < resolved_last; ++index) {
        const auto &layer = session->title.layers[index];
        if (!layer)
            continue;
        if (layer->type != LayerType::Adjustment)
            needed_ids.insert(gpu_session_layer_id(*layer));
        if (layer->type != LayerType::Adjustment &&
            layer->mask_mode != MaskMode::None &&
            !layer->mask_source_id.empty())
            needed_ids.insert(layer->mask_source_id);
    }
    bool mask_dependency_added = true;
    while (mask_dependency_added) {
        mask_dependency_added = false;
        std::vector<std::string> snapshot(needed_ids.begin(), needed_ids.end());
        for (const std::string &id : snapshot) {
            const Layer *dependency = find_layer_by_id(session->title, id);
            if (!dependency || dependency->mask_mode == MaskMode::None ||
                dependency->mask_source_id.empty())
                continue;
            mask_dependency_added = needed_ids.insert(
                dependency->mask_source_id).second || mask_dependency_added;
        }
    }

    bool raster_changed = false;
    std::unordered_set<std::string> live_ids;
    live_ids.reserve(needed_ids.size());
    const qint64 clock_second = QDateTime::currentSecsSinceEpoch();

    for (const auto &layer_ptr : session->title.layers) {
        if (!layer_ptr)
            continue;
        const Layer &layer = *layer_ptr;
        const std::string id = gpu_session_layer_id(layer);
        if (needed_ids.find(id) == needed_ids.end())
            continue;
        live_ids.insert(id);
        const double layer_time = gpu_layer_render_time(
            session->title, layer, session->time);
        Layer base_key_layer = layer;
        base_key_layer.effects.clear();
        const double base_local_time = std::max(0.0, layer_time - layer.in_time);
        QRect base_surface_rect =
            (layer.type == LayerType::Text || layer.type == LayerType::Clock)
                ? gpu_text_surface_rect(session->title, layer, base_local_time)
                : clipped_effect_surface_rect(
                      layer, base_local_time,
                      std::max(1, session->title.width),
                      std::max(1, session->title.height));
        const int transition_blur_padding =
            general_transition_blur_padding(layer);
        if (transition_blur_padding > 0) {
            base_surface_rect = base_surface_rect.adjusted(
                -transition_blur_padding, -transition_blur_padding,
                transition_blur_padding, transition_blur_padding);
        }
        std::string key = effect_layer_cache_key(
            nullptr, session->title, base_key_layer, layer_time,
            std::max(1, session->title.width),
            std::max(1, session->title.height), false, true) +
            "|gpu-preview-scale=" + std::to_string(session->editor_draft ? session->preview_quality_scale : 1.0f) +
            "|gpu-base-surface=" + std::to_string(base_surface_rect.x()) + ',' +
            std::to_string(base_surface_rect.y()) + ',' +
            std::to_string(base_surface_rect.width()) + 'x' +
            std::to_string(base_surface_rect.height());
        if (layer.type == LayerType::Clock)
            key += "|clock=" + std::to_string(clock_second);
        else if (layer.type == LayerType::Ticker)
            key += "|ticker=" + std::to_string(QDateTime::currentMSecsSinceEpoch());

        auto &entry = session->layers[id];
        /* Moving/rotating a layer or one of its parents changes only the GPU
         * world matrix. Keep the existing transform-neutral raster and avoid
         * synchronous text/vector/image regeneration on the UI thread. */
        const double current_box_width = std::max(
            0.0001, eval_box_width(layer, layer_time));
        const double current_box_height = std::max(
            0.0001, eval_box_height(layer, layer_time));
        const bool local_geometry_changed =
            std::abs(current_box_width - entry.base_box_width) > 0.0001 ||
            std::abs(current_box_height - entry.base_box_height) > 0.0001;
        /* During interactive resize, direct image layers keep their decoded
         * texture resident and update only logical box geometry. This works for
         * Stretch/Fit/Fill because layout is metadata; no bitmap rescale, SVG
         * reraster or texture upload is required until the settled edit. */
        const bool direct_image = layer_can_use_direct_gpu_image_raster(
            layer, layer_time, transition_blur_padding);
        if (transform_only_update && direct_image &&
            (entry.texture || entry.pending_upload || !entry.pending_image.isNull()) &&
            !entry.key.empty()) {
            QPointF updated_origin;
            double updated_width = 0.0;
            double updated_height = 0.0;
            if (direct_gpu_image_geometry(layer, layer_time, updated_origin,
                                          updated_width, updated_height)) {
                entry.origin = updated_origin;
                entry.logical_width = updated_width;
                entry.logical_height = updated_height;
                const double local_t = std::max(0.0, layer_time - layer.in_time);
                entry.layer_box_rect = QRectF(
                    -eval_origin_x(layer, local_t) * current_box_width - updated_origin.x(),
                    -eval_origin_y(layer, local_t) * current_box_height - updated_origin.y(),
                    current_box_width, current_box_height);
                const QRectF source_rect(0.0, 0.0, updated_width, updated_height);
                entry.image_clip_rect = layer.image_crop_when_outside_box
                    ? source_rect.intersected(entry.layer_box_rect)
                    : source_rect;
                entry.base_box_width = current_box_width;
                entry.base_box_height = current_box_height;
                raster_changed = raster_changed || local_geometry_changed;
                continue;
            }
        }
        if (transform_only_update && !local_geometry_changed &&
            (entry.texture || entry.pending_upload || !entry.pending_image.isNull()) &&
            !entry.key.empty())
            continue;
        const bool dynamic_raster = layer.type == LayerType::Clock ||
            layer.type == LayerType::Ticker ||
            layer_has_non_transform_animation(base_key_layer) ||
            active_text_layer_transition(layer, layer_time) != nullptr;
        if (!model_changed && !dynamic_raster && !local_geometry_changed &&
            !entry.key.empty())
            continue;
        if (entry.key == key)
            continue;
        /* Phase 12C consumes the immutable Phase 12B glyph layout first.
         * Unsupported font/color-glyph/transition semantics stay on the exact
         * compatibility raster, while successful text layers become atlas
         * quads rendered into an inactive persistent target. */
        const bool gpu_text_candidate = layer_can_use_gpu_text_raster(
            layer, layer_time, !session->text_backend_unavailable);
        const bool gpu_primitive = !gpu_text_candidate &&
            layer_can_use_gpu_primitive_raster(
                layer, layer_time, !session->primitive_backend_unavailable);
        entry.key = std::move(key);
        entry.base_box_width = current_box_width;
        entry.base_box_height = current_box_height;

        bool gpu_text_prepared = false;
        if (gpu_text_candidate) {
            std::string text_failure;
            gpu_text_prepared = prepare_gpu_text_raster(
                session, layer, layer_time, base_surface_rect, entry,
                &text_failure);
            if (!gpu_text_prepared && !text_failure.empty() &&
                TitlePreferences::cache_playback_logging_enabled()) {
                BGL_LOG_WARNING("GpuText", QStringLiteral(
                    "stage=phase12c-fallback layer=%1 reason=%2")
                    .arg(QString::fromStdString(layer.id),
                         QString::fromStdString(text_failure)));
            }
        }

        if (gpu_text_prepared) {
            /* Geometry/material batches are now pending in entry.text_layer. */
        } else if (gpu_primitive) {
            const ShapeType primitive_type =
                (layer.type == LayerType::SolidRect || layer.type == LayerType::ColorSolid)
                    ? ShapeType::RoundedRectangle : layer.shape_type;
            const bool has_stroke = layer_has_visible_outline(layer, base_local_time);
            const double padding = gpu_primitive_padding(
                layer, base_local_time);
            entry.gpu_text = false;
            entry.gpu_primitive = true;
            entry.pending_image = QImage();
            entry.primitive_shape_type = static_cast<int>(primitive_type);
            entry.primitive_vertex_count = primitive_type == ShapeType::Star
                ? std::clamp(layer.shape_points, 2, 64)
                : std::clamp(layer.shape_sides, 3, 64);
            entry.primitive_inner_radius = std::clamp(
                layer.shape_inner_radius * 2.0f, 0.0f, 1.0f);
            entry.primitive_outer_radius = std::clamp(
                layer.shape_outer_radius * 2.0f, 0.0001f, 1.0f);
            entry.primitive_stroke_width = has_stroke
                ? static_cast<float>(eval_outline_width(layer, base_local_time))
                : 0.0f;
            entry.primitive_stroke_alignment = eval_outline_alignment(
                layer, base_local_time);
            entry.primitive_stroke_on_front = eval_outline_on_front(
                layer, base_local_time);
            entry.primitive_shape_width = current_box_width;
            entry.primitive_shape_height = current_box_height;
            entry.primitive_padding = padding;
            entry.primitive_fill_color = eval_fill_color(layer, base_local_time);
            entry.primitive_stroke_color = has_stroke
                ? argb_with_multiplied_alpha(
                      eval_outline_color(layer, base_local_time),
                      eval_outline_opacity(layer, base_local_time))
                : 0x00000000u;
            if (primitive_type == ShapeType::Rectangle ||
                primitive_type == ShapeType::RoundedRectangle) {
                entry.primitive_corner_radii = {
                    std::max(0.0f, layer.corner_radius_tl),
                    std::max(0.0f, layer.corner_radius_tr),
                    std::max(0.0f, layer.corner_radius_br),
                    std::max(0.0f, layer.corner_radius_bl)};
            } else {
                entry.primitive_corner_radii = {0.0f, 0.0f, 0.0f, 0.0f};
            }
            entry.logical_width = current_box_width + padding * 2.0;
            entry.logical_height = current_box_height + padding * 2.0;
            entry.origin = QPointF(
                -eval_origin_x(layer, base_local_time) * current_box_width - padding,
                -eval_origin_y(layer, base_local_time) * current_box_height - padding);
            entry.layer_box_rect = QRectF(
                padding, padding, current_box_width, current_box_height);
            entry.image_clip_rect = QRectF();
        } else {
            const double raster_scale = session->editor_draft
                ? session->preview_quality_scale : 1.0;
            const MotionBaseRaster raster = render_gpu_layer_base_raster(
                session->title, layer, layer_time, raster_scale);
            entry.gpu_text = false;
            entry.gpu_primitive = false;
            entry.pending_image = raster.image;
            entry.origin = raster.origin;
            entry.logical_width = raster.logical_width > 0.0
                ? raster.logical_width : raster.image.width();
            entry.logical_height = raster.logical_height > 0.0
                ? raster.logical_height : raster.image.height();
            entry.layer_box_rect = raster.layer_box_rect;
            entry.image_clip_rect = raster.image_clip_rect;
        }
        entry.pending_upload = true;
        raster_changed = true;
    }

    for (auto it = session->layers.begin(); it != session->layers.end();) {
        if (is_gpu_scene_mask_raster_id(it->first) ||
            live_ids.find(it->first) != live_ids.end()) {
            ++it;
            continue;
        }
        if (it->second.texture || it->second.text_layer) {
            /* GPU destruction must occur on the graphics thread. */
            it->second.pending_image = QImage();
            it->second.gpu_text = false;
            it->second.gpu_primitive = false;
            it->second.pending_upload = true;
            it->second.key.clear();
            ++it;
        } else {
            it = session->layers.erase(it);
        }
        raster_changed = true;
    }

    session->frame_dirty = session->frame_dirty || was_submitted || had_base ||
                           model_changed || range_changed || time_changed ||
                           raster_changed;
}

void title_gpu_render_session_set_preview_quality(TitleGpuRenderSession *session,
                                                   double scale, bool editor_draft)
{
    if (!session || session->destroying.load())
        return;
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->destroying.load())
        return;
    const float resolved_scale = static_cast<float>(std::clamp(scale, 0.25, 1.0));
    if (std::abs(session->preview_quality_scale - resolved_scale) > 0.0001f ||
        session->editor_draft != editor_draft) {
        session->preview_quality_scale = resolved_scale;
        session->editor_draft = editor_draft;
        session->frame_dirty = true;
        for (auto &pair : session->layers) {
            pair.second.effect_cache_key.clear();
            pair.second.key.clear();
        }
    }
}

void title_gpu_render_session_update(TitleGpuRenderSession *session,
                                     const Title &title,
                                     double time,
                                     uint64_t model_revision,
                                     bool transform_only_update)
{
    title_gpu_render_session_update_range(session, title, time, model_revision,
                                          0, title.layers.size(),
                                          transform_only_update);
}

bool title_gpu_frame_cache_contains(const std::string &cache_key)
{
    if (cache_key.empty())
        return false;
    std::lock_guard<std::mutex> lock(g_gpu_frame_cache_mutex);
    return g_gpu_frame_cache.find(cache_key) != g_gpu_frame_cache.end();
}

bool title_gpu_frame_cache_store_image(
    const std::string &cache_key, const QImage &sparse_image,
    uint32_t canvas_width, uint32_t canvas_height)
{
    if (cache_key.empty() || sparse_image.isNull() ||
        canvas_width == 0 || canvas_height == 0)
        return false;

    bgs::cache_frame_payload::Placement placement;
    if (!bgs::cache_frame_payload::read_placement(sparse_image, placement)) {
        if (sparse_image.width() != static_cast<int>(canvas_width) ||
            sparse_image.height() != static_cast<int>(canvas_height))
            return false;
        placement.x = 0;
        placement.y = 0;
        placement.canvas_width = static_cast<int>(canvas_width);
        placement.canvas_height = static_cast<int>(canvas_height);
    }
    if (placement.canvas_width != static_cast<int>(canvas_width) ||
        placement.canvas_height != static_cast<int>(canvas_height))
        return false;

    /* Alpha scanning and hashing are CPU work. Complete them before taking the
     * OBS graphics context or the global cache mutex, otherwise a large frame
     * can stall live/editor presentation while its tiles are being analyzed. */
    const QVector<bgs::cache_tile_payload::Tile> extracted =
        bgs::cache_tile_payload::extract_nonempty_tiles(
            sparse_image, placement, kGpuRamTileSize);

    obs_enter_graphics();
    bool stored = false;
    {
        std::lock_guard<std::mutex> lock(g_gpu_frame_cache_mutex);
        stored = store_global_gpu_frame_tiles_locked(
            cache_key, extracted, canvas_width, canvas_height);
    }
    obs_leave_graphics();
    return stored;
}

void title_gpu_frame_cache_remove(const std::string &cache_key)
{
    if (cache_key.empty())
        return;
    obs_enter_graphics();
    {
        std::lock_guard<std::mutex> lock(g_gpu_frame_cache_mutex);
        auto found = g_gpu_frame_cache.find(cache_key);
        if (found != g_gpu_frame_cache.end()) {
            release_gpu_ram_frame_locked(found->second);
            g_gpu_frame_cache.erase(found);
        }
    }
    obs_leave_graphics();
}

void title_gpu_frame_cache_remove_title(const std::string &title_id)
{
    if (title_id.empty())
        return;
    const std::string prefix = title_id + "-";
    obs_enter_graphics();
    {
        std::lock_guard<std::mutex> lock(g_gpu_frame_cache_mutex);
        for (auto it = g_gpu_frame_cache.begin();
             it != g_gpu_frame_cache.end();) {
            if (it->first.compare(0, prefix.size(), prefix) != 0) {
                ++it;
                continue;
            }
            release_gpu_ram_frame_locked(it->second);
            it = g_gpu_frame_cache.erase(it);
        }
    }
    obs_leave_graphics();
}

void title_gpu_frame_cache_clear()
{
    obs_enter_graphics();
    {
        std::lock_guard<std::mutex> lock(g_gpu_frame_cache_mutex);
        for (auto &entry : g_gpu_frame_cache)
            release_gpu_ram_frame_locked(entry.second);
        g_gpu_frame_cache.clear();
        /* Defensive cleanup for a failed/aborted insertion. Normally every
         * shared tile reaches zero references through frame release. */
        for (auto &tile : g_gpu_ram_tiles) {
            if (tile.second.texture)
                gs_texture_destroy(tile.second.texture);
        }
        g_gpu_ram_tiles.clear();
        g_gpu_frame_cache_bytes = 0;
    }
    obs_leave_graphics();
}

void title_gpu_frame_cache_set_budget(uint64_t bytes)
{
    obs_enter_graphics();
    {
        std::lock_guard<std::mutex> lock(g_gpu_frame_cache_mutex);
        g_gpu_frame_cache_budget = std::max<uint64_t>(
            16ull * 1024ull * 1024ull, bytes);
        evict_global_gpu_frame_cache_locked();
    }
    obs_leave_graphics();
}

uint64_t title_gpu_frame_cache_bytes_used()
{
    std::lock_guard<std::mutex> lock(g_gpu_frame_cache_mutex);
    return g_gpu_frame_cache_bytes;
}

bool title_gpu_render_session_submit_gpu_cached_frame(
    TitleGpuRenderSession *session, const Title &title,
    const std::string &cache_key, uint64_t model_revision)
{
    if (!session || session->destroying.load() || cache_key.empty() ||
        !title_gpu_frame_cache_contains(cache_key))
        return false;

    std::lock_guard<std::mutex> lock(session->mutex);
    const bool model_changed = !session->has_title ||
        session->model_revision != model_revision ||
        session->title.id != title.id;
    if (model_changed) {
        session->title = clone_title_snapshot(title);
        session->has_title = true;
        session->model_revision = model_revision;
    }

    session->use_submitted_final = false;
    session->pending_submitted_final = QImage();
    session->submitted_final_pending = false;
    session->use_gpu_cached_final = true;
    session->submitted_gpu_cache_key = cache_key;
    session->use_base_frame = false;
    session->use_gpu_cached_base = false;
    session->base_gpu_cache_key.clear();
    session->pending_base_frame = QImage();
    session->base_frame_pending = false;
    session->first_layer = 0;
    session->last_layer = 0;
    session->frame_dirty = true;
    return true;
}

bool title_gpu_render_session_submit_gpu_cached_prefix(
    TitleGpuRenderSession *session, const Title &title,
    const std::string &cache_key, double time,
    std::size_t first_dynamic_layer, uint64_t model_revision)
{
    if (!session || session->destroying.load() || cache_key.empty() ||
        !title_gpu_frame_cache_contains(cache_key))
        return false;

    struct PrefixSubmissionTransaction {
        TitleGpuRenderSession *session = nullptr;
        explicit PrefixSubmissionTransaction(TitleGpuRenderSession *value)
            : session(value)
        {
            session->state_transaction_pending.store(true,
                                                     std::memory_order_release);
        }
        ~PrefixSubmissionTransaction()
        {
            session->state_transaction_pending.store(false,
                                                     std::memory_order_release);
        }
    } transaction(session);

    title_gpu_render_session_update_range(
        session, title, time, model_revision, first_dynamic_layer,
        title.layers.size());

    std::lock_guard<std::mutex> lock(session->mutex);
    session->use_submitted_final = false;
    session->use_gpu_cached_final = false;
    session->submitted_gpu_cache_key.clear();
    session->pending_submitted_final = QImage();
    session->submitted_final_pending = false;
    session->use_base_frame = true;
    session->use_gpu_cached_base = true;
    session->base_gpu_cache_key = cache_key;
    session->pending_base_frame = QImage();
    session->base_frame_pending = false;
    session->frame_dirty = true;
    return true;
}

bool title_gpu_render_session_submit_final_frame(TitleGpuRenderSession *session,
                                                 const Title &title,
                                                 const QImage &image,
                                                 uint64_t model_revision)
{
    if (!session || session->destroying.load() || image.isNull())
        return false;

    QRect destination;
    if (!gpu_cached_image_rect(image, title, destination)) {
        if (TitlePreferences::cache_playback_logging_enabled()) {
            BGL_LOG_WARNING("CachePlayback", QStringLiteral(
                "stage=gpu-reject-final title=%1 image=%2x%3 canvas=%4x%5 reason=invalid-placement")
                .arg(QString::fromStdString(title.id))
                .arg(image.width()).arg(image.height())
                .arg(std::max(1, title.width)).arg(std::max(1, title.height)));
        }
        return false;
    }

    /* QImage::cacheKey() is only an implicit-sharing identifier, not a
     * durable content identity. Reusing GPU textures by that value could show
     * a stale/invalid texture on later playback loops. Keep a session-owned
     * upload for correctness until cache frames carry an explicit immutable
     * frame identity. */
    const qint64 image_key = 0;
    std::lock_guard<std::mutex> lock(session->mutex);
    const bool model_changed = !session->has_title ||
        session->model_revision != model_revision || session->title.id != title.id;
    if (model_changed) {
        session->title = clone_title_snapshot(title);
        session->has_title = true;
        session->model_revision = model_revision;
    }
    if (image_key != 0 && session->use_submitted_final &&
        session->submitted_final_image_key == image_key &&
        session->submitted_final_rect == destination && !model_changed)
        return true;

    session->use_base_frame = false;
    session->use_gpu_cached_base = false;
    session->base_gpu_cache_key.clear();
    session->pending_base_frame = QImage();
    session->base_frame_pending = false;
    session->use_gpu_cached_final = false;
    session->submitted_gpu_cache_key.clear();
    session->use_submitted_final = true;
    session->pending_submitted_final = image;
    session->submitted_final_rect = destination;
    session->submitted_final_image_key = image_key;
    session->submitted_final_pending = true;
    const uint64_t submission_serial = ++session->submitted_final_serial;
    if (TitlePreferences::cache_playback_logging_enabled()) {
        BGL_LOG_INFO("CachePlayback", QStringLiteral(
            "stage=gpu-submit-final serial=%1 title=%2 modelRevision=%3 imageCacheKey=%4 size=%5x%6 rect=%7,%8,%9x%10")
            .arg(submission_serial)
            .arg(QString::fromStdString(title.id))
            .arg(model_revision)
            .arg(image.cacheKey())
            .arg(image.width()).arg(image.height())
            .arg(destination.x()).arg(destination.y())
            .arg(destination.width()).arg(destination.height()));
    }
    session->first_layer = 0;
    session->last_layer = 0;
    session->frame_dirty = true;
    return true;
}

bool title_gpu_render_session_submit_cached_prefix(
    TitleGpuRenderSession *session, const Title &title,
    const QImage &cached_prefix, double time,
    std::size_t first_dynamic_layer, uint64_t model_revision)
{
    if (!session || session->destroying.load() || cached_prefix.isNull())
        return false;

    QRect destination;
    if (!gpu_cached_image_rect(cached_prefix, title, destination)) {
        if (TitlePreferences::cache_playback_logging_enabled()) {
            BGL_LOG_WARNING("CachePlayback", QStringLiteral(
                "stage=gpu-reject-prefix title=%1 image=%2x%3 canvas=%4x%5 reason=invalid-placement")
                .arg(QString::fromStdString(title.id))
                .arg(cached_prefix.width()).arg(cached_prefix.height())
                .arg(std::max(1, title.width)).arg(std::max(1, title.height)));
        }
        return false;
    }

    struct PrefixSubmissionTransaction {
        TitleGpuRenderSession *session = nullptr;
        explicit PrefixSubmissionTransaction(TitleGpuRenderSession *value)
            : session(value)
        {
            session->state_transaction_pending.store(true,
                                                     std::memory_order_release);
        }
        ~PrefixSubmissionTransaction()
        {
            session->state_transaction_pending.store(false,
                                                     std::memory_order_release);
        }
    } transaction(session);

    title_gpu_render_session_update_range(
        session, title, time, model_revision, first_dynamic_layer,
        title.layers.size());

    const qint64 image_key = 0;
    std::lock_guard<std::mutex> lock(session->mutex);
    const bool needs_upload = !session->base_frame_texture ||
        session->base_frame_image_key != image_key ||
        session->base_frame_rect != destination;
    session->use_submitted_final = false;
    session->use_gpu_cached_final = false;
    session->submitted_gpu_cache_key.clear();
    session->pending_submitted_final = QImage();
    session->submitted_final_pending = false;
    session->use_base_frame = true;
    session->use_gpu_cached_base = false;
    session->base_gpu_cache_key.clear();
    session->base_frame_rect = destination;
    session->base_frame_image_key = image_key;
    if (needs_upload) {
        session->pending_base_frame = cached_prefix;
        session->base_frame_pending = true;
    }
    session->frame_dirty = true;
    return true;
}

static void title_gpu_render_session_prepare_auxiliary_layers(
    TitleGpuRenderSession *session, const Title &title, double time,
    uint64_t model_revision, const std::vector<std::string> &layer_ids)
{
    if (!session || layer_ids.empty())
        return;

    std::unordered_set<std::string> requested_ids;
    requested_ids.reserve(layer_ids.size());
    for (const std::string &id : layer_ids) {
        if (!id.empty())
            requested_ids.insert(id);
    }
    if (requested_ids.empty())
        return;

    /* A scene-mask layer may itself use a track matte.  Prepare the complete
     * dependency chain as auxiliary GPU inputs so cached title playback never
     * falls back to a CPU mask surface. */
    bool dependency_added = true;
    while (dependency_added) {
        dependency_added = false;
        std::vector<std::string> snapshot(requested_ids.begin(),
                                          requested_ids.end());
        for (const std::string &id : snapshot) {
            const Layer *layer = find_layer_by_id(title, id);
            if (!layer || layer->mask_mode == MaskMode::None ||
                layer->mask_source_id.empty())
                continue;
            dependency_added = requested_ids.insert(
                layer->mask_source_id).second || dependency_added;
        }
    }

    std::lock_guard<std::mutex> lock(session->mutex);
    const bool model_changed = !session->has_title ||
        session->model_revision != model_revision || session->title.id != title.id;
    if (model_changed) {
        session->title = clone_title_snapshot(title);
        session->has_title = true;
        session->model_revision = model_revision;
    }
    session->time = std::clamp(time, 0.0,
        std::max(0.0, session->title.duration));

    std::unordered_set<std::string> requested_raster_ids;
    requested_raster_ids.reserve(requested_ids.size());
    for (const std::string &id : requested_ids)
        requested_raster_ids.insert(gpu_scene_mask_raster_id(id));
    for (auto it = session->layers.begin(); it != session->layers.end();) {
        if (!is_gpu_scene_mask_raster_id(it->first) ||
            requested_raster_ids.find(it->first) != requested_raster_ids.end()) {
            ++it;
            continue;
        }
        if (it->second.texture || it->second.text_layer) {
            it->second.pending_image = QImage();
            it->second.gpu_text = false;
            it->second.gpu_primitive = false;
            it->second.pending_upload = true;
            it->second.key.clear();
            ++it;
        } else {
            it = session->layers.erase(it);
        }
    }

    const qint64 clock_second = QDateTime::currentSecsSinceEpoch();
    for (const std::string &id : requested_ids) {
        const Layer *layer = find_layer_by_id(session->title, id);
        if (!layer)
            continue;
        const double layer_time = gpu_layer_render_time(
            session->title, *layer, session->time);
        std::string key = effect_layer_cache_key(
            nullptr, session->title, *layer, layer_time,
            std::max(1, session->title.width),
            std::max(1, session->title.height), false, true) +
            "|gpu-scene-mask";
        if (layer->type == LayerType::Clock)
            key += "|clock=" + std::to_string(clock_second);
        else if (layer->type == LayerType::Ticker)
            key += "|ticker=" + std::to_string(QDateTime::currentMSecsSinceEpoch());

        auto &entry = session->layers[gpu_scene_mask_raster_id(id)];
        if (entry.key == key)
            continue;
        /* Build only the layer's reusable visual source.  This may be an exact
         * compatibility raster for unsupported source semantics, but it is
         * never converted into a CPU alpha/luma mask; all mask extraction and
         * composition happen after upload in the GPU mask graph. */
        const MotionBaseRaster raster = render_gpu_layer_base_raster(
            session->title, *layer, layer_time);
        entry.key = std::move(key);
        entry.gpu_text = false;
        entry.gpu_primitive = false;
        entry.pending_image = raster.image;
        entry.origin = raster.origin;
        entry.logical_width = raster.logical_width > 0.0
            ? raster.logical_width : raster.image.width();
        entry.logical_height = raster.logical_height > 0.0
            ? raster.logical_height : raster.image.height();
        entry.base_box_width = std::max(0.0001, eval_box_width(*layer, layer_time));
        entry.base_box_height = std::max(0.0001, eval_box_height(*layer, layer_time));
        entry.pending_upload = true;
    }
}

static gs_texture_t *title_gpu_render_session_render_auxiliary_layer(
    TitleGpuRenderSession *session, const std::string &layer_id,
    double title_time)
{
    if (!session || layer_id.empty())
        return nullptr;

    std::lock_guard<std::mutex> lock(session->mutex);
    if (!session->has_title)
        return nullptr;
    session->width = clamped_source_dimension(session->title.width);
    session->height = clamped_source_dimension(session->title.height);
    if (!ensure_gpu_session_objects(session, session->width, session->height))
        return nullptr;

    const Layer *layer = find_layer_by_id(session->title, layer_id);
    auto found = session->layers.find(gpu_scene_mask_raster_id(layer_id));
    if (!layer || found == session->layers.end())
        return nullptr;

    /* Auxiliary scene masks use the same cached GPU matte graph as track
     * mattes.  This preserves parent transforms, nested mattes and mask-layer
     * effects without creating a CPU alpha surface. */
    ScopedGpuCompositorState state;
    if (!upload_gpu_layer_raster(session, found->second) ||
        !found->second.texture)
        return nullptr;
    return render_gpu_mask_graph_texture(
        session, session->title, *layer, title_time,
        gpu_scene_mask_raster_id(layer_id));
}

bool title_gpu_render_session_draw(TitleGpuRenderSession *session,
                                   uint32_t output_width,
                                   uint32_t output_height)
{
    if (!session || session->destroying.load())
        return false;
    std::lock_guard<std::mutex> lock(session->mutex);
    if (session->destroying.load())
        return false;
    gs_texture_t *texture = render_gpu_session_locked(session);
    if (!texture && gpu_session_has_published_frame_for_current_title(session))
        texture = session->final_texture;
    if (!texture)
        return false;
    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t *image = effect ? gs_effect_get_param_by_name(effect, "image") : nullptr;
    if (!effect || !image)
        return false;
    gs_effect_set_texture(image, texture);
    const uint64_t draw_serial = ++session->draw_serial;
    if (TitlePreferences::cache_playback_logging_enabled()) {
        BGL_LOG_INFO("CachePlayback", QStringLiteral(
            "stage=gpu-draw title=%1 session=%2 drawSerial=%3 submittedSerial=%4 uploadedSerial=%5 publishedSerial=%6 texture=%7 output=%8x%9 dirty=%10 useSubmitted=%11 useGpuRam=%12")
            .arg(session->has_title
                     ? QString::fromStdString(session->title.id)
                     : QStringLiteral("<none>"))
            .arg(reinterpret_cast<quintptr>(session), 0, 16)
            .arg(draw_serial)
            .arg(session->submitted_final_serial)
            .arg(session->uploaded_final_serial)
            .arg(session->published_final_serial)
            .arg(reinterpret_cast<quintptr>(texture), 0, 16)
            .arg(output_width).arg(output_height)
            .arg(session->frame_dirty ? 1 : 0)
            .arg(session->use_submitted_final ? 1 : 0)
            .arg(session->use_gpu_cached_final ? 1 : 0));
    }
    gs_blend_state_push();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(texture, 0, output_width, output_height);
    gs_blend_state_pop();
    return true;
}

std::string title_gpu_render_session_last_error(TitleGpuRenderSession *session)
{
    if (!session)
        return "GPU render session is null.";
    std::lock_guard<std::mutex> lock(session->mutex);
    return session->last_error ? std::string(session->last_error) : std::string();
}

static bool title_gpu_render_session_submit_async_readback(
    TitleGpuRenderSession *session, const std::string &cache_key,
    const QRect &requested_region, TitleGpuReadbackTicket &ticket)
{
    ticket = {};
    if (!session || session->destroying.load())
        return false;

    obs_enter_graphics();
    std::unique_lock<std::mutex> lock(session->mutex);
    if (session->destroying.load()) {
        lock.unlock();
        obs_leave_graphics();
        return false;
    }

    gs_texture_t *texture = render_gpu_session_locked(session);
    /* A readback is valid only when the texture was published from the exact
     * current model and the graph is clean. render_gpu_session_locked() may
     * deliberately return the previous same-title frame while a replacement
     * text/shape raster is pending; caching that fallback under the new key
     * would permanently associate stale or transparent pixels with 100% cache
     * progress. */
    if (!texture || session->frame_dirty ||
        !gpu_session_final_matches_model(session)) {
        BGL_LOG_DEBUG("Prerender", QStringLiteral(
            "stage=readback-submit action=reject-incomplete title=%1 revision=%2 publishedRevision=%3 dirty=%4 texture=%5 error=%6")
            .arg(session->has_title
                     ? QString::fromStdString(session->title.id)
                     : QStringLiteral("<none>"))
            .arg(session->model_revision)
            .arg(session->published_model_revision)
            .arg(session->frame_dirty ? 1 : 0)
            .arg(texture ? 1 : 0)
            .arg(session->last_error ? QString::fromUtf8(session->last_error)
                                     : QStringLiteral("none")));
        lock.unlock();
        obs_leave_graphics();
        return false;
    }

    /* The RAM cache is published only after the final readback has been
     * resolved and converted to sparse aligned tiles. Keeping a full-canvas
     * texrender here was the source of the excessive per-frame RAM usage. */
    (void)cache_key;

    std::size_t slot_index = session->readback_slots.size();
    for (std::size_t offset = 0; offset < session->readback_slots.size(); ++offset) {
        const std::size_t candidate =
            (session->next_readback_slot + offset) % session->readback_slots.size();
        if (!session->readback_slots[candidate].pending) {
            slot_index = candidate;
            break;
        }
    }
    if (slot_index >= session->readback_slots.size()) {
        lock.unlock();
        obs_leave_graphics();
        return false;
    }

    const QRect canvas_rect(0, 0, static_cast<int>(session->width),
                            static_cast<int>(session->height));
    QRect region = requested_region.isEmpty()
        ? canvas_rect : requested_region.intersected(canvas_rect);
    if (region.isEmpty())
        region = canvas_rect;

    auto &slot = session->readback_slots[slot_index];
    gs_texture_t *readback_texture = texture;
    const bool cropped = region != canvas_rect;
    if (cropped) {
        ScopedGpuCompositorState crop_state;
        if (!slot.crop_target)
            slot.crop_target = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
        struct vec4 clear;
        vec4_zero(&clear);
        if (!slot.crop_target ||
            !begin_gpu_target(slot.crop_target,
                              static_cast<uint32_t>(region.width()),
                              static_cast<uint32_t>(region.height()), clear)) {
            lock.unlock();
            obs_leave_graphics();
            return false;
        }
        gs_eparam_t *image = session->blit_effect
            ? gs_effect_get_param_by_name(session->blit_effect, "image")
            : nullptr;
        if (!image) {
            gs_texrender_end(slot.crop_target);
            lock.unlock();
            obs_leave_graphics();
            return false;
        }
        gs_effect_set_texture(image, texture);
        gs_enable_blending(false);
        gs_matrix_push();
        gs_matrix_identity();
        gs_matrix_translate3f(static_cast<float>(-region.x()),
                              static_cast<float>(-region.y()), 0.0f);
        while (gs_effect_loop(session->blit_effect, "Draw"))
            gs_draw_sprite(texture, 0, session->width, session->height);
        gs_matrix_pop();
        gs_texrender_end(slot.crop_target);
        readback_texture = gs_texrender_get_texture(slot.crop_target);
    }

    const uint32_t readback_width = static_cast<uint32_t>(region.width());
    const uint32_t readback_height = static_cast<uint32_t>(region.height());
    if (!slot.stage || slot.width != readback_width ||
        slot.height != readback_height) {
        if (slot.stage)
            gs_stagesurface_destroy(slot.stage);
        slot.stage = gs_stagesurface_create(readback_width, readback_height,
                                            GS_BGRA);
        slot.width = slot.stage ? readback_width : 0;
        slot.height = slot.stage ? readback_height : 0;
    }
    if (!readback_texture || !slot.stage) {
        lock.unlock();
        obs_leave_graphics();
        return false;
    }

    gs_stage_texture(slot.stage, readback_texture);
    slot.region = region;
    slot.serial = ++session->next_readback_serial;
    slot.pending = true;
    session->next_readback_slot =
        (slot_index + 1) % session->readback_slots.size();

    ticket.session = session;
    ticket.serial = slot.serial;
    ticket.region = region;
    ticket.canvas_width = session->width;
    ticket.canvas_height = session->height;
    lock.unlock();
    obs_leave_graphics();
    return true;
}

bool title_gpu_render_session_resolve_readback(
    const TitleGpuReadbackTicket &ticket, QImage &image)
{
    image = QImage();
    if (!ticket.valid() || ticket.session->destroying.load())
        return false;

    obs_enter_graphics();
    std::unique_lock<std::mutex> lock(ticket.session->mutex);
    TitleGpuRenderSession::AsyncReadbackSlot *slot = nullptr;
    for (auto &candidate : ticket.session->readback_slots) {
        if (candidate.pending && candidate.serial == ticket.serial) {
            slot = &candidate;
            break;
        }
    }
    if (!slot || !slot->stage) {
        lock.unlock();
        obs_leave_graphics();
        return false;
    }

    uint8_t *mapped = nullptr;
    uint32_t linesize = 0;
    const bool mapped_ok = gs_stagesurface_map(slot->stage, &mapped, &linesize);
    if (mapped_ok && mapped && linesize >= slot->width * 4) {
        QImage result(static_cast<int>(slot->width),
                      static_cast<int>(slot->height),
                      QImage::Format_ARGB32_Premultiplied);
        if (!result.isNull()) {
            for (uint32_t y = 0; y < slot->height; ++y) {
                std::memcpy(result.scanLine(static_cast<int>(y)),
                            mapped + static_cast<size_t>(y) * linesize,
                            static_cast<size_t>(slot->width) * 4);
            }
            bgs::cache_frame_payload::set_placement(
                result, slot->region.x(), slot->region.y(),
                static_cast<int>(ticket.canvas_width),
                static_cast<int>(ticket.canvas_height));
            image = std::move(result);
        }
        gs_stagesurface_unmap(slot->stage);
    }
    slot->pending = false;
    slot->serial = 0;
    slot->region = QRect();
    lock.unlock();
    obs_leave_graphics();
    return !image.isNull();
}

void title_gpu_render_session_discard_readback(
    const TitleGpuReadbackTicket &ticket)
{
    QImage discarded;
    title_gpu_render_session_resolve_readback(ticket, discarded);
}

bool render_title_gpu_cache_submit_readback(
    const Title &title, double time, uint64_t model_revision,
    const std::string &cache_key, const QRect &region,
    TitleGpuReadbackTicket &ticket)
{
    /* This guard covers model preparation as well as graph submission. Legacy
     * compatibility raster helpers contain optional GPU surface passes that
     * map back into Cairo/QImage; the prerender path must never enter them.
     * The only permitted map is title_gpu_render_session_resolve_readback(),
     * after the completed unified frame has reached its staging slot. */
    ScopedGpuReadbackContract final_frame_only(
        GpuReadbackContract::FinalFrameOnly);
    const TitleDynamicLayerAnalysis analysis =
        analyze_title_dynamic_layers(title);
    if (analysis.has_dynamic_layers && !analysis.has_cacheable_prefix)
        return false;
    const std::size_t cache_end = analysis.has_dynamic_layers
        ? analysis.first_dynamic_layer : title.layers.size();
    const auto acquired = acquire_gpu_readback_session(model_revision);
    title_gpu_render_session_update_range(
        acquired.first, title, time, acquired.second, 0, cache_end);
    return title_gpu_render_session_submit_async_readback(
        acquired.first, cache_key, region, ticket);
}

QImage title_gpu_render_session_readback(TitleGpuRenderSession *session)
{
    if (!session || session->destroying.load())
        return QImage();
    QImage result;
    /* Match the graphics -> session ordering used by OBS display callbacks and
     * session destruction. The previous inverse order could deadlock a cache
     * worker against Preview/Program while one side waited for the graphics
     * lock and the other waited for this mutex. */
    obs_enter_graphics();
    std::unique_lock<std::mutex> lock(session->mutex);
    if (session->destroying.load()) {
        lock.unlock();
        obs_leave_graphics();
        return QImage();
    }
    gs_texture_t *texture = render_gpu_session_locked(session);
    const bool complete_current_frame = texture && !session->frame_dirty &&
        gpu_session_final_matches_model(session);
    if (!complete_current_frame) {
        BGL_LOG_DEBUG("Prerender", QStringLiteral(
            "stage=sync-readback action=reject-incomplete title=%1 revision=%2 publishedRevision=%3 dirty=%4 texture=%5")
            .arg(session->has_title
                     ? QString::fromStdString(session->title.id)
                     : QStringLiteral("<none>"))
            .arg(session->model_revision)
            .arg(session->published_model_revision)
            .arg(session->frame_dirty ? 1 : 0)
            .arg(texture ? 1 : 0));
        texture = nullptr;
    }
    if (texture) {
        if (!session->stage || session->stage_width != session->width ||
            session->stage_height != session->height) {
            if (session->stage)
                gs_stagesurface_destroy(session->stage);
            session->stage = gs_stagesurface_create(session->width, session->height, GS_BGRA);
            session->stage_width = session->stage ? session->width : 0;
            session->stage_height = session->stage ? session->height : 0;
        }
    }
    if (texture && session->stage) {
        gs_stage_texture(session->stage, texture);
        uint8_t *mapped = nullptr;
        uint32_t linesize = 0;
        if (gs_stagesurface_map(session->stage, &mapped, &linesize) && mapped &&
            linesize >= session->width * 4) {
            result = QImage((int)session->width, (int)session->height,
                            QImage::Format_ARGB32_Premultiplied);
            for (uint32_t y = 0; y < session->height; ++y)
                std::memcpy(result.scanLine((int)y), mapped + (size_t)y * linesize,
                            (size_t)session->width * 4);
            gs_stagesurface_unmap(session->stage);
        }
    }
    lock.unlock();
    obs_leave_graphics();
    return result;
}

QImage render_title_gpu_frame_readback(const Title &title, double time,
                                           uint64_t model_revision)
{
    const auto acquired = acquire_gpu_readback_session(model_revision);
    title_gpu_render_session_update(acquired.first, title, time,
                                    acquired.second);
    return title_gpu_render_session_readback(acquired.first);
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

static obs_source_t *active_scene_mask_source_for_name(const TitleSourceData &data, const std::string &name)
{
    for (const auto &active : data.active_scene_mask_scenes) {
        if (active.name == name && active.source)
            return active.source;
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
        data->scene_mask_effect = gs_effect_create(kSceneMaskEffect, "obs-bgs-scene-mask.effect", nullptr);
    return data->scene_mask_effect;
}

static void render_scene_masks_gpu(TitleSourceData *data, const Title &title, double title_time)
{
    if (!data || data->scene_masks.empty() || data->active_scene_mask_scenes.empty())
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

        obs_source_t *scene = active_scene_mask_source_for_name(*data, cfg->scene_name);
        if (!scene)
            continue;

        const double lt = std::max(0.0, title_time - layer->in_time);
        const double w = eval_box_width(*layer, lt);
        const double h = eval_box_height(*layer, lt);
        if (w <= 0.0 || h <= 0.0) {
            continue;
        }
        const double zoom = std::clamp(cfg->zoom, 0.01, 100.0);

        gs_texture_t *mask_texture =
            title_gpu_render_session_render_auxiliary_layer(
                data->gpu_render_session, layer->id, title_time);
        if (!mask_texture)
            continue;

        gs_viewport_push();
        gs_projection_push();
        gs_blend_state_push();
        gs_texrender_reset(data->scene_mask_scene_texrender);
        if (!gs_texrender_begin(data->scene_mask_scene_texrender, data->tex_w, data->tex_h)) {
            gs_blend_state_pop();
            gs_projection_pop();
            gs_viewport_pop();
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
                gs_effect_set_texture(mask, mask_texture);
                gs_blend_state_push();
                /* This pass overlays a masked scene after the title. Merely
                 * selecting a blend function does not enable blending in OBS;
                 * with blending disabled the transparent full-canvas quad can
                 * overwrite the title output in Preview/Program. */
                gs_enable_blending(true);
                gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
                while (gs_effect_loop(effect, "Draw"))
                    gs_draw_sprite(scene_texture, 0, data->tex_w, data->tex_h);
                gs_blend_state_pop();
            }
        }
    }
}

static void source_video_render(void *priv, gs_effect_t * /*effect*/)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data ||
        !data->shown_on_display.load(std::memory_order_acquire) ||
        !data->output_visible.load(std::memory_order_acquire) ||
        !data->gpu_render_session)
        return;
    if (g_source_scene_collection_transition.load(
            std::memory_order_acquire))
        return;
    /* Frontend/source callbacks may invalidate a presentation between video
     * ticks. Never draw the previous generation while waiting for the tick to
     * rebuild the current title graph. */
    if (!source_presentation_generation_is_current(data))
        return;

    auto title = TitleDataStore::instance().get_title(data->title_id);
    if (!title)
        return;

    const uint32_t width = clamped_source_dimension(title->width);
    const uint32_t height = clamped_source_dimension(title->height);
    const bool rendered = title_gpu_render_session_draw(data->gpu_render_session,
                                                        width, height);
    const std::string gpu_error = rendered
        ? std::string()
        : title_gpu_render_session_last_error(data->gpu_render_session);
    TitlePreferences::set_gpu_available(
        rendered, rendered ? nullptr
                           : (gpu_error.empty()
                                  ? "Unified GPU compositor could not render the title."
                                  : gpu_error.c_str()));
    if (!rendered) {
        BGL_LOG_WARNING("GpuPipeline", QStringLiteral(
            "consumer=source action=draw-failed title=%1 failures=%2 error=%3")
            .arg(QString::fromStdString(title->id))
            .arg(data->consecutive_draw_failures + 1)
            .arg(gpu_error.empty()
                     ? QStringLiteral("unknown GPU compositor error")
                     : QString::fromStdString(gpu_error)));
        ++data->consecutive_draw_failures;
        data->dirty = true;
        data->first_frame_pending = true;
        /* Reconcile the full model/cache contract on the next tick rather than
         * waiting for an unrelated cue or edit to bump a revision. */
        if (data->consecutive_draw_failures >= 2) {
            data->seen_store_revision = std::numeric_limits<uint64_t>::max();
            data->cache_hash_revision = std::numeric_limits<uint64_t>::max();
            data->cached_content_hash.clear();
        }
        return;
    }

    data->consecutive_draw_failures = 0;
    data->first_frame_pending = false;
    data->dirty = false;
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
        props, PROP_TITLE_ID, bgl_tr_c("OBSTitles.TitleID"),
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

    obs_property_list_add_string(p, bgl_tr_c("OBSTitles.NoTitle"), "");
    for (auto &t : TitleDataStore::instance().titles())
        obs_property_list_add_string(p, t->name.c_str(), t->id.c_str());
    obs_property_set_modified_callback(p, source_title_property_modified);

    add_scene_mask_properties_for_title(props, data ? data->title_id : std::string());

    return props;
}

static void source_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, PROP_TITLE_ID, "");
}

/* ══════════════════════════════════════════════════════════════════
 *  Registration
 * ══════════════════════════════════════════════════════════════════ */
void title_source_register()
{
    static obs_source_info si = {};
    si.id             = "broadcast_graphics_live_source";
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
    si.show           = source_show;
    si.hide           = source_hide;
    si.get_properties = source_get_properties;
    si.get_defaults   = source_get_defaults;

    obs_register_source(&si);
    blog(LOG_INFO, "[Broadcast Graphics Live] Source type registered.");
}
