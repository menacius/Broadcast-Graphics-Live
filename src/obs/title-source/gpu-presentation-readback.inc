struct GpuEffectLayerSpace {
    QRectF bounds_uv{0.0, 0.0, 1.0, 1.0};
    QPointF origin_uv{0.0, 0.0};
    QPointF axis_x_uv{1.0, 0.0};
    QPointF axis_y_uv{0.0, 1.0};
    QPointF pixel_size{1.0, 1.0};
};

static GpuEffectLayerSpace gpu_effect_layer_space(
    const TitleGpuRenderSession *session, const Layer &layer,
    const TitleGpuRenderSession::LayerRaster *entry,
    uint32_t texture_width, uint32_t texture_height)
{
    GpuEffectLayerSpace space;
    if (texture_width == 0 || texture_height == 0)
        return space;

    if (entry && entry->logical_width > 0.0 && entry->logical_height > 0.0 &&
        entry->layer_box_rect.isValid()) {
        const QRectF rect(entry->layer_box_rect.x() / entry->logical_width,
                          entry->layer_box_rect.y() / entry->logical_height,
                          entry->layer_box_rect.width() / entry->logical_width,
                          entry->layer_box_rect.height() / entry->logical_height);
        space.bounds_uv = rect;
        space.origin_uv = rect.topLeft();
        space.axis_x_uv = QPointF(rect.width(), 0.0);
        space.axis_y_uv = QPointF(0.0, rect.height());
        space.pixel_size = QPointF(
            std::max(1.0, std::abs(rect.width()) * texture_width),
            std::max(1.0, std::abs(rect.height()) * texture_height));
        return space;
    }

    if (!session || !session->has_title)
        return space;

    const double resolved_time = gpu_layer_render_time(
        session->title, layer, session->time);
    const double local_time = std::max(0.0, resolved_time - layer.in_time);
    const double box_width = std::max(0.0001, eval_box_width(layer, local_time));
    const double box_height = std::max(0.0001, eval_box_height(layer, local_time));
    const QPointF local_origin(-eval_origin_x(layer, local_time) * box_width,
                               -eval_origin_y(layer, local_time) * box_height);
    const QTransform world = layer_world_transform_qt(
        session->title, layer, session->time);
    const double frame_scale = session->editor_draft
        ? std::clamp(static_cast<double>(session->preview_quality_scale), 0.25, 1.0)
        : 1.0;
    auto to_uv = [&](const QPointF &canvas_point) {
        return QPointF(canvas_point.x() * frame_scale / texture_width,
                       canvas_point.y() * frame_scale / texture_height);
    };
    const QPointF origin = to_uv(world.map(local_origin));
    const QPointF x_end = to_uv(world.map(local_origin + QPointF(box_width, 0.0)));
    const QPointF y_end = to_uv(world.map(local_origin + QPointF(0.0, box_height)));
    const QPointF axis_x = x_end - origin;
    const QPointF axis_y = y_end - origin;
    const QPointF opposite = origin + axis_x + axis_y;
    const double min_x = std::min({origin.x(), x_end.x(), y_end.x(), opposite.x()});
    const double min_y = std::min({origin.y(), x_end.y(), y_end.y(), opposite.y()});
    const double max_x = std::max({origin.x(), x_end.x(), y_end.x(), opposite.x()});
    const double max_y = std::max({origin.y(), x_end.y(), y_end.y(), opposite.y()});
    space.bounds_uv = QRectF(min_x, min_y,
                             std::max(0.000001, max_x - min_x),
                             std::max(0.000001, max_y - min_y));
    space.origin_uv = origin;
    space.axis_x_uv = axis_x;
    space.axis_y_uv = axis_y;
    space.pixel_size = QPointF(
        std::max(1.0, std::hypot(axis_x.x() * texture_width,
                                 axis_x.y() * texture_height)),
        std::max(1.0, std::hypot(axis_y.x() * texture_width,
                                 axis_y.y() * texture_height)));
    return space;
}

static QRectF gpu_effect_layer_uv_rect(
    const TitleGpuRenderSession *session, const Layer &layer,
    const TitleGpuRenderSession::LayerRaster *entry,
    uint32_t texture_width, uint32_t texture_height)
{
    return gpu_effect_layer_space(session, layer, entry,
                                  texture_width, texture_height).bounds_uv;
}

static void set_gpu_layer_space_params(
    gs_effect_t *effect, const TitleGpuRenderSession *session,
    const Layer &layer, const TitleGpuRenderSession::LayerRaster *entry,
    uint32_t texture_width, uint32_t texture_height)
{
    if (!effect || texture_width == 0 || texture_height == 0)
        return;
    const GpuEffectLayerSpace space = gpu_effect_layer_space(
        session, layer, entry, texture_width, texture_height);
    set_effect_vec4_param(effect, "layerUvRect",
                          static_cast<float>(space.bounds_uv.x()),
                          static_cast<float>(space.bounds_uv.y()),
                          static_cast<float>(std::max(space.bounds_uv.width(), 0.000001)),
                          static_cast<float>(std::max(space.bounds_uv.height(), 0.000001)));
    set_effect_vec2_param(effect, "layerUvOrigin",
                          static_cast<float>(space.origin_uv.x()),
                          static_cast<float>(space.origin_uv.y()));
    set_effect_vec2_param(effect, "layerUvAxisX",
                          static_cast<float>(space.axis_x_uv.x()),
                          static_cast<float>(space.axis_x_uv.y()));
    set_effect_vec2_param(effect, "layerUvAxisY",
                          static_cast<float>(space.axis_y_uv.x()),
                          static_cast<float>(space.axis_y_uv.y()));
    set_effect_vec2_param(effect, "layerPixelSize",
                          static_cast<float>(space.pixel_size.x()),
                          static_cast<float>(space.pixel_size.y()));
}

static void set_gpu_background_geometry_params(
    gs_effect_t *effect, const TitleGpuRenderSession *session,
    const LayerEffect &resolved, const Layer &layer,
    double title_time, const TitleGpuRenderSession::LayerRaster *entry,
    uint32_t texture_width, uint32_t texture_height)
{
    if (!effect || texture_width == 0 || texture_height == 0)
        return;
    const GpuEffectLayerSpace space = gpu_effect_layer_space(
        session, layer, entry, texture_width, texture_height);
    const double resolved_time = session
        ? gpu_layer_render_time(session->title, layer, session->time)
        : title_time;
    const double local_time = std::max(0.0, resolved_time - layer.in_time);
    const double base_width = entry && entry->base_box_width > 0.0001
        ? entry->base_box_width
        : std::max(0.0001, eval_box_width(layer, local_time));
    const double base_height = entry && entry->base_box_height > 0.0001
        ? entry->base_box_height
        : std::max(0.0001, eval_box_height(layer, local_time));
    const double pixel_width = std::max(1.0, space.pixel_size.x());
    const double pixel_height = std::max(1.0, space.pixel_size.y());
    const double scale_x = pixel_width / base_width;
    const double scale_y = pixel_height / base_height;

    /* Geometry is expressed relative to the layer box rather than the backing
     * surface. That keeps Background Color attached to translated/rotated
     * layers and groups and prevents a later shadow/glow pass from turning the
     * background into a full-canvas rectangle. */
    const double x = -resolved.effect_padding_left * scale_x;
    const double y = -resolved.effect_padding_top * scale_y;
    const double w = std::max(0.0, pixel_width +
        resolved.effect_padding_left * scale_x +
        resolved.effect_padding_right * scale_x);
    const double h = std::max(0.0, pixel_height +
        resolved.effect_padding_top * scale_y +
        resolved.effect_padding_bottom * scale_y);
    set_effect_vec4_param(effect, "backgroundRect",
        static_cast<float>(x), static_cast<float>(y),
        static_cast<float>(w), static_cast<float>(h));
    set_effect_vec2_param(effect, "textureSize",
                          static_cast<float>(pixel_width),
                          static_cast<float>(pixel_height));
    const float geometry_scale = static_cast<float>(
        std::max(0.0001, std::min(std::abs(scale_x), std::abs(scale_y))));
    set_effect_vec4_param(effect, "cornerRadii",
                          resolved.effect_corner_radius_tl * geometry_scale,
                          resolved.effect_corner_radius_tr * geometry_scale,
                          resolved.effect_corner_radius_br * geometry_scale,
                          resolved.effect_corner_radius_bl * geometry_scale);
    set_effect_int_param(effect, "fillType", resolved.effect_fill_type);
    set_effect_int_param(effect, "gradientType", resolved.effect_gradient_type);
    set_effect_int_param(effect, "gradientSpread", resolved.effect_gradient_spread);
    set_effect_color_param(effect, "gradientStartColor", resolved.effect_gradient_start_color);
    set_effect_color_param(effect, "gradientEndColor", resolved.effect_gradient_end_color);
    set_effect_float_param(effect, "gradientStartPos", resolved.effect_gradient_start_pos);
    set_effect_float_param(effect, "gradientEndPos", resolved.effect_gradient_end_pos);
    set_effect_float_param(effect, "gradientStartOpacity", resolved.effect_gradient_start_opacity);
    set_effect_float_param(effect, "gradientEndOpacity", resolved.effect_gradient_end_opacity);
    set_effect_float_param(effect, "gradientOpacity", resolved.effect_gradient_opacity);
    set_effect_float_param(effect, "gradientAngle", resolved.effect_gradient_angle);
    set_effect_vec2_param(effect, "gradientCenter", resolved.effect_gradient_center_x,
                          resolved.effect_gradient_center_y);
    set_effect_vec2_param(effect, "gradientFocal", resolved.effect_gradient_focal_x,
                          resolved.effect_gradient_focal_y);
    set_effect_float_param(effect, "gradientScale", resolved.effect_gradient_scale);
    set_effect_color_param(effect, "strokeColor", resolved.effect_stroke_color);
    set_effect_float_param(effect, "strokeWidth",
                           resolved.effect_stroke_width * geometry_scale);
    set_effect_float_param(effect, "strokeOpacity", resolved.effect_stroke_opacity);
}

static gs_texture_t *apply_gpu_layer_effect_stack(TitleGpuRenderSession *session,
                                                   const Layer &layer,
                                                   double title_time,
                                                   gs_texture_t *input,
                                                   uint32_t width,
                                                   uint32_t height,
                                                   TitleGpuRenderSession::LayerRaster *cache_entry = nullptr,
                                                   bool adjustment_only = false)
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
        LayerEffectType registered_builtin_type{};
        const bool is_registered_builtin = BglEffectExtensionCatalog::builtInTypeForId(
            QString::fromStdString(resolved.extension_id), &registered_builtin_type);
        const std::string execution_id = (resolved.extension_id.empty() || is_registered_builtin)
            ? BglEffectExtensionCatalog::builtInId(shader_type).toStdString()
            : resolved.extension_id;
        gs_effect_t *effect = session->effect_registry->compile(execution_id);
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
            effect_config.type == LayerEffectType::MotionBlur ||
            (adjustment_only && !effect_config.affect_layers_behind) ||
            (!adjustment_only && effect_config.affect_layers_behind))
            continue;
        LayerEffect resolved = resolve_gpu_layer_effect(effect_config, local_time);
        if (session->editor_draft) {
            /* Draft preview must preserve the same effect topology as final
             * rendering. Omitting shadows/glows here made group effects appear
             * unsupported and hid clipping bugs. Reduce expensive radii rather
             * than dropping the pass entirely. */
            switch (resolved.type) {
            case LayerEffectType::Blur:
            case LayerEffectType::DropShadow:
            case LayerEffectType::Glow:
            case LayerEffectType::InnerGlow:
            case LayerEffectType::InnerShadow:
            case LayerEffectType::Bloom:
                resolved.effect_size *= session->preview_quality_scale;
                break;
            case LayerEffectType::LongShadow:
                resolved.effect_distance *= session->preview_quality_scale;
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
        ? cache_entry->key + "|gpu-effects-v8-lens-flare-dx11-keyword-fix-v9-effects-cleanup-4-color-gradient-v10-layer-space-stack|" +
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
            if (pass.resolved.effect_size > 0.01f) {
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
        /* Outside/inside hard-alpha clipping must always use the untouched
         * artwork silhouette, not the output of an earlier effect pass. This is
         * especially important for groups, whose first effect can expand alpha
         * over the complete child composite. */
        if (gs_eparam_t *hard_mask =
                gs_effect_get_param_by_name(pass.effect, "hardMask"))
            gs_effect_set_texture(hard_mask, input);
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
        set_gpu_layer_space_params(pass.effect, session, layer, cache_entry,
                                   width, height);
        /* Extension parameters use a name-based ABI: scalar JSON values map to
         * same-named shader uniforms. Compound graphs are flattened to
         * element<N>_<property> uniforms, keeping native plugins optional. */
        if (!pass.resolved.extension_id.empty()) {
            const QJsonDocument extDoc = QJsonDocument::fromJson(
                QByteArray::fromStdString(pass.resolved.extension_parameters_json));
            if (extDoc.isObject()) {
                auto &extensionCatalog = BglEffectExtensionCatalog::instance();
                if (extensionCatalog.effects().empty())
                    extensionCatalog.reload();
                const auto *extensionDefinition = extensionCatalog.find(
                    QString::fromStdString(pass.resolved.extension_id));
                const QJsonObject parameterSchema = extensionDefinition
                    ? extensionDefinition->parameterSchema : QJsonObject{};
                auto setUniform = [&](const QString &name, const QJsonValue &value) {
                    gs_eparam_t *param = gs_effect_get_param_by_name(pass.effect, name.toUtf8().constData());
                    if (!param) return;
                    const QString declaredType = parameterSchema.value(name).toObject()
                        .value(QStringLiteral("type")).toString();
                    if (value.isBool()) gs_effect_set_bool(param, value.toBool());
                    else if (value.isDouble()) {
                        if (declaredType == QStringLiteral("int") ||
                            declaredType == QStringLiteral("enum"))
                            gs_effect_set_int(param, value.toInt());
                        else
                            gs_effect_set_float(param, static_cast<float>(value.toDouble()));
                    } else if (value.isString()) {
                        const QColor color(value.toString());
                        if (color.isValid()) {
                            vec4 v; vec4_set(&v, color.redF(), color.greenF(), color.blueF(), color.alphaF());
                            gs_effect_set_vec4(param, &v);
                        }
                    } else if (value.isArray()) {
                        const QJsonArray a = value.toArray();
                        if (a.size() == 2) {
                            vec2 v;
                            vec2_set(&v, static_cast<float>(a.at(0).toDouble()),
                                         static_cast<float>(a.at(1).toDouble()));
                            gs_effect_set_vec2(param, &v);
                        } else if (a.size() == 3) {
                            vec3 v;
                            vec3_set(&v, static_cast<float>(a.at(0).toDouble()),
                                         static_cast<float>(a.at(1).toDouble()),
                                         static_cast<float>(a.at(2).toDouble()));
                            gs_effect_set_vec3(param, &v);
                        } else if (a.size() == 4) {
                            vec4 v;
                            vec4_set(&v, static_cast<float>(a.at(0).toDouble()),
                                         static_cast<float>(a.at(1).toDouble()),
                                         static_cast<float>(a.at(2).toDouble()),
                                         static_cast<float>(a.at(3).toDouble()));
                            gs_effect_set_vec4(param, &v);
                        }
                    }
                };
                QJsonObject object = extDoc.object();
                /* Evaluate host-owned extension animation tracks before binding
                 * uniforms. Tracks are keyed by JSON paths such as
                 * "intensity" or "elements.0.opacity". */
                const QJsonDocument tracksDoc = QJsonDocument::fromJson(
                    QByteArray::fromStdString(pass.resolved.extension_keyframes_json));
                auto interpolateValue = [](const QJsonValue &a, const QJsonValue &b, double t) -> QJsonValue {
                    if (a.isDouble() && b.isDouble())
                        return a.toDouble() + (b.toDouble() - a.toDouble()) * t;
                    if (a.isArray() && b.isArray()) {
                        const QJsonArray aa = a.toArray(), bb = b.toArray();
                        QJsonArray out;
                        const qsizetype n = std::min(aa.size(), bb.size());
                        for (qsizetype i = 0; i < n; ++i)
                            out.append(aa.at(i).toDouble() + (bb.at(i).toDouble() - aa.at(i).toDouble()) * t);
                        return out;
                    }
                    return t < 1.0 ? a : b;
                };
                auto setPath = [](QJsonObject &root, const QString &path, const QJsonValue &value) {
                    const QStringList parts = path.split(QLatin1Char('.'), Qt::SkipEmptyParts);
                    if (parts.size() == 1) { root.insert(parts.front(), value); return; }
                    if (parts.size() == 3 && parts.at(0) == QStringLiteral("elements")) {
                        bool ok = false; const int index = parts.at(1).toInt(&ok);
                        QJsonArray elements = root.value(QStringLiteral("elements")).toArray();
                        if (ok && index >= 0 && index < elements.size()) {
                            QJsonObject element = elements.at(index).toObject();
                            element.insert(parts.at(2), value);
                            elements.replace(index, element);
                            root.insert(QStringLiteral("elements"), elements);
                        }
                    }
                };
                if (tracksDoc.isObject()) {
                    const QJsonObject tracks = tracksDoc.object();
                    for (auto trackIt = tracks.begin(); trackIt != tracks.end(); ++trackIt) {
                        const QJsonArray keys = trackIt.value().toArray();
                        if (keys.isEmpty()) continue;
                        QJsonObject left = keys.first().toObject(), right = keys.last().toObject();
                        for (int k = 0; k < keys.size(); ++k) {
                            const QJsonObject key = keys.at(k).toObject();
                            const double kt = key.value(QStringLiteral("time")).toDouble();
                            if (kt <= local_time) left = key;
                            if (kt >= local_time) { right = key; break; }
                        }
                        const double lt0 = left.value(QStringLiteral("time")).toDouble();
                        const double rt0 = right.value(QStringLiteral("time")).toDouble();
                        const double linear_mix = rt0 > lt0
                            ? (local_time - lt0) / (rt0 - lt0) : 0.0;
                        const double mix = bgs::effects::animation::eased_mix(linear_mix, left);
                        setPath(object, trackIt.key(), interpolateValue(left.value(QStringLiteral("value")), right.value(QStringLiteral("value")), mix));
                    }
                }
                if (extensionDefinition) {
                    const QJsonArray assets = extensionDefinition->assetIndex.value(QStringLiteral("items")).toArray();
                    for (const QJsonValue &assetValue : assets) {
                        const QJsonObject asset = assetValue.toObject();
                        const QString uniformName = asset.value(QStringLiteral("uniform")).toString();
                        const QString relativeFile = asset.value(QStringLiteral("file")).toString();
                        if (uniformName.isEmpty() || relativeFile.isEmpty()) continue;
                        gs_eparam_t *textureParam = gs_effect_get_param_by_name(pass.effect, uniformName.toUtf8().constData());
                        if (!textureParam) continue;
                        const QString assetPath = QFileInfo(relativeFile).isAbsolute()
                            ? relativeFile : QDir(extensionDefinition->basePath).filePath(relativeFile);
                        if (gs_texture_t *assetTexture = extension_image_texture(assetPath))
                            gs_effect_set_texture(textureParam, assetTexture);
                    }
                }
                for (auto it = object.begin(); it != object.end(); ++it) {
                    if (it.key() == QStringLiteral("elements") && it.value().isArray()) {
                        const QJsonArray elements = it.value().toArray();
                        if (gs_eparam_t *count = gs_effect_get_param_by_name(pass.effect, "elementCount"))
                            gs_effect_set_int(count, std::min<int>(static_cast<int>(elements.size()), 16));
                        for (int ei = 0; ei < elements.size() && ei < 16; ++ei) {
                            const QJsonObject element = elements.at(ei).toObject();
                            for (auto ep = element.begin(); ep != element.end(); ++ep)
                                setUniform(QStringLiteral("element%1_%2").arg(ei).arg(ep.key()), ep.value());
                        }
                    } else setUniform(it.key(), it.value());
                }
            }
        }
        if (pass.resolved.type == LayerEffectType::BackgroundColor)
            set_gpu_background_geometry_params(pass.effect, session,
                                               pass.resolved, layer, title_time,
                                               cache_entry, width, height);
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

enum class GpuMaskGraphPurpose {
    MatteArtwork,
    ClippingShape,
};

static gs_texture_t *render_gpu_mask_graph_texture(
    TitleGpuRenderSession *session, const Title &title, const Layer &layer,
    double title_time, const std::string &raster_id,
    const std::string &compositor_group_id,
    GpuMaskGraphPurpose purpose = GpuMaskGraphPurpose::MatteArtwork);

static gs_texture_t *apply_gpu_mask(TitleGpuRenderSession *session,
                                    gs_texture_t *layer_texture,
                                    gs_texture_t *mask_texture,
                                    MaskMode mode);

static gs_texture_t *apply_gpu_track_matte_texture(
    TitleGpuRenderSession *session, const Title &title,
    gs_texture_t *target_texture, const Layer &mask_layer,
    double title_time, const std::string &raster_id,
    const std::string &compositor_group_id, MaskMode mode);

static gs_texture_t *render_gpu_adjustment_coverage(
    TitleGpuRenderSession *session, const Title &title, const Layer &layer,
    double title_time)
{
    const double resolved_time = gpu_layer_render_time(title, layer, title_time);
    if (!session || !session->adjustment_coverage_target)
        return nullptr;
    if (!session->adjustment_coverage_effect)
        session->adjustment_coverage_effect = gs_effect_create(
            kGpuAdjustmentCoverageEffect, "obs-bgs-adjustment-coverage.effect", nullptr);
    if (!session->adjustment_coverage_effect)
        return nullptr;
    struct vec4 clear; vec4_zero(&clear);
    if (!begin_gpu_target(session->adjustment_coverage_target,
                          session->width, session->height, clear))
        return nullptr;
    const double lt = std::max(0.0, resolved_time - layer.in_time);
    const double w = std::max(0.0001, eval_box_width(layer, lt));
    const double h = std::max(0.0001, eval_box_height(layer, lt));
    gs_enable_blending(false);
    gs_matrix_push();
    gs_matrix_identity();
    const float frame_scale = session->editor_draft
        ? std::clamp(session->preview_quality_scale, 0.25f, 1.0f) : 1.0f;
    gs_matrix_scale3f(frame_scale, frame_scale, 1.0f);
    apply_layer_world_transform_gs(title, layer, title_time, 0);
    gs_matrix_translate3f(static_cast<float>(-eval_origin_x(layer, lt) * w),
                          static_cast<float>(-eval_origin_y(layer, lt) * h), 0.0f);
    while (gs_effect_loop(session->adjustment_coverage_effect, "Draw"))
        gs_draw_sprite(nullptr, 0, static_cast<uint32_t>(std::ceil(w)),
                       static_cast<uint32_t>(std::ceil(h)));
    gs_matrix_pop();
    gs_texrender_end(session->adjustment_coverage_target);
    gs_texture_t *coverage = gs_texrender_get_texture(session->adjustment_coverage_target);
    if (!coverage)
        return nullptr;
    if (layer.mask_mode != MaskMode::None && !layer.mask_source_id.empty()) {
        const Layer *mask_layer = find_layer_by_id(title, layer.mask_source_id);
        if (mask_layer && gpu_layer_chain_active_as_matte(title, *mask_layer, title_time)) {
            coverage = apply_gpu_track_matte_texture(
                session, title, coverage, *mask_layer, title_time,
                std::string(), layer.parent_id, layer.mask_mode);
            if (!coverage)
                return nullptr;
        } else if (!mask_mode_is_inverted(layer.mask_mode)) {
            return nullptr;
        }
    }
    return coverage;
}

static gs_texture_t *mix_gpu_adjustment_layer(
    TitleGpuRenderSession *session, const Title &title, const Layer &layer,
    double title_time, gs_texture_t *original, gs_texture_t *effected,
    gs_texture_t *coverage, gs_texrender_t *target,
    bool bounded_backdrop = false, double opacity_override = -1.0)
{
    const double resolved_time = gpu_layer_render_time(title, layer, title_time);
    if (!session || !original || !effected || !coverage || !target)
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
    if (gs_eparam_t *param = gs_effect_get_param_by_name(effect, "coverageImage"))
        gs_effect_set_texture(param, coverage);
    /* A bounded backdrop effect is a replacement of the pixels behind the
     * artwork, not another semi-transparent copy of them.  Layer opacity and
     * blend mode belong exclusively to the later artwork composite. */
    const double resolved_opacity = opacity_override >= 0.0
        ? opacity_override
        : gpu_layer_chain_opacity(title, layer, title_time);
    const float amount = bounded_backdrop
        ? 1.0f
        : static_cast<float>(std::clamp(resolved_opacity, 0.0, 1.0));
    set_effect_float_param(effect, "amount", amount);
    set_effect_int_param(effect, "blendMode", static_cast<int>(
        bounded_backdrop ? EffectBlendMode::Normal : layer.blend_mode));
    bool invert_coverage = false;
    for (const LayerEffect &candidate : layer.effects) {
        if (candidate.enabled && candidate.affect_layers_behind && candidate.affect_layers_behind_invert) {
            invert_coverage = true;
            break;
        }
    }
    set_effect_int_param(effect, "coverageInvert", invert_coverage ? 1 : 0);
    const LayerTransitionVisualState transition = evaluate_layer_general_transitions(
        layer.transitions, layer.in_time, layer.out_time, resolved_time);
    set_gpu_transition_effect_params(session, effect, transition);
    gs_enable_blending(false);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(original, 0, session->width, session->height);
    gs_texrender_end(target);
    gs_texture_t *result = gs_texrender_get_texture(target);
    return result ? result : original;
}

static Vec2Value render_quad_value(const AnimatedVec2Property &prop, float x, float y, double local_time)
{
    if (prop.is_animated() || prop.static_value.x != 0.0 || prop.static_value.y != 0.0)
        return prop.evaluate(local_time);
    return {x, y};
}

static bool layer_has_free_transform_quad(const Layer &layer, double local_time)
{
    const Vec2Value values[] = {
        render_quad_value(layer.transform_quad_tl, layer.transform_quad_tl_x, layer.transform_quad_tl_y, local_time),
        render_quad_value(layer.transform_quad_tr, layer.transform_quad_tr_x, layer.transform_quad_tr_y, local_time),
        render_quad_value(layer.transform_quad_br, layer.transform_quad_br_x, layer.transform_quad_br_y, local_time),
        render_quad_value(layer.transform_quad_bl, layer.transform_quad_bl_x, layer.transform_quad_bl_y, local_time)};
    for (const auto &value : values)
        if (std::abs(value.x) > 0.00001 || std::abs(value.y) > 0.00001) return true;
    return false;
}

static gs_vertbuffer_t *create_free_transform_grid(
    const Layer &layer, uint32_t texture_width, uint32_t texture_height,
    double logical_width, double logical_height, const QPointF &origin,
    double base_box_width, double base_box_height, double raster_sx,
    double raster_sy, double origin_x, double origin_y, double local_time)
{
    /* A coarse fixed grid makes glyph edges visibly kink at cell boundaries,
     * especially when a text layer is compressed into a strong perspective
     * trapezoid.  Use an adaptive grid so each cell covers roughly 20-24
     * source pixels, while keeping the mesh bounded for realtime playback. */
    const int divisions = std::clamp(
        static_cast<int>(std::ceil(std::max(texture_width, texture_height) / 22.0)),
        24, 96);
    const int quads = divisions * divisions;
    gs_vb_data *data = gs_vbdata_create();
    if (!data) return nullptr;
    data->num = static_cast<size_t>(quads * 6);
    data->points = static_cast<vec3 *>(bzalloc(sizeof(vec3) * data->num));
    data->num_tex = 1;
    data->tvarray = static_cast<gs_tvertarray *>(bzalloc(sizeof(gs_tvertarray)));
    if (!data->points || !data->tvarray) { gs_vbdata_destroy(data); return nullptr; }
    data->tvarray[0].width = 2;
    data->tvarray[0].array = bzalloc(sizeof(vec2) * data->num);
    if (!data->tvarray[0].array) { gs_vbdata_destroy(data); return nullptr; }
    auto *uvs = static_cast<vec2 *>(data->tvarray[0].array);

    const Vec2Value qtl = render_quad_value(
        layer.transform_quad_tl, layer.transform_quad_tl_x,
        layer.transform_quad_tl_y, local_time);
    const Vec2Value qtr = render_quad_value(
        layer.transform_quad_tr, layer.transform_quad_tr_x,
        layer.transform_quad_tr_y, local_time);
    const Vec2Value qbr = render_quad_value(
        layer.transform_quad_br, layer.transform_quad_br_x,
        layer.transform_quad_br_y, local_time);
    const Vec2Value qbl = render_quad_value(
        layer.transform_quad_bl, layer.transform_quad_bl_x,
        layer.transform_quad_bl_y, local_time);

    /* Map the unit square to the distorted layer quad with one projective
     * transform.  Bilinear corner-offset interpolation bends horizontal text
     * rows and makes neighboring mesh cells disagree visually.  A homography
     * preserves straight lines, which is the expected behavior for perspective
     * and free-distort transforms. */
    const double x0 = qtl.x;
    const double y0 = qtl.y;
    const double x1 = 1.0 + qtr.x;
    const double y1 = qtr.y;
    const double x2 = 1.0 + qbr.x;
    const double y2 = 1.0 + qbr.y;
    const double x3 = qbl.x;
    const double y3 = 1.0 + qbl.y;

    const double dx1 = x1 - x2;
    const double dx2 = x3 - x2;
    const double dy1 = y1 - y2;
    const double dy2 = y3 - y2;
    const double sxq = x0 - x1 + x2 - x3;
    const double syq = y0 - y1 + y2 - y3;
    const double det = dx1 * dy2 - dx2 * dy1;

    double g = 0.0;
    double h = 0.0;
    if (std::abs(det) > 1.0e-10) {
        g = (sxq * dy2 - dx2 * syq) / det;
        h = (dx1 * syq - sxq * dy1) / det;
    }
    const double a = x1 - x0 + g * x1;
    const double b = x3 - x0 + h * x3;
    const double c = x0;
    const double d = y1 - y0 + g * y1;
    const double e = y3 - y0 + h * y3;
    const double f = y0;

    size_t out = 0;
    auto vertex = [&](double u, double v) {
        const double logical_x = origin.x() + u * logical_width;
        const double logical_y = origin.y() + v * logical_height;
        const double nx = (logical_x + origin_x * base_box_width) /
            std::max(0.0001, base_box_width);
        const double ny = (logical_y + origin_y * base_box_height) /
            std::max(0.0001, base_box_height);

        double denom = g * nx + h * ny + 1.0;
        if (std::abs(denom) < 1.0e-8)
            denom = std::copysign(1.0e-8, denom == 0.0 ? 1.0 : denom);
        const double warped_x = (a * nx + b * ny + c) / denom;
        const double warped_y = (d * nx + e * ny + f) / denom;
        const double dx = (warped_x - nx) * base_box_width /
            std::max(0.000001, raster_sx);
        const double dy = (warped_y - ny) * base_box_height /
            std::max(0.000001, raster_sy);

        vec3_set(&data->points[out],
                 static_cast<float>(u * texture_width + dx),
                 static_cast<float>(v * texture_height + dy), 0.0f);
        vec2_set(&uvs[out], static_cast<float>(u), static_cast<float>(v));
        ++out;
    };
    for (int y = 0; y < divisions; ++y) {
        for (int x = 0; x < divisions; ++x) {
            const double u0 = static_cast<double>(x) / divisions;
            const double v0 = static_cast<double>(y) / divisions;
            const double u1 = static_cast<double>(x + 1) / divisions;
            const double v1 = static_cast<double>(y + 1) / divisions;
            vertex(u0,v0); vertex(u1,v0); vertex(u1,v1);
            vertex(u0,v0); vertex(u1,v1); vertex(u0,v1);
        }
    }
    return gs_vertexbuffer_create(data, 0);
}

static bool draw_gpu_layer_texture(TitleGpuRenderSession *session,
                                   TitleGpuRenderSession::LayerRaster *raster_entry,
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
    const double resolved_time = gpu_layer_render_time(title, layer, title_time);
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
        layer.transitions, layer.in_time, layer.out_time, resolved_time);
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
    const double local_time = std::max(0.0, resolved_time - layer.in_time);
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
    gs_vertbuffer_t *transform_grid = nullptr;
    if (layer_has_free_transform_quad(layer, local_time)) {
        const Vec2Value qtl = render_quad_value(layer.transform_quad_tl, layer.transform_quad_tl_x, layer.transform_quad_tl_y, local_time);
        const Vec2Value qtr = render_quad_value(layer.transform_quad_tr, layer.transform_quad_tr_x, layer.transform_quad_tr_y, local_time);
        const Vec2Value qbr = render_quad_value(layer.transform_quad_br, layer.transform_quad_br_x, layer.transform_quad_br_y, local_time);
        const Vec2Value qbl = render_quad_value(layer.transform_quad_bl, layer.transform_quad_bl_x, layer.transform_quad_bl_y, local_time);
        auto q = [](double v) { return std::llround(v * 1000000.0); };
        std::ostringstream key_stream;
        key_stream << texture_width << 'x' << texture_height << '|'
                   << q(logical_width) << '|' << q(logical_height) << '|'
                   << q(origin.x()) << '|' << q(origin.y()) << '|'
                   << q(base_box_width) << '|' << q(base_box_height) << '|'
                   << q(raster_sx) << '|' << q(raster_sy) << '|'
                   << q(eval_origin_x(layer, local_time)) << '|'
                   << q(eval_origin_y(layer, local_time)) << '|'
                   << q(qtl.x) << '|' << q(qtl.y) << '|'
                   << q(qtr.x) << '|' << q(qtr.y) << '|'
                   << q(qbr.x) << '|' << q(qbr.y) << '|'
                   << q(qbl.x) << '|' << q(qbl.y);
        const std::string grid_key = key_stream.str();
        if (raster_entry) {
            if (!raster_entry->transform_grid || raster_entry->transform_grid_key != grid_key) {
                if (raster_entry->transform_grid)
                    gs_vertexbuffer_destroy(raster_entry->transform_grid);
                raster_entry->transform_grid = create_free_transform_grid(
                    layer, texture_width, texture_height, logical_width, logical_height,
                    origin, base_box_width, base_box_height, raster_sx, raster_sy,
                    eval_origin_x(layer, local_time), eval_origin_y(layer, local_time), local_time);
                raster_entry->transform_grid_key = raster_entry->transform_grid ? grid_key : std::string();
            }
            transform_grid = raster_entry->transform_grid;
        } else {
            transform_grid = create_free_transform_grid(
                layer, texture_width, texture_height, logical_width, logical_height,
                origin, base_box_width, base_box_height, raster_sx, raster_sy,
                eval_origin_x(layer, local_time), eval_origin_y(layer, local_time), local_time);
        }
    }
    while (gs_effect_loop(session->copy_effect, "Draw")) {
        if (transform_grid) {
            gs_load_vertexbuffer(transform_grid);
            gs_draw(GS_TRIS, 0, 0);
            gs_load_vertexbuffer(nullptr);
        } else {
            gs_draw_sprite(texture, 0, texture_width, texture_height);
        }
    }
    if (transform_grid && !raster_entry) gs_vertexbuffer_destroy(transform_grid);
    gs_matrix_pop();
    return true;
}

static bool copy_full_canvas_gpu_texture(TitleGpuRenderSession *session,
                                         gs_texture_t *texture,
                                         gs_texrender_t *target);

/* Ordinary artwork effects execute on the transform-neutral, padded layer
 * raster. This preserves effect order and layer-space coordinates; the final
 * raster is transformed only after the whole stack has been evaluated. */

static bool render_gpu_layer_to_target(TitleGpuRenderSession *session,
                                       const Title &title,
                                       const Layer &layer,
                                       double title_time,
                                       gs_texrender_t *target,
                                       bool apply_pixel_effects = true,
                                       const std::string &raster_id = std::string(),
                                       double opacity_override = -1.0,
                                       const std::string &visibility_group_id = std::string())
{
    const double resolved_time = gpu_layer_render_time(title, layer, title_time);

    const std::string resolved_raster_id = raster_id.empty()
        ? gpu_session_layer_id(layer) : raster_id;
    auto found = session->layers.find(resolved_raster_id);
    if (found == session->layers.end() || !found->second.texture)
        return false;
    auto &entry = found->second;
    gs_texture_t *local_texture = apply_pixel_effects
        ? apply_gpu_layer_effect_stack(session, layer, resolved_time,
                                       entry.texture, entry.width, entry.height,
                                       &entry)
        : entry.texture;
    if (!local_texture)
        return false;

    struct vec4 clear;
    vec4_zero(&clear);
    if (!begin_gpu_target(target, session->width, session->height, clear))
        return false;

    const bool compositor_visible = visibility_group_id.empty()
        ? gpu_layer_chain_visible(title, layer, title_time)
        : gpu_layer_visible_in_group_compositor(title, layer, title_time,
                                            visibility_group_id);
    const double current_opacity = compositor_visible
        ? (opacity_override >= 0.0
            ? std::clamp(opacity_override, 0.0, 1.0)
            : std::clamp(gpu_layer_chain_opacity(title, layer, title_time), 0.0, 1.0))
        : 0.0;
    const LayerEffect *motion_config = layer_motion_blur_effect(layer);
    const LayerEffect motion = motion_config
        ? resolve_gpu_layer_effect(*motion_config,
                                   std::max(0.0, resolved_time - layer.in_time))
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
            draw_gpu_layer_texture(session, &entry, title, layer, local_texture, entry.origin,
                                   entry.width, entry.height,
                                   entry.logical_width, entry.logical_height,
                                   entry.image_clip_rect, entry.base_box_width, entry.base_box_height,
                                   title_time, static_cast<float>(current_opacity));
        } else {
            if (mix < 1.0)
                draw_gpu_layer_texture(session, &entry, title, layer, local_texture,
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
                const bool sample_visible = visibility_group_id.empty()
                    ? gpu_layer_chain_visible(title, layer, sample_time)
                    : gpu_layer_visible_in_group_compositor(
                          title, layer, sample_time, visibility_group_id);
                if (sample_visible)
                    visible_samples.push_back(sample_time);
            }
            /* Shutter intervals outside the layer's visibility range are
             * transparent samples. Divide by the requested sample count, not
             * by visible_samples.size(); renormalizing only the visible samples
             * caused the final blur frame to pop to a different opacity. */
            const float sample_weight = static_cast<float>(
                current_opacity * mix / static_cast<double>(samples));
            for (double sample_time : visible_samples) {
                draw_gpu_layer_texture(session, &entry, title, layer, local_texture,
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
        draw_gpu_layer_texture(session, &entry, title, layer, local_texture, entry.origin,
                               entry.width, entry.height, entry.logical_width,
                               entry.logical_height, entry.image_clip_rect,
                               entry.base_box_width, entry.base_box_height, title_time,
                               (float)current_opacity);
    }
    gs_texrender_end(target);
    return true;
}

static bool copy_full_canvas_gpu_texture(TitleGpuRenderSession *session,
                                         gs_texture_t *texture,
                                         gs_texrender_t *target);

static gs_texture_t *apply_gpu_mask(TitleGpuRenderSession *session,
                                    gs_texture_t *layer_texture,
                                    gs_texture_t *mask_texture,
                                    MaskMode mode)
{
    if (!session || !layer_texture || !mask_texture || mode == MaskMode::None)
        return layer_texture;

    /* Never sample a texture while rendering back into the texrender that owns
     * it. Group composition can legitimately leave its complete foreground in
     * masked_target when a child has its own matte. Applying the group target
     * matte directly would then create a GPU read/write feedback loop and the
     * group would appear unmasked or empty depending on the backend. Snapshot
     * any aliased input into an independent full-canvas texture first. */
    gs_texrender_t *layer_snapshot = nullptr;
    gs_texrender_t *mask_snapshot = nullptr;
    gs_texture_t *masked_storage = session->masked_target
        ? gs_texrender_get_texture(session->masked_target) : nullptr;
    if (masked_storage && layer_texture == masked_storage) {
        layer_snapshot = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
        if (!layer_snapshot ||
            !copy_full_canvas_gpu_texture(session, layer_texture, layer_snapshot)) {
            if (layer_snapshot)
                gs_texrender_destroy(layer_snapshot);
            return nullptr;
        }
        layer_texture = gs_texrender_get_texture(layer_snapshot);
    }
    if (masked_storage && mask_texture == masked_storage) {
        mask_snapshot = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
        if (!mask_snapshot ||
            !copy_full_canvas_gpu_texture(session, mask_texture, mask_snapshot)) {
            if (layer_snapshot)
                gs_texrender_destroy(layer_snapshot);
            if (mask_snapshot)
                gs_texrender_destroy(mask_snapshot);
            return nullptr;
        }
        mask_texture = gs_texrender_get_texture(mask_snapshot);
    }

    if (!session->mask_effect)
        session->mask_effect = gs_effect_create(
            kGpuMaskEffect, "obs-bgs-gpu-mask.effect", nullptr);
    if (!session->mask_effect) {
        session->last_error = "Could not compile GPU mask shader.";
        if (layer_snapshot) gs_texrender_destroy(layer_snapshot);
        if (mask_snapshot) gs_texrender_destroy(mask_snapshot);
        return nullptr;
    }
    struct vec4 clear;
    vec4_zero(&clear);
    if (!begin_gpu_target(session->masked_target, session->width, session->height, clear)) {
        if (layer_snapshot) gs_texrender_destroy(layer_snapshot);
        if (mask_snapshot) gs_texrender_destroy(mask_snapshot);
        return nullptr;
    }
    gs_eparam_t *image = gs_effect_get_param_by_name(session->mask_effect, "image");
    gs_eparam_t *mask = gs_effect_get_param_by_name(session->mask_effect, "maskImage");
    gs_eparam_t *mask_mode = gs_effect_get_param_by_name(session->mask_effect, "maskMode");
    if (!image || !mask || !mask_mode) {
        gs_texrender_end(session->masked_target);
        if (layer_snapshot) gs_texrender_destroy(layer_snapshot);
        if (mask_snapshot) gs_texrender_destroy(mask_snapshot);
        return nullptr;
    }
    gs_effect_set_texture(image, layer_texture);
    gs_effect_set_texture(mask, mask_texture);
    gs_effect_set_int(mask_mode, (int)mode);
    gs_enable_blending(false);
    /* Full-canvas mask composition must never inherit the model matrix left by
     * the OBS source, editor presentation, or a preceding transformed layer.
     * The mask shader samples both full-canvas textures in normalized UV space;
     * applying an inherited translate/scale/rotation moves the fullscreen quad
     * away from the target and makes otherwise valid layer masks appear empty. */
    gs_matrix_push();
    gs_matrix_identity();
    while (gs_effect_loop(session->mask_effect, "Draw"))
        gs_draw_sprite(layer_texture, 0, session->width, session->height);
    gs_matrix_pop();
    gs_texrender_end(session->masked_target);
    gs_texture_t *result = gs_texrender_get_texture(session->masked_target);
    if (layer_snapshot) gs_texrender_destroy(layer_snapshot);
    if (mask_snapshot) gs_texrender_destroy(mask_snapshot);
    return result;
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
        /* Publishing a retained matte is also a full-canvas pass. Keep it
         * transform-neutral for the same reason as apply_gpu_mask(). */
        gs_matrix_push();
        gs_matrix_identity();
        while (gs_effect_loop(session->blit_effect, "Draw"))
            gs_draw_sprite(texture, 0, session->width, session->height);
        gs_matrix_pop();
    }
    gs_texrender_end(target);
    return true;
}


enum class ExternalBackgroundMapping {
    Rectangle = 0,
    CurrentTransform = 1,
};

static bool ensure_external_background_snapshot(
    TitleGpuRenderSession *session, uint32_t width, uint32_t height,
    enum gs_color_format format)
{
    if (!session || width == 0 || height == 0 || format == GS_UNKNOWN)
        return false;
    const bool recreate = !session->external_background_snapshot ||
        session->external_background_snapshot_width != width ||
        session->external_background_snapshot_height != height ||
        session->external_background_snapshot_format != format;
    if (!recreate)
        return true;
    if (session->external_background_snapshot)
        gs_texture_destroy(session->external_background_snapshot);
    session->external_background_snapshot = gs_texture_create(
        width, height, format, 1, nullptr, 0);
    if (!session->external_background_snapshot) {
        session->external_background_snapshot_width = 0;
        session->external_background_snapshot_height = 0;
        session->external_background_snapshot_format = GS_UNKNOWN;
        return false;
    }
    session->external_background_snapshot_width = width;
    session->external_background_snapshot_height = height;
    session->external_background_snapshot_format = format;
    return true;
}

static gs_texture_t *render_external_background_local_locked(
    TitleGpuRenderSession *session, gs_texture_t *background,
    uint32_t background_width, uint32_t background_height,
    ExternalBackgroundMapping mapping, const struct matrix4 *local_to_background,
    float rect_x, float rect_y, float rect_width, float rect_height,
    uint32_t logical_width, uint32_t logical_height)
{
    if (!session || !background || background_width == 0 ||
        background_height == 0 || session->width == 0 || session->height == 0)
        return nullptr;
    if (!session->external_background_local)
        session->external_background_local =
            gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    if (!session->external_background_effect)
        session->external_background_effect = gs_effect_create(
            kGpuExternalBackgroundEffect,
            "obs-bgs-external-background.effect", nullptr);
    if (!session->external_background_local ||
        !session->external_background_effect)
        return nullptr;

    struct vec4 clear;
    vec4_zero(&clear);
    if (!begin_gpu_target(session->external_background_local,
                          session->width, session->height, clear))
        return nullptr;

    gs_effect_t *effect = session->external_background_effect;
    gs_eparam_t *background_param =
        gs_effect_get_param_by_name(effect, "background");
    gs_eparam_t *matrix_param =
        gs_effect_get_param_by_name(effect, "localToBackground");
    gs_eparam_t *background_size_param =
        gs_effect_get_param_by_name(effect, "backgroundSize");
    gs_eparam_t *logical_size_param =
        gs_effect_get_param_by_name(effect, "logicalSize");
    gs_eparam_t *rect_param =
        gs_effect_get_param_by_name(effect, "backgroundRect");
    gs_eparam_t *title_background_param =
        gs_effect_get_param_by_name(effect, "titleBackground");
    gs_eparam_t *mapping_param =
        gs_effect_get_param_by_name(effect, "mappingMode");
    if (!background_param || !matrix_param || !background_size_param ||
        !logical_size_param || !rect_param || !title_background_param ||
        !mapping_param) {
        gs_texrender_end(session->external_background_local);
        return nullptr;
    }

    struct matrix4 mapping_matrix;
    if (local_to_background)
        matrix4_copy(&mapping_matrix, local_to_background);
    else
        matrix4_identity(&mapping_matrix);
    struct vec2 background_size;
    vec2_set(&background_size, static_cast<float>(background_width),
             static_cast<float>(background_height));
    struct vec2 logical_size;
    vec2_set(&logical_size, static_cast<float>(std::max(1u, logical_width)),
             static_cast<float>(std::max(1u, logical_height)));
    struct vec4 background_rect;
    vec4_set(&background_rect, rect_x, rect_y, rect_width, rect_height);
    double r, g, b, a;
    unpack_color(session->title.bg_color, r, g, b, a);
    struct vec4 title_background;
    vec4_set(&title_background, static_cast<float>(r * a),
             static_cast<float>(g * a), static_cast<float>(b * a),
             static_cast<float>(a));

    gs_effect_set_texture(background_param, background);
    gs_effect_set_matrix4(matrix_param, &mapping_matrix);
    gs_effect_set_vec2(background_size_param, &background_size);
    gs_effect_set_vec2(logical_size_param, &logical_size);
    gs_effect_set_vec4(rect_param, &background_rect);
    gs_effect_set_vec4(title_background_param, &title_background);
    gs_effect_set_int(mapping_param, static_cast<int>(mapping));
    gs_enable_blending(false);
    while (gs_effect_loop(effect, "Draw"))
        gs_draw_sprite(background, 0, session->width, session->height);
    gs_texrender_end(session->external_background_local);
    return gs_texrender_get_texture(session->external_background_local);
}

