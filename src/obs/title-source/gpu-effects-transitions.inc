static void source_hide(void *priv)
{
    auto *data = static_cast<TitleSourceData *>(priv);
    if (!data)
        return;
    data->shown_on_display.store(false, std::memory_order_release);
    clear_live_cue_runtime_for_source(data, data->title_id);
    data->cue_timer_row = -1;
    data->cue_timer_elapsed_seconds = 0.0;
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
        clear_live_cue_runtime_for_source(data, data->title_id);
        data->cue_timer_row = -1;
        data->cue_timer_elapsed_seconds = 0.0;
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
            const int cue_row_count = live_text_playlist_row_count(*title);
            bool has_pending = title->pending_cue_row >= 0 &&
                               title->pending_cue_row < cue_row_count;
            bool has_current = title->current_cue_row >= 0 &&
                               title->current_cue_row < cue_row_count;
            bool is_uncue = title->cue_uncue_requested && !has_pending && has_current;
            data->manual_uncue = is_uncue;
            if (title->playback_mode == 1) {
                if (has_pending) {
                    data->playhead = loop_end;
                    data->cue_phase = TitleSourceData::CuePhase::OutroThenIntro;
                } else if (is_uncue) {
                    /* Manual uncue releases the title from its current frame.
                     * Do not seek to the configured loop boundary: the outro
                     * must continue naturally from the exact on-air position. */
                    data->cue_phase = TitleSourceData::CuePhase::OutroOnly;
                } else {
                    data->playhead = 0.0;
                    data->cue_phase = TitleSourceData::CuePhase::IntroLoop;
                }
            } else if (title->playback_mode == 2 && (has_pending || is_uncue)) {
                /* Cue-to-cue transitions still start from the authored pause
                 * point. Manual uncue, however, resumes from the current
                 * playhead (normally the held pause frame, or an earlier frame
                 * when toggled during the intro). */
                if (has_pending)
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
    const bool has_independent_asset_layer =
        title_has_independent_asset_layer(title);
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
                    /* Automatic completion is an uncue just like a manual
                     * toggle-off.  Always clear the runtime cue identity, then
                     * keep/hide the source according to When cue ends. */
                    title->current_cue_row = -1;
                    title->pending_cue_row = -1;
                    title->cue_persistence_transition = false;
                    title->cue_persistent_text_columns.clear();
                    data->active_cue_row = -1;
                    if (title->cue_end_behavior == 1) {
                        set_source_output_visible(data, false,
                                                  "play-once-show-nothing");
                    } else if (title->cue_end_behavior == 2) {
                        data->playhead = 0.0;
                        set_source_output_visible(data, true,
                                                  "play-once-show-first-frame");
                    } else {
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

    /* Publish the exact source playhead plus a wall-clock cue elapsed timer.
     * This remains active while Pause/Loop titles hold their timeline and is
     * cleared as soon as the source is inactive or the cue actually ends. */
    publish_live_cue_runtime(data, title);

    if (has_ticker_layer || has_independent_asset_layer)
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
        const bool destination_composite =
            title_requires_destination_compositing(*title);
        /* A flattened cache frame has already lost the destination-dependent
         * layer operations. Screen/Multiply/Overlay/Color and adjustment
         * layers must be evaluated against the actual canvas/scene backdrop. */
        if (!destination_composite && cache.cacheEnabled() &&
            cacheability != TitleCacheability::NonCacheable) {
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
                                           double title_time, int /*depth*/ = 0)
{
    const QTransform transform = layer_world_transform_qt(title, layer, title_time);

    /* OBS stores affine basis vectors in matrix4::x/y and translation in
     * matrix4::t.  Apply the complete QTransform in one multiplication rather
     * than decomposing it into translate/rotate/scale: nested non-uniform
     * parent transforms can legitimately produce shear, which a decomposition
     * would silently discard. */
    struct matrix4 affine;
    matrix4_identity(&affine);
    vec4_set(&affine.x,
             static_cast<float>(transform.m11()),
             static_cast<float>(transform.m12()),
             0.0f, 0.0f);
    vec4_set(&affine.y,
             static_cast<float>(transform.m21()),
             static_cast<float>(transform.m22()),
             0.0f, 0.0f);
    vec4_set(&affine.t,
             static_cast<float>(transform.dx()),
             static_cast<float>(transform.dy()),
             0.0f, 1.0f);
    gs_matrix_mul(&affine);
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
uniform texture2d coverageImage;
uniform texture2d transitionMatte;
uniform float amount;
uniform int blendMode;
uniform int coverageInvert;
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
float mix_lum(float3 c){return dot(c,float3(0.30,0.59,0.11));}
float mix_sat(float3 c){return max(c.r,max(c.g,c.b))-min(c.r,min(c.g,c.b));}
float3 mix_clip(float3 c){float l=mix_lum(c);float n=min(c.r,min(c.g,c.b));float x=max(c.r,max(c.g,c.b));if(n<0.0)c=l+((c-l)*l)/max(l-n,0.000001);if(x>1.0)c=l+((c-l)*(1.0-l))/max(x-l,0.000001);return clamp(c,0.0,1.0);}
float3 mix_set_lum(float3 c,float l){return mix_clip(c+(l-mix_lum(c)));}
float3 mix_set_sat(float3 c,float sat){float n=min(c.r,min(c.g,c.b));float x=max(c.r,max(c.g,c.b));return x>n?(c-n)*(sat/(x-n)):float3(0.0,0.0,0.0);}
float3 adjustment_blend(float3 base,float3 src){if(blendMode==1)return base*src;if(blendMode==2)return min(base+src,1.0);if(blendMode==3)return 1.0-(1.0-base)*(1.0-src);if(blendMode==4)return lerp(2.0*base*src,1.0-2.0*(1.0-base)*(1.0-src),step(0.5,base));if(blendMode==5)return mix_set_lum(mix_set_sat(src,mix_sat(base)),mix_lum(base));return src;}
float4 PSMix(VertDataOut v):TARGET{float4 a=originalImage.Sample(textureSampler,v.uv);float4 b=effectedImage.Sample(textureSampler,v.uv);float coverage=step(0.5,clamp(coverageImage.Sample(textureSampler,v.uv).a,0.0,1.0));if(coverageInvert!=0)coverage=1.0-coverage;float aa=clamp(a.a,0.0,1.0);float ba=clamp(b.a,0.0,1.0);float3 ac=aa>0.000001?a.rgb/aa:float3(0.0,0.0,0.0);float3 bc=ba>0.000001?b.rgb/ba:float3(0.0,0.0,0.0);float3 blended=adjustment_blend(ac,bc);/* Process the existing artwork only inside the adjustment coverage. The editor checkerboard is not part of either texture, so it cannot be blurred or color-corrected. *//* The effect is evaluated from the complete lower composite. Inside the
coverage use the effected alpha as well as its color, otherwise blur kernels
are clipped back to the original per-pixel alpha and crossing text/shapes only
blur partially. Color-only effects keep the same alpha naturally. */float4 inside=float4(blended*ba,ba);float t=clamp(amount*coverage*transition_alpha(v.uv),0.0,1.0);float4 result=lerp(a,inside,t);/* Preserve effect-generated pixels that expand beyond the adjustment bounds (drop shadow, glow, bloom, blur tails). Only the alpha newly created by the effect is admitted outside coverage, so opaque background pixels outside the adjustment region are not recolored. */float halo=0.0;return lerp(result,b,halo);}
technique Draw{pass{vertex_shader=VSDefault(v);pixel_shader=PSMix(v);}}
)";

static constexpr const char *kGpuAdjustmentCoverageEffect = R"(
uniform float4x4 ViewProj;
struct VertDataIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VertDataOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertDataOut VSDefault(VertDataIn v){VertDataOut o;o.pos=mul(float4(v.pos.xyz,1.0),ViewProj);o.uv=v.uv;return o;}
float4 PSCoverage(VertDataOut v):TARGET{return float4(1.0,1.0,1.0,1.0);}
technique Draw{pass{vertex_shader=VSDefault(v);pixel_shader=PSCoverage(v);}}
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
uniform float opacity;
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
    float sourceAlpha = clamp(src.a, 0.0, 1.0);
    float layerOpacity = clamp(opacity, 0.0, 1.0);
    float sa = sourceAlpha * layerOpacity;
    float3 cb = da > 0.000001 ? dst.rgb / da : float3(0.0, 0.0, 0.0);
    float3 cs = sourceAlpha > 0.000001 ? src.rgb / sourceAlpha : float3(0.0, 0.0, 0.0);
    float3 blended = blend_color(cb, cs, blendMode);
    float outA = sa + da * (1.0 - sa);
    // AE-style source-over composition in premultiplied-alpha space.
    // The foreground texture is already premultiplied by sourceAlpha, so its
    // RGB contribution must also be scaled by the animated layer opacity.
    // Without this, lowering opacity only changed alpha while RGB stayed at
    // full strength, making Screen/Add layers look opaque in Preview/Program
    // and preventing the canvas checkerboard from showing through correctly.
    float3 effectiveSrc = src.rgb * layerOpacity;
    float3 outRGB = dst.rgb * (1.0 - sa) +
                    effectiveSrc * (1.0 - da) +
                    blended * (sa * da);
    return float4(clamp(outRGB, 0.0, 1.0), clamp(outA, 0.0, 1.0));
}
technique Draw { pass { vertex_shader = VSDefault(v); pixel_shader = PSBlend(v); } }
)";

static constexpr const char *kGpuExternalBackgroundEffect = R"(
uniform float4x4 ViewProj;
uniform texture2d background;
uniform float4x4 localToBackground;
uniform float2 backgroundSize;
uniform float2 logicalSize;
uniform float4 backgroundRect;
uniform float4 titleBackground;
uniform int mappingMode;
sampler_state textureSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };
struct VertDataIn { float4 pos : POSITION; float2 uv : TEXCOORD0; };
struct VertDataOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertDataOut VSDefault(VertDataIn v) { VertDataOut o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }
float4 PSBackground(VertDataOut v) : TARGET
{
    float2 pixel;
    if (mappingMode == 0) {
        pixel = backgroundRect.xy + v.uv * backgroundRect.zw;
    } else {
        float4 mapped = mul(float4(v.uv * logicalSize, 0.0, 1.0), localToBackground);
        pixel = mapped.xy;
    }
    float2 size = max(backgroundSize, float2(1.0, 1.0));
    float2 uv = pixel / size;
    float inside = step(0.0, uv.x) * step(0.0, uv.y) *
                   step(uv.x, 1.0) * step(uv.y, 1.0);
    float4 dst = background.Sample(textureSampler, clamp(uv, 0.0, 1.0)) * inside;
    return titleBackground + dst * (1.0 - titleBackground.a);
}
technique Draw { pass { vertex_shader = VSDefault(v); pixel_shader = PSBackground(v); } }
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
    /* Clipping mattes consume geometric artwork coverage. Their mask graph is
     * rendered at full compositor opacity, so modes 5/6 remain channel-
     * compatible with Alpha/Inverse Alpha here while ignoring the source
     * layer's animated opacity. Intrinsic image transparency and anti-aliased
     * shape/text edges remain part of the coverage. Visible-base artwork is
     * rendered separately with its normal opacity by composite_clipping_base(). */
    if (maskMode == 2 || maskMode == 4 || maskMode == 6)
        maskValue = 1.0 - maskValue;
    return src * clamp(maskValue, 0.0, 1.0);
}
technique Draw { pass { vertex_shader = VSDefault(v); pixel_shader = PSMask(v); } }
)";


struct ExtensionImageTexture {
    gs_image_file_t image{};
    bool initialized = false;
    qint64 modified_msecs = 0;
    qint64 file_size = -1;
    uint64_t last_used = 0;
    ~ExtensionImageTexture()
    {
        if (initialized)
            gs_image_file_free(&image);
    }
};

static std::mutex g_extension_image_texture_mutex;
static std::unordered_map<std::string, std::unique_ptr<ExtensionImageTexture>>
    g_extension_image_textures;
static uint64_t g_extension_image_texture_tick = 0;
static constexpr std::size_t kMaxExtensionImageTextures = 64;

static void clear_extension_image_textures_locked()
{
    g_extension_image_textures.clear();
    g_extension_image_texture_tick = 0;
}

static void clear_extension_image_textures()
{
    std::lock_guard<std::mutex> lock(g_extension_image_texture_mutex);
    clear_extension_image_textures_locked();
}

static gs_texture_t *extension_image_texture(const QString &absolute_path)
{
    if (absolute_path.trimmed().isEmpty())
        return nullptr;
    const QFileInfo info(absolute_path);
    if (!info.isFile() || !info.isReadable())
        return nullptr;
    const QString canonical = info.canonicalFilePath().isEmpty()
        ? info.absoluteFilePath() : info.canonicalFilePath();
    const std::string key = canonical.toUtf8().constData();
    const qint64 modified = info.lastModified().toMSecsSinceEpoch();
    const qint64 fileSize = info.size();

    std::lock_guard<std::mutex> lock(g_extension_image_texture_mutex);
    auto found = g_extension_image_textures.find(key);
    if (found != g_extension_image_textures.end()) {
        if (found->second->modified_msecs == modified &&
            found->second->file_size == fileSize) {
            found->second->last_used = ++g_extension_image_texture_tick;
            return found->second->image.texture;
        }
        g_extension_image_textures.erase(found);
    }

    auto entry = std::make_unique<ExtensionImageTexture>();
    gs_image_file_init(&entry->image, key.c_str());
    entry->initialized = true;
    if (!entry->image.loaded)
        return nullptr;
    gs_image_file_init_texture(&entry->image);
    if (!entry->image.texture)
        return nullptr;
    entry->modified_msecs = modified;
    entry->file_size = fileSize;
    entry->last_used = ++g_extension_image_texture_tick;
    gs_texture_t *texture = entry->image.texture;
    g_extension_image_textures.emplace(key, std::move(entry));

    while (g_extension_image_textures.size() > kMaxExtensionImageTextures) {
        auto oldest = g_extension_image_textures.end();
        for (auto it = g_extension_image_textures.begin();
             it != g_extension_image_textures.end(); ++it) {
            if (it->first == key)
                continue;
            if (oldest == g_extension_image_textures.end() ||
                it->second->last_used < oldest->second->last_used)
                oldest = it;
        }
        if (oldest == g_extension_image_textures.end())
            break;
        g_extension_image_textures.erase(oldest);
    }
    return texture;
}

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
        /* Free-transform geometry is immutable for ordinary ticker playback.
         * Keep it GPU-resident and rebuild only when the quad, raster bounds or
         * evaluated transform actually changes. */
        gs_vertbuffer_t *transform_grid = nullptr;
        std::string transform_grid_key;
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

