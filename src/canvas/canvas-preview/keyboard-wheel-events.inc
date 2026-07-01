    dirty_ = true;
    drag_changed_ = true;
    invalidate_canvas_overlay_caches();
    update();
}
void CanvasPreview::render_to_frame()
{
    if (render_in_progress_)
        return;
    render_in_progress_ = true;
    QElapsedTimer render_cost;
    render_cost.start();

    frame_image_ = QImage();
    frame_image_canvas_offset_ = QPoint();
    frame_image_canvas_size_ = title_ ? QSize(title_->width, title_->height) : QSize();
    frame_image_preview_scale_ = 1.0;

    if (title_ && gpu_render_session_) {
        /* Timeline playback changes only the session time. Model snapshots are
         * replaced only after an actual edit, avoiding a deep copy of every
         * layer/keyframe on each preview frame. */
        if (drag_changed_)
            gpu_model_dirty_ = true;
        if (gpu_model_dirty_)
            ++gpu_model_revision_;
        const bool resize_drag = drag_mode_ == DragMode::ResizeNW ||
            drag_mode_ == DragMode::ResizeN || drag_mode_ == DragMode::ResizeNE ||
            drag_mode_ == DragMode::ResizeE || drag_mode_ == DragMode::ResizeSE ||
            drag_mode_ == DragMode::ResizeS || drag_mode_ == DragMode::ResizeSW ||
            drag_mode_ == DragMode::ResizeW;
        /* Move/rotate are pure GPU matrix edits. Resize is also presented
         * through the existing GPU texture when the layer contract permits it;
         * the render session selectively refreshes rasters whose local layout
         * actually depends on the resized box. */
        const bool transform_only_update = drag_changed_ &&
            (drag_mode_ == DragMode::Move || drag_mode_ == DragMode::Rotate ||
             resize_drag);

        bool submitted_cached_frame = false;
        bool preserve_previous_cached_frame = false;
        CacheManager &cache = CacheManager::instance();
        const CachePlaybackSettings playback = cache.playbackSettings();
        const TitleCacheability cacheability = cache.titleCacheability(title_);
        const bool destination_composite =
            title_requires_destination_compositing(*title_);
        const bool direct_interaction = drag_changed_ || adaptive_interaction_active_;

        /* The editor and OBS source must present the same cache payload. The
         * previous editor path used requestFrame() only as a transport gate and
         * then rebuilt the live GPU graph, which could diverge from the actual
         * prerender. Cache frames are full-quality canvas payloads, so disable
         * adaptive draft scaling before submitting their sparse coordinates. */
        QImage cached;
        QString gpu_cache_token;
        if (!destination_composite && !direct_interaction &&
            cache.cacheEnabled() &&
            cacheability != TitleCacheability::NonCacheable) {
            const int cue_row = title_->current_cue_row;
            const bool has_active_cue = cue_row >= 0 &&
                cue_row < static_cast<int>(title_->live_text_rows.size());
            gpu_cache_token = has_active_cue
                ? cache.requestLiveCueFrameGpuToken(
                      title_, cue_row, playhead_, false)
                : cache.requestFrameGpuToken(title_, playhead_, false);
            if (gpu_cache_token.isEmpty()) {
                if (!prefetched_cache_frame_.isNull() &&
                    std::abs(prefetched_cache_time_ - playhead_) < 1e-9) {
                    cached = prefetched_cache_frame_;
                } else if (has_active_cue) {
                    cached = cache.requestLiveCueFrame(
                        title_, cue_row, playhead_,
                        !playback.cached_frames_only);
                } else {
                    cached = cache.requestFrame(title_, playhead_,
                                                playback.cached_frames_only);
                }
            }
            prefetched_cache_frame_ = QImage();
            prefetched_cache_time_ = -1.0;
            if (!gpu_cache_token.isEmpty()) {
                title_gpu_render_session_set_preview_quality(
                    gpu_render_session_, 1.0, false);
                frame_image_preview_scale_ = 1.0;
                if (cacheability == TitleCacheability::PartiallyCacheable) {
                    const TitleDynamicLayerAnalysis analysis =
                        analyze_title_dynamic_layers(*title_);
                    if (analysis.has_cacheable_prefix) {
                        submitted_cached_frame =
                            title_gpu_render_session_submit_gpu_cached_prefix(
                                gpu_render_session_, *title_,
                                gpu_cache_token.toStdString(), playhead_,
                                analysis.first_dynamic_layer,
                                gpu_model_revision_);
                    }
                } else {
                    submitted_cached_frame =
                        title_gpu_render_session_submit_gpu_cached_frame(
                            gpu_render_session_, *title_,
                            gpu_cache_token.toStdString(),
                            gpu_model_revision_);
                }
            } else if (!cached.isNull()) {
                title_gpu_render_session_set_preview_quality(
                    gpu_render_session_, 1.0, false);
                frame_image_preview_scale_ = 1.0;
                if (cacheability == TitleCacheability::PartiallyCacheable) {
                    const TitleDynamicLayerAnalysis analysis =
                        analyze_title_dynamic_layers(*title_);
                    if (analysis.has_cacheable_prefix) {
                        submitted_cached_frame =
                            title_gpu_render_session_submit_cached_prefix(
                                gpu_render_session_, *title_, cached, playhead_,
                                analysis.first_dynamic_layer,
                                gpu_model_revision_);
                    }
                } else {
                    submitted_cached_frame =
                        title_gpu_render_session_submit_final_frame(
                            gpu_render_session_, *title_, cached,
                            gpu_model_revision_);
                }
                if (!submitted_cached_frame) {
                    cache.rejectFramePayload(title_, playhead_);
                    if (playback.cached_frames_only &&
                        playback_frame_pending_ && !gpu_model_dirty_)
                        preserve_previous_cached_frame = true;
                }
            } else if (playback.cached_frames_only &&
                       playback_frame_pending_ && !gpu_model_dirty_) {
                /* Match the source contract: while transport waits for an exact
                 * cached frame, keep the last published texture rather than
                 * silently switching to a different live render. */
                preserve_previous_cached_frame = true;
            }

            if (TitlePreferences::cache_playback_logging_enabled()) {
                const QString action = submitted_cached_frame
                    ? (!gpu_cache_token.isEmpty()
                           ? QStringLiteral("submit-gpu-ram")
                           : cacheability == TitleCacheability::PartiallyCacheable
                               ? QStringLiteral("submit-prefix")
                               : QStringLiteral("submit-final"))
                    : preserve_previous_cached_frame
                        ? QStringLiteral("hold-previous")
                        : cached.isNull() ? QStringLiteral("cache-miss")
                                          : QStringLiteral("cache-rejected");
                BGL_LOG_INFO("CachePlayback", QStringLiteral(
                    "consumer=editor-canvas action=%1 title=%2 time=%3 cacheKey=%4 size=%5x%6 cachedOnly=%7")
                    .arg(action)
                    .arg(QString::fromStdString(title_->id))
                    .arg(playhead_, 0, 'f', 6)
                    .arg(cached.isNull() ? 0 : cached.cacheKey())
                    .arg(cached.width()).arg(cached.height())
                    .arg(playback.cached_frames_only));
            }
        }

        if (destination_composite || direct_interaction ||
            !cache.cacheEnabled() ||
            cacheability == TitleCacheability::NonCacheable) {
            prefetched_cache_frame_ = QImage();
            prefetched_cache_time_ = -1.0;
        }

        if (!submitted_cached_frame && !preserve_previous_cached_frame) {
            const double adaptive_scale = destination_composite
                ? 1.0
                : adaptive_preview_scale();
            const bool adaptive_draft = !destination_composite &&
                adaptive_rendering_enabled_ && adaptive_scale < 0.999;
            title_gpu_render_session_set_preview_quality(gpu_render_session_,
                                                         adaptive_scale,
                                                         adaptive_draft);
            frame_image_preview_scale_ = adaptive_scale;
            title_gpu_render_session_update(gpu_render_session_, *title_, playhead_,
                                            gpu_model_revision_,
                                            transform_only_update);
        }
        if (!preserve_previous_cached_frame)
            gpu_model_dirty_ = false;
    }

    dirty_ = false;
    playback_frame_pending_ = false;
    force_live_full_quality_render_ = false;
    const int cost_ms = std::max(1, (int)render_cost.elapsed());
    last_full_quality_render_cost_ms_ = cost_ms;
    BGL_LOG_TRACE("Performance", QStringLiteral(
        "Canvas frame prepared title=%1 time=%2 costMs=%3 scale=%4")
        .arg(title_ ? QString::fromStdString(title_->id) : QStringLiteral("<none>"))
        .arg(playhead_, 0, 'f', 6)
        .arg(cost_ms)
        .arg(frame_image_preview_scale_, 0, 'f', 3));
    /* GPU presentation can remain at display cadence. Coalescing protects the
     * UI only while a changed layer raster is being regenerated. */
    /* Keep one render-cost interval of headroom, but do not impose the old
     * 60 Hz floor: a fast title on a high-refresh display may now update up to
     * that display's cadence while editing. */
    render_interval_ms_ = std::clamp(cost_ms + 1, 1, 34);
    last_render_clock_.restart();
    render_in_progress_ = false;
}


void CanvasPreview::render_dirty_frame_if_due()
{
    if (!dirty_ || render_in_progress_)
        return;

    const qint64 elapsed = last_render_clock_.isValid()
        ? last_render_clock_.elapsed() : render_interval_ms_;
    /* Editing is never rendered faster than the monitor can present it, and
     * never faster than the previous render cost can sustain. Playback is
     * different: its updates are already generated by the transport timer at
     * the project frame rate, so those exact frame requests are not throttled
     * by the monitor cadence here. */
    const int editing_cadence_ms = std::max(display_refresh_interval_ms_,
                                             render_interval_ms_);
    const int cadence_ms = playback_frame_pending_ ? 0 : editing_cadence_ms;

    if ((!gpu_render_session_ && frame_image_.isNull()) || elapsed >= cadence_ms) {
        render_to_frame();
        return;
    }

    if (render_coalesce_timer_ && !render_coalesce_timer_->isActive())
        render_coalesce_timer_->start(std::max(1, cadence_ms - static_cast<int>(elapsed)));
}


QRectF CanvasPreview::canvas_view_rect() const
{
    if (!title_) return QRectF();
    const double scale = view_scale();
    const QPointF origin = view_origin();
    return QRectF(origin.x(), origin.y(), title_->width * scale, title_->height * scale);
}

QRectF CanvasPreview::ruler_top_rect() const
{
    if (!rulers_visible_) return QRectF();
    return QRectF(kCanvasRulerThickness, 0, std::max(0, width() - kCanvasRulerThickness), kCanvasRulerThickness);
}

QRectF CanvasPreview::ruler_left_rect() const
{
    if (!rulers_visible_) return QRectF();
    return QRectF(0, kCanvasRulerThickness, kCanvasRulerThickness, std::max(0, height() - kCanvasRulerThickness));
}

QRectF CanvasPreview::ruler_corner_rect() const
{
    if (!rulers_visible_) return QRectF();
    return QRectF(0, 0, kCanvasRulerThickness, kCanvasRulerThickness);
}

bool CanvasPreview::ruler_hit_test(const QPointF &view_pt, bool &vertical_guide) const
{
    if (!rulers_visible_ || guides_locked_) return false;
    // Match the requested ruler behavior:
    // - vertical (left) ruler creates vertical guides (x-axis guides)
    // - horizontal (top) ruler creates horizontal guides (y-axis guides)
    if (ruler_left_rect().contains(view_pt)) { vertical_guide = true; return true; }
    if (ruler_top_rect().contains(view_pt)) { vertical_guide = false; return true; }
    return false;
}

int CanvasPreview::guide_hit_test(const QPointF &view_pt, bool &x_axis, bool include_locked) const
{
    if (!guides_visible_ || (!include_locked && guides_locked_) || !title_) return -1;
    const QRectF canvas_rect = canvas_view_rect();
    for (size_t i = 0; i < vertical_guides_.size(); ++i) {
        const double x = canvas_to_view(QPointF(vertical_guides_[i], 0.0)).x();
        if (std::abs(view_pt.x() - x) <= kCanvasGuideHitTolerancePx &&
            view_pt.y() >= canvas_rect.top() - 8.0 && view_pt.y() <= canvas_rect.bottom() + 8.0) {
            x_axis = true;
            return (int)i;
        }
    }
    for (size_t i = 0; i < horizontal_guides_.size(); ++i) {
        const double y = canvas_to_view(QPointF(0.0, horizontal_guides_[i])).y();
        if (std::abs(view_pt.y() - y) <= kCanvasGuideHitTolerancePx &&
            view_pt.x() >= canvas_rect.left() - 8.0 && view_pt.x() <= canvas_rect.right() + 8.0) {
            x_axis = false;
            return (int)i;
        }
    }
    return -1;
}



double CanvasPreview::snap_guide_value_to_objects(bool x_axis, double raw_value)
{
    if (!title_ || !snap_settings_.enabled || !snap_settings_.object_edges) {
        clear_snap_feedback();
        return raw_value;
    }

    const double tolerance = 6.0 / std::max(0.1, view_scale());
    double best_value = raw_value;
    double best_distance = tolerance + 1.0;
    QString best_label;

    auto consider = [&](double value, const QString &label) {
        if (!std::isfinite(value)) return;
        const double distance = std::abs(value - raw_value);
        if (distance < best_distance) {
            best_distance = distance;
            best_value = value;
            best_label = label;
        }
    };

    for (const auto &layer : title_->layers) {
        if (!layer || !layer->visible) continue;
        if (playhead_ < layer->in_time || playhead_ > layer->out_time) continue;
        QRectF bounds = layer_canvas_bounds(*layer);
        if (!bounds.isValid() || bounds.isEmpty()) continue;

        consider(x_axis ? bounds.left() : bounds.top(), bgl_tr("OBSTitles.ObjectEdge"));
        consider(x_axis ? bounds.right() : bounds.bottom(), bgl_tr("OBSTitles.ObjectEdge"));
        if (snap_settings_.object_centers)
            consider(x_axis ? bounds.center().x() : bounds.center().y(), bgl_tr("OBSTitles.ObjectCenter"));
    }

    snap_feedback_.clear();
    if (best_distance <= tolerance) {
        add_snap_feedback(x_axis, best_value, best_label);
        return best_value;
    }
    return raw_value;
}

void CanvasPreview::draw_rulers(QPainter &p, const QRectF &canvas_rect, double scale, const QPointF &origin)
{
    if (!rulers_visible_ || !title_) return;

    const QPalette pal = palette();
    const QColor bg = pal.color(QPalette::Button);
    const QColor border = pal.color(QPalette::Mid);
    const QColor fg = pal.color(QPalette::ButtonText);
    const QRectF top = ruler_top_rect();
    const QRectF left = ruler_left_rect();
    const QRectF corner = ruler_corner_rect();

    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(top, bg);
    p.fillRect(left, bg);
    p.fillRect(corner, bg.darker(105));
    p.setPen(border);
    p.drawLine(top.bottomLeft(), top.bottomRight());
    p.drawLine(left.topRight(), left.bottomRight());
    p.drawRect(corner.adjusted(0, 0, -1, -1));

    auto nice_step = [](double target_canvas_units) {
        static const double bases[] = {1.0, 2.0, 5.0, 10.0};
        const double exponent = std::pow(10.0, std::floor(std::log10(std::max(1.0, target_canvas_units))));
        for (double base : bases) {
            const double step = base * exponent;
            if (step >= target_canvas_units) return step;
        }
        return 10.0 * exponent;
    };
    const double major_step = nice_step(72.0 / std::max(0.001, scale));
    const double minor_step = major_step / 5.0;
    QFont small = font();
    small.setPointSize(std::max(7, small.pointSize() - 2));
    p.setFont(small);
    p.setPen(fg);

    const double start_x = std::floor(std::max(0.0, view_to_canvas(QPointF(top.left(), 0)).x()) / minor_step) * minor_step;
    const double end_x = std::min((double)title_->width, view_to_canvas(QPointF(top.right(), 0)).x());
    for (double v = start_x; v <= end_x + 0.01; v += minor_step) {
        const bool major = std::fmod(std::abs(v), major_step) < 0.001 || std::abs(std::fmod(std::abs(v), major_step) - major_step) < 0.001;
        const double x = origin.x() + v * scale;
        const double h = major ? 12.0 : 6.0;
        p.drawLine(QPointF(x, top.bottom()), QPointF(x, top.bottom() - h));
        if (major)
            p.drawText(QRectF(x + 3.0, top.top() + 2.0, 80.0, top.height() - 6.0), QString::number((int)std::round(v)));
    }

    const double start_y = std::floor(std::max(0.0, view_to_canvas(QPointF(0, left.top())).y()) / minor_step) * minor_step;
    const double end_y = std::min((double)title_->height, view_to_canvas(QPointF(0, left.bottom())).y());
    for (double v = start_y; v <= end_y + 0.01; v += minor_step) {
        const bool major = std::fmod(std::abs(v), major_step) < 0.001 || std::abs(std::fmod(std::abs(v), major_step) - major_step) < 0.001;
        const double y = origin.y() + v * scale;
        const double w = major ? 12.0 : 6.0;
        p.drawLine(QPointF(left.right(), y), QPointF(left.right() - w, y));
        if (major) {
            p.save();
            p.translate(left.left() + 2.0, y - 3.0);
            p.rotate(-90.0);
            p.drawText(QRectF(0, 0, 80.0, left.width() - 6.0), QString::number((int)std::round(v)));
            p.restore();
        }
    }
    draw_ruler_mouse_indicators(p, canvas_rect);
    p.restore();
}

void CanvasPreview::draw_guide_coordinate(QPainter &p, const QPointF &view_pt, bool x_axis, double value) const
{
    if (!show_guide_coordinates_) return;
    const QString text = bgl_tr("OBSTitles.AxisPixelReadout").arg(x_axis ? bgl_tr("OBSTitles.X") : bgl_tr("OBSTitles.Y")).arg(value, 0, 'f', 1);
    const QRectF bubble(view_pt.x() + 12.0, view_pt.y() + 12.0, 92.0, 22.0);
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 185));
    p.drawRoundedRect(bubble, 4, 4);
    p.setPen(Qt::white);
    p.drawText(bubble.adjusted(6, 2, -4, -2), Qt::AlignVCenter | Qt::AlignLeft, text);
    p.restore();
}

void CanvasPreview::draw_user_guides(QPainter &p, const QRectF &canvas_rect)
{
    if (!guides_visible_ || !title_) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    QPen pen(editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::Guides), 1.0, Qt::DashLine);
    pen.setDashPattern({4.0, 3.0});
    p.setPen(pen);
    for (double x_value : vertical_guides_) {
        const double x = canvas_to_view(QPointF(x_value, 0.0)).x();
        p.drawLine(QPointF(x, canvas_rect.top()), QPointF(x, canvas_rect.bottom()));
    }
    for (double y_value : horizontal_guides_) {
        const double y = canvas_to_view(QPointF(0.0, y_value)).y();
        p.drawLine(QPointF(canvas_rect.left(), y), QPointF(canvas_rect.right(), y));
    }
    if (dragging_new_guide_ || dragging_guide_index_ >= 0) {
        QPen active_pen(editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::ActiveGuide), 1.0, Qt::DashLine);
        active_pen.setDashPattern({6.0, 3.0});
        p.setPen(active_pen);
        if (dragging_guide_x_axis_) {
            const double x = canvas_to_view(QPointF(dragging_guide_value_, 0.0)).x();
            p.drawLine(QPointF(x, canvas_rect.top()), QPointF(x, canvas_rect.bottom()));
            draw_guide_coordinate(p, QPointF(x, canvas_rect.top()), true, dragging_guide_value_);
        } else {
            const double y = canvas_to_view(QPointF(0.0, dragging_guide_value_)).y();
            p.drawLine(QPointF(canvas_rect.left(), y), QPointF(canvas_rect.right(), y));
            draw_guide_coordinate(p, QPointF(canvas_rect.left(), y), false, dragging_guide_value_);
        }
    }
    p.restore();
}


void CanvasPreview::invalidate_checkerboard_cache()
{
    checkerboard_tile_ = QPixmap();
    checkerboard_tile_pattern_ = -1;
}

void CanvasPreview::draw_static_checkerboard(QPainter &p, const QRect &canvas_rect_px)
{
    if (canvas_rect_px.isEmpty())
        return;

    auto checkerboard_colors = [this]() {
        if (checkerboard_pattern_ == 3)
            return std::pair<QColor, QColor>(Qt::white, Qt::white);
        if (checkerboard_pattern_ == 4)
            return std::pair<QColor, QColor>(Qt::black, Qt::black);
        if (checkerboard_pattern_ == 5)
            return std::pair<QColor, QColor>(QColor(0x80, 0x80, 0x80), QColor(0x80, 0x80, 0x80));
        if (checkerboard_pattern_ == 0)
            return std::pair<QColor, QColor>(QColor(0xee, 0xee, 0xee), QColor(0xc8, 0xc8, 0xc8));
        if (checkerboard_pattern_ == 2)
            return std::pair<QColor, QColor>(QColor(0x1f, 0x1f, 0x1f), QColor(0x36, 0x36, 0x36));
        return std::pair<QColor, QColor>(QColor(0x33, 0x33, 0x33), QColor(0x4a, 0x4a, 0x4a));
    };

    constexpr int checker_size = 12;
    constexpr int tile_size = checker_size * 2;
    if (checkerboard_tile_.isNull() || checkerboard_tile_pattern_ != checkerboard_pattern_) {
        const auto [checker_a, checker_b] = checkerboard_colors();
        checkerboard_tile_ = QPixmap(tile_size, tile_size);
        checkerboard_tile_.fill(checker_a);
        if (checker_a != checker_b) {
            QPainter tile_painter(&checkerboard_tile_);
            tile_painter.setPen(Qt::NoPen);
            tile_painter.setBrush(checker_b);
            tile_painter.drawRect(0, 0, checker_size, checker_size);
            tile_painter.drawRect(checker_size, checker_size, checker_size, checker_size);
        }
        checkerboard_tile_pattern_ = checkerboard_pattern_;
    }

    p.save();
    p.setClipRect(canvas_rect_px);
    p.drawTiledPixmap(canvas_rect_px, checkerboard_tile_, QPoint(0, 0));
    p.restore();
}

void CanvasPreview::paint_canvas(QPainter &p, CanvasPaintPass pass)
{
    const QPalette pal = palette();
    if (pass != CanvasPaintPass::Overlay)
        p.fillRect(rect(), pal.color(QPalette::Window));

    if (!title_) return;


    /* A null frame means that the title is visually empty, not that the canvas
     * itself should disappear. Draw the checkerboard, rulers, guides and other
     * editor chrome regardless, and only skip the artwork pixmap below. */
    double scale = view_scale();
    QPointF origin = view_origin();
    int dw = (int)(title_->width * scale);
    int dh = (int)(title_->height * scale);
    int ox = (int)origin.x();
    int oy = (int)origin.y();

    const QRectF canvas_rect_f(ox, oy, dw, dh);
    const QRect canvas_rect_px = canvas_rect_f.toAlignedRect();
    if (pass != CanvasPaintPass::Overlay)
        draw_static_checkerboard(p, canvas_rect_px);
    if (pass == CanvasPaintPass::Underlay)
        return;

    if (pass == CanvasPaintPass::All && !frame_image_.isNull()) {
        const int pix_x = ox + static_cast<int>(std::round(frame_image_canvas_offset_.x() * scale));
        const int pix_y = oy + static_cast<int>(std::round(frame_image_canvas_offset_.y() * scale));
        const double logical_pix_w = frame_image_preview_scale_ > 0.0
            ? frame_image_.width() / frame_image_preview_scale_ : frame_image_.width();
        const double logical_pix_h = frame_image_preview_scale_ > 0.0
            ? frame_image_.height() / frame_image_preview_scale_ : frame_image_.height();
        const int pix_w = static_cast<int>(std::round(logical_pix_w * scale));
        const int pix_h = static_cast<int>(std::round(logical_pix_h * scale));
        const bool scaled_artwork = pix_w != frame_image_.width() || pix_h != frame_image_.height();
        const bool adaptive_motion = adaptive_rendering_enabled_ && adaptive_interaction_active_;
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, scaled_artwork && !adaptive_motion);
        p.drawImage(QRect(pix_x, pix_y, pix_w, pix_h), frame_image_);
        p.restore();
    }

    p.save();
    p.setClipRect(QRectF(ox, oy, dw, dh));
    p.translate(origin);
    p.scale(scale, scale);
    for (const auto &layer : title_->layers) {
        if (!layer || !layer->visible || !layer->use_as_scene_mask)
            continue;

        const QRectF local_rect = layer_local_rect(*layer);
        if (!local_rect.isValid() || local_rect.isEmpty())
            continue;

        const double local_time = std::clamp(playhead_ - layer->in_time, 0.0,
                                             std::max(0.0, layer->out_time - layer->in_time));
        QPainterPath path = editor_scene_mask_layer_path(*layer, local_rect, local_time,
                                                          title_->id);
        QPen border(TitlePreferences::scene_mask_color(), layer->type == LayerType::Shape && layer->shape_type == ShapeType::Line ? 3.0 : 2.0);
        border.setCosmetic(true);

        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setTransform(editor_layer_world_transform(title_, *layer, playhead_), true);
        p.setPen(border);
        /* Scene masks remain matte controls in the editor. Preserve the
         * familiar hatched fill while the live OBS scene is visible through
         * the matte in the rendered artwork below. */
        p.setBrush(scene_mask_hatch_brush());
        p.drawPath(path);
        p.restore();
    }
    p.restore();

    draw_empty_image_placeholders(p);

    if (safe_guides_visible_) {
        auto draw_guide = [&](double inset, const QColor &color) {
            QRectF r(ox + dw * inset, oy + dh * inset, dw * (1.0 - 2.0 * inset), dh * (1.0 - 2.0 * inset));
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(color, 1.0, Qt::DashLine));
            p.drawRect(r);
        };
        draw_guide(OBS_ACTION_SAFE_PERCENT, editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::ActionSafe));
        draw_guide(OBS_GRAPHICS_SAFE_PERCENT, editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::GraphicsSafe));
    }

    draw_user_guides(p, QRectF(ox, oy, dw, dh));
    draw_hover_layer_box(p);

    if (!snap_feedback_.empty()) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setBrush(QColor(0, 20, 30, 180));
        for (const auto &feedback : snap_feedback_) {
            QPen guide_pen(editor_canvas_helper_color(snap_feedback_role_from_label(feedback.label)), 1.0, Qt::DashLine);
            guide_pen.setDashPattern({5.0, 3.0});
            guide_pen.setCosmetic(true);
            p.setPen(guide_pen);
            if (feedback.x_axis) {
                double x = canvas_to_view(QPointF(feedback.value, 0.0)).x();
                p.drawLine(QPointF(x, oy), QPointF(x, oy + dh));
                if (!feedback.label.isEmpty())
                    p.drawText(QRectF(x + 5.0, oy + 5.0, 120.0, 18.0), feedback.label);
            } else {
                double y = canvas_to_view(QPointF(0.0, feedback.value)).y();
                p.drawLine(QPointF(ox, y), QPointF(ox + dw, y));
                if (!feedback.label.isEmpty())
                    p.drawText(QRectF(ox + 5.0, y + 5.0, 120.0, 18.0), feedback.label);
            }
        }
        p.restore();
    }

    const SelectionOverlayGeometry &selection_overlay = selection_overlay_geometry();

    if (active_tool_ == CanvasTool::Selection && selection_overlay.layers.size() == 1 &&
        selection_overlay.layers.front().layer) {
        draw_position_motion_path(p, *selection_overlay.layers.front().layer);
    }

    auto draw_layer_box = [&](const SelectionOverlayLayerGeometry &layer_geometry, bool handles) {
        if (!layer_geometry.layer)
            return;
        const Layer &layer = *layer_geometry.layer;
        const bool editing_text_layer = layer_geometry.editing_text_layer;
        QColor outline_color = layer_color(layer, 0);
        if (!handles && outline_color.alpha() > 150)
            outline_color.setAlpha(150);
        QColor handle_stroke_color = layer_color(layer, 0);
        if (handle_stroke_color.alpha() < 180)
            handle_stroke_color.setAlpha(180);

        p.save();
        p.setBrush(Qt::NoBrush);
        const Qt::PenStyle outline_style = editing_text_layer ? Qt::SolidLine : Qt::DashLine;
        p.setPen(QPen(outline_color, editing_text_layer ? 2.0 : 1.5, outline_style));
        p.drawPolygon(layer_geometry.outline);
        if (handles) {
            p.setPen(QPen(handle_stroke_color, 1.0));
            p.setBrush(QColor(255, 255, 255));
            const double half_handle = CANVAS_CONTROL_SIZE_PX / 2.0;
            for (const QPointF &pt : layer_geometry.handles)
                p.drawRect(QRectF(pt.x() - half_handle, pt.y() - half_handle,
                                  CANVAS_CONTROL_SIZE_PX, CANVAS_CONTROL_SIZE_PX));

            QPointF origin = layer_geometry.origin;
            p.setPen(QPen(QColor(255, 160, 0), 1.5));
            p.setBrush(QColor(255, 220, 80));
            p.drawEllipse(origin, CANVAS_ORIGIN_RADIUS_PX, CANVAS_ORIGIN_RADIUS_PX);
            p.drawLine(QPointF(origin.x() - CANVAS_CONTROL_SIZE_PX, origin.y()),
                       QPointF(origin.x() + CANVAS_CONTROL_SIZE_PX, origin.y()));
            p.drawLine(QPointF(origin.x(), origin.y() - CANVAS_CONTROL_SIZE_PX),
                       QPointF(origin.x(), origin.y() + CANVAS_CONTROL_SIZE_PX));

            if (gradient_handles_visible())
                draw_gradient_handles(p, layer);
        }
        p.restore();
    };

    if (active_tool_ != CanvasTool::DirectSelection) {
        for (const auto &group_geometry : selection_overlay.group_contexts)
            draw_layer_box(group_geometry, false);
    }

    if (selection_overlay.layers.size() == 1) {
        if (active_tool_ != CanvasTool::DirectSelection)
            draw_layer_box(selection_overlay.layers.front(), true);
    } else if (selection_overlay.layers.size() > 1 && active_tool_ != CanvasTool::DirectSelection) {
        for (const auto &layer_geometry : selection_overlay.layers)
            draw_layer_box(layer_geometry, false);

        if (selection_overlay.multi_bounds_view.isValid() && !selection_overlay.multi_bounds_view.isEmpty()) {
            const QRectF &view_bounds = selection_overlay.multi_bounds_view;
            QColor aggregate_color = editor_canvas_helper_color(
                TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox);
            if (auto active_layer = selected_layer())
                aggregate_color = layer_color(*active_layer, 0);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(aggregate_color, 1.5, Qt::SolidLine));
            p.drawRect(view_bounds);
            p.setPen(QPen(aggregate_color, 1.0));
            p.setBrush(QColor(255, 255, 255));
            for (const QPointF &pt : selection_overlay.multi_handles)
                p.drawRect(QRectF(pt.x() - CANVAS_CONTROL_SIZE_PX / 2.0,
                                  pt.y() - CANVAS_CONTROL_SIZE_PX / 2.0,
                                  CANVAS_CONTROL_SIZE_PX, CANVAS_CONTROL_SIZE_PX));
        }
    }

    if (active_tool_ == CanvasTool::DirectSelection) {
        if (auto layer = selected_layer())
            draw_direct_selection_path(p, *layer);
    }
    const bool show_selected_live_corners =
        active_tool_ == CanvasTool::Selection ||
        active_tool_ == CanvasTool::DirectSelection ||
        active_tool_ == CanvasTool::Shape;
    if (show_selected_live_corners) {
        for (const auto &layer : selected_layers()) {
            if (layer && layer_supports_corner_radius_handles(*layer))
                draw_corner_radius_handles(p, *layer);
        }
    }
    if (title_ && !extension_canvas_handles_.isEmpty()) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        const auto handle_layer = selected_layer();
        for (const QJsonValue &value : extension_canvas_handles_) {
            const QJsonObject handle = value.toObject();
            const QJsonArray point = handle.value(QStringLiteral("value")).toArray();
            if (point.size() < 2) continue;
            QPointF canvasPoint;
            if (handle.value(QStringLiteral("space")).toString() == QStringLiteral("layer") &&
                handle_layer) {
                const QRectF rect = layer_local_rect(*handle_layer);
                const QPointF local(rect.left() + point.at(0).toDouble() * rect.width(),
                                    rect.top() + point.at(1).toDouble() * rect.height());
                canvasPoint = layer_to_canvas(*handle_layer, local);
            } else {
                canvasPoint = QPointF(point.at(0).toDouble() * title_->width,
                                      point.at(1).toDouble() * title_->height);
            }
            const QPointF viewPoint = canvas_to_view(canvasPoint);
            QColor color(handle.value(QStringLiteral("color")).toString(QStringLiteral("#ffb52e")));
            if (!color.isValid()) color = QColor(255, 181, 46);
            QPen pen(color, 2.0); pen.setCosmetic(true);
            p.setPen(pen); p.setBrush(QColor(255, 255, 255, 235));
            p.drawEllipse(viewPoint, 7.0, 7.0);
            p.drawLine(viewPoint + QPointF(-12.0, 0.0), viewPoint + QPointF(12.0, 0.0));
            p.drawLine(viewPoint + QPointF(0.0, -12.0), viewPoint + QPointF(0.0, 12.0));
            const QString label = handle.value(QStringLiteral("label")).toString();
            if (!label.isEmpty()) {
                p.setPen(QPen(color, 1.0));
                p.drawText(QRectF(viewPoint.x() + 14.0, viewPoint.y() - 10.0, 180.0, 22.0), label);
            }
        }
        p.restore();
    }
    draw_pen_path_preview(p);
    draw_toolbar_preview(p);
    draw_snap_cursor_indicator(p);

    QPolygonF text_caret;
    if (inline_text_caret_view_polygon(text_caret)) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(Qt::NoPen);
        p.setBrush(editor_canvas_helper_color(
            TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox));
        p.drawPolygon(text_caret);
        p.restore();
    }

    if (drag_mode_ == DragMode::Rotate) {
        QPointF pivot = canvas_to_view(drag_rotation_pivot_canvas_);
        double start_angle = std::atan2(drag_start_view_.y() - pivot.y(), drag_start_view_.x() - pivot.x());
        double radius = std::max(24.0, QLineF(pivot, drag_current_view_).length());
        QRectF arc_rect(pivot.x() - radius, pivot.y() - radius, radius * 2.0, radius * 2.0);
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 190, 40, 235), 1.5, Qt::DashLine));
        p.drawLine(pivot, drag_current_view_);
        p.setPen(QPen(QColor(255, 190, 40, 235), 2.0));
        p.drawEllipse(pivot, CANVAS_ORIGIN_RADIUS_PX + 2.0, CANVAS_ORIGIN_RADIUS_PX + 2.0);
        p.drawArc(arc_rect, (int)std::round(-radians_to_degrees(start_angle) * 16.0),
                  (int)std::round(-drag_current_rotation_delta_ * 16.0));
        p.restore();
    }

    if ((drag_mode_ == DragMode::Marquee && marquee_active_) ||
        (drag_mode_ == DragMode::PathMarquee && path_marquee_active_)) {
        QRectF marquee(drag_start_view_, drag_current_view_);
        marquee = marquee.normalized();
        QColor marquee_fill = editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox);
        marquee_fill.setAlpha(std::min(marquee_fill.alpha(), 45));
        p.setBrush(marquee_fill);
        p.setPen(QPen(editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox), 1.0, Qt::DashLine));
        p.drawRect(marquee);
    }

    draw_canvas_drag_tooltip(p);
    draw_color_picker_tooltip(p);
    draw_canvas_border(p, QRectF(ox, oy, dw, dh));
    draw_rulers(p, QRectF(ox, oy, dw, dh), scale, origin);
}

void CanvasPreview::set_extension_canvas_handles(const QJsonArray &handles)
{
    if (shutting_down_ || handles == extension_canvas_handles_)
        return;

    QString active_path;
    if (drag_mode_ == DragMode::ExtensionPoint && active_extension_handle_ >= 0 &&
        active_extension_handle_ < extension_canvas_handles_.size()) {
        active_path = extension_canvas_handles_.at(active_extension_handle_)
                          .toObject().value(QStringLiteral("path")).toString();
    }

    extension_canvas_handles_ = handles;
    if (!active_path.isEmpty()) {
        active_extension_handle_ = -1;
        for (int i = 0; i < extension_canvas_handles_.size(); ++i) {
            if (extension_canvas_handles_.at(i).toObject()
                    .value(QStringLiteral("path")).toString() == active_path) {
                active_extension_handle_ = i;
                break;
            }
        }
        if (active_extension_handle_ < 0)
            drag_mode_ = DragMode::None;
    } else if (drag_mode_ != DragMode::ExtensionPoint) {
        active_extension_handle_ = -1;
    }
    invalidate_canvas_overlay_caches();
    update();
}

void CanvasPreview::paintEvent(QPaintEvent *)
{
    if (shutting_down_)
        return;
    /* Artwork is presented through the editor-owned OBS GPU swap chain, so underlay/overlay QImage cache
     * changes are not a prerequisite for rendering a dirty GPU frame. */
    render_dirty_frame_if_due();
    if (ensure_gpu_display()) {
        const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
        const QSize pixel_size(std::max(1, qRound(width() * dpr)),
                               std::max(1, qRound(height() * dpr)));
        const QPointF origin = title_ ? view_origin() : QPointF();
        const double scale = title_ ? view_scale() : 1.0;
        const QString underlay_key = QStringLiteral("%1|%2x%3|%4|%5|%6|%7|%8|%9|%10")
            .arg(QString::number(static_cast<qulonglong>(
                     reinterpret_cast<quintptr>(title_.get())), 16))
            .arg(pixel_size.width()).arg(pixel_size.height())
            .arg(dpr, 0, 'f', 4)
            .arg(title_ ? title_->width : 0).arg(title_ ? title_->height : 0)
            .arg(scale, 0, 'f', 8)
            .arg(origin.x(), 0, 'f', 4).arg(origin.y(), 0, 'f', 4)
            .arg(QString::number(palette().cacheKey()) + QLatin1Char(':') +
                 QString::number(checkerboard_pattern_));

        QImage underlay;
        const bool update_underlay = gpu_underlay_key_ != underlay_key;
        if (update_underlay) {
            underlay = QImage(pixel_size, QImage::Format_ARGB32_Premultiplied);
            underlay.setDevicePixelRatio(dpr);
            underlay.fill(Qt::transparent);
            QPainter painter(&underlay);
            paint_canvas(painter, CanvasPaintPass::Underlay);
        }

        /* Every interaction path that changes handles, guides, tooltips or
         * marquee geometry explicitly invalidates the overlay. Do not force a
         * full-window QImage repaint/upload merely because a mouse button is
         * held; that was a major source of editor UI latency. */
        const bool update_overlay = gpu_overlay_dirty_ ||
            gpu_overlay_cache_.size() != pixel_size ||
            std::abs(gpu_overlay_cache_.devicePixelRatio() - dpr) > 0.001;
        if (update_overlay) {
            gpu_overlay_cache_ = QImage(pixel_size, QImage::Format_ARGB32_Premultiplied);
            gpu_overlay_cache_.setDevicePixelRatio(dpr);
            gpu_overlay_cache_.fill(Qt::transparent);
            QPainter painter(&gpu_overlay_cache_);
            paint_canvas(painter, CanvasPaintPass::Overlay);
            gpu_overlay_dirty_ = false;
        }

        QImage selection_mask;
        if (update_overlay) {
            selection_mask = QImage(pixel_size, QImage::Format_ARGB32_Premultiplied);
            selection_mask.setDevicePixelRatio(dpr);
            selection_mask.fill(Qt::transparent);
            if (inline_text_editor_ && inline_text_editor_->isVisible()) {
                const std::vector<QPolygonF> selection_polygons =
                    inline_text_selection_view_polygons();
                if (!selection_polygons.empty()) {
                    QPainter selection_painter(&selection_mask);
                    selection_painter.setRenderHint(QPainter::Antialiasing, false);
                    selection_painter.setClipRect(rect());
                    selection_painter.setPen(Qt::NoPen);
                    selection_painter.setBrush(Qt::white);
                    for (const QPolygonF &selection_polygon : selection_polygons)
                        selection_painter.drawPolygon(selection_polygon);
                }
            }
        }

        update_gpu_display_textures(update_underlay ? &underlay : nullptr,
                                    update_overlay ? &gpu_overlay_cache_ : nullptr,
                                    update_overlay ? &selection_mask : nullptr);
        if (update_underlay)
            gpu_underlay_key_ = underlay_key;

        const uint32_t display_w = static_cast<uint32_t>(pixel_size.width());
        const uint32_t display_h = static_cast<uint32_t>(pixel_size.height());
        if (gpu_display_->requested_w != display_w ||
            gpu_display_->requested_h != display_h) {
            gpu_display_->requested_w = display_w;
            gpu_display_->requested_h = display_h;
        }
        present_gpu_display_if_due();
        return;
    }

    /* GPU-only editor contract: never fall back to the retired CPU artwork
     * compositor. A failed OBS GPU swap chain is surfaced explicitly instead
     * of silently switching rendering backends. */
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(rect().adjusted(24, 24, -24, -24),
                     Qt::AlignCenter | Qt::TextWordWrap,
                     QStringLiteral("GPU canvas initialization failed. "
                                    "Broadcast Graphics Live requires the OBS GPU renderer."));
}

double CanvasPreview::toolbar_draw_aspect_ratio() const
{
    if (active_tool_ == CanvasTool::Text) {
        const double default_width = title_ ? std::max(1.0, title_->width * 0.5) : 960.0;
        return default_width / 160.0;
    }
    return 1.0;
}

static QRectF editor_modifier_rect(const QPointF &anchor, const QPointF &current,
                                   Qt::KeyboardModifiers modifiers, double aspect,
                                   double fixed_width, double fixed_height)
{
    QPointF delta = current - anchor;
    double width = fixed_width > 0.0 ? fixed_width : std::abs(delta.x());
    double height = fixed_height > 0.0 ? fixed_height : std::abs(delta.y());

    if (modifiers.testFlag(Qt::ShiftModifier)) {
        aspect = std::max(0.001, aspect);
        if (fixed_width > 0.0 && fixed_height <= 0.0)
            width = height * aspect;
        else if (fixed_height > 0.0 && fixed_width <= 0.0)
            height = width / aspect;
        else if (width / std::max(1.0, height) > aspect)
            height = width / aspect;
        else
            width = height * aspect;
    }

    const double sign_x = delta.x() < 0.0 ? -1.0 : 1.0;
    const double sign_y = delta.y() < 0.0 ? -1.0 : 1.0;
    QRectF rect;
    if (modifiers.testFlag(Qt::AltModifier)) {
        rect = QRectF(anchor.x() - width,
                      anchor.y() - height,
                      width * 2.0, height * 2.0);
    } else {
        const bool center_fixed_width = fixed_width > 0.0;
        const bool center_fixed_height = fixed_height > 0.0;
        const double left = center_fixed_width ? anchor.x() - width * 0.5 : std::min(anchor.x(), anchor.x() + sign_x * width);
        const double right = center_fixed_width ? anchor.x() + width * 0.5 : std::max(anchor.x(), anchor.x() + sign_x * width);
        const double top = center_fixed_height ? anchor.y() - height * 0.5 : std::min(anchor.y(), anchor.y() + sign_y * height);
        const double bottom = center_fixed_height ? anchor.y() + height * 0.5 : std::max(anchor.y(), anchor.y() + sign_y * height);
        rect = QRectF(QPointF(left, top), QPointF(right, bottom));
    }

    rect = rect.normalized();
    if (rect.width() < 1.0) rect.setWidth(1.0);
    if (rect.height() < 1.0) rect.setHeight(1.0);
    return rect;
}

QRectF CanvasPreview::toolbar_draw_rect(const QPointF &canvas_pt, Qt::KeyboardModifiers modifiers) const
{
    return editor_modifier_rect(shape_draw_start_canvas_, canvas_pt, modifiers, toolbar_draw_aspect_ratio());
}

QRectF CanvasPreview::snapped_toolbar_draw_rect(const QRectF &raw_rect, bool allow_snap)
{
    QRectF rect = raw_rect.normalized();
    if (!allow_snap || !rect.isValid()) {
        clear_snap_feedback();
        return rect;
    }

    return rect;
}

QRect CanvasPreview::toolbar_preview_update_rect() const
{
    if (!title_ || !drawing_shape_)
        return QRect();

    const QRectF canvas_rect = shape_draw_current_rect_.isValid()
                                  ? shape_draw_current_rect_
                                  : toolbar_draw_rect(shape_draw_current_canvas_, shape_draw_modifiers_);
    QRectF view_rect(canvas_to_view(canvas_rect.topLeft()), canvas_to_view(canvas_rect.bottomRight()));
    view_rect = view_rect.normalized();
    return view_rect.adjusted(-24.0, -24.0, 24.0, 24.0).toAlignedRect();
}

void CanvasPreview::draw_toolbar_preview(QPainter &p)
{
    if (!title_ || !drawing_shape_)
        return;

    const QRectF canvas_rect = shape_draw_current_rect_.isValid()
                                  ? shape_draw_current_rect_
                                  : toolbar_draw_rect(shape_draw_current_canvas_, shape_draw_modifiers_);
    QRectF view_rect(canvas_to_view(canvas_rect.topLeft()), canvas_to_view(canvas_rect.bottomRight()));
    view_rect = view_rect.normalized();

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool text_preview = active_tool_ == CanvasTool::Text;
    const bool image_preview = active_tool_ == CanvasTool::Image;
    const QColor accent = text_preview ? layer_type_color(active_text_layer_type_)
                                       : layer_type_color(image_preview ? LayerType::Image : LayerType::Shape);
    QColor fill = accent;
    fill.setAlpha(text_preview ? 38 : (image_preview ? 20 : (active_shape_type_ == ShapeType::Line ? 0 : 42)));
    QColor stroke = accent.lighter(135);
    stroke.setAlpha(235);

    QPen pen(stroke, active_shape_type_ == ShapeType::Line && !text_preview ? 2.0 : 1.6, Qt::DashLine);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(text_preview ? QColor(20, 20, 20, 90) : fill);

    if (text_preview) {
        p.drawRoundedRect(view_rect, 4.0, 4.0);
        QString label = text_tool_display_name(active_text_layer_type_);
        p.setPen(stroke);
        QFont label_font = p.font();
        label_font.setBold(true);
        p.setFont(label_font);
        p.drawText(view_rect.adjusted(8.0, 6.0, -8.0, -6.0), Qt::AlignLeft | Qt::AlignTop, label);
    } else if (image_preview) {
        p.drawRect(view_rect);
        p.save();
        QPainterPath clip_path;
        clip_path.addRect(view_rect);
        p.setClipPath(clip_path);
        QPen hatch_pen(stroke, 1.2, Qt::SolidLine);
        hatch_pen.setCosmetic(true);
        p.setPen(hatch_pen);
        const double spacing = 12.0;
        for (double offset = -view_rect.height(); offset <= view_rect.width(); offset += spacing) {
            p.drawLine(QPointF(view_rect.left() + offset, view_rect.bottom()),
                       QPointF(view_rect.left() + offset + view_rect.height(), view_rect.top()));
        }
        p.restore();
    } else {
        p.drawPath(tool_shape_path(active_shape_type_, view_rect));
    }

    p.restore();
}

void CanvasPreview::draw_empty_image_placeholders(QPainter &p)
{
    if (!title_)
        return;

    QColor color = editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox);
    if (!color.isValid())
        color = QColor(0, 170, 255, 220);
    QColor fill = color;
    fill.setAlpha(std::clamp(color.alpha() / 8, 12, 34));
    QColor hatch = color;
    hatch.setAlpha(std::clamp(color.alpha(), 110, 210));

    p.save();
    p.setClipRect(canvas_view_rect());
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const auto &layer : title_->layers) {
        if (!layer || layer->type != LayerType::Image || !layer->visible ||
            !layer->image_path.empty() || playhead_ < layer->in_time || playhead_ > layer->out_time)
            continue;

        const QRectF local_rect = layer_local_rect(*layer);
        if (!local_rect.isValid() || local_rect.isEmpty())
            continue;

        QPolygonF polygon;
        polygon << canvas_to_view(layer_to_canvas(*layer, local_rect.topLeft()))
                << canvas_to_view(layer_to_canvas(*layer, local_rect.topRight()))
                << canvas_to_view(layer_to_canvas(*layer, local_rect.bottomRight()))
                << canvas_to_view(layer_to_canvas(*layer, local_rect.bottomLeft()));
        QPainterPath clip_path;
        clip_path.addPolygon(polygon);
        clip_path.closeSubpath();
        p.save();
        p.setClipPath(clip_path);
        p.fillPath(clip_path, fill);
        QPen hatch_pen(hatch, 1.25, Qt::SolidLine);
        hatch_pen.setCosmetic(true);
        p.setPen(hatch_pen);

        // Draw the hatch in layer-local coordinates so it follows rotation and
        // scale, while keeping roughly constant screen-space line spacing.
        const QPointF local_zero_view = canvas_to_view(layer_to_canvas(*layer, QPointF(0.0, 0.0)));
        const QPointF local_x_view = canvas_to_view(layer_to_canvas(*layer, QPointF(1.0, 0.0)));
        const QPointF local_y_view = canvas_to_view(layer_to_canvas(*layer, QPointF(0.0, 1.0)));
        const double pixels_per_local_unit = std::max(
            0.01, (QLineF(local_zero_view, local_x_view).length() +
                   QLineF(local_zero_view, local_y_view).length()) * 0.5);
        const double spacing = 12.0 / pixels_per_local_unit;
        for (double offset = -local_rect.height(); offset <= local_rect.width(); offset += spacing) {
            const QPointF start_local(local_rect.left() + offset, local_rect.bottom());
            const QPointF end_local(local_rect.left() + offset + local_rect.height(), local_rect.top());
            p.drawLine(canvas_to_view(layer_to_canvas(*layer, start_local)),
                       canvas_to_view(layer_to_canvas(*layer, end_local)));
        }
        p.restore();
    }
    p.restore();
}

bool CanvasPreview::sample_color_at_view(const QPointF &view_pt, QColor &color)
{
    if (!title_ || !gpu_render_session_)
        return false;
    if (dirty_)
        render_to_frame();

