    for (const auto &layer : title.layers) {
        if (!layer) continue;
        add(layer->visible && time >= layer->in_time && time <= layer->out_time);
        add_vec(layer->position, 0.25);
        add_vec(layer->scale, 0.0005);
        add_anim(layer->rotation, 0.05);
        add(quant(layer->transform_quad_tl_x, 0.0005)); add(quant(layer->transform_quad_tl_y, 0.0005));
        add(quant(layer->transform_quad_tr_x, 0.0005)); add(quant(layer->transform_quad_tr_y, 0.0005));
        add(quant(layer->transform_quad_br_x, 0.0005)); add(quant(layer->transform_quad_br_y, 0.0005));
        add(quant(layer->transform_quad_bl_x, 0.0005)); add(quant(layer->transform_quad_bl_y, 0.0005));
        add_anim(layer->opacity, 0.002);
        add_vec(layer->size, 0.25);
        add_vec(layer->image_size, 0.25);
        add_vec(layer->origin_prop, 0.25);
        add_anim(layer->font_size_prop, 0.10);
        add_anim(layer->char_tracking_prop, 0.10);
        add_anim(layer->char_scale_x_prop, 0.0005);
        add_anim(layer->char_scale_y_prop, 0.0005);
        add_anim(layer->baseline_shift_prop, 0.10);
        add_anim(layer->text_color_a, 1.0 / 255.0); add_anim(layer->text_color_r, 1.0 / 255.0);
        add_anim(layer->text_color_g, 1.0 / 255.0); add_anim(layer->text_color_b, 1.0 / 255.0);
        add_anim(layer->paragraph_indent_left_prop, 0.25);
        add_anim(layer->paragraph_indent_right_prop, 0.25);
        add_anim(layer->paragraph_indent_first_line_prop, 0.25);
        add_anim(layer->paragraph_space_before_prop, 0.25);
        add_anim(layer->paragraph_space_after_prop, 0.25);
        add_anim(layer->fill_color_a, 1.0 / 255.0); add_anim(layer->fill_color_r, 1.0 / 255.0);
        add_anim(layer->fill_color_g, 1.0 / 255.0); add_anim(layer->fill_color_b, 1.0 / 255.0);
        add_anim(layer->background_enabled_prop, 1.0);
        add_anim(layer->background_opacity_prop, 0.002);
        add_anim(layer->background_padding_x_prop, 0.25);
        add_anim(layer->background_padding_y_prop, 0.25);
        add_anim(layer->background_padding_left_prop, 0.25);
        add_anim(layer->background_padding_right_prop, 0.25);
        add_anim(layer->background_padding_top_prop, 0.25);
        add_anim(layer->background_padding_bottom_prop, 0.25);
        add_anim(layer->background_corner_radius_prop, 0.25);
        add_anim(layer->background_corner_radius_tl_prop, 0.25);
        add_anim(layer->background_corner_radius_tr_prop, 0.25);
        add_anim(layer->background_corner_radius_br_prop, 0.25);
        add_anim(layer->background_corner_radius_bl_prop, 0.25);
        add_anim(layer->background_stroke_width_prop, 0.25);
        add_anim(layer->background_stroke_opacity_prop, 0.002);
        add_anim(layer->background_color_a, 1.0 / 255.0);
        add_anim(layer->background_color_r, 1.0 / 255.0);
        add_anim(layer->background_color_g, 1.0 / 255.0);
        add_anim(layer->background_color_b, 1.0 / 255.0);
        add_anim(layer->background_stroke_color_a, 1.0 / 255.0);
        add_anim(layer->background_stroke_color_r, 1.0 / 255.0);
        add_anim(layer->background_stroke_color_g, 1.0 / 255.0);
        add_anim(layer->background_stroke_color_b, 1.0 / 255.0);
        add_anim(layer->shadow_enabled_prop, 1.0);
        add_anim(layer->shadow_opacity_prop, 0.002);
        add_anim(layer->shadow_distance_prop, 0.25);
        add_anim(layer->shadow_angle_prop, 0.05);
        add_anim(layer->shadow_blur_prop, 0.25);
        add_anim(layer->shadow_spread_prop, 0.25);
        add_anim(layer->shadow_color_a, 1.0 / 255.0);
        add_anim(layer->shadow_color_r, 1.0 / 255.0);
        add_anim(layer->shadow_color_g, 1.0 / 255.0);
        add_anim(layer->shadow_color_b, 1.0 / 255.0);
        for (const auto &effect : layer->effects) add_effect(effect);
        for (const auto &transition : layer->transitions) {
            if (!transition.enabled)
                continue;
            add(quant(layer_transition_progress(transition, layer->in_time, layer->out_time, time), 0.002));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString CacheManager::temporalStateKey(const CacheFrameKey &key,
                                       const QString &visual_state_hash) const
{
    return QStringLiteral("%1|%2|%3x%4|%5")
        .arg(key.title_id, key.content_hash)
        .arg(key.width).arg(key.height)
        .arg(visual_state_hash);
}

CacheFrameKey CacheManager::keyForFrame(const Title &title, int frame) const
{
    const int clamped_frame = titleHasTimelineChanges(title)
        ? std::clamp(frame, 0, cache_last_frame_for_title(title, effectiveFrameRate()))
        : 0;
    return {QString::fromStdString(title.id), contentHash(title), clamped_frame, title.width, title.height};
}

CacheFrameKey CacheManager::keyForTime(const Title &title, double time) const
{
    return keyForTime(title, time, QString());
}

CacheFrameKey CacheManager::keyForTime(const Title &title, double time,
                                        const QString &known_content_hash) const
{
    const int clamped_frame = frameIndexForTitleTime(title, time);
    return {QString::fromStdString(title.id),
            known_content_hash.isEmpty() ? contentHash(title) : known_content_hash,
            clamped_frame, title.width, title.height};
}

int CacheManager::frameIndexForTitleTime(const Title &title, double time) const
{
    const double clamped_time = std::clamp(
        time, 0.0, std::max(0.0, title.duration));
    const double frame_rate = effectiveFrameRate();
    return cache_frame_index_for_title_time(
        clamped_time, frame_rate, titleHasTimelineChanges(title),
        cache_last_frame_for_title(title, frame_rate));
}

QVector<CacheFrameKey> CacheManager::frameKeysForTitle(const Title &title) const
{
    const bool animated = titleHasTimelineChanges(title);
    const int last_frame = animated ? cache_last_frame_for_title(title, effectiveFrameRate()) : 0;
    const QString title_id = QString::fromStdString(title.id);
    const QString content_hash = contentHash(title);
    QVector<CacheFrameKey> keys;
    keys.reserve(last_frame + 1);
    for (int frame = 0; frame <= last_frame; ++frame)
        keys.push_back({title_id, content_hash, frame, title.width, title.height});
    return keys;
}

bool CacheManager::titleHasTimelineChanges(const Title &title) const
{
    for (const auto &layer : title.layers) {
        if (!layer) continue;
        if (layer->type == LayerType::Clock || layer->type == LayerType::Ticker ||
            (layer->type == LayerType::Asset && layer->asset_animated &&
             layer->asset_playback_mode == 1))
            return true;
        if (std::any_of(layer->transitions.begin(), layer->transitions.end(),
                        [](const LayerTransition &transition) { return transition.enabled; }))
            return true;
        auto animated = [](const auto &prop) { return !prop.keyframes.empty(); };
        if (animated(layer->position) || animated(layer->scale) || animated(layer->rotation) ||
            animated(layer->opacity) || animated(layer->size) || animated(layer->image_size) || animated(layer->origin_prop) ||
            animated(layer->font_size_prop) || animated(layer->char_tracking_prop) ||
            animated(layer->char_scale_x_prop) || animated(layer->char_scale_y_prop) ||
            animated(layer->baseline_shift_prop) || animated(layer->paragraph_indent_left_prop) ||
            animated(layer->paragraph_indent_right_prop) || animated(layer->paragraph_indent_first_line_prop) ||
            animated(layer->paragraph_space_before_prop) || animated(layer->paragraph_space_after_prop) ||
            animated(layer->text_color_a) || animated(layer->text_color_r) ||
            animated(layer->text_color_g) || animated(layer->text_color_b) ||
            animated(layer->fill_color_a) || animated(layer->fill_color_r) ||
            animated(layer->fill_color_g) || animated(layer->fill_color_b) ||
            animated(layer->background_enabled_prop) || animated(layer->background_opacity_prop) ||
            animated(layer->background_padding_x_prop) || animated(layer->background_padding_y_prop) ||
            animated(layer->background_padding_left_prop) || animated(layer->background_padding_right_prop) ||
            animated(layer->background_padding_top_prop) || animated(layer->background_padding_bottom_prop) ||
            animated(layer->background_corner_radius_prop) || animated(layer->background_corner_radius_tl_prop) ||
            animated(layer->background_corner_radius_tr_prop) || animated(layer->background_corner_radius_br_prop) ||
            animated(layer->background_corner_radius_bl_prop) || animated(layer->background_stroke_width_prop) ||
            animated(layer->background_stroke_opacity_prop) || animated(layer->background_color_a) ||
            animated(layer->background_color_r) || animated(layer->background_color_g) ||
            animated(layer->background_color_b) || animated(layer->background_stroke_color_a) ||
            animated(layer->background_stroke_color_r) || animated(layer->background_stroke_color_g) ||
            animated(layer->background_stroke_color_b) || animated(layer->shadow_enabled_prop) ||
            animated(layer->shadow_opacity_prop) || animated(layer->shadow_distance_prop) ||
            animated(layer->shadow_angle_prop) || animated(layer->shadow_blur_prop) ||
            animated(layer->shadow_spread_prop) || animated(layer->shadow_color_a) ||
            animated(layer->shadow_color_r) || animated(layer->shadow_color_g) ||
            animated(layer->shadow_color_b))
            return true;
        for (const auto &effect : layer->effects) {
            if (animated(effect.enabled_prop) || animated(effect.opacity_prop) || animated(effect.size_prop) ||
                animated(effect.distance_prop) || animated(effect.angle_prop) || animated(effect.spread_prop) ||
                animated(effect.falloff_prop) || animated(effect.stroke_width_prop) ||
                animated(effect.stroke_opacity_prop) || animated(effect.padding_left_prop) ||
                animated(effect.padding_right_prop) || animated(effect.padding_top_prop) ||
                animated(effect.padding_bottom_prop) || animated(effect.corner_radius_tl_prop) ||
                animated(effect.corner_radius_tr_prop) || animated(effect.corner_radius_br_prop) ||
                animated(effect.corner_radius_bl_prop) || animated(effect.color_a) ||
                animated(effect.color_r) || animated(effect.color_g) || animated(effect.color_b) ||
                animated(effect.stroke_color_a) || animated(effect.stroke_color_r) ||
                animated(effect.stroke_color_g) || animated(effect.stroke_color_b) ||
                animated(effect.amount_prop) || animated(effect.scale_prop) ||
                animated(effect.softness_prop) || animated(effect.roundness_prop) ||
                animated(effect.speed_prop) || animated(effect.center_x_prop) ||
                animated(effect.center_y_prop) || animated(effect.complexity_prop) ||
                animated(effect.evolution_prop) || animated(effect.secondary_color_a) ||
                animated(effect.secondary_color_r) || animated(effect.secondary_color_g) ||
                animated(effect.secondary_color_b) ||
                (effect.type == LayerEffectType::Noise && effect.effect_animated && effect.enabled))
                return true;
        }
        if (layer->in_time > 0.0 || layer->out_time < title.duration)
            return true;
    }
    return false;
}

QImage CacheManager::requestFrame(const std::shared_ptr<Title> &title, double time, bool cached_only)
{
    if (!title) {
        log_cache_playback(QStringLiteral("request kind=editor result=null-title time=%1 cachedOnly=%2")
            .arg(time, 0, 'f', 6).arg(cached_only));
        return QImage();
    }

    const TitleCacheability cacheability = titleCacheability(title);
    if (cacheability == TitleCacheability::NonCacheable ||
        interactive_bypass_.load() || !cache_enabled_.load()) {
        log_cache_playback(QStringLiteral("request kind=editor result=bypass title=%1 time=%2 cachedOnly=%3 cacheability=%4 enabled=%5 interactive=%6")
            .arg(QString::fromStdString(title->id)).arg(time, 0, 'f', 6).arg(cached_only)
            .arg(static_cast<int>(cacheability)).arg(cache_enabled_.load()).arg(interactive_bypass_.load()));
        return QImage();
    }

    const double clamped_time = std::clamp(
        time, 0.0, std::max(0.0, title->duration));
    const auto finish_cached_frame = [&](const QImage &cached_frame) {
        return cached_frame;
    };

    const QString content_hash = contentHash(*title);
    const CacheFrameKey key = keyForTime(*title, time, content_hash);
    const FrameCacheState initial_state = state_tracker_.state(key);
    log_cache_playback(QStringLiteral("request kind=editor title=%1 time=%2 clamped=%3 frame=%4 state=%5 cachedOnly=%6 key=%7")
        .arg(key.title_id).arg(time, 0, 'f', 6).arg(clamped_time, 0, 'f', 6).arg(key.frame)
        .arg(QString::fromUtf8(cache_playback_state_name(initial_state))).arg(cached_only).arg(key.toString()));
    QImage image;
    if (initial_state != FrameCacheState::Stale && ram_cache_.get(key, image)) {
        state_tracker_.setState(key, FrameCacheState::CachedRam);
        log_cache_playback(QStringLiteral("result=ram-hit kind=editor title=%1 frame=%2 cacheKey=%3 size=%4x%5 key=%6")
            .arg(key.title_id).arg(key.frame).arg(image.cacheKey()).arg(image.width()).arg(image.height()).arg(key.toString()));
        return finish_cached_frame(image);
    }

    if (initial_state != FrameCacheState::Stale && disk_cache_.get(key, image)) {
        ram_cache_.put(key, image);
        state_tracker_.setState(key, FrameCacheState::CachedRam);
        emit diagnosticsChanged();
        log_cache_playback(QStringLiteral("result=disk-hit-promoted kind=editor title=%1 frame=%2 cacheKey=%3 size=%4x%5 key=%6")
            .arg(key.title_id).arg(key.frame).arg(image.cacheKey()).arg(image.width()).arg(image.height()).arg(key.toString()));
        return finish_cached_frame(image);
    }

    queueRealtimeJob(title, clamped_time, false, -1, QString(), content_hash);
    log_cache_playback(QStringLiteral("result=miss-queued kind=editor title=%1 frame=%2 initialState=%3 cachedOnly=%4 key=%5")
        .arg(key.title_id).arg(key.frame).arg(QString::fromUtf8(cache_playback_state_name(initial_state)))
        .arg(cached_only).arg(key.toString()));
    return QImage();
}

QString CacheManager::requestFrameGpuToken(
    const std::shared_ptr<Title> &title, double time,
    bool queue_if_missing, const QString &known_content_hash)
{
    if (!title || !cache_enabled_.load() || interactive_bypass_.load() ||
        titleCacheability(title) == TitleCacheability::NonCacheable)
        return QString();

    const double clamped_time = std::clamp(
        time, 0.0, std::max(0.0, title->duration));
    const CacheFrameKey key = keyForTime(
        *title, clamped_time, known_content_hash);
    const FrameCacheState state = state_tracker_.state(key);
    if (state != FrameCacheState::Stale &&
        title_gpu_frame_cache_contains(key.toString().toStdString())) {
        state_tracker_.setState(key, FrameCacheState::CachedRam);
        return key.toString();
    }

    if (queue_if_missing) {
        queueRealtimeJob(title, clamped_time, false, -1, QString(),
                         known_content_hash);
    }
    return QString();
}

quint64 CacheManager::ramBytesUsed() const
{
    return static_cast<quint64>(title_gpu_frame_cache_bytes_used()) +
           ram_cache_.bytesUsed();
}

void CacheManager::queueRealtimeJob(const std::shared_ptr<Title> &title, double time,
                                    bool live_cue, int cue_row, const QString &cue_state_key,
                                    const QString &known_content_hash)
{
    if (!title || !cache_enabled_.load() || interactive_bypass_.load() ||
        titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    const CacheFrameKey key = keyForTime(*title, time, known_content_hash);
    const bool force_render = state_tracker_.state(key) == FrameCacheState::Stale;
    RenderQueueManager::Job job;
    job.key = key;
    job.title = snapshotForJob(title, key);
    job.time = timeForFrame(key.frame);
    job.priority = live_cue ? static_cast<int>(RenderQueueManager::PriorityBand::LiveCue) * 100000
                            : static_cast<int>(RenderQueueManager::PriorityBand::Visible) * 100000;
    job.live_cue = live_cue;
    job.realtime = true;
    job.urgent = live_cue;
    job.force_render = force_render;
    job.cue_row = cue_row;
    job.cue_state_key = cue_state_key;
    job.cache_epoch = cache_epoch_.load();
    job.title_generation = titleGeneration(key.title_id);
    if (queue_.enqueue(job)) {
        state_tracker_.setState(key, FrameCacheState::Queued);
        wakeWorker();
    }
}

void CacheManager::rejectFramePayload(const std::shared_ptr<Title> &title,
                                      double time,
                                      const QString &known_content_hash)
{
    if (!title)
        return;

    const CacheFrameKey key = keyForTime(*title, time, known_content_hash);
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        ram_cache_.remove(key);
        title_gpu_frame_cache_remove(key.toString().toStdString());
        disk_cache_.remove(key);
        state_tracker_.setState(key, FrameCacheState::Stale);
    }
    BGL_LOG_WARNING("CachePlayback", QStringLiteral(
        "Rejected invalid cache payload title=%1 frame=%2 key=%3")
        .arg(key.title_id).arg(key.frame).arg(key.toString()));
    queueRealtimeJob(title, time, false, -1, QString(), known_content_hash);
    emit diagnosticsChanged();
}

QImage CacheManager::requestFrameRealtime(const std::shared_ptr<Title> &title, double time,
                                              const QString &known_content_hash)
{
    if (!title || !cache_enabled_.load() || interactive_bypass_.load() ||
        titleCacheability(title) == TitleCacheability::NonCacheable) {
        log_cache_playback(QStringLiteral("request kind=standard result=bypass title=%1 time=%2 enabled=%3 interactive=%4")
            .arg(title ? QString::fromStdString(title->id) : QStringLiteral("<null>"))
            .arg(time, 0, 'f', 6).arg(cache_enabled_.load()).arg(interactive_bypass_.load()));
        return QImage();
    }
    const CacheFrameKey key = keyForTime(*title, time, known_content_hash);
    const FrameCacheState initial_state = state_tracker_.state(key);
    log_cache_playback(QStringLiteral("request kind=standard title=%1 time=%2 frame=%3 state=%4 key=%5")
        .arg(key.title_id).arg(time, 0, 'f', 6).arg(key.frame)
        .arg(QString::fromUtf8(cache_playback_state_name(initial_state)), key.toString()));
    if (initial_state == FrameCacheState::Stale) {
        queueRealtimeJob(title, time, false, -1, QString(), known_content_hash);
        log_cache_playback(QStringLiteral("result=stale-queued title=%1 frame=%2 key=%3")
            .arg(key.title_id).arg(key.frame).arg(key.toString()));
        return QImage();
    }
    QImage image;
    if (ram_cache_.get(key, image)) {
        state_tracker_.setState(key, FrameCacheState::CachedRam);
        log_cache_playback(QStringLiteral("result=ram-hit title=%1 frame=%2 cacheKey=%3 size=%4x%5 key=%6")
            .arg(key.title_id).arg(key.frame).arg(image.cacheKey()).arg(image.width()).arg(image.height()).arg(key.toString()));
        return image;
    }

    if (initial_state == FrameCacheState::CachedDisk && disk_cache_.get(key, image)) {
        ram_cache_.put(key, image);
        state_tracker_.setState(key, FrameCacheState::CachedRam);
        log_cache_playback(QStringLiteral("result=disk-hit-promoted title=%1 frame=%2 cacheKey=%3 size=%4x%5 key=%6")
            .arg(key.title_id).arg(key.frame).arg(image.cacheKey()).arg(image.width()).arg(image.height()).arg(key.toString()));
        return image;
    }

    queueRealtimeJob(title, time, false, -1, QString(), known_content_hash);
    log_cache_playback(QStringLiteral("result=miss-queued title=%1 frame=%2 initialState=%3 diskAttempt=%4 key=%5")
        .arg(key.title_id).arg(key.frame)
        .arg(QString::fromUtf8(cache_playback_state_name(initial_state)))
        .arg(initial_state == FrameCacheState::CachedDisk).arg(key.toString()));
    return QImage();
}

void CacheManager::restoreDiskStates(const std::shared_ptr<Title> &title)
{
    if (!title || !cache_enabled_.load())
        return;
    if (titleCacheability(title) == TitleCacheability::NonCacheable) {
        /* Clock/ticker titles are dynamic by definition.  Remove stale cache
         * artifacts from older versions instead of publishing them as valid
         * disk states during session restore. */
        removeTitleCache(QString::fromStdString(title->id), true);
        return;
    }
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    const QString title_id = QString::fromStdString(title->id);
    const QString current_hash = contentHash(*title);
    const auto keys = disk_cache_.keysForTitle(title_id);
    for (const CacheFrameKey &key : keys) {
        if (key.content_hash == current_hash && key.width == title->width && key.height == title->height)
            state_tracker_.setState(key, FrameCacheState::CachedDisk);
    }

    ensure_live_text_row_ids(*title);
    for (int row = 0; row < (int)title->live_text_rows.size(); ++row) {
        const QString state_key = liveCueStateKey(title, row);
        const QString row_id = liveCueRowIdentity(title, row);
        QVector<CacheFrameKey> required_keys;
        QSet<CacheFrameKey> seen;
        for (const auto &variant : liveCueVariants(title, row)) {
            if (!variant)
                continue;
            for (const CacheFrameKey &frame_key : frameKeysForTitle(*variant)) {
                if (seen.contains(frame_key))
                    continue;
                seen.insert(frame_key);
                required_keys.push_back(frame_key);
                if (disk_cache_.contains(frame_key))
                    state_tracker_.setState(frame_key, FrameCacheState::CachedDisk);
            }
        }

        if (required_keys.isEmpty())
            continue;

        live_cue_required_keys_[state_key] = required_keys;
        live_cue_required_total_[state_key] = static_cast<int>(required_keys.size());
        live_cue_rows_[state_key] = row;
        live_cue_row_ids_[state_key] = row_id;
        live_cue_title_ids_[state_key] = title_id;
        live_cue_transition_state_keys_.remove(state_key);

        const FrameCacheState restored_state = liveCueStoredState(state_key);
        if (restored_state == FrameCacheState::CachedRam ||
            restored_state == FrameCacheState::CachedDisk) {
            live_cue_states_[state_key] = restored_state;
            live_cue_progress_percent_[state_key] = 100;
        } else {
            live_cue_required_keys_.remove(state_key);
            live_cue_required_total_.remove(state_key);
            live_cue_rows_.remove(state_key);
            live_cue_row_ids_.remove(state_key);
            live_cue_title_ids_.remove(state_key);
        }
    }
    /* Establish the editor's visual baseline even when every frame came from
     * disk. Without this, the first cue-list-only edit looked like an unknown
     * visual change and invalidateAll() discarded every live-cue state. */
    rememberVisualHash(*title, current_hash);
    emit diagnosticsChanged();
}

void CacheManager::queueFrame(const std::shared_ptr<Title> &title, double time,
                              RenderQueueManager::PriorityBand band)
{
    if (!title)
        return;
    queueFrameWithHash(title, time, band, contentHash(*title));
}

void CacheManager::queueFrameWithHash(const std::shared_ptr<Title> &title, double time,
                                      RenderQueueManager::PriorityBand band,
                                      const QString &known_content_hash)
{
    if (!title) return;
    if (interactive_bypass_.load() || !cache_enabled_.load() || titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    if (time < 0.0 || time > std::max(0.0, title->duration))
        return;
    const CacheFrameKey key = keyForTime(*title, time, known_content_hash);
    const FrameCacheState existing_state = state_tracker_.state(key);
    if (existing_state != FrameCacheState::Stale &&
        (cache_ram_frame_contains(ram_cache_, key) ||
         disk_cache_.contains(key)))
        return;
    if (existing_state == FrameCacheState::Rendering || queue_.contains(key))
        return;
    const int band_offset = (int)band * 100000;
    RenderQueueManager::Job job;
    job.key = key;
    job.title = snapshotForJob(title, key);
    job.time = timeForFrame(key.frame);
    job.priority = band_offset + std::abs(key.frame);
    job.force_render = existing_state == FrameCacheState::Stale;
    job.cache_epoch = cache_epoch_.load();
    job.title_generation = titleGeneration(key.title_id);
    if (queue_.enqueue(job)) {
        BGL_LOG_TRACE("Cache", QStringLiteral("Queued frame title=%1 frame=%2 time=%3 band=%4 key=%5")
                                   .arg(key.title_id)
                                   .arg(key.frame)
                                   .arg(timeForFrame(key.frame), 0, 'f', 3)
                                   .arg((int)band)
                                   .arg(key.toString()));
        state_tracker_.setState(key, FrameCacheState::Queued);
        wakeWorker();
    }
}

void CacheManager::queueRange(const std::shared_ptr<Title> &title, double start, double end, RenderQueueManager::PriorityBand band)
{
    if (!title) return;
    if (interactive_bypass_.load() || !cache_enabled_.load() || titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    if (end < start) std::swap(start, end);
    const int max_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
    const int first = std::clamp(frameForTime(std::clamp(start, 0.0, title->duration)), 0, max_frame);
    const int last = std::clamp(frameForTime(std::clamp(end, 0.0, title->duration)), 0, max_frame);
    const QString content_hash = contentHash(*title);
    for (int frame = first; frame <= last; ++frame)
        queueFrameWithHash(title, timeForFrame(frame), band, content_hash);
}

void CacheManager::queueWholeTimeline(const std::shared_ptr<Title> &title)
{
    if (!title) return;
    if (interactive_bypass_.load() || !cache_enabled_.load() || titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    const int max_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
    const QString title_id = QString::fromStdString(title->id);
    const int start_frame = last_reprioritize_title_id_ == title_id
        ? std::clamp(last_reprioritize_frame_, 0, max_frame)
        : 0;
    const QString content_hash = contentHash(*title);
    for (int frame = start_frame; frame <= max_frame; ++frame)
        queueFrameWithHash(title, timeForFrame(frame), RenderQueueManager::PriorityBand::FullTimeline, content_hash);
    for (int frame = 0; frame < start_frame; ++frame)
        queueFrameWithHash(title, timeForFrame(frame), RenderQueueManager::PriorityBand::FullTimeline, content_hash);
}

void CacheManager::queueWorkArea(const std::shared_ptr<Title> &title)
{
    if (!title) return;
    queueRange(title, title->loop_start, title->loop_end, RenderQueueManager::PriorityBand::WorkArea);
}

void CacheManager::reprioritize(const std::shared_ptr<Title> &title, double current_time)
{
    if (!title) return;
    if (interactive_bypass_.load() || !cache_enabled_.load() || titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    const int max_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
    const int current = std::clamp(frameForTime(current_time), 0, max_frame);
    const QString title_id = QString::fromStdString(title->id);
    if (last_reprioritize_title_id_ == title_id && last_reprioritize_frame_ == current)
        return;
    last_reprioritize_title_id_ = title_id;
    last_reprioritize_frame_ = current;
    queue_.reprioritizeAround(title_id, current);
    const QString content_hash = contentHash(*title);
    queueFrameWithHash(title, current_time, RenderQueueManager::PriorityBand::Visible, content_hash);
    const int lookahead = std::min(6, std::max(2, (int)std::round(effectiveFrameRate() / 4.0)));
    const int lookbehind = 2;
    for (int i = 1; i <= lookahead; ++i)
        if (current + i <= max_frame)
            queueFrameWithHash(title, timeForFrame(current + i), RenderQueueManager::PriorityBand::AfterCurrent, content_hash);
    for (int i = 1; i <= lookbehind; ++i) {
        if (current - i >= 0)
            queueFrameWithHash(title, timeForFrame(current - i), RenderQueueManager::PriorityBand::BeforeCurrent, content_hash);
    }
}

void CacheManager::resetCancelledWorkState(const QString &title_id)
{
    state_tracker_.resetTransient(title_id);

    QVector<QPair<QString, int>> changed_rows;
    {
        std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
        for (auto it = live_cue_states_.begin(); it != live_cue_states_.end(); ++it) {
            const QString state_key = it.key();
            if (!title_id.isEmpty() && live_cue_title_ids_.value(state_key) != title_id)
                continue;
            if (it.value() != FrameCacheState::Queued && it.value() != FrameCacheState::Rendering)
                continue;
            it.value() = FrameCacheState::NotCached;
            live_cue_progress_percent_[state_key] = liveCueStoredProgress(state_key);
            live_cue_last_emit_states_.remove(state_key);
            live_cue_last_emit_buckets_.remove(state_key);
            changed_rows.push_back({live_cue_title_ids_.value(state_key),
                                    live_cue_rows_.value(state_key, -1)});
        }
    }
    for (const auto &changed : changed_rows) {
        if (!changed.first.isEmpty() && changed.second >= 0)
            emit liveCueStateChanged(changed.first, changed.second);
    }
}

void CacheManager::clearRam()
{
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        cache_epoch_.fetch_add(1);
        queue_.clear();
        ram_cache_.clear();
        QMutexLocker dedup_lock(&temporal_dedup_mutex_);
        temporal_canonical_keys_.clear();
        adaptive_canonical_keys_.clear();
    }
    title_gpu_frame_cache_clear();
    resetCancelledWorkState();
    {
        std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
        live_cue_known_keys_.clear();
    }
    state_tracker_.clear();
    wakeWorker();
    emit liveCueStateChanged(QString(), -1);
    emit diagnosticsChanged();
}

void CacheManager::clearDisk()
{
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        cache_epoch_.fetch_add(1);
        queue_.clear();
        QMutexLocker dedup_lock(&temporal_dedup_mutex_);
        temporal_canonical_keys_.clear();
        adaptive_canonical_keys_.clear();
    }
    disk_cache_.clearFast();
    resetCancelledWorkState();
    state_tracker_.clear();
    wakeWorker();
    emit liveCueStateChanged(QString(), -1);
    emit diagnosticsChanged();
}

void CacheManager::clearAll()
{
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        cache_epoch_.fetch_add(1);
        queue_.clear();
        ram_cache_.clear();
        QMutexLocker dedup_lock(&temporal_dedup_mutex_);
        temporal_canonical_keys_.clear();
        adaptive_canonical_keys_.clear();
    }
    title_gpu_frame_cache_clear();
    disk_cache_.clearFast();
    resetCancelledWorkState();
    state_tracker_.clear();
    {
        std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
        live_cue_states_.clear();
        live_cue_progress_percent_.clear();
        live_cue_last_emit_states_.clear();
        live_cue_last_emit_buckets_.clear();
        live_cue_required_keys_.clear();
        live_cue_required_total_.clear();
        live_cue_rendered_since_emit_.clear();
        live_cue_total_by_key_.clear();
        live_cue_done_by_key_.clear();
        live_cue_rows_.clear();
        live_cue_row_ids_.clear();
        live_cue_title_ids_.clear();
        live_cue_transition_state_keys_.clear();
        live_cue_structure_row_ids_.clear();
        live_cue_row_fingerprints_.clear();
        live_cue_transition_signatures_.clear();
        live_cue_known_keys_.clear();
    }
    wakeWorker();
    emit liveCueStateChanged(QString(), -1);
    emit diagnosticsChanged();
}

void CacheManager::pausePrerender()
{
    paused_.store(true);
    wakeWorker();
}

void CacheManager::setCacheEnabled(bool enabled)
{
    if (cache_enabled_.load() == enabled)
        return;

    /* Invalidate every dequeued snapshot before changing externally visible
     * state. The worker may finish its local render, but jobIsCurrent() prevents
     * it from publishing into RAM/disk or touching live-cue state. */
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        cache_epoch_.fetch_add(1);
        queue_.setAcceptingJobs(false);
        cache_enabled_.store(enabled);
        paused_.store(!enabled);
        if (enabled)
            queue_.setAcceptingJobs(true);
    }
    resetCancelledWorkState();
    BGL_LOG_INFO("Cache", QStringLiteral("Cache enabled changed to %1").arg(enabled));
    TitlePreferences::set_cache_enabled(enabled);

    state_tracker_.clear();
    wakeWorker();
    emit cacheEnabledChanged(enabled);
    emit diagnosticsChanged();
}

void CacheManager::setInteractiveBypass(bool bypass)
{
    if (interactive_bypass_.load() == bypass)
        return;
    interactive_bypass_.store(bypass);
    BGL_LOG_DEBUG("Cache", QStringLiteral("Interactive bypass changed to %1").arg(bypass));
    /* Keep accepting live-cue work. The worker predicate pauses only normal
     * prerender jobs while the editor is manipulating the canvas. */
    queue_.setAcceptingJobs(cache_enabled_.load());
    wakeWorker();
}

void CacheManager::setEditorPrerenderFocus(const QString &title_id, bool active)
{
    {
        QMutexLocker lock(&editor_focus_mutex_);
        editor_focus_active_ = active && !title_id.isEmpty();
        editor_focus_title_id_ = editor_focus_active_ ? title_id : QString();
    }
    BGL_LOG_DEBUG("Cache", QStringLiteral("Editor prerender focus active=%1 title=%2")
                               .arg(active && !title_id.isEmpty())
                               .arg(title_id));
    wakeWorker();
}


void CacheManager::setRamCacheLimitMb(int megabytes)
{
    const int clamped = std::clamp(megabytes, 64, 32768);
    TitlePreferences::set_cache_ram_limit_mb(clamped);
    const quint64 bytes = (quint64)clamped * 1024ull * 1024ull;
    const quint64 compatibility_ram_budget = std::clamp<quint64>(
        bytes / 8ull,
        16ull * 1024ull * 1024ull,
        64ull * 1024ull * 1024ull);
    ram_cache_.setMaxBytes(compatibility_ram_budget);
    title_gpu_frame_cache_set_budget(
        bytes > compatibility_ram_budget
            ? bytes - compatibility_ram_budget
            : bytes);
    emit diagnosticsChanged();
}

void CacheManager::setDiskCacheLocation(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty())
        return;
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        cache_epoch_.fetch_add(1);
        queue_.clear();
        disk_cache_.setCacheDirectory(clean);
    }
    TitlePreferences::set_cache_disk_location(clean);
    resetCancelledWorkState();
    BGL_LOG_INFO("Cache", QStringLiteral("Disk cache location changed to %1").arg(clean));
    state_tracker_.clear();
    wakeWorker();
    emit diagnosticsChanged();
}

TitleCacheability CacheManager::titleCacheability(const std::shared_ptr<Title> &title) const
{
    if (!title)
        return TitleCacheability::NonCacheable;
    const TitleDynamicLayerAnalysis analysis = analyze_title_dynamic_layers(*title);
    if (!analysis.has_dynamic_layers)
        return TitleCacheability::Cacheable;
    return analysis.has_cacheable_prefix
        ? TitleCacheability::PartiallyCacheable
        : TitleCacheability::NonCacheable;
}

QString CacheManager::titleCacheabilityMessage(const std::shared_ptr<Title> &title) const
{
    if (!cache_enabled_.load())
        return bgl_tr("OBSTitles.CacheDisabledMessage");
    if (titleCacheability(title) == TitleCacheability::NonCacheable)
        return bgl_tr("OBSTitles.PrerenderDynamicUnavailable");
    return QString();
}

FrameCacheState CacheManager::displayStateForFrame(const std::shared_ptr<Title> &title, int frame) const
{
    if (!title)
        return FrameCacheState::NotCached;
    const QString title_id = QString::fromStdString(title->id);
    const int display_frame = titleHasTimelineChanges(*title)
        ? std::clamp(frame, 0, cache_last_frame_for_title(*title, effectiveFrameRate()))
        : 0;
    return state_tracker_.stateForFrame(title_id, display_frame);
}

bool CacheManager::displayFrameIsStatic(const std::shared_ptr<Title> &title, int frame) const
{
    if (!title)
        return false;
    if (!titleHasTimelineChanges(*title))
        return true;

    const int last_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
    const int f = std::clamp(frame, 0, last_frame);
    const QString content_hash = contentHash(*title);
    const double fps = std::max(1.0, effectiveFrameRate());
    const QString current = adaptiveVisualStateHash(*title, double(f) / fps, content_hash);
    if (f > 0 && adaptiveVisualStateHash(*title, double(f - 1) / fps, content_hash) == current)
        return true;
    if (f < last_frame && adaptiveVisualStateHash(*title, double(f + 1) / fps, content_hash) == current)
        return true;
    return false;
}

QVector<CacheTileRegion> CacheManager::tilesForRect(const QRect &rect, const QSize &frame_size) const
{
    QVector<CacheTileRegion> tiles;
    if (rect.isEmpty() || frame_size.isEmpty())
        return tiles;
    const QRect frame_rect(QPoint(0, 0), frame_size);
    const QRect clipped = rect.intersected(frame_rect);
    if (clipped.isEmpty())
        return tiles;
    const int first_x = clipped.left() / kCacheTileSize;
    const int last_x = clipped.right() / kCacheTileSize;
    const int first_y = clipped.top() / kCacheTileSize;
    const int last_y = clipped.bottom() / kCacheTileSize;
    for (int ty = first_y; ty <= last_y; ++ty) {
        for (int tx = first_x; tx <= last_x; ++tx) {
            const QRect tile_rect(tx * kCacheTileSize, ty * kCacheTileSize, kCacheTileSize, kCacheTileSize);
            tiles.push_back({tx, ty, tile_rect.intersected(frame_rect)});
        }
    }
    return tiles;
}

static std::pair<double, double> animated_scalar_extents(
    const AnimatedProperty &property)
{
    double minimum = property.static_value;
    double maximum = property.static_value;
    for (const Keyframe &keyframe : property.keyframes) {
        minimum = std::min(minimum, keyframe.value);
        maximum = std::max(maximum, keyframe.value);
    }
    return {minimum, maximum};
}

static double animated_scalar_max_nonnegative(
    const AnimatedProperty &property, double fallback)
{
    const auto extents = animated_scalar_extents(property);
    return std::max({0.0, fallback, extents.second});
}

struct AnimatedVec2Extents {
    double min_x = 0.0;
    double max_x = 0.0;
    double min_y = 0.0;
    double max_y = 0.0;
};

static AnimatedVec2Extents animated_vec2_extents(
    const AnimatedVec2Property &property)
{
    AnimatedVec2Extents extents {
        property.static_value.x, property.static_value.x,
        property.static_value.y, property.static_value.y};
    auto include = [&](const Vec2Value &value) {
        extents.min_x = std::min(extents.min_x, value.x);
        extents.max_x = std::max(extents.max_x, value.x);
        extents.min_y = std::min(extents.min_y, value.y);
        extents.max_y = std::max(extents.max_y, value.y);
    };
    for (const VectorKeyframe &keyframe : property.keyframes)
        include(keyframe.value);

    /* A cubic Bezier is contained by the convex hull of its four control
     * points. Including those points gives a deterministic conservative dirty
     * envelope for manual/continuous/auto spatial curves without frame
     * sampling, so prerender tiles cannot clip curved position motion. */
    for (size_t segment = 0; segment + 1 < property.keyframes.size(); ++segment) {
        const VectorKeyframe &start = property.keyframes[segment];
        const VectorKeyframe &end = property.keyframes[segment + 1];
        const Vec2Value outgoing = property.resolved_outgoing_tangent(segment);
        const Vec2Value incoming = property.resolved_incoming_tangent(segment + 1);
        include({start.value.x + outgoing.x, start.value.y + outgoing.y});
        include({end.value.x + incoming.x, end.value.y + incoming.y});
    }
    return extents;
}

QRect CacheManager::layerDirtyRect(const Title &title, const Layer &layer) const
{
    const QSize frame_size(std::max(1, title.width), std::max(1, title.height));
    const QRect frame_rect(QPoint(0, 0), frame_size);
    if (!layer.visible || layer.live_cue_hidden_if_empty)
        return QRect();
    if (layer.type == LayerType::Adjustment || layer.type == LayerType::ColorSolid)
        return frame_rect;

    /* Dirty tiles are shared by every invalidated frame in the layer range, so
     * their bounds must cover the whole animated envelope rather than only the
     * static/default values. Scalar temporal easing remains within keyframe
     * extrema; vector spatial curves additionally include their cubic control
     * hull in animated_vec2_extents(). */
    const AnimatedVec2Extents position = animated_vec2_extents(layer.position);
    const AnimatedVec2Extents scale = animated_vec2_extents(layer.scale);
    const AnimatedVec2Extents size = animated_vec2_extents(layer.size);
    const double sx = std::max({0.01, std::abs(scale.min_x),
                                std::abs(scale.max_x)});
    const double sy = std::max({0.01, std::abs(scale.min_y),
                                std::abs(scale.max_y)});
    double w = std::max({1.0, std::abs(size.min_x),
                         std::abs(size.max_x)}) * sx;
    double h = std::max({1.0, std::abs(size.min_y),
                         std::abs(size.max_y)}) * sy;

    if (layer.type == LayerType::Text || layer.type == LayerType::Clock ||
        layer.type == LayerType::Ticker) {
        const double font_size = animated_scalar_max_nonnegative(
            layer.font_size_prop, std::max(1, layer.font_size));
        const std::string effective_text = effective_external_string(
            layer, ExternalPropertyPaths::TextContent, layer.text_content);
        w = std::max(w, font_size *
                         std::max<size_t>(1, effective_text.size()) *
                         0.75 * sx);
        h = std::max(h, font_size * 1.6 * sy);
    } else if (layer.type == LayerType::Image && w <= 1.0 && h <= 1.0) {
        w = frame_size.width();
        h = frame_size.height();
    }

