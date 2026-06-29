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
    const double bg_left = effect.padding_left_prop.is_animated()
        ? effect.padding_left_prop.evaluate(t)
        : static_cast<double>(effect.effect_padding_left);
    const double bg_right = effect.padding_right_prop.is_animated()
        ? effect.padding_right_prop.evaluate(t)
        : static_cast<double>(effect.effect_padding_right);
    const double bg_top = effect.padding_top_prop.is_animated()
        ? effect.padding_top_prop.evaluate(t)
        : static_cast<double>(effect.effect_padding_top);
    const double bg_bottom = effect.padding_bottom_prop.is_animated()
        ? effect.padding_bottom_prop.evaluate(t)
        : static_cast<double>(effect.effect_padding_bottom);
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
    case LayerEffectType::FourColorGradient:
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
    /* Compatibility and auxiliary passes operate on a complete surface. The
     * main compositor overwrites this identity basis with the actual layer
     * transform before drawing spatial effects. */
    set_effect_vec4_param(effect, "layerUvRect", 0.0f, 0.0f, 1.0f, 1.0f);
    set_effect_vec2_param(effect, "layerUvOrigin", 0.0f, 0.0f);
    set_effect_vec2_param(effect, "layerUvAxisX", 1.0f, 0.0f);
    set_effect_vec2_param(effect, "layerUvAxisY", 0.0f, 1.0f);
    set_effect_vec2_param(effect, "layerPixelSize",
                          static_cast<float>(std::max(1, width)),
                          static_cast<float>(std::max(1, height)));
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
    if (resolved.type == LayerEffectType::Emboss) {
        set_effect_float_param(effect, "depth", std::max(0.1f, resolved.effect_size));
        set_effect_float_param(effect, "height", std::max(0.1f, resolved.effect_distance));
        set_effect_float_param(effect, "softness", std::max(0.0f, resolved.effect_spread));
    }

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
    set_effect_int_param(effect, "outsideHardAlpha", resolved.effect_outside_hard_alpha ? 1 : 0);
    set_effect_int_param(effect, "outsideHardAlphaInvert", resolved.effect_outside_hard_alpha_invert ? 1 : 0);
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

static QJsonValue interpolate_extension_surface_value(const QJsonValue &a,
                                                        const QJsonValue &b,
                                                        double mix)
{
    if (a.isDouble() && b.isDouble())
        return a.toDouble() + (b.toDouble() - a.toDouble()) * mix;
    if (a.isArray() && b.isArray()) {
        const QJsonArray aa = a.toArray();
        const QJsonArray bb = b.toArray();
        QJsonArray out;
        const qsizetype count = std::min(aa.size(), bb.size());
        for (qsizetype index = 0; index < count; ++index)
            out.append(aa.at(index).toDouble() +
                       (bb.at(index).toDouble() - aa.at(index).toDouble()) * mix);
        return out;
    }
    return mix < 1.0 ? a : b;
}

static QJsonObject evaluated_extension_surface_parameters(const LayerEffect &effect,
                                                           double time)
{
    const QJsonDocument parameters_document = QJsonDocument::fromJson(
        QByteArray::fromStdString(effect.extension_parameters_json));
    QJsonObject parameters = parameters_document.isObject()
        ? parameters_document.object() : QJsonObject{};
    const QJsonDocument tracks_document = QJsonDocument::fromJson(
        QByteArray::fromStdString(effect.extension_keyframes_json));
    if (!tracks_document.isObject())
        return parameters;

    const QJsonObject tracks = tracks_document.object();
    for (auto track_it = tracks.begin(); track_it != tracks.end(); ++track_it) {
        const QJsonArray keys = track_it.value().toArray();
        if (keys.isEmpty())
            continue;
        QJsonObject left = keys.first().toObject();
        QJsonObject right = keys.last().toObject();
        for (const QJsonValue &key_value : keys) {
            const QJsonObject key = key_value.toObject();
            const double key_time = key.value(QStringLiteral("time")).toDouble();
            if (key_time <= time)
                left = key;
            if (key_time >= time) {
                right = key;
                break;
            }
        }
        const double left_time = left.value(QStringLiteral("time")).toDouble();
        const double right_time = right.value(QStringLiteral("time")).toDouble();
        const double linear_mix = right_time > left_time
            ? (time - left_time) / (right_time - left_time) : 0.0;
        const double mix = bgs::effects::animation::eased_mix(linear_mix, left);
        parameters.insert(track_it.key(), interpolate_extension_surface_value(
            left.value(QStringLiteral("value")),
            right.value(QStringLiteral("value")), mix));
    }
    return parameters;
}

static void set_gpu_extension_surface_params(gs_effect_t *effect,
                                              const LayerEffect &resolved,
                                              double time)
{
    if (!effect || resolved.extension_id.empty())
        return;

    QJsonObject parameter_schema;
    auto &catalog = BglEffectExtensionCatalog::instance();
    if (catalog.effects().empty())
        catalog.reload();
    if (const auto *definition = catalog.find(
            QString::fromStdString(resolved.extension_id)))
        parameter_schema = definition->parameterSchema;

    const QJsonObject parameters = evaluated_extension_surface_parameters(
        resolved, time);
    for (auto it = parameters.begin(); it != parameters.end(); ++it) {
        const QByteArray uniform_name = it.key().toUtf8();
        gs_eparam_t *param = gs_effect_get_param_by_name(
            effect, uniform_name.constData());
        if (!param)
            continue;
        const QJsonValue value = it.value();
        const QString declared_type = parameter_schema.value(it.key())
            .toObject().value(QStringLiteral("type")).toString();
        if (value.isBool()) {
            gs_effect_set_bool(param, value.toBool());
        } else if (value.isDouble()) {
            if (declared_type == QStringLiteral("int") ||
                declared_type == QStringLiteral("enum"))
                gs_effect_set_int(param, value.toInt());
            else
                gs_effect_set_float(param, static_cast<float>(value.toDouble()));
        } else if (value.isString()) {
            const QColor color(value.toString());
            if (color.isValid()) {
                struct vec4 vector;
                vec4_set(&vector, color.redF(), color.greenF(),
                         color.blueF(), color.alphaF());
                gs_effect_set_vec4(param, &vector);
            }
        } else if (value.isArray()) {
            const QJsonArray array = value.toArray();
            if (array.size() == 2) {
                struct vec2 vector;
                vec2_set(&vector, static_cast<float>(array.at(0).toDouble()),
                         static_cast<float>(array.at(1).toDouble()));
                gs_effect_set_vec2(param, &vector);
            } else if (array.size() == 3) {
                struct vec3 vector;
                vec3_set(&vector, static_cast<float>(array.at(0).toDouble()),
                         static_cast<float>(array.at(1).toDouble()),
                         static_cast<float>(array.at(2).toDouble()));
                gs_effect_set_vec3(param, &vector);
            } else if (array.size() == 4) {
                struct vec4 vector;
                vec4_set(&vector, static_cast<float>(array.at(0).toDouble()),
                         static_cast<float>(array.at(1).toDouble()),
                         static_cast<float>(array.at(2).toDouble()),
                         static_cast<float>(array.at(3).toDouble()));
                gs_effect_set_vec4(param, &vector);
            }
        }
    }
}

static std::mutex g_surface_effect_registry_mutex;
static std::unique_ptr<TitleEffectRegistry> g_surface_effect_registry;

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

    std::lock_guard<std::mutex> lock(g_surface_effect_registry_mutex);

    bool success = false;
    obs_enter_graphics();
    if (!g_surface_effect_registry)
        g_surface_effect_registry = std::make_unique<TitleEffectRegistry>();

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

            gs_effect_t *gpu_effect = g_surface_effect_registry->compile(resolved.type);
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
            /* Effects that must stay clipped to the original artwork (rather
             * than a previous pass or the render-target rectangle) receive
             * the immutable source texture as their hard alpha matte. */
            if (gs_eparam_t *hard_mask = gs_effect_get_param_by_name(gpu_effect, "hardMask"))
                gs_effect_set_texture(hard_mask, source);
            set_gpu_surface_effect_params(gpu_effect, resolved, width, height, t);
            set_gpu_extension_surface_params(gpu_effect, resolved, t);

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
    layer.transform_parent_id.clear();
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
        if (!eval_effect_enabled(effect, t) || effect.affect_layers_behind)
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
        case LayerEffectType::BackgroundColor: {
            const double left = effect.padding_left_prop.is_animated()
                ? effect.padding_left_prop.evaluate(t)
                : static_cast<double>(effect.effect_padding_left);
            const double right = effect.padding_right_prop.is_animated()
                ? effect.padding_right_prop.evaluate(t)
                : static_cast<double>(effect.effect_padding_right);
            const double top = effect.padding_top_prop.is_animated()
                ? effect.padding_top_prop.evaluate(t)
                : static_cast<double>(effect.effect_padding_top);
            const double bottom = effect.padding_bottom_prop.is_animated()
                ? effect.padding_bottom_prop.evaluate(t)
                : static_cast<double>(effect.effect_padding_bottom);
            const double stroke = std::max(0.0,
                effect.stroke_width_prop.is_animated()
                    ? effect.stroke_width_prop.evaluate(t)
                    : static_cast<double>(effect.effect_stroke_width));
            required += std::max({0.0, left, right, top, bottom}) + stroke + 4.0;
            break;
        }
        case LayerEffectType::DropShadow:
            required += distance + gaussian_blur_support(size + spread) +
                        size + spread + 12.0;
            break;
        case LayerEffectType::InnerShadow:
            /* Inner effects do not expand the layer alpha. */
            break;
        case LayerEffectType::LongShadow:
            required += distance +
                (effect.effect_blur_type == static_cast<int>(LongShadowBlurType::None)
                    ? 0.0 : gaussian_blur_support(size) + size) + 12.0;
            break;
        case LayerEffectType::Outline:
            required += size + distance + 4.0;
            break;
        case LayerEffectType::Glow:
        case LayerEffectType::Bloom:
            required += gaussian_blur_support(size + spread) +
                        size + spread + 12.0;
            break;
        case LayerEffectType::InnerGlow:
            /* Inner glow never expands alpha outside the source. */
            break;
        case LayerEffectType::Blur:
            required += gaussian_blur_support(size) + 4.0;
            break;
        case LayerEffectType::LensFlare: {
            /* Lens flare is generated in layer space and can extend well past
             * the original alpha. Reserve the longest ray/halo support so the
             * whole stack can remain transform-neutral until final composite. */
            const double box_extent = std::max(
                std::max(1.0, eval_box_width(layer, t)),
                std::max(1.0, eval_box_height(layer, t)));
            const double scale_value = std::max(0.001,
                effect.scale_prop.is_animated()
                    ? effect.scale_prop.evaluate(t)
                    : static_cast<double>(effect.effect_scale));
            required += box_extent * std::max(0.0, size) * scale_value * 11.0 + 12.0;
            break;
        }
        case LayerEffectType::RoughenEdges: {
            const double amount = std::max(0.0,
                effect.amount_prop.is_animated()
                    ? effect.amount_prop.evaluate(t)
                    : static_cast<double>(effect.effect_amount));
            required += amount * 64.0 + 4.0;
            break;
        }
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

static bool layer_requires_preserved_effect_surface(const Layer &layer,
                                                   double t)
{
    return std::any_of(layer.effects.begin(), layer.effects.end(),
        [t](const LayerEffect &effect) {
            return effect.type != LayerEffectType::MotionBlur &&
                   !effect.affect_layers_behind &&
                   eval_effect_enabled(effect, t);
        });
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
            << effect.effect_outside_hard_alpha << ',' << effect.effect_outside_hard_alpha_invert << ','
            << effect.affect_layers_behind << ',' << effect.affect_layers_behind_invert << ','
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

static QTransform layer_local_transform_qt(const Title &title,
                                           const Layer &layer,
                                           double title_time)
{
    QTransform local;
    const double resolved_time = layer_asset_resolved_time(
        title, layer, title_time);
    const double local_time = std::max(0.0, resolved_time - layer.in_time);
    const LayerTransitionVisualState transition = evaluate_layer_general_transitions(
        layer.transitions, layer.in_time, layer.out_time, resolved_time);
    const auto position = layer.position.evaluate(local_time);
    const auto scale = layer.scale.evaluate(local_time);
    local.translate(position.x + transition.translate_x,
                    position.y + transition.translate_y);
    local.rotate(layer.rotation.evaluate(local_time));
    local.scale(scale.x * transition.scale, scale.y * transition.scale);
    return local;
}

static std::vector<std::string> layer_group_chain(
    const Title &title, const std::string &group_parent_id)
{
    std::vector<std::string> chain;
    std::string cursor = group_parent_id;
    std::unordered_set<std::string> visited;
    while (!cursor.empty() && visited.insert(cursor).second) {
        const Layer *group = find_layer_by_id(title, cursor);
        if (!group || !layer_type_is_container(group->type))
            break;
        chain.push_back(cursor);
        cursor = group->parent_id;
    }
    return chain;
}

static std::vector<std::string> layer_group_chain_for_layer(
    const Title &title, const Layer &layer)
{
    std::vector<std::string> chain;
    if (layer_type_is_container(layer.type))
        chain.push_back(layer.id);
    const auto ancestors = layer_group_chain(title, layer.parent_id);
    chain.insert(chain.end(), ancestors.begin(), ancestors.end());
    return chain;
}

static std::string layer_common_group(const Title &title,
                                      const std::string &child_group_parent_id,
                                      const Layer &transform_parent)
{
    const auto child_groups = layer_group_chain(title, child_group_parent_id);
    const auto parent_groups = layer_group_chain_for_layer(title, transform_parent);
    const std::unordered_set<std::string> parent_set(parent_groups.begin(), parent_groups.end());
    for (const auto &id : child_groups)
        if (parent_set.find(id) != parent_set.end()) return id;
    return {};
}

static QTransform layer_world_transform_qt_impl(
    const Title &title, const Layer &layer, double title_time,
    std::unordered_set<std::string> &visiting);

static QTransform layer_parent_basis_qt(
    const Title &title, const Layer &layer, double title_time,
    std::unordered_set<std::string> &visiting)
{
    QTransform group_world;
    if (!layer.parent_id.empty()) {
        if (const Layer *group_parent = find_layer_by_id(title, layer.parent_id);
            group_parent && layer_type_is_container(group_parent->type)) {
            group_world = layer_world_transform_qt_impl(
                title, *group_parent, title_time, visiting);
        }
    }
    if (layer.transform_parent_id.empty())
        return group_world;
    const Layer *transform_parent = find_layer_by_id(title, layer.transform_parent_id);
    if (!transform_parent)
        return group_world;
    const QTransform transform_world = layer_world_transform_qt_impl(
        title, *transform_parent, title_time, visiting);
    const std::string common_id = layer_common_group(
        title, layer.parent_id, *transform_parent);
    if (common_id.empty())
        return group_world * transform_world;
    const Layer *common_group = find_layer_by_id(title, common_id);
    if (!common_group)
        return group_world * transform_world;
    std::unordered_set<std::string> common_visiting;
    const QTransform common_world = layer_world_transform_qt_impl(
        title, *common_group, title_time, common_visiting);
    bool invertible = false;
    const QTransform common_inverse = common_world.inverted(&invertible);
    return invertible ? group_world * common_inverse * transform_world
                      : group_world * transform_world;
}

static QTransform layer_world_transform_qt_impl(
    const Title &title, const Layer &layer, double title_time,
    std::unordered_set<std::string> &visiting)
{
    if (!layer.id.empty() && !visiting.insert(layer.id).second)
        return QTransform();
    const QTransform parent_basis =
        layer_parent_basis_qt(title, layer, title_time, visiting);
    /* Match editor/canvas and the pre-existing Group transform order exactly:
     * local layer transform first, then Group/transform-parent basis. */
    const QTransform world = layer_local_transform_qt(title, layer, title_time) * parent_basis;
    if (!layer.id.empty()) visiting.erase(layer.id);
    return world;
}

static QTransform layer_world_transform_qt(const Title &title, const Layer &layer,
                                           double title_time, int /*depth*/)
{
    std::unordered_set<std::string> visiting;
    return layer_world_transform_qt_impl(title, layer, title_time, visiting);
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

