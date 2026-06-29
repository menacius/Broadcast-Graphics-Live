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
    uint64_t last_used = 0;
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
    static uint64_t cache_tick = 0;
    static qsizetype cache_bytes = 0;
    constexpr std::size_t kMaxLayerImageCacheEntries = 128;
    constexpr qsizetype kMaxLayerImageCacheBytes = 256ll * 1024ll * 1024ll;

    const std::string key = cache_key.toStdString();
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end() && it->second.last_modified_msecs == modified &&
            it->second.file_size == size_on_disk) {
            it->second.last_used = ++cache_tick;
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
        if (auto existing = cache.find(key); existing != cache.end()) {
            cache_bytes -= std::min(cache_bytes,
                                    std::max<qsizetype>(0, existing->second.image.sizeInBytes()));
            cache.erase(existing);
        }
        cache.emplace(key, CachedLayerImage{loaded, modified, size_on_disk,
                                            ++cache_tick});
        cache_bytes += std::max<qsizetype>(0, loaded.sizeInBytes());
        while ((cache.size() > kMaxLayerImageCacheEntries ||
                cache_bytes > kMaxLayerImageCacheBytes) && cache.size() > 1) {
            auto oldest = cache.end();
            for (auto it = cache.begin(); it != cache.end(); ++it) {
                if (it->first == key)
                    continue;
                if (oldest == cache.end() ||
                    it->second.last_used < oldest->second.last_used)
                    oldest = it;
            }
            if (oldest == cache.end())
                break;
            cache_bytes -= std::min(cache_bytes,
                                    std::max<qsizetype>(0, oldest->second.image.sizeInBytes()));
            cache.erase(oldest);
        }
    }
    return loaded;
}
}

// Forward declaration must live in the same (global) scope as the definition below.
// Keeping it inside the anonymous namespace creates a second overload on MSVC.
static void box_blur_pixels(std::vector<uint8_t> &pixels, int w, int h, int radius);
static void clear_extension_image_textures();

/* ══════════════════════════════════════════════════════════════════
 *  Source private data
 * ══════════════════════════════════════════════════════════════════ */
static std::atomic<uint64_t> g_source_frontend_presentation_generation {1};
static std::atomic<bool> g_source_scene_collection_transition {false};
static std::atomic<bool> g_source_frontend_shutting_down {false};

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

void title_source_begin_shutdown()
{
    g_source_frontend_shutting_down.store(true, std::memory_order_release);
    g_source_scene_collection_transition.store(true, std::memory_order_release);
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
    bool        cue_first_row_when_active = false;

    enum class CuePhase { FreeRun, IntroLoop, OutroThenIntro, OutroOnly };

    /* Playback state */
    double      playhead     = 0.0;    /* seconds */
    bool        playing      = true;
    bool        playback_reverse = false;
    uint64_t    seen_cue_revision = 0;
    CuePhase    cue_phase    = CuePhase::FreeRun;
    int         active_cue_row = -1;
    int         cue_timer_row = -1;
    double      cue_timer_elapsed_seconds = 0.0;
    std::chrono::steady_clock::time_point cue_timer_started_at;
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
    gs_texrender_t *scene_mask_matted_texrender = nullptr;
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

static void clear_live_cue_runtime_for_source(
    TitleSourceData *data, const std::string &title_id)
{
    if (!data || title_id.empty())
        return;

    clear_live_cue_runtime_state(
        title_id, reinterpret_cast<uintptr_t>(data));
}

static void publish_live_cue_runtime(
    TitleSourceData *data, const std::shared_ptr<Title> &title)
{
    if (!data || !title)
        return;

    const bool source_running = data->scene_mask_foreground_active &&
        data->shown_on_display.load(std::memory_order_acquire);
    const int row = title->current_cue_row;
    if (!source_running || row < 0) {
        data->cue_timer_row = -1;
        data->cue_timer_elapsed_seconds = 0.0;
        clear_live_cue_runtime_for_source(data, title->id);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (data->cue_timer_row != row) {
        data->cue_timer_row = row;
        data->cue_timer_started_at = now;
        data->cue_timer_elapsed_seconds = 0.0;
    } else {
        data->cue_timer_elapsed_seconds = std::max(
            0.0, std::chrono::duration<double>(
                     now - data->cue_timer_started_at).count());
    }

    publish_live_cue_runtime_state(
        title->id, reinterpret_cast<uintptr_t>(data), row,
        std::clamp(data->playhead, 0.0, std::max(0.0, title->duration)),
        data->cue_timer_elapsed_seconds,
        QDateTime::currentMSecsSinceEpoch());
}

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
           (!effect.extension_keyframes_json.empty() &&
            effect.extension_keyframes_json != "{}") ||
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

static bool title_has_independent_asset_layer(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    return std::any_of(title->layers.begin(), title->layers.end(),
                       [](const std::shared_ptr<Layer> &layer) {
                           return layer && layer->type == LayerType::Asset &&
                                  layer->asset_animated &&
                                  layer->asset_playback_mode == 1;
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
            /* Manual scene rendering must mirror OBS scene-item lifecycle.
             * active keeps media/ticks alive; showing allows the scene and its
             * child sources to participate in video rendering. */
            /* During frontend teardown OBS may already be dismantling the
             * nested scene graph. Calling dec_showing/dec_active at that point
             * can synchronously re-enter partially destroyed child sources.
             * Runtime deactivation remains fully balanced; process shutdown
             * only drops our strong reference and lets OBS finish teardown. */
            if (!g_source_frontend_shutting_down.load(std::memory_order_acquire)) {
                obs_source_dec_showing(active.source);
                obs_source_dec_active(active.source);
            }
            obs_source_release(active.source);
            active.source = nullptr;
        }
    }
    data->active_scene_mask_scenes.clear();
}

static bool title_has_valid_scene_mask_cue(const TitleSourceData *data, const Title &title)
{
    /* Titles without exposed live-text fields are ordinary, continuously
     * visible sources. They do not require a dock cue before their configured
     * OBS scene masks may render. Requiring live_text_rows here left every
     * static scene-mask title permanently inactive in Preview/Program. */
    if (exposed_text_layers(title).empty())
        return true;

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

        /* The configured scene is rendered outside a normal obs_sceneitem.
         * Supply both lifecycle references that OBS would normally maintain
         * for a visible scene item, otherwise nested sources can remain hidden
         * or inactive even though obs_source_video_render() is called. */
        obs_source_inc_active(scene);
        obs_source_inc_showing(scene);
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
    else if (layer->rich_text.version < 2 || layer->rich_text.blocks.empty())
        rich_text_document_ensure_canonical(*layer);

    RichTextCharFormat insertion_format = rich_text_effective_typing_format(layer->rich_text);
    rich_text_document_replace_text_canonical(
        layer->rich_text, text, insertion_format,
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
        rich_text_document_replace_text_canonical(
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

static double layer_asset_resolved_time_impl(
    const Title &title, const Layer &layer, double title_time,
    std::unordered_set<std::string> &visiting)
{
    if (layer.asset_owner_id.empty() ||
        !visiting.insert(layer.asset_owner_id).second)
        return title_time;

    const Layer *asset = find_layer_by_id(title, layer.asset_owner_id);
    if (!asset || asset->type != LayerType::Asset) {
        visiting.erase(layer.asset_owner_id);
        return title_time;
    }

    const double owner_time = layer_asset_resolved_time_impl(
        title, *asset, title_time, visiting);
    const double resolved = bgs::asset_runtime::resolve_local_time(
        title.id, *asset, owner_time - asset->in_time);

    visiting.erase(layer.asset_owner_id);
    return resolved;
}

static double layer_asset_resolved_time(const Title &title, const Layer &layer,
                                        double title_time)
{
    std::unordered_set<std::string> visiting;
    return layer_asset_resolved_time_impl(title, layer, title_time, visiting);
}

/* Keep one root-title clock at every compositor boundary. Asset descendants
 * resolve that clock into their owning Asset Layer's local timeline here;
 * ordinary layers keep the root clock (or the cue-persistence hold clock). */
static double gpu_layer_render_time(const Title &title, const Layer &layer,
                                    double title_time)
{
    if (!layer.asset_owner_id.empty())
        return layer_asset_resolved_time(title, layer, title_time);

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

static bool gpu_layer_chain_visible(const Title &title, const Layer &layer,
                                    double title_time, int depth = 0)
{
    const double layer_time = gpu_layer_render_time(title, layer, title_time);
    if (depth > 64 || !layer.visible || layer.live_cue_hidden_if_empty ||
        layer_time < layer.in_time || layer_time > layer.out_time)
        return false;
    if (layer.parent_id.empty())
        return true;
    const Layer *parent = find_layer_by_id(title, layer.parent_id);
    return parent ? gpu_layer_chain_visible(
                        title, *parent, title_time, depth + 1)
                  : true;
}

/* The compositor owner is intentionally excluded, matching the legacy Group
 * behavior while allowing each nested Asset/Group ancestor to use its own
 * resolved timeline. */
static bool gpu_layer_visible_in_group_compositor(
    const Title &title, const Layer &layer, double title_time,
    const std::string &compositor_group_id, int depth = 0)
{
    const double layer_time = gpu_layer_render_time(title, layer, title_time);
    if (depth > 64 || !layer.visible || layer.live_cue_hidden_if_empty ||
        layer_time < layer.in_time || layer_time > layer.out_time)
        return false;
    if (layer.parent_id.empty() || layer.parent_id == compositor_group_id)
        return true;
    const Layer *parent = find_layer_by_id(title, layer.parent_id);
    return parent ? gpu_layer_visible_in_group_compositor(
                        title, *parent, title_time, compositor_group_id,
                        depth + 1)
                  : true;
}

static bool gpu_layer_chain_active_as_matte(const Title &title,
                                             const Layer &layer,
                                             double title_time,
                                             int depth = 0)
{
    const double layer_time = gpu_layer_render_time(title, layer, title_time);
    if (depth > 64 ||
        layer.matte_visibility_mode == MatteVisibilityMode::HiddenInactive ||
        layer.live_cue_hidden_if_empty ||
        layer_time < layer.in_time || layer_time > layer.out_time)
        return false;
    if (layer.parent_id.empty())
        return true;
    const Layer *parent = find_layer_by_id(title, layer.parent_id);
    return parent ? gpu_layer_chain_visible(
                        title, *parent, title_time, depth + 1)
                  : true;
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

/* Visibility inside an off-screen group compositor. The owning group itself is
 * deliberately not tested: a Group used as a track-matte source is hidden from
 * normal composition, but its children must still render into the matte texture.
 * Intermediate nested parents remain subject to their own visibility/timing. */
static bool layer_visible_in_group_compositor(
    const Title &title, const Layer &layer, double title_time,
    const std::string &compositor_group_id, int depth = 0)
{
    if (depth > 64 || !layer.visible || layer.live_cue_hidden_if_empty ||
        title_time < layer.in_time || title_time > layer.out_time)
        return false;
    if (layer.parent_id.empty() || layer.parent_id == compositor_group_id)
        return true;
    const Layer *parent = find_layer_by_id(title, layer.parent_id);
    return parent ? layer_visible_in_group_compositor(
                        title, *parent, title_time, compositor_group_id, depth + 1)
                  : true;
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

static bool layer_is_clipping_matte_source(const Title &title, const Layer &layer)
{
    if (layer.id.empty())
        return false;
    for (const auto &candidate : title.layers) {
        if (!candidate || candidate.get() == &layer ||
            candidate->mask_source_id != layer.id)
            continue;
        if (mask_mode_is_clipping(candidate->mask_mode))
            return true;
    }
    return false;
}

static bool layer_chain_active_as_matte(const Title &title, const Layer &layer,
                                        double title_time, int depth = 0)
{
    if (depth > 64 ||
        layer.matte_visibility_mode == MatteVisibilityMode::HiddenInactive ||
        layer.live_cue_hidden_if_empty ||
        title_time < layer.in_time || title_time > layer.out_time)
        return false;
    if (layer.parent_id.empty())
        return true;
    const Layer *parent = find_layer_by_id(title, layer.parent_id);
    return parent ? layer_chain_visible(title, *parent, title_time, depth + 1) : true;
}

static bool layer_should_render_as_visible_content(const Title &title, const Layer &layer)
{
    /*
     * After Effects-style track mattes: a layer selected as another layer's
     * mask is not composited as normal visible artwork, but it is still
     * rendered into a cached GPU matte texture by the Phase 13 mask graph.
     */
    /* Scene-mask layers are matte controls, not ordinary visible artwork.
     * Their editor hatch is painted by CanvasPreview while the selected OBS
     * scene is composited through the auxiliary scene-mask pass. */
    if (layer.use_as_scene_mask)
        return false;
    if (!layer_is_track_matte_source(title, layer))
        return true;
    /* Clipping matte sources are handled by the compositor immediately before
     * the first linked clipping target. They stay out of the ordinary stack
     * so ordering cannot cover the clipped result. The compositor publishes
     * their artwork only for VisibleAndMatte; MatteOnly still supplies alpha
     * to the clipping target without appearing in the frame. */
    if (layer_is_clipping_matte_source(title, layer))
        return false;
    return layer.matte_visibility_mode == MatteVisibilityMode::VisibleAndMatte;
}

bool title_requires_destination_compositing(const Title &title)
{
    /* Keep editor and OBS source selection on the same compositor path. Any
     * layer (including a group) with a backdrop effect needs the destination
     * snapshot even when its own blend mode is Normal. */
    for (const auto &layer_ptr : title.layers) {
        if (!layer_ptr)
            continue;
        const Layer &layer = *layer_ptr;
        if (layer.type == LayerType::Adjustment ||
            layer.blend_mode != EffectBlendMode::Normal)
            return true;
        for (const LayerEffect &effect : layer.effects) {
            if (effect.enabled && effect.affect_layers_behind)
                return true;
        }
    }
    return false;
}

static QTransform layer_world_transform_qt(const Title &title, const Layer &layer,
                                           double title_time, int depth = 0);

static void apply_layer_world_transform(cairo_t *cr, const Title &title, const Layer &layer,
                                        double title_time, int /*depth*/ = 0)
{
    const QTransform transform = layer_world_transform_qt(title, layer, title_time);
    cairo_matrix_t matrix;
    cairo_matrix_init(&matrix, transform.m11(), transform.m12(),
                      transform.m21(), transform.m22(),
                      transform.dx(), transform.dy());
    cairo_transform(cr, &matrix);
}

