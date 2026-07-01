            /* A group can also be a bounded adjustment layer.  Its composed
             * alpha is the coverage silhouette, while the actual group artwork
             * is still composited normally afterwards. */
            for (const LayerEffect &backdrop_effect : layer.effects) {
                if (!eval_effect_enabled(backdrop_effect,
                                         std::max(0.0, layer_time - layer.in_time)) ||
                    !backdrop_effect.affect_layers_behind)
                    continue;
                Layer adjustment_group = layer;
                adjustment_group.effects.clear();
                adjustment_group.effects.push_back(backdrop_effect);
                gs_texture_t *adjusted_background = apply_gpu_layer_effect_stack(
                    session, adjustment_group, layer_time, frame,
                    width, height, nullptr, true);
                if (!adjusted_background || adjusted_background == frame)
                    continue;
                gs_texrender_t *backdrop_target = current_is_a
                    ? session->frame_b : session->frame_a;
                gs_texture_t *mixed_background = mix_gpu_adjustment_layer(
                    session, session->title, adjustment_group, session->time,
                    frame, adjusted_background, group_silhouette,
                    backdrop_target, true);
                if (mixed_background && mixed_background != frame) {
                    frame = mixed_background;
                    current_is_a = !current_is_a;
                }
            }

            gs_texrender_t *next = current_is_a ? session->frame_b
                                                : session->frame_a;
            const double opacity = gpu_layer_local_opacity(layer, layer_time);
            if (!composite_gpu_frame_layer(session, frame, foreground,
                                           layer.blend_mode, opacity, next)) {
                if (group_silhouette_target)
                    gs_texrender_destroy(group_silhouette_target);
                continue;
            }
            frame = gs_texrender_get_texture(next);
            current_is_a = !current_is_a;
            if (group_silhouette_target)
                gs_texrender_destroy(group_silhouette_target);
            continue;
        }
        if (layer.type == LayerType::Adjustment) {
            /* The editor canvas destination is a presentation background, not
             * artwork input. AE-style opacity and blend modes may sample it,
             * but adjustment effects must not blur, color-correct, distort or
             * otherwise process the checkerboard/editor underlay. OBS source
             * rendering keeps the full destination-aware adjustment behavior. */
            gs_texture_t *coverage = render_gpu_adjustment_coverage(
                session, session->title, layer, session->time);
            if (!coverage)
                continue;
            gs_texture_t *adjusted = apply_gpu_layer_effect_stack(
                session, layer, layer_time, frame, width, height);
            if (adjusted && adjusted != frame) {
                gs_texrender_t *next = current_is_a ? session->frame_b
                                                    : session->frame_a;
                gs_texture_t *mixed = mix_gpu_adjustment_layer(
                    session, session->title, layer, session->time,
                    frame, adjusted, coverage, next);
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
                                        session->time, session->layer_target,
                                        !effects_after_mask, std::string(), 1.0))
            continue;
        gs_texture_t *foreground = gs_texrender_get_texture(session->layer_target);
        if (!foreground)
            continue;

        bool has_backdrop_effect = false;
        for (const LayerEffect &effect : layer.effects) {
            if (effect.enabled && effect.affect_layers_behind) {
                has_backdrop_effect = true;
                break;
            }
        }
        if (has_backdrop_effect) {
            /* Build the bounded-adjustment mask from the untouched artwork,
             * before any normal or backdrop effect can expand/change alpha.
             * The artwork remains visible later in the regular foreground pass. */
            gs_texture_t *silhouette = nullptr;
            if (render_gpu_layer_to_target(session, session->title, layer,
                                           session->time,
                                           session->adjustment_coverage_target,
                                           false, std::string(), 1.0)) {
                silhouette = gs_texrender_get_texture(
                    session->adjustment_coverage_target);
            }
            if (silhouette && has_mask) {
                const Layer *silhouette_mask_layer = find_layer_by_id(
                    session->title, layer.mask_source_id);
                if (silhouette_mask_layer && gpu_layer_chain_active_as_matte(
                        session->title, *silhouette_mask_layer,
                        session->time)) {
                    silhouette = apply_gpu_track_matte_texture(
                        session, session->title, silhouette,
                        *silhouette_mask_layer, session->time, std::string(),
                        layer.parent_id, layer.mask_mode);
                } else if (!mask_mode_is_inverted(layer.mask_mode)) {
                    silhouette = nullptr;
                }
            }

            if (silhouette) {
                /* Evaluate backdrop effects independently. Besides preserving
                 * stack order, this lets every effect own its mask inversion. */
                for (const LayerEffect &backdrop_effect : layer.effects) {
                    if (!eval_effect_enabled(backdrop_effect,
                                             std::max(0.0, layer_time - layer.in_time)) ||
                        !backdrop_effect.affect_layers_behind)
                        continue;
                    Layer adjustment_layer = layer;
                    adjustment_layer.effects.clear();
                    adjustment_layer.effects.push_back(backdrop_effect);
                    gs_texture_t *adjusted_background = apply_gpu_layer_effect_stack(
                        session, adjustment_layer, layer_time, frame,
                        width, height, nullptr, true);
                    if (!adjusted_background || adjusted_background == frame)
                        continue;
                    gs_texrender_t *backdrop_target = current_is_a
                        ? session->frame_b : session->frame_a;
                    gs_texture_t *mixed_background = mix_gpu_adjustment_layer(
                        session, session->title, adjustment_layer, session->time,
                        frame, adjusted_background, silhouette,
                        backdrop_target, true);
                    if (mixed_background && mixed_background != frame) {
                        frame = mixed_background;
                        current_is_a = !current_is_a;
                    }
                }
            }
        }

        /* Building a bounded backdrop silhouette may evaluate the same track
         * matte graph before the visible foreground is masked. A Group matte
         * recursively reuses shared layer/effect targets, so restore the
         * consumer foreground after the backdrop pass instead of trusting a
         * texture pointer whose storage may have been repurposed. */
        if (has_backdrop_effect) {
            if (!render_gpu_layer_to_target(
                    session, session->title, layer, session->time,
                    session->layer_target, !effects_after_mask,
                    std::string(), 1.0))
                continue;
            foreground = gs_texrender_get_texture(session->layer_target);
            if (!foreground)
                continue;
        }

        if (has_mask) {
            const Layer *mask_layer = find_layer_by_id(session->title,
                                                       layer.mask_source_id);
            if (mask_layer &&
                gpu_layer_chain_active_as_matte(session->title, *mask_layer, session->time)) {
                foreground = apply_gpu_track_matte_texture(
                    session, session->title, foreground, *mask_layer,
                    session->time, std::string(), layer.parent_id,
                    layer.mask_mode);
                if (!foreground)
                    continue;
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
        const double composite_opacity = std::clamp(
            gpu_layer_chain_opacity(session->title, layer, session->time), 0.0, 1.0);
        if (!composite_gpu_frame_layer(session, frame, foreground,
                                       layer.blend_mode, composite_opacity, next))
            continue;
        frame = gs_texrender_get_texture(next);
        current_is_a = !current_is_a;
    }

    if (destination_composite) {
        if (isolate_editor_background_from_effects) {
            gs_texrender_t *next = current_is_a ? session->frame_b : session->frame_a;
            if (composite_gpu_frame_layer(session, external_background, frame,
                                          EffectBlendMode::Normal, 1.0, next))
                frame = gs_texrender_get_texture(next);
        }
        prune_gpu_mask_texture_cache(session);
        session->last_error = nullptr;
        return frame;
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

static bool stage_gpu_texture_locked(TitleGpuRenderSession *session,
                                     gs_texture_t *texture,
                                     QImage &result)
{
    result = QImage();
    if (!session || !texture || session->width == 0 || session->height == 0)
        return false;
    if (!session->stage || session->stage_width != session->width ||
        session->stage_height != session->height) {
        if (session->stage)
            gs_stagesurface_destroy(session->stage);
        session->stage = gs_stagesurface_create(session->width,
                                                session->height,
                                                GS_BGRA);
        session->stage_width = session->stage ? session->width : 0;
        session->stage_height = session->stage ? session->height : 0;
    }
    if (!session->stage)
        return false;

    gs_stage_texture(session->stage, texture);
    uint8_t *mapped = nullptr;
    uint32_t linesize = 0;
    if (!gs_stagesurface_map(session->stage, &mapped, &linesize) ||
        !mapped || linesize < session->width * 4)
        return false;

    QImage image(static_cast<int>(session->width),
                 static_cast<int>(session->height),
                 QImage::Format_ARGB32_Premultiplied);
    if (!image.isNull()) {
        for (uint32_t y = 0; y < session->height; ++y) {
            std::memcpy(image.scanLine(static_cast<int>(y)),
                        mapped + static_cast<size_t>(y) * linesize,
                        static_cast<size_t>(session->width) * 4);
        }
    }
    gs_stagesurface_unmap(session->stage);
    if (image.isNull())
        return false;
    result = std::move(image);
    return true;
}

static QImage make_ae_checkerboard(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return QImage();
    QImage image(static_cast<int>(width), static_cast<int>(height),
                 QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return image;
    const int square = 24;
    const QRgb dark = qRgba(58, 58, 58, 255);
    const QRgb light = qRgba(82, 82, 82, 255);
    for (int y = 0; y < image.height(); ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        const int tile_y = y / square;
        for (int x = 0; x < image.width(); ++x)
            line[x] = (((x / square) + tile_y) & 1) ? light : dark;
    }
    return image;
}

static QImage title_gpu_render_session_readback_over_checkerboard(
    TitleGpuRenderSession *session)
{
    if (!session || session->destroying.load())
        return QImage();

    QImage result;
    obs_enter_graphics();
    std::unique_lock<std::mutex> lock(session->mutex);
    if (session->destroying.load() ||
        !prepare_external_composite_dimensions_locked(session)) {
        lock.unlock();
        obs_leave_graphics();
        return QImage();
    }

    /* Render the title artwork against transparent black. The checkerboard is
     * editor chrome, not an adjustment-layer input: blur/distort/color effects
     * must sample only actual title pixels. Passing a destination texture here
     * previously made the checkerboard part of the GPU frame and produced
     * incorrect adjustment blur semantics. */
    gs_texture_t *texture = render_gpu_session_locked(
        session, nullptr, true);
    if (texture)
        stage_gpu_texture_locked(session, texture, result);
    lock.unlock();
    obs_leave_graphics();

    if (result.isNull())
        return result;

    /* Composite transparency chrome only after every artwork/effect pass. This
     * keeps the checkerboard sharp while retaining effect-generated alpha such
     * as blur tails, shadows and glows outside layer bounds. */
    QImage background = make_ae_checkerboard(
        static_cast<uint32_t>(result.width()),
        static_cast<uint32_t>(result.height()));
    if (background.isNull())
        return result;
    QPainter painter(&background);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawImage(0, 0, result);
    painter.end();
    return background;
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
    /* Editor presentation and OBS source drawing execute under the graphics
     * lock and then take session->mutex. Taking the mutex first here creates
     * the inverse order (session -> graphics) and can deadlock shutdown against
     * an in-flight draw (graphics -> session). Mark destruction first, wait for
     * graphics presentation to drain, and only then take the session mutex. */
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
        if (pair.second.transform_grid)
            gs_vertexbuffer_destroy(pair.second.transform_grid);
        pair.second.transform_grid = nullptr;
        pair.second.transform_grid_key.clear();
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
    destroy_target(session->adjustment_coverage_target);
    destroy_target(session->effect_a);
    destroy_target(session->effect_b);
    destroy_target(session->blur_a);
    destroy_target(session->blur_b);
    destroy_target(session->external_background_local);
    if (session->external_background_snapshot)
        gs_texture_destroy(session->external_background_snapshot);
    session->external_background_snapshot = nullptr;
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
    if (session->adjustment_coverage_effect)
        gs_effect_destroy(session->adjustment_coverage_effect);
    if (session->primitive_shape_effect)
        gs_effect_destroy(session->primitive_shape_effect);
    if (session->blend_effect)
        gs_effect_destroy(session->blend_effect);
    if (session->mask_effect)
        gs_effect_destroy(session->mask_effect);
    if (session->external_background_effect)
        gs_effect_destroy(session->external_background_effect);
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
                    snapshot_layer.transform_parent_id = source_layer.transform_parent_id;
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
        if (layer->type != LayerType::Adjustment &&
            !layer_type_is_container(layer->type))
            needed_ids.insert(gpu_session_layer_id(*layer));
        if (layer->mask_mode != MaskMode::None &&
            !layer->mask_source_id.empty())
            needed_ids.insert(layer->mask_source_id);
        if (layer_type_is_container(layer->type)) {
            std::vector<std::string> group_ids{layer->id};
            for (std::size_t cursor = 0; cursor < group_ids.size(); ++cursor) {
                for (const auto &candidate : session->title.layers) {
                    if (!candidate || candidate->parent_id != group_ids[cursor])
                        continue;
                    if (layer_type_is_container(candidate->type))
                        group_ids.push_back(candidate->id);
                    else if (candidate->type != LayerType::Adjustment)
                        needed_ids.insert(gpu_session_layer_id(*candidate));
                    if (candidate->mask_mode != MaskMode::None &&
                        !candidate->mask_source_id.empty())
                        needed_ids.insert(candidate->mask_source_id);
                }
            }
        }
    }
    /* Expand the complete render dependency graph, not just direct matte IDs.
     * A Group has no raster of its own: when it is referenced only as a track
     * matte, all descendant artwork (including nested Groups and their mattes)
     * must still be rasterized. Previously only the Group ID was added here,
     * so render_gpu_group_graph_texture() found no child textures and produced
     * an empty matte. */
    bool dependency_added = true;
    while (dependency_added) {
        dependency_added = false;
        std::vector<std::string> snapshot(needed_ids.begin(), needed_ids.end());
        for (const std::string &id : snapshot) {
            const Layer *dependency = find_layer_by_id(session->title, id);
            if (!dependency)
                continue;

            if (layer_type_is_container(dependency->type)) {
                for (const auto &candidate : session->title.layers) {
                    if (!candidate || candidate->parent_id != dependency->id)
                        continue;
                    const std::string child_id = layer_type_is_container(candidate->type)
                        ? candidate->id : gpu_session_layer_id(*candidate);
                    if (candidate->type != LayerType::Adjustment)
                        dependency_added = needed_ids.insert(child_id).second ||
                                           dependency_added;
                    if (candidate->mask_mode != MaskMode::None &&
                        !candidate->mask_source_id.empty())
                        dependency_added = needed_ids.insert(
                            candidate->mask_source_id).second || dependency_added;
                }
            }

            if (dependency->mask_mode != MaskMode::None &&
                !dependency->mask_source_id.empty())
                dependency_added = needed_ids.insert(
                    dependency->mask_source_id).second || dependency_added;
        }
    }

    bool raster_changed = false;
    std::unordered_set<std::string> live_ids;
    live_ids.reserve(needed_ids.size());
    const qint64 clock_second = QDateTime::currentSecsSinceEpoch();

    for (const auto &layer_ptr : session->title.layers) {
        if (!layer_ptr)
            continue;
        const Layer &source_layer = *layer_ptr;
        Layer runtime_layer;
        const Layer *runtime_layer_ptr = &source_layer;
        const bool has_text_transition_descriptor = std::any_of(
            source_layer.transitions.begin(), source_layer.transitions.end(),
            [](const LayerTransition &transition) {
                return layer_transition_type_is_text(transition.type);
            });
        if (has_text_transition_descriptor &&
            (source_layer.type == LayerType::Text ||
             source_layer.type == LayerType::Clock ||
             source_layer.type == LayerType::Ticker)) {
            runtime_layer = source_layer;
            runtime_layer.text_animators =
                resolved_text_transition_animator_stack(
                    source_layer.transitions, source_layer.text_animators,
                    source_layer.in_time, source_layer.out_time);
            runtime_layer_ptr = &runtime_layer;
        }
        const Layer &layer = *runtime_layer_ptr;
        const std::string id = gpu_session_layer_id(layer);
        if (needed_ids.find(id) == needed_ids.end())
            continue;
        live_ids.insert(id);
        const double layer_time = gpu_layer_render_time(
            session->title, layer, session->time);
        Layer base_key_layer = layer;
        base_key_layer.effects.clear();
        const double base_local_time = std::max(0.0, layer_time - layer.in_time);
        const bool preserve_effect_surface =
            layer_requires_preserved_effect_surface(layer, base_local_time);
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
            std::to_string(base_surface_rect.height()) +
            "|gpu-effect-surface=" +
            (preserve_effect_surface ? std::string("preserved")
                                     : std::string("cropped"));
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
            text_animator_stack_is_time_dependent(layer.text_animators);
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
            if (it->second.transform_grid)
                gs_vertexbuffer_destroy(it->second.transform_grid);
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

bool title_gpu_frame_cache_alias(
    const std::string &cache_key,
    const std::string &canonical_cache_key)
{
    if (cache_key.empty() || canonical_cache_key.empty())
        return false;
    obs_enter_graphics();
    bool aliased = false;
    {
        std::lock_guard<std::mutex> lock(g_gpu_frame_cache_mutex);
        aliased = alias_global_gpu_frame_locked(
            cache_key, canonical_cache_key);
    }
    obs_leave_graphics();
    return aliased;
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

