static bool layer_has_non_rigid_transition_during_shutter(
    const Layer &layer, double start_time, double end_time)
{
    const std::array<double, 3> times = {
        start_time, (start_time + end_time) * 0.5, end_time};
    for (double time : times) {
        const LayerTransitionVisualState state = evaluate_layer_general_transitions(
            layer.transitions, layer.in_time, layer.out_time, time);
        if ((state.active && state.blur > 0.01) ||
            (state.active && state.wipe < 0.999999)) {
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
                const double current_opacity = gpu_layer_chain_visible(title, layer, title_time)
                    ? std::clamp(gpu_layer_chain_opacity(title, layer, title_time), 0.0, 1.0)
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
        const bool visible_start = gpu_layer_chain_visible(title, layer, shutter_start);
        const bool visible_mid = gpu_layer_chain_visible(title, layer, shutter_mid);
        const bool visible_end = gpu_layer_chain_visible(title, layer, shutter_end);
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
            if (gpu_layer_chain_visible(title, layer, sample_time))
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
                    if (!gpu_layer_chain_visible(title, base_layer, sample_time))
                        return;
                    cairo_save(cr);
                    apply_layer_world_transform(cr, title, base_layer, sample_time);
                    cairo_set_source_surface(cr, cached_surface.get(),
                                             cached->origin.x(), cached->origin.y());
                    cairo_paint_with_alpha(cr, alpha * std::clamp(gpu_layer_chain_opacity(title, base_layer, sample_time),
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
    cairo_paint_with_alpha(cr, std::clamp(gpu_layer_chain_opacity(title, render_layer, title_time), 0.0, 1.0));
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
    cairo_paint_with_alpha(cr, std::clamp(gpu_layer_chain_opacity(title, layer, title_time), 0.0, 1.0));
    cairo_restore(cr);
    return true;
}


static bool mask_mode_is_inverted(MaskMode mode)
{
    return mode == MaskMode::InvertedAlpha || mode == MaskMode::InvertedLuma ||
           mode == MaskMode::InvertedClipping;
}

/* Phase 13 intentionally has no Cairo/QPainter mask compositor.  Mask source
 * coverage remains a GPU texture (or GPU-rendered vector/text geometry), and
 * alpha/luma/clipping/inversion are resolved by kGpuMaskEffect. */

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

static bool prepare_external_composite_dimensions_locked(
    TitleGpuRenderSession *session);
static QImage title_gpu_render_session_readback_over_checkerboard(
    TitleGpuRenderSession *session);

QImage render_title_to_image(const Title &title, double t,
                             uint64_t model_revision)
{
    if (!title_requires_destination_compositing(title))
        return render_title_gpu_frame_readback(title, t, model_revision);

    const auto acquired = acquire_gpu_readback_session(model_revision);
    title_gpu_render_session_update(acquired.first, title, t,
                                    acquired.second);
    return title_gpu_render_session_readback_over_checkerboard(acquired.first);
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
    {
        std::lock_guard<std::mutex> surfaceLock(g_surface_effect_registry_mutex);
        obs_enter_graphics();
        g_surface_effect_registry.reset();
        clear_extension_image_textures();
        obs_leave_graphics();
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
    QImage image = render_title_to_image(title, t);
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
        data->cue_first_row_when_active = obs_data_get_bool(settings, PROP_CUE_FIRST_ROW_WHEN_ACTIVE);
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
    data->seen_external_data_revision = std::numeric_limits<uint64_t>::max();
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
    clear_live_cue_runtime_for_source(data, data->title_id);
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
        if (data->scene_mask_matted_texrender) gs_texrender_destroy(data->scene_mask_matted_texrender);
        if (data->scene_mask_effect) gs_effect_destroy(data->scene_mask_effect);
        obs_leave_graphics();
        data->scene_mask_scene_texrender = nullptr;
        data->scene_mask_matted_texrender = nullptr;
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
    clear_live_cue_runtime_for_source(data, previous_title_id);
    data->cue_timer_row = -1;
    data->cue_timer_elapsed_seconds = 0.0;
    const bool keep_scene_masks_foreground = data->scene_mask_foreground_active;
    if (keep_scene_masks_foreground)
        release_active_scene_mask_scenes(data);

    data->title_id = obs_data_get_string(settings, PROP_TITLE_ID);
    data->cue_first_row_when_active = obs_data_get_bool(settings, PROP_CUE_FIRST_ROW_WHEN_ACTIVE);
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
    data->seen_external_data_revision = std::numeric_limits<uint64_t>::max();
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

static bool cue_first_live_text_row_on_activation(
    TitleSourceData *data, const std::shared_ptr<Title> &title)
{
    if (!data || !data->cue_first_row_when_active || !title)
        return false;

    auto exposed = exposed_text_layers(title);
    if (exposed.empty())
        return false;

    bgs::live_text::normalize_live_text_rows(title, exposed);
    if (title->live_text_rows.empty())
        return false;

    /* Activation starts from a clean first-row state. Unlike a cue issued
     * while the source is already live, there is no outgoing frame that needs
     * an outro/persistence hand-off. Apply the values immediately and let the
     * normal source cue state machine start the intro/playback on the next
     * video tick. */
    apply_live_text_row(title, 0);
    title->current_cue_row = 0;
    title->pending_cue_row = -1;
    title->cue_uncue_requested = false;
    title->cue_persistence_transition = false;
    title->cue_persistent_text_columns.clear();
    ++title->cue_revision;
    TitleDataStore::instance().touch_runtime_change();

    data->force_cue_state_sync = true;
    data->waiting_for_cue = true;
    data->dirty = true;
    data->first_frame_pending = true;
    BGL_LOG_INFO("LiveCue", QStringLiteral(
        "Cueing first row on source activation title=%1 revision=%2")
        .arg(QString::fromStdString(title->id))
        .arg(static_cast<qulonglong>(title->cue_revision)));
    return true;
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
    cue_first_live_text_row_on_activation(data, title);
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
    if (!data->cue_first_row_when_active) {
        title->current_cue_row = -1;
        title->pending_cue_row = -1;
        title->cue_uncue_requested = false;
        title->cue_persistence_transition = false;
        title->cue_persistent_text_columns.clear();
        ++title->cue_revision;
        TitleDataStore::instance().touch_runtime_change();
    } else {
        /* Row zero is already active. Continue a forward playlist from the
         * following row; reverse mode keeps its configured reverse start. */
        title->playlist_next_row = title->playlist_reverse
            ? row_count - 1
            : (row_count > 1 ? 1 : 0);
    }
}

static void source_deactivate(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data)
        return;
    data->scene_mask_foreground_active = false;
    clear_live_cue_runtime_for_source(data, data->title_id);
    data->cue_timer_row = -1;
    data->cue_timer_elapsed_seconds = 0.0;
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
    sync_scene_mask_scenes_for_cue(
        data, TitleDataStore::instance().get_title(data->title_id));
}

