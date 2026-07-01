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
    gs_texrender_t *adjustment_coverage_target = nullptr;
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
     * GPU textures.  The cache is shared by alpha/luma/clipping variants because the
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
    gs_effect_t *adjustment_coverage_effect = nullptr;
    gs_effect_t *primitive_shape_effect = nullptr;
    std::unique_ptr<bgs::gpu_text::Renderer> text_renderer;
    gs_effect_t *blend_effect = nullptr;
    gs_effect_t *mask_effect = nullptr;
    gs_effect_t *external_background_effect = nullptr;
    gs_texrender_t *external_background_local = nullptr;
    gs_texture_t *external_background_snapshot = nullptr;
    uint32_t external_background_snapshot_width = 0;
    uint32_t external_background_snapshot_height = 0;
    enum gs_color_format external_background_snapshot_format = GS_UNKNOWN;
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

static LayerEffect resolve_gpu_layer_effect(const LayerEffect &effect, double t)
{
    LayerEffect resolved = effect;
    resolved.brightness = static_cast<float>(std::clamp(
        effect.brightness_prop.is_animated() ? effect.brightness_prop.evaluate(t)
                                              : static_cast<double>(effect.brightness),
        -1.0, 1.0));
    resolved.contrast = static_cast<float>(std::clamp(
        effect.contrast_prop.is_animated() ? effect.contrast_prop.evaluate(t)
                                            : static_cast<double>(effect.contrast),
        0.0, 4.0));
    resolved.saturation = static_cast<float>(std::clamp(
        effect.saturation_prop.is_animated() ? effect.saturation_prop.evaluate(t)
                                              : static_cast<double>(effect.saturation),
        0.0, 4.0));
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
    resolved.effect_padding_left = static_cast<float>(
        effect.padding_left_prop.is_animated() ? effect.padding_left_prop.evaluate(t)
                                                : static_cast<double>(effect.effect_padding_left));
    resolved.effect_padding_right = static_cast<float>(
        effect.padding_right_prop.is_animated() ? effect.padding_right_prop.evaluate(t)
                                                 : static_cast<double>(effect.effect_padding_right));
    resolved.effect_padding_top = static_cast<float>(
        effect.padding_top_prop.is_animated() ? effect.padding_top_prop.evaluate(t)
                                               : static_cast<double>(effect.effect_padding_top));
    resolved.effect_padding_bottom = static_cast<float>(
        effect.padding_bottom_prop.is_animated() ? effect.padding_bottom_prop.evaluate(t)
                                                  : static_cast<double>(effect.effect_padding_bottom));
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
    resolved.effect_gradient_start_pos = static_cast<float>(std::clamp(
        effect.gradient_start_pos_prop.is_animated() ? effect.gradient_start_pos_prop.evaluate(t)
                                                     : static_cast<double>(effect.effect_gradient_start_pos),
        0.0, 1.0));
    resolved.effect_gradient_end_pos = static_cast<float>(std::clamp(
        effect.gradient_end_pos_prop.is_animated() ? effect.gradient_end_pos_prop.evaluate(t)
                                                   : static_cast<double>(effect.effect_gradient_end_pos),
        0.0, 1.0));
    resolved.effect_gradient_start_opacity = static_cast<float>(std::clamp(
        effect.gradient_start_opacity_prop.is_animated() ? effect.gradient_start_opacity_prop.evaluate(t)
                                                         : static_cast<double>(effect.effect_gradient_start_opacity),
        0.0, 1.0));
    resolved.effect_gradient_end_opacity = static_cast<float>(std::clamp(
        effect.gradient_end_opacity_prop.is_animated() ? effect.gradient_end_opacity_prop.evaluate(t)
                                                       : static_cast<double>(effect.effect_gradient_end_opacity),
        0.0, 1.0));
    resolved.effect_gradient_angle = static_cast<float>(
        effect.gradient_angle_prop.is_animated() ? effect.gradient_angle_prop.evaluate(t)
                                                  : static_cast<double>(effect.effect_gradient_angle));
    resolved.effect_gradient_center_x = static_cast<float>(
        effect.gradient_center_x_prop.is_animated() ? effect.gradient_center_x_prop.evaluate(t)
                                                     : static_cast<double>(effect.effect_gradient_center_x));
    resolved.effect_gradient_center_y = static_cast<float>(
        effect.gradient_center_y_prop.is_animated() ? effect.gradient_center_y_prop.evaluate(t)
                                                     : static_cast<double>(effect.effect_gradient_center_y));
    resolved.effect_gradient_scale = static_cast<float>(std::max(
        0.001, effect.gradient_scale_prop.is_animated() ? effect.gradient_scale_prop.evaluate(t)
                                                         : static_cast<double>(effect.effect_gradient_scale)));
    resolved.effect_gradient_focal_x = static_cast<float>(
        effect.gradient_focal_x_prop.is_animated() ? effect.gradient_focal_x_prop.evaluate(t)
                                                    : static_cast<double>(effect.effect_gradient_focal_x));
    resolved.effect_gradient_focal_y = static_cast<float>(
        effect.gradient_focal_y_prop.is_animated() ? effect.gradient_focal_y_prop.evaluate(t)
                                                    : static_cast<double>(effect.effect_gradient_focal_y));
    resolved.effect_gradient_opacity = static_cast<float>(std::clamp(
        effect.gradient_opacity_prop.is_animated() ? effect.gradient_opacity_prop.evaluate(t)
                                                    : static_cast<double>(effect.effect_gradient_opacity),
        0.0, 1.0));
    resolved.effect_gradient_start_color = eval_effect_gradient_start_color(effect, t);
    resolved.effect_gradient_end_color = eval_effect_gradient_end_color(effect, t);
    return resolved;
}

static bool layer_can_use_direct_gpu_image_raster(const Layer &layer,
                                                   double title_time,
                                                   int transition_blur_padding)
{
    if (layer.type != LayerType::Image ||
        effective_external_string(layer, ExternalPropertyPaths::ImagePath, layer.image_path).empty())
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

    const QString image_path = QString::fromStdString(
        effective_external_string(layer, ExternalPropertyPaths::ImagePath, layer.image_path));
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

    const QString image_path = QString::fromStdString(
        effective_external_string(layer, ExternalPropertyPaths::ImagePath, layer.image_path));
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
    const bool preserve_effect_surface = transition_blur_padding > 0 ||
        layer_requires_preserved_effect_surface(layer, local_time);
    if (preserve_effect_surface || resolved_scale < 0.999) {
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
        /* Metadata is expressed in the retained raster's coordinate system.
         * Cropping alpha-only base rasters without translating these rectangles
         * made a later Background Color pass use stale pre-crop bounds. */
        result.layer_box_rect.translate(-bounds.x(), -bounds.y());
        if (result.image_clip_rect.isValid())
            result.image_clip_rect.translate(-bounds.x(), -bounds.y());
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
    const double box_width = std::max(0.0, eval_box_width(layer, local_time));
    const double box_height = std::max(0.0, eval_box_height(layer, local_time));
    const int inline_stroke_pad = static_cast<int>(std::ceil(
        std::max(0.0, max_rich_text_stroke_width(layer, local_time)))) + 2;
    const int animator_pad = static_cast<int>(std::ceil(
        text_animator_stack_visual_padding(layer.text_animators,
                                           box_width, box_height)));
    const int visual_pad = std::max(inline_stroke_pad, animator_pad);
    if (visual_pad > 2) {
        surface = surface.adjusted(-visual_pad, -visual_pad,
                                   visual_pad, visual_pad);
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
    if (layer.type != LayerType::Text && layer.type != LayerType::Clock &&
        layer.type != LayerType::Ticker)
        return false;
    /* Transition-managed animators currently use the compatibility raster
     * compositor deliberately. It consumes the same TextAnimatorEvaluation as
     * the SDF renderer, but preserves the proven BGL transition behaviour while
     * the per-glyph GPU path is validated independently. This is not a legacy
     * transition evaluator: descriptors have already been converted to the
     * generic animator stack before reaching this branch. */
    if (text_animator_stack_has_managed_transition(layer.text_animators))
        return false;
    /* Legacy project/preset data is converted before rendering. There is no
     * legacy text-transition runtime fallback; all text animation reaches this
     * shared shaped-layout/TextAnimator path. */
    (void)title_time;
    return true;
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
    double ticker_offset_x = 0.0;
    double ticker_offset_y = 0.0;
    const bool ticker_layer = layer.type == LayerType::Ticker;
    const bool ticker_custom_playback = ticker_layer &&
        layer.ticker_playback_mode == static_cast<int>(TickerPlaybackMode::CustomPlayback);
    const double ticker_completion = ticker_layer ? std::clamp(
        layer.ticker_completion_prop.is_animated()
            ? layer.ticker_completion_prop.evaluate(local_time)
            : layer.ticker_completion,
        0.0, 100.0) / 100.0 : 0.0;
    TickerRuntimeSnapshot ticker_state;
    if (ticker_layer)
        ticker_state = bgs::ticker_runtime::sample(
            session->title.id, layer,
            session->title.current_cue_row >= 0 ||
                session->title.pending_cue_row >= 0);

    if (ticker_layer && layer.ticker_style == 0) {
        QString ticker_text = QString::fromStdString(model.plain_text);
        ticker_text.replace('\r', ' ');
        ticker_text.replace('\n', QStringLiteral("     •     "));
        const RichTextCharFormat insertion_format =
            rich_text_effective_typing_format(model);
        rich_text_document_replace_text_canonical(
            model, ticker_text.toStdString(), insertion_format,
            model.has_typing_format ? model.typing_format_mask : 0);
    } else if (ticker_layer && layer.ticker_style == 1) {
        const QFont ticker_font = font_for_layer(layer, local_time);
        const QStringList lines = ticker_lines(
            QString::fromStdString(model.plain_text), ticker_font,
            style_rect.width(), false);
        const int line_count = std::max(1, static_cast<int>(lines.size()));
        int index = ticker_custom_playback
            ? std::min(line_count - 1,
                       static_cast<int>(std::floor(ticker_completion * line_count)))
            : static_cast<int>(std::floor(
                  ticker_state.time_seconds / std::max(0.1, layer.ticker_line_hold))) %
                  line_count;
        if (layer.ticker_direction == 0) index = line_count - 1 - index;
        const RichTextCharFormat insertion_format =
            rich_text_effective_typing_format(model);
        rich_text_document_replace_text_canonical(
            model, lines.value(index).toStdString(), insertion_format,
            model.has_typing_format ? model.typing_format_mask : 0);
    } else if (layer.text_overflow_mode == 2) {
        const QString fitted = overflow_layout_text(
            QString::fromStdString(model.plain_text), layer);
        const RichTextCharFormat insertion_format =
            rich_text_effective_typing_format(model);
        rich_text_document_replace_text_canonical(
            model, fitted.toStdString(), insertion_format,
            model.has_typing_format ? model.typing_format_mask : 0);
    }

    TextLayoutRequest request;
    request.document = model;
    request.max_width = static_cast<float>(std::max(
        1.0, ticker_layer && layer.ticker_style == 0
            ? 1000000.0 : style_rect.width()));
    request.max_height = static_cast<float>(std::max(
        1.0, ticker_layer && layer.ticker_style == 2
            ? std::max(style_rect.height(), 100000.0) : style_rect.height()));
    request.device_scale = 1.0f;
    request.minimum_horizontal_fit =
        std::clamp(layer.text_fit_min_scale, 0.05f, 1.0f);
    request.overflow_mode = ticker_layer
        ? (layer.ticker_style == 2 ? layer.text_overflow_mode : 1)
        : layer.text_overflow_mode;
    /* LayerRaster already owns the last prepared editor layer and calls this
     * function only after its model key changes.  Keeping every intermediate
     * typed string in the process-wide layout cache provided no editor reuse
     * and retained hundreds of obsolete glyph/cluster vectors. */
    const ImmutableTextLayout layout = session->editor_draft
        ? build_text_layout(request)
        : cached_text_layout(request);
    if (!layout || !layout->valid)
        return false;

    if (ticker_layer) {
        const double speed = std::max(1.0, layer.ticker_speed);
        if (layer.ticker_style == 0) {
            const double text_width = std::max(1.0,
                static_cast<double>(layout->natural_width));
            const double travel = style_rect.width() + text_width;
            const double progress = ticker_custom_playback
                ? ticker_completion * travel
                : std::fmod(ticker_state.time_seconds * speed, travel);
            ticker_offset_x = layer.ticker_direction == 0
                ? -text_width + progress
                : style_rect.width() - progress;
        } else if (layer.ticker_style == 2) {
            const double content_height = std::max(1.0,
                static_cast<double>(layout->natural_height));
            const double travel = style_rect.height() + content_height;
            const double progress = ticker_custom_playback
                ? ticker_completion * travel
                : std::fmod(ticker_state.time_seconds * speed, travel);
            ticker_offset_y = layer.ticker_direction == 0
                ? -content_height + progress
                : style_rect.height() - progress;
        }
    }

    if (!session->text_renderer)
        session->text_renderer =
            std::make_unique<bgs::gpu_text::Renderer>();
    if (!entry.text_layer)
        entry.text_layer = std::make_unique<bgs::gpu_text::Layer>();

    const double box_local_x =
        -eval_origin_x(layer, local_time) * box_width - surface_rect.x();
    const double box_local_y =
        -eval_origin_y(layer, local_time) * box_height - surface_rect.y();
    const double text_offset_x = box_local_x + style_rect.x() + ticker_offset_x;
    const double text_offset_y = box_local_y + style_rect.y() + ticker_offset_y;
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

    if (!entry.text_animation_layout ||
        entry.text_animation_layout->key.content != layout->key.content) {
        if (entry.text_animation_layout && entry.text_animation_layout->valid) {
            entry.text_animation_change = map_text_animator_content_change(
                *entry.text_animation_layout, *layout);
            entry.text_animation_change_local_time = local_time;
            if (!entry.text_animation_change.identical) {
                BGL_LOG_DEBUG("TextAnimator", QStringLiteral(
                    "stage=content-remap layer=%1 added=%2 changed=%3 removed=%4 localTime=%5")
                    .arg(QString::fromStdString(layer.id))
                    .arg(entry.text_animation_change.added_byte_ranges.size())
                    .arg(entry.text_animation_change.changed_byte_ranges.size())
                    .arg(entry.text_animation_change.removed_byte_ranges.size())
                    .arg(local_time, 0, 'f', 6));
            }
        } else {
            entry.text_animation_change = TextAnimatorContentChange();
            entry.text_animation_change.identical = true;
            entry.text_animation_change_local_time = local_time;
        }
        entry.text_animation_layout = layout;
    }

    const TextAnimatorStack evaluation_stack =
        entry.text_animation_change.identical
            ? layer.text_animators
            : text_animator_stack_for_content_change(
                  layer.text_animators, entry.text_animation_change,
                  entry.text_animation_change_local_time);
    const TextAnimatorEvaluation text_animation =
        evaluate_text_animators(evaluation_stack, *layout, local_time);
    if (!session->text_renderer->prepare(
            *entry.text_layer, layout,
            text_layout_paint_runs_canonical(request.document), options,
            &text_animation, failure_reason))
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
        !create_target(session->masked_target) || !create_target(session->adjustment_coverage_target) || !create_target(session->effect_a) ||
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
        if (entry.transform_grid)
            gs_vertexbuffer_destroy(entry.transform_grid);
        entry.transform_grid = nullptr;
        entry.transform_grid_key.clear();
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

