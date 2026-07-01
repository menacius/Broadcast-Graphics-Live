/* AE-style temporal Graph Editor.  This module owns only timeline UI state;
 * all values are sampled and edited through TimelinePropertyRef so editor,
 * prerender/cache and OBS continue to share animation.cpp evaluation. */

void TimelineWidget::set_graph_editor_enabled(bool enabled)
{
    if (graph_editor_enabled_ == enabled) return;
    graph_editor_enabled_ = enabled;
    drag_mode_ = DragMode::None;
    graph_fit_pending_ = true;
    unsetCursor();
    update();
    emit graph_editor_enabled_changed(enabled);
}

void TimelineWidget::set_graph_view_mode(int mode)
{
    const GraphViewMode next = mode == (int)GraphViewMode::Speed
        ? GraphViewMode::Speed : GraphViewMode::Value;
    if (graph_view_mode_ == next) return;
    graph_view_mode_ = next;
    graph_fit_pending_ = true;
    update();
    emit graph_view_mode_changed((int)graph_view_mode_);
}

QRect TimelineWidget::graph_rect() const
{
    return QRect(0, ruler_height(), width(), std::max(0, height() - ruler_height()));
}

bool TimelineWidget::graph_ref_property(const KeyframeRef &ref,
                                        TimelinePropertyRef *prop,
                                        std::shared_ptr<Layer> *layer) const
{
    if (!title_) return false;
    auto found_layer = title_->find_layer(ref.layer_id);
    if (!found_layer) return false;
    TimelinePropertyRef found = const_cast<TimelineWidget *>(this)
        ->find_timeline_property(*found_layer, ref.prop_name);
    if (!found || ref.index < 0 || ref.index >= (int)found.keyframe_count())
        return false;
    if (prop) *prop = found;
    if (layer) *layer = found_layer;
    return true;
}

TimelinePropertyRef TimelineWidget::active_graph_property(
    std::shared_ptr<Layer> *layer) const
{
    if (!title_) return {};
    for (const KeyframeRef &ref : selected_keyframes_) {
        TimelinePropertyRef prop;
        std::shared_ptr<Layer> owner;
        if (graph_ref_property(ref, &prop, &owner)) {
            if (layer) *layer = owner;
            return prop;
        }
    }
    auto choose_layer = [&](const std::string &id) -> TimelinePropertyRef {
        auto owner = title_->find_layer(id);
        if (!owner) return {};
        for (TimelinePropertyRef prop : timeline_properties(*owner)) {
            if (prop && prop.is_animated() && prop.keyframe_count() > 0) {
                if (layer) *layer = owner;
                return prop;
            }
        }
        return {};
    };
    if (!sel_layer_id_.empty()) {
        TimelinePropertyRef prop = choose_layer(sel_layer_id_);
        if (prop) return prop;
    }
    for (const std::string &id : selected_layer_ids_) {
        TimelinePropertyRef prop = choose_layer(id);
        if (prop) return prop;
    }
    return {};
}

double TimelineWidget::graph_value_to_y(double value) const
{
    const QRect area = graph_rect().adjusted(0, 8, 0, -8);
    const double span = std::max(1e-12, graph_value_max_ - graph_value_min_);
    return area.bottom() - (value - graph_value_min_) / span * area.height();
}

double TimelineWidget::graph_y_to_value(double y) const
{
    const QRect area = graph_rect().adjusted(0, 8, 0, -8);
    const double span = std::max(1e-12, graph_value_max_ - graph_value_min_);
    return graph_value_min_ + (area.bottom() - y) / std::max(1, area.height()) * span;
}

double TimelineWidget::graph_x_to_time(double x) const
{
    return (x + scroll_x_) / std::max(1e-9, pixels_per_sec_);
}

void TimelineWidget::update_graph_fit(bool selection_only)
{
    std::shared_ptr<Layer> owner;
    TimelinePropertyRef prop = active_graph_property(&owner);
    if (!prop || !title_ || !owner) {
        graph_value_min_ = -1.0;
        graph_value_max_ = 1.0;
        graph_fit_pending_ = false;
        return;
    }
    double minimum = std::numeric_limits<double>::infinity();
    double maximum = -std::numeric_limits<double>::infinity();
    auto include = [&](double value) {
        if (!std::isfinite(value)) return;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    };
    const auto sample_value = [&](double local_time) {
        return graph_view_mode_ == GraphViewMode::Speed
            ? prop.graph_speed(local_time) : prop.graph_value(local_time);
    };

    std::vector<int> selected_indices;
    for (const KeyframeRef &ref : selected_keyframes_) {
        if (ref.layer_id == owner->id && ref.prop_name == prop.name())
            selected_indices.push_back(ref.index);
    }
    if (selection_only && !selected_indices.empty()) {
        for (int index : selected_indices) {
            if (index < 0 || index >= (int)prop.keyframe_count()) continue;
            const double local_time = prop.keyframe_time((size_t)index);
            const double key_value = prop.graph_value(local_time);
            if (graph_view_mode_ == GraphViewMode::Speed) {
                include(prop.keyframe_temporal_speed((size_t)index, true));
                include(prop.keyframe_temporal_speed((size_t)index, false));
            } else {
                include(key_value);
                if (index > 0) {
                    const double seconds = (local_time - prop.keyframe_time((size_t)index - 1)) *
                        prop.keyframe_temporal_influence((size_t)index, true) / 100.0;
                    include(key_value - prop.keyframe_temporal_speed((size_t)index, true) * seconds);
                }
                if (index + 1 < (int)prop.keyframe_count()) {
                    const double seconds = (prop.keyframe_time((size_t)index + 1) - local_time) *
                        prop.keyframe_temporal_influence((size_t)index, false) / 100.0;
                    include(key_value + prop.keyframe_temporal_speed((size_t)index, false) * seconds);
                }
            }
        }
    } else {
        const int samples = std::clamp(width() / 3, 64, 1024);
        const double local_duration = std::max(0.0, owner->out_time - owner->in_time);
        for (int i = 0; i <= samples; ++i) {
            const double local_time = local_duration * (double)i / (double)samples;
            include(sample_value(local_time));
        }
        for (size_t i = 0; i < prop.keyframe_count(); ++i) {
            const double key_time = prop.keyframe_time(i);
            const double key_value = prop.graph_value(key_time);
            include(graph_view_mode_ == GraphViewMode::Speed
                ? prop.keyframe_temporal_speed(i, true) : key_value);
            include(graph_view_mode_ == GraphViewMode::Speed
                ? prop.keyframe_temporal_speed(i, false) : key_value);
            if (graph_view_mode_ == GraphViewMode::Value) {
                if (i > 0) {
                    const double seconds = (key_time - prop.keyframe_time(i - 1)) *
                        prop.keyframe_temporal_influence(i, true) / 100.0;
                    include(key_value - prop.keyframe_temporal_speed(i, true) * seconds);
                }
                if (i + 1 < prop.keyframe_count()) {
                    const double seconds = (prop.keyframe_time(i + 1) - key_time) *
                        prop.keyframe_temporal_influence(i, false) / 100.0;
                    include(key_value + prop.keyframe_temporal_speed(i, false) * seconds);
                }
            }
        }
    }
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
        minimum = -1.0;
        maximum = 1.0;
    }
    if (std::abs(maximum - minimum) < 1e-9) {
        const double pad = std::max(1.0, std::abs(maximum) * 0.1);
        minimum -= pad;
        maximum += pad;
    } else {
        const double pad = (maximum - minimum) * 0.08;
        minimum -= pad;
        maximum += pad;
    }
    graph_value_min_ = minimum;
    graph_value_max_ = maximum;
    graph_fit_pending_ = false;
}

void TimelineWidget::fit_graph_time_range(bool selection_only)
{
    std::shared_ptr<Layer> owner;
    TimelinePropertyRef prop = active_graph_property(&owner);
    if (!prop || !owner || !title_) return;
    double first = std::numeric_limits<double>::infinity();
    double last = -std::numeric_limits<double>::infinity();
    auto include_time = [&](double local_time) {
        const double global_time = owner->in_time + local_time;
        first = std::min(first, global_time);
        last = std::max(last, global_time);
    };
    if (selection_only) {
        for (const KeyframeRef &ref : selected_keyframes_) {
            if (ref.layer_id != owner->id || ref.prop_name != prop.name() ||
                ref.index < 0 || ref.index >= (int)prop.keyframe_count()) continue;
            include_time(prop.keyframe_time((size_t)ref.index));
        }
    }
    if (!std::isfinite(first) || !std::isfinite(last)) {
        for (size_t index = 0; index < prop.keyframe_count(); ++index)
            include_time(prop.keyframe_time(index));
    }
    if (!std::isfinite(first) || !std::isfinite(last)) {
        first = owner->in_time;
        last = owner->out_time;
    }
    const double minimum_span = std::max(obs_frame_duration() * 4.0, 0.05);
    if (last - first < minimum_span) {
        const double center = 0.5 * (first + last);
        first = center - minimum_span * 0.5;
        last = center + minimum_span * 0.5;
    }
    first = std::max(0.0, first);
    last = std::min(title_->duration, std::max(first + minimum_span, last));
    const int margin = 28;
    const double usable_width = std::max(1, width() - margin * 2);
    pixels_per_sec_ = std::clamp(usable_width / std::max(minimum_span, last - first),
                                 5.0, 1200.0);
    scroll_x_ = (int)std::round(first * pixels_per_sec_) - margin;
    clamp_scroll();
    emit zoom_percent_changed(zoom_percent());
}

void TimelineWidget::fit_graph_to_view()
{
    update_graph_fit(false);
    fit_graph_time_range(false);
    update();
}

void TimelineWidget::fit_graph_selection()
{
    update_graph_fit(true);
    fit_graph_time_range(true);
    update();
}

static QString temporal_mode_label(TemporalInterpolationMode mode)
{
    switch (mode) {
    case TemporalInterpolationMode::Linear: return bgl_tr("OBSTitles.Linear");
    case TemporalInterpolationMode::Hold: return bgl_tr("OBSTitles.Hold");
    case TemporalInterpolationMode::AutoBezier: return bgl_tr("OBSTitles.TemporalAutoBezier");
    case TemporalInterpolationMode::ContinuousBezier: return bgl_tr("OBSTitles.TemporalContinuousBezier");
    case TemporalInterpolationMode::ManualBezier: return bgl_tr("OBSTitles.TemporalManualBezier");
    }
    return bgl_tr("OBSTitles.Linear");
}

void TimelineWidget::paint_graph_editor(QPainter &p, const QRect &dirty)
{
    if (graph_fit_pending_) update_graph_fit(false);
    const QPalette pal = palette();
    const QColor background = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor grid = pal.color(QPalette::Mid);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor curve = pal.color(QPalette::Link);
    const QColor handle = pal.color(QPalette::BrightText);
    const QColor ruler = background.lightness() < 128 ? background.darker(116)
                                                      : background.darker(106);
    p.fillRect(dirty, background);
    const QRect header(0, 0, width(), ruler_height());
    p.fillRect(header.intersected(dirty), ruler);
    p.setPen(grid);
    p.drawLine(0, ruler_height() - 1, width(), ruler_height() - 1);

    TimelinePropertyRef prop;
    std::shared_ptr<Layer> owner;
    prop = active_graph_property(&owner);

    const QRect area = graph_rect();
    if (area.isEmpty()) return;

    /* Keep the ordinary time ruler visible in Graph Editor mode.  The graph
     * body uses its own clip, so ruler labels must be painted before that clip
     * is installed (the old code drew them outside the active clip). */
    const double seconds_per_major = std::max(1.0 / obs_frame_rate(),
        std::pow(10.0, std::floor(std::log10(std::max(1e-6, 80.0 / pixels_per_sec_)))));
    const double first_time = std::max(0.0, graph_x_to_time(0.0));
    double first_tick = std::floor(first_time / seconds_per_major) * seconds_per_major;
    const QFontMetrics ruler_metrics(p.font());
    int last_label_right = std::numeric_limits<int>::min();
    p.save();
    p.setClipRect(header.intersected(dirty));
    for (double tick = first_tick; tick <= (title_ ? title_->duration : 0.0);
         tick += seconds_per_major) {
        const int x = time_to_x(tick);
        if (x < 0 || x > width()) continue;
        p.setPen(QPen(grid, 0.8));
        p.drawLine(x, ruler_height() - 12, x, ruler_height() - 2);
        const QString label = format_timecode(tick);
        const int label_width = ruler_metrics.horizontalAdvance(label);
        const int label_left = x + 3;
        if (label_left > last_label_right + 6) {
            p.setPen(text);
            p.drawText(QRect(label_left, 18, label_width + 6, ruler_height() - 19),
                       Qt::AlignLeft | Qt::AlignVCenter, label);
            last_label_right = label_left + label_width;
        }
    }
    const int ruler_playhead_x = time_to_x(playhead_);
    const QColor playhead_color = TitlePreferences::timeline_color(
        TitlePreferences::TimelineColorRole::Current);
    p.setPen(QPen(playhead_color, 1.2));
    p.drawLine(ruler_playhead_x, 18, ruler_playhead_x, ruler_height() - 1);
    p.setBrush(playhead_color);
    p.setPen(Qt::NoPen);
    QPolygon playhead_marker;
    playhead_marker << QPoint(ruler_playhead_x - 5, 18)
                    << QPoint(ruler_playhead_x + 5, 18)
                    << QPoint(ruler_playhead_x, 25);
    p.drawPolygon(playhead_marker);
    p.restore();

    if (!prop) {
        p.setPen(text);
        p.drawText(graph_rect(), Qt::AlignCenter, bgl_tr("OBSTitles.GraphEditorNoProperty"));
        return;
    }

    p.setRenderHint(QPainter::Antialiasing, true);
    p.save();
    p.setClipRect(area.intersected(dirty));

    for (int line = 0; line <= 5; ++line) {
        const double ratio = (double)line / 5.0;
        const int y = area.top() + (int)std::round(ratio * area.height());
        const double value = graph_value_max_ - ratio * (graph_value_max_ - graph_value_min_);
        p.setPen(QPen(grid, line == 5 ? 1.0 : 0.5));
        p.drawLine(area.left(), y, area.right(), y);
        p.setPen(text);
        p.drawText(area.left() + 4, y - 2, QString::number(value, 'g', 5));
    }
    if (graph_view_mode_ == GraphViewMode::Speed && graph_value_min_ <= 0.0 && graph_value_max_ >= 0.0) {
        p.setPen(QPen(grid.lighter(140), 1.2));
        const int zero_y = (int)std::round(graph_value_to_y(0.0));
        p.drawLine(area.left(), zero_y, area.right(), zero_y);
    }

    for (double tick = first_tick; tick <= (title_ ? title_->duration : 0.0);
         tick += seconds_per_major) {
        const int x = time_to_x(tick);
        if (x < area.left() || x > area.right()) continue;
        p.setPen(QPen(grid, 0.5));
        p.drawLine(x, area.top(), x, area.bottom());
    }

    QPainterPath path;
    bool started = false;
    for (int x = area.left(); x <= area.right(); x += 2) {
        const double global_time = std::clamp(graph_x_to_time(x), 0.0, title_->duration);
        if (global_time < owner->in_time || global_time > owner->out_time) {
            started = false;
            continue;
        }
        const double local_time = global_time - owner->in_time;
        const double value = graph_view_mode_ == GraphViewMode::Speed
            ? prop.graph_speed(local_time) : prop.graph_value(local_time);
        if (!std::isfinite(value)) { started = false; continue; }
        const QPointF point(x, graph_value_to_y(value));
        if (!started) { path.moveTo(point); started = true; }
        else path.lineTo(point);
    }
    p.setPen(QPen(curve, 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    for (size_t index = 0; index < prop.keyframe_count(); ++index) {
        const double time = prop.keyframe_time(index);
        const double global_time = owner->in_time + time;
        const double in_speed = prop.keyframe_temporal_speed(index, true);
        const double out_speed = prop.keyframe_temporal_speed(index, false);
        const double key_value = prop.graph_value(time);
        const KeyframeRef ref{owner ? owner->id : std::string(), prop.name(), (int)index};
        const bool selected = selected_keyframes_.count(ref) != 0;
        p.setPen(QPen(selected ? highlight : curve.darker(140), 1.0));
        p.setBrush(selected ? highlight : background);
        QPointF value_vertex(time_to_x(global_time), graph_value_to_y(key_value));
        QPointF incoming_vertex(time_to_x(global_time), graph_value_to_y(in_speed));
        QPointF outgoing_vertex(time_to_x(global_time), graph_value_to_y(out_speed));
        if (graph_view_mode_ == GraphViewMode::Value) {
            p.drawEllipse(value_vertex, selected ? 4.5 : 3.5, selected ? 4.5 : 3.5);
        } else {
            if (index > 0)
                p.drawEllipse(incoming_vertex, selected ? 4.5 : 3.5, selected ? 4.5 : 3.5);
            if (index + 1 < prop.keyframe_count() &&
                (index == 0 || std::abs(out_speed - in_speed) > 1e-9))
                p.drawEllipse(outgoing_vertex, selected ? 4.5 : 3.5, selected ? 4.5 : 3.5);
        }

        if (!selected) continue;
        if (index > 0) {
            const double influence = prop.keyframe_temporal_influence(index, true) / 100.0;
            const double seconds = (time - prop.keyframe_time(index - 1)) * influence;
            const double handle_value = graph_view_mode_ == GraphViewMode::Speed
                ? in_speed : key_value - in_speed * seconds;
            const QPointF hp(time_to_x(global_time - seconds), graph_value_to_y(handle_value));
            const QPointF base = graph_view_mode_ == GraphViewMode::Speed
                ? incoming_vertex : value_vertex;
            p.setPen(QPen(handle, 1.0)); p.drawLine(base, hp);
            p.setBrush(highlight); p.drawRect(QRectF(hp.x() - 3, hp.y() - 3, 6, 6));
        }
        if (index + 1 < prop.keyframe_count()) {
            const double influence = prop.keyframe_temporal_influence(index, false) / 100.0;
            const double seconds = (prop.keyframe_time(index + 1) - time) * influence;
            const double handle_value = graph_view_mode_ == GraphViewMode::Speed
                ? out_speed : key_value + out_speed * seconds;
            const QPointF hp(time_to_x(global_time + seconds), graph_value_to_y(handle_value));
            const QPointF base = graph_view_mode_ == GraphViewMode::Speed
                ? outgoing_vertex : value_vertex;
            p.setPen(QPen(handle, 1.0)); p.drawLine(base, hp);
            p.setBrush(highlight); p.drawRect(QRectF(hp.x() - 3, hp.y() - 3, 6, 6));
        }
    }

    const int playhead_x = time_to_x(playhead_);
    p.setPen(QPen(TitlePreferences::timeline_color(TitlePreferences::TimelineColorRole::Current), 1.2));
    p.drawLine(playhead_x, area.top(), playhead_x, area.bottom());
    if (playhead_ >= owner->in_time && playhead_ <= owner->out_time) {
        const double local_playhead = playhead_ - owner->in_time;
        const double current = graph_view_mode_ == GraphViewMode::Speed
            ? prop.graph_speed(local_playhead) : prop.graph_value(local_playhead);
        p.setBrush(TitlePreferences::timeline_color(TitlePreferences::TimelineColorRole::Current));
        p.drawEllipse(QPointF(playhead_x, graph_value_to_y(current)), 3.5, 3.5);
    }

    if (drag_mode_ == DragMode::GraphMarquee) {
        p.setPen(QPen(highlight, 1.0, Qt::DashLine));
        QColor fill = highlight; fill.setAlpha(40); p.setBrush(fill);
        p.drawRect(QRect(graph_drag_start_, graph_marquee_current_).normalized());
    }
    p.restore();

    p.setPen(text);
    const QString view = graph_view_mode_ == GraphViewMode::Speed
        ? bgl_tr("OBSTitles.SpeedGraph") : bgl_tr("OBSTitles.ValueGraph");
    p.drawText(8, 0, width() - 16, 18, Qt::AlignVCenter | Qt::AlignLeft,
               QStringLiteral("%1 · %2 · %3").arg(view,
                   owner ? QString::fromStdString(owner->name) : QString(),
                   property_label(prop.name())));
}

TimelineWidget::GraphHit TimelineWidget::graph_hit_test(const QPoint &pos) const
{
    GraphHit result;
    if (!graph_editor_enabled_ || !graph_rect().contains(pos)) return result;
    std::shared_ptr<Layer> owner;
    TimelinePropertyRef prop = active_graph_property(&owner);
    if (!prop || !owner) return result;
    const double radius2 = 64.0;
    auto consider = [&](GraphHitType type, const KeyframeRef &ref, const QPointF &point) {
        const double dx = point.x() - pos.x(), dy = point.y() - pos.y();
        if (dx * dx + dy * dy <= radius2) {
            result.type = type; result.ref = ref; result.point = point;
            return true;
        }
        return false;
    };
    for (int index = (int)prop.keyframe_count() - 1; index >= 0; --index) {
        const double time = prop.keyframe_time((size_t)index);
        const double global_time = owner->in_time + time;
        const double in_speed = prop.keyframe_temporal_speed((size_t)index, true);
        const double out_speed = prop.keyframe_temporal_speed((size_t)index, false);
        const double key_value = prop.graph_value(time);
        const KeyframeRef ref{owner->id, prop.name(), index};
        if (selected_keyframes_.count(ref)) {
            if (index > 0) {
                const double seconds = (time - prop.keyframe_time((size_t)index - 1)) *
                    prop.keyframe_temporal_influence((size_t)index, true) / 100.0;
                const double value = graph_view_mode_ == GraphViewMode::Speed
                    ? in_speed : key_value - in_speed * seconds;
                if (consider(GraphHitType::IncomingHandle, ref,
                             QPointF(time_to_x(global_time - seconds), graph_value_to_y(value)))) return result;
            }
            if (index + 1 < (int)prop.keyframe_count()) {
                const double seconds = (prop.keyframe_time((size_t)index + 1) - time) *
                    prop.keyframe_temporal_influence((size_t)index, false) / 100.0;
                const double value = graph_view_mode_ == GraphViewMode::Speed
                    ? out_speed : key_value + out_speed * seconds;
                if (consider(GraphHitType::OutgoingHandle, ref,
                             QPointF(time_to_x(global_time + seconds), graph_value_to_y(value)))) return result;
            }
        }
        if (graph_view_mode_ == GraphViewMode::Speed) {
            if (index > 0 && consider(GraphHitType::Keyframe, ref,
                    QPointF(time_to_x(global_time), graph_value_to_y(in_speed)))) return result;
            if (index + 1 < (int)prop.keyframe_count() && consider(GraphHitType::Keyframe, ref,
                    QPointF(time_to_x(global_time), graph_value_to_y(out_speed)))) return result;
        } else if (consider(GraphHitType::Keyframe, ref,
                    QPointF(time_to_x(global_time), graph_value_to_y(key_value)))) return result;
    }
    return result;
}

std::vector<TimelineWidget::KeyframeRef> TimelineWidget::graph_edit_targets(
    const KeyframeRef &primary) const
{
    std::vector<KeyframeRef> targets;
    for (const KeyframeRef &ref : selected_keyframes_)
        if (ref.layer_id == primary.layer_id && ref.prop_name == primary.prop_name)
            targets.push_back(ref);
    if (targets.empty()) targets.push_back(primary);
    return targets;
}

bool TimelineWidget::graph_mouse_press(QMouseEvent *ev)
{
    if (!graph_editor_enabled_ || !title_) return false;
    /* The graph shares the ordinary timeline ruler.  Do not consume ruler
     * presses: the normal timeline handler owns playhead, loop and pause
     * marker interaction there. */
    if (ev->pos().y() < ruler_height()) return false;

    if (!graph_rect().contains(ev->pos())) return false;
    if (ev->button() == Qt::MiddleButton ||
        (ev->button() == Qt::LeftButton && (ev->modifiers() & Qt::AltModifier) &&
         graph_hit_test(ev->pos()).type == GraphHitType::None)) {
        drag_mode_ = DragMode::GraphPan;
        graph_drag_start_ = ev->pos();
        graph_drag_start_min_ = graph_value_min_;
        graph_drag_start_max_ = graph_value_max_;
        graph_drag_start_scroll_x_ = scroll_x_;
        setCursor(Qt::ClosedHandCursor);
        ev->accept();
        return true;
    }
    if (ev->button() != Qt::LeftButton) return false;
    const GraphHit hit = graph_hit_test(ev->pos());

    /* The playhead line remains directly draggable inside the graph body.
     * Keyframes and temporal handles take priority when they overlap it. */
    if (hit.type == GraphHitType::None &&
        std::abs(ev->pos().x() - time_to_x(playhead_)) <= 6) {
        drag_mode_ = DragMode::Playhead;
        emit playhead_changed(std::clamp(x_to_time(ev->pos().x()), 0.0, title_->duration));
        setCursor(Qt::SizeHorCursor);
        ev->accept();
        return true;
    }

    graph_drag_changed_ = false;
    graph_drag_start_ = ev->pos();
    graph_marquee_current_ = ev->pos();
    temporal_drag_snapshots_.clear();
    graph_drag_hit_ = hit;
    if (hit.type == GraphHitType::None) {
        drag_mode_ = DragMode::GraphMarquee;
        marquee_additive_ = ev->modifiers() & Qt::ShiftModifier;
        if (!marquee_additive_) selected_keyframes_.clear();
        update();
        ev->accept();
        return true;
    }
    const bool additive = ev->modifiers() & Qt::ShiftModifier;
    if (additive && hit.type == GraphHitType::Keyframe) {
        select_keyframe(hit.ref.layer_id, hit.ref.prop_name, hit.ref.index, true, true);
        update(); ev->accept(); return true;
    }
    if (!is_keyframe_selected(hit.ref.layer_id, hit.ref.prop_name, hit.ref.index))
        select_keyframe(hit.ref.layer_id, hit.ref.prop_name, hit.ref.index, false, false);
    for (const KeyframeRef &ref : graph_edit_targets(hit.ref)) {
        TimelinePropertyRef prop;
        if (!graph_ref_property(ref, &prop)) continue;
        TemporalDragSnapshot snapshot;
        snapshot.ref = ref;
        snapshot.incoming_influence = prop.keyframe_temporal_influence((size_t)ref.index, true);
        snapshot.outgoing_influence = prop.keyframe_temporal_influence((size_t)ref.index, false);
        snapshot.incoming_speed = prop.keyframe_temporal_speed((size_t)ref.index, true);
        snapshot.outgoing_speed = prop.keyframe_temporal_speed((size_t)ref.index, false);
        snapshot.time = prop.keyframe_time((size_t)ref.index);
        snapshot.value = prop.graph_value(snapshot.time);
        snapshot.linked = prop.keyframe_temporal_tangents_linked((size_t)ref.index);
        temporal_drag_snapshots_.push_back(snapshot);
    }
    drag_mode_ = hit.type == GraphHitType::IncomingHandle ? DragMode::GraphIncomingHandle
               : hit.type == GraphHitType::OutgoingHandle ? DragMode::GraphOutgoingHandle
               : DragMode::GraphKeyframe;
    setCursor(hit.type == GraphHitType::Keyframe ? Qt::ClosedHandCursor : Qt::CrossCursor);
    ev->accept();
    return true;
}

bool TimelineWidget::graph_mouse_move(QMouseEvent *ev)
{
    if (!graph_editor_enabled_ || !title_) return false;

    /* Non-graph drags (most importantly the shared playhead) must continue
     * through TimelineWidget::mouseMoveEvent instead of being swallowed by
     * graph hover handling. */
    const bool graph_drag = drag_mode_ == DragMode::GraphPan ||
                            drag_mode_ == DragMode::GraphMarquee ||
                            drag_mode_ == DragMode::GraphKeyframe ||
                            drag_mode_ == DragMode::GraphIncomingHandle ||
                            drag_mode_ == DragMode::GraphOutgoingHandle;
    if (!graph_drag && drag_mode_ != DragMode::None)
        return false;

    if (drag_mode_ == DragMode::GraphPan) {
        QPoint delta = ev->pos() - graph_drag_start_;
        if (ev->modifiers() & Qt::ShiftModifier) {
            if (std::abs(delta.x()) >= std::abs(delta.y())) delta.setY(0);
            else delta.setX(0);
        }
        scroll_x_ = graph_drag_start_scroll_x_ - delta.x();
        clamp_scroll();
        const double span = graph_drag_start_max_ - graph_drag_start_min_;
        const double value_delta = delta.y() * span / std::max(1, graph_rect().height());
        graph_value_min_ = graph_drag_start_min_ + value_delta;
        graph_value_max_ = graph_drag_start_max_ + value_delta;
        update(); ev->accept(); return true;
    }
    if (drag_mode_ == DragMode::GraphMarquee) {
        graph_marquee_current_ = ev->pos();
        update(); ev->accept(); return true;
    }
    if (drag_mode_ == DragMode::GraphKeyframe) {
        double time_delta = x_to_time(ev->pos().x()) - x_to_time(graph_drag_start_.x());
        double value_delta = graph_y_to_value(ev->pos().y()) - graph_y_to_value(graph_drag_start_.y());
        const QPoint pixel_delta = ev->pos() - graph_drag_start_;
        if (ev->modifiers() & Qt::ShiftModifier) {
            /* Shift constrains key movement to the dominant axis, matching the
             * ordinary canvas/timeline manipulation convention. */
            if (std::abs(pixel_delta.x()) >= std::abs(pixel_delta.y())) value_delta = 0.0;
            else time_delta = 0.0;
        }
        const bool snap_modifier =
            (ev->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) !=
            Qt::NoModifier;
        if (snap_modifier && !temporal_drag_snapshots_.empty() && std::abs(time_delta) > 1e-12) {
            const TemporalDragSnapshot &primary_snapshot = temporal_drag_snapshots_.front();
            std::shared_ptr<Layer> primary_layer;
            if (graph_ref_property(primary_snapshot.ref, nullptr, &primary_layer) && primary_layer) {
                const double desired_global = primary_layer->in_time +
                    primary_snapshot.time + time_delta;
                time_delta = snap_time(desired_global) -
                    (primary_layer->in_time + primary_snapshot.time);
            }
        }
        if (snap_modifier && std::abs(value_delta) > 1e-12) {
            const double span = std::max(1e-9, graph_value_max_ - graph_value_min_);
            const double raw_step = span / 100.0;
            const double value_step = std::pow(10.0, std::floor(std::log10(raw_step)));
            value_delta = std::round(value_delta / value_step) * value_step;
        }
        for (const TemporalDragSnapshot &snapshot : temporal_drag_snapshots_) {
            TimelinePropertyRef prop; std::shared_ptr<Layer> layer;
            if (!graph_ref_property(snapshot.ref, &prop, &layer) || !layer || layer->locked) continue;
            prop.set_keyframe_time((size_t)snapshot.ref.index,
                std::clamp(snapshot.time + time_delta, 0.0,
                           std::max(0.0, layer->out_time - layer->in_time)));
            if (graph_view_mode_ == GraphViewMode::Value && prop.graph_value_editable())
                prop.set_keyframe_graph_value((size_t)snapshot.ref.index,
                                              snapshot.value + value_delta);
        }
        graph_drag_changed_ = true;
        update(); ev->accept(); return true;
    }
    if (drag_mode_ == DragMode::GraphIncomingHandle ||
        drag_mode_ == DragMode::GraphOutgoingHandle) {
        const bool incoming = drag_mode_ == DragMode::GraphIncomingHandle;
        TimelinePropertyRef primary;
        if (!graph_ref_property(graph_drag_hit_.ref, &primary)) return true;
        const int index = graph_drag_hit_.ref.index;
        const double key_time = primary.keyframe_time((size_t)index);
        std::shared_ptr<Layer> primary_owner;
        if (!graph_ref_property(graph_drag_hit_.ref, nullptr, &primary_owner) || !primary_owner)
            return true;
        const double global_key_time = primary_owner->in_time + key_time;
        double interval = 0.0;
        if (incoming && index > 0) interval = key_time - primary.keyframe_time((size_t)index - 1);
        if (!incoming && index + 1 < (int)primary.keyframe_count())
            interval = primary.keyframe_time((size_t)index + 1) - key_time;
        if (!(interval > 1e-10)) return true;
        const double pointer_time = graph_x_to_time(ev->pos().x());
        double influence = (incoming ? global_key_time - pointer_time
                                     : pointer_time - global_key_time) / interval * 100.0;
        influence = std::clamp(influence, 0.0, 100.0);
        const double seconds = std::max(1e-10, interval * influence / 100.0);
        const double graph_value = graph_y_to_value(ev->pos().y());
        const double key_value = primary.graph_value(key_time);
        double speed = graph_view_mode_ == GraphViewMode::Speed
            ? graph_value
            : incoming ? (key_value - graph_value) / seconds
                       : (graph_value - key_value) / seconds;
        const TemporalDragSnapshot *primary_snapshot = nullptr;
        for (const auto &snapshot : temporal_drag_snapshots_)
            if (!(snapshot.ref < graph_drag_hit_.ref) && !(graph_drag_hit_.ref < snapshot.ref)) {
                primary_snapshot = &snapshot; break;
            }
        if (!primary_snapshot) return true;
        const double original_influence = incoming
            ? primary_snapshot->incoming_influence : primary_snapshot->outgoing_influence;
        const double original_speed = incoming
            ? primary_snapshot->incoming_speed : primary_snapshot->outgoing_speed;
        const QPoint handle_delta = ev->pos() - graph_drag_start_;
        if (ev->modifiers() & Qt::ShiftModifier) {
            /* Shift locks handle editing to influence (horizontal) or speed
             * (vertical), preventing accidental changes to the other axis. */
            if (std::abs(handle_delta.x()) >= std::abs(handle_delta.y()))
                speed = original_speed;
            else
                influence = original_influence;
        }
        if (ev->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) {
            influence = std::round(influence / 5.0) * 5.0;
            const double speed_step = std::max(0.01,
                std::pow(10.0, std::floor(std::log10(std::max(0.01, std::abs(speed)))) - 1.0));
            speed = std::round(speed / speed_step) * speed_step;
        }
        influence = std::clamp(influence, 0.0, 100.0);
        const double influence_delta = influence - original_influence;
        const double speed_delta = speed - original_speed;
        for (const TemporalDragSnapshot &snapshot : temporal_drag_snapshots_) {
            TimelinePropertyRef prop; std::shared_ptr<Layer> layer;
            if (!graph_ref_property(snapshot.ref, &prop, &layer) || !layer || layer->locked) continue;
            const double next_influence = (incoming ? snapshot.incoming_influence
                                                    : snapshot.outgoing_influence) + influence_delta;
            const double next_speed = (incoming ? snapshot.incoming_speed
                                                : snapshot.outgoing_speed) + speed_delta;
            const bool break_pair = (ev->modifiers() & Qt::AltModifier) || !snapshot.linked;
            prop.set_temporal_handle((size_t)snapshot.ref.index, incoming,
                                     next_influence, next_speed, break_pair);
        }
        graph_drag_changed_ = true;
        graph_fit_pending_ = false;
        update(); ev->accept(); return true;
    }
    if (!graph_rect().contains(ev->pos()))
        return false;

    const GraphHit hover = graph_hit_test(ev->pos());
    setCursor(hover.type == GraphHitType::None ? Qt::ArrowCursor
              : hover.type == GraphHitType::Keyframe ? Qt::OpenHandCursor
              : Qt::CrossCursor);
    return true;
}

bool TimelineWidget::graph_mouse_release(QMouseEvent *ev)
{
    if (!graph_editor_enabled_) return false;
    const bool graph_drag = drag_mode_ == DragMode::GraphPan ||
                            drag_mode_ == DragMode::GraphMarquee ||
                            drag_mode_ == DragMode::GraphKeyframe ||
                            drag_mode_ == DragMode::GraphIncomingHandle ||
                            drag_mode_ == DragMode::GraphOutgoingHandle;
    if (!graph_drag) return false;

    if (drag_mode_ == DragMode::GraphMarquee) {
        const QRect marquee(graph_drag_start_, graph_marquee_current_);
        std::shared_ptr<Layer> owner;
        TimelinePropertyRef prop = active_graph_property(&owner);
        if (prop && owner) {
            for (size_t index = 0; index < prop.keyframe_count(); ++index) {
                const double time = prop.keyframe_time(index);
                const int x = time_to_x(owner->in_time + time);
                bool contained = false;
                if (graph_view_mode_ == GraphViewMode::Speed) {
                    if (index > 0)
                        contained = marquee.normalized().contains(QPoint(x, (int)std::round(
                            graph_value_to_y(prop.keyframe_temporal_speed(index, true)))));
                    if (!contained && index + 1 < prop.keyframe_count())
                        contained = marquee.normalized().contains(QPoint(x, (int)std::round(
                            graph_value_to_y(prop.keyframe_temporal_speed(index, false)))));
                } else {
                    contained = marquee.normalized().contains(QPoint(x, (int)std::round(
                        graph_value_to_y(prop.graph_value(time)))));
                }
                if (contained)
                    selected_keyframes_.insert({owner->id, prop.name(), (int)index});
            }
        }
    }
    if (drag_mode_ == DragMode::GraphKeyframe && graph_drag_changed_) {
        // Capture the selected keyframes by their final times before sorting.
        // This preserves selection identity even when keys cross during a drag.
        std::map<KeyframeRef, double> selected_times;
        for (const KeyframeRef &ref : selected_keyframes_) {
            TimelinePropertyRef prop;
            if (graph_ref_property(ref, &prop) && ref.index >= 0 &&
                ref.index < (int)prop.keyframe_count())
                selected_times[ref] = prop.keyframe_time((size_t)ref.index);
        }

        std::set<std::pair<std::string, std::string>> touched;
        for (const auto &snapshot : temporal_drag_snapshots_)
            touched.insert({snapshot.ref.layer_id, snapshot.ref.prop_name});
        for (const auto &entry : touched) {
            if (auto layer = title_->find_layer(entry.first)) {
                TimelinePropertyRef prop = find_timeline_property(*layer, entry.second);
                if (prop) prop.sort_keyframes();
            }
        }

        std::set<KeyframeRef> remapped;
        std::map<std::pair<std::string, std::string>, std::set<int>> used_indices;
        for (const auto &[ref, selected_time] : selected_times) {
            auto layer = title_->find_layer(ref.layer_id);
            TimelinePropertyRef prop = layer
                ? find_timeline_property(*layer, ref.prop_name)
                : TimelinePropertyRef{};
            if (!prop) continue;
            const auto property_key = std::make_pair(ref.layer_id, ref.prop_name);
            int best = -1;
            double best_distance = std::numeric_limits<double>::max();
            for (int index = 0; index < (int)prop.keyframe_count(); ++index) {
                if (used_indices[property_key].count(index)) continue;
                const double distance = std::abs(prop.keyframe_time((size_t)index) -
                                                 selected_time);
                if (distance < best_distance) {
                    best = index;
                    best_distance = distance;
                }
            }
            if (best >= 0) {
                used_indices[property_key].insert(best);
                remapped.insert({ref.layer_id, ref.prop_name, best});
            }
        }
        selected_keyframes_ = std::move(remapped);
    }
    const bool changed = graph_drag_changed_;
    drag_mode_ = DragMode::None;
    graph_drag_hit_ = {};
    temporal_drag_snapshots_.clear();
    graph_drag_changed_ = false;
    unsetCursor();
    update();
    if (changed) emit keyframe_easing_changed();
    if (ev) ev->accept();
    return true;
}

void TimelineWidget::show_property_velocity_dialog(const std::string &layer_id,
                                                   const std::string &property_name,
                                                   double local_time)
{
    if (!title_) return;
    auto layer = title_->find_layer(layer_id);
    if (!layer || layer->locked) return;
    TimelinePropertyRef prop = find_timeline_property(*layer, property_name);
    if (!prop || prop.keyframe_count() == 0) return;
    const double tolerance = std::max(1e-6, obs_frame_duration() * 0.5 + 1e-6);
    int index = -1;
    double best_distance = std::numeric_limits<double>::infinity();
    for (int candidate = 0; candidate < static_cast<int>(prop.keyframe_count()); ++candidate) {
        const double distance = std::abs(prop.keyframe_time(static_cast<size_t>(candidate)) - local_time);
        if (distance < best_distance) {
            best_distance = distance;
            index = candidate;
        }
    }
    if (index < 0 || best_distance > tolerance) return;
    show_temporal_velocity_dialog(prop, {index});
}

void TimelineWidget::show_temporal_velocity_dialog(TimelinePropertyRef prop,
                                                   const std::vector<int> &indices)
{
    if (!prop || indices.empty()) return;
    const int first = indices.front();
    if (first < 0 || first >= (int)prop.keyframe_count()) return;
    QDialog dialog(this);
    dialog.setWindowTitle(bgl_tr("OBSTitles.KeyframeVelocity"));
    auto *layout = new QFormLayout(&dialog);
    auto *mode = new QComboBox(&dialog);
    for (TemporalInterpolationMode item : {TemporalInterpolationMode::Linear,
             TemporalInterpolationMode::Hold, TemporalInterpolationMode::AutoBezier,
             TemporalInterpolationMode::ContinuousBezier,
             TemporalInterpolationMode::ManualBezier})
        mode->addItem(temporal_mode_label(item), (int)item);
    mode->setCurrentIndex(std::max(0, mode->findData((int)prop.keyframe_temporal_mode((size_t)first))));
    auto make_spin = [&](double value, bool influence) {
        auto *spin = new QDoubleSpinBox(&dialog);
        spin->setDecimals(6);
        spin->setRange(influence ? 0.0 : -1.0e15, influence ? 100.0 : 1.0e15);
        spin->setValue(value);
        if (influence) spin->setSuffix(QStringLiteral(" %"));
        return spin;
    };
    auto *influence_in = make_spin(prop.keyframe_temporal_influence((size_t)first, true), true);
    auto *speed_in = make_spin(prop.keyframe_temporal_speed((size_t)first, true), false);
    auto *influence_out = make_spin(prop.keyframe_temporal_influence((size_t)first, false), true);
    auto *speed_out = make_spin(prop.keyframe_temporal_speed((size_t)first, false), false);
    layout->addRow(bgl_tr("OBSTitles.TemporalMode"), mode);
    layout->addRow(bgl_tr("OBSTitles.IncomingInfluence"), influence_in);
    layout->addRow(bgl_tr("OBSTitles.IncomingSpeed"), speed_in);
    layout->addRow(bgl_tr("OBSTitles.OutgoingInfluence"), influence_out);
    layout->addRow(bgl_tr("OBSTitles.OutgoingSpeed"), speed_out);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;

    const double base_in_inf = prop.keyframe_temporal_influence((size_t)first, true);
    const double base_out_inf = prop.keyframe_temporal_influence((size_t)first, false);
    const double base_in_speed = prop.keyframe_temporal_speed((size_t)first, true);
    const double base_out_speed = prop.keyframe_temporal_speed((size_t)first, false);
    const double delta_in_inf = influence_in->value() - base_in_inf;
    const double delta_out_inf = influence_out->value() - base_out_inf;
    const double delta_in_speed = speed_in->value() - base_in_speed;
    const double delta_out_speed = speed_out->value() - base_out_speed;
    const auto selected_mode = (TemporalInterpolationMode)mode->currentData().toInt();
    for (int index : indices) {
        if (index < 0 || index >= (int)prop.keyframe_count()) continue;
        const double next_in_inf = prop.keyframe_temporal_influence((size_t)index, true) + delta_in_inf;
        const double next_out_inf = prop.keyframe_temporal_influence((size_t)index, false) + delta_out_inf;
        const double next_in_speed = prop.keyframe_temporal_speed((size_t)index, true) + delta_in_speed;
        const double next_out_speed = prop.keyframe_temporal_speed((size_t)index, false) + delta_out_speed;
        prop.apply_temporal_mode((size_t)index, selected_mode);
        if (selected_mode == TemporalInterpolationMode::ManualBezier ||
            selected_mode == TemporalInterpolationMode::ContinuousBezier) {
            prop.set_temporal_handle((size_t)index, true, next_in_inf, next_in_speed, true);
            prop.set_temporal_handle((size_t)index, false, next_out_inf, next_out_speed, true);
            if (selected_mode == TemporalInterpolationMode::ContinuousBezier)
                prop.apply_temporal_mode((size_t)index, selected_mode);
        }
    }
    update();
    emit keyframe_easing_changed();
}

void TimelineWidget::graph_context_menu(QContextMenuEvent *ev)
{
    if (!title_) return;
    const GraphHit hit = graph_hit_test(ev->pos());
    if (hit.type != GraphHitType::None &&
        !is_keyframe_selected(hit.ref.layer_id, hit.ref.prop_name, hit.ref.index))
        select_keyframe(hit.ref.layer_id, hit.ref.prop_name, hit.ref.index, false, false);
    std::shared_ptr<Layer> owner;
    TimelinePropertyRef prop = active_graph_property(&owner);
    QMenu menu(this);
    QAction *value_graph = menu.addAction(bgl_tr("OBSTitles.ValueGraph"));
    QAction *speed_graph = menu.addAction(bgl_tr("OBSTitles.SpeedGraph"));
    value_graph->setCheckable(true); speed_graph->setCheckable(true);
    value_graph->setChecked(graph_view_mode_ == GraphViewMode::Value);
    speed_graph->setChecked(graph_view_mode_ == GraphViewMode::Speed);
    menu.addSeparator();
    QAction *fit_all = menu.addAction(bgl_tr("OBSTitles.FitGraphsToView"));
    QAction *fit_selection = menu.addAction(bgl_tr("OBSTitles.FitGraphSelection"));
    fit_selection->setEnabled(!selected_keyframes_.empty());
    menu.addSeparator();

    std::vector<int> indices;
    if (prop && owner) {
        for (const KeyframeRef &ref : selected_keyframes_)
            if (ref.layer_id == owner->id && ref.prop_name == prop.name()) indices.push_back(ref.index);
    }
    QAction *easy = nullptr, *easy_in = nullptr, *easy_out = nullptr, *velocity = nullptr;
    std::vector<std::pair<QAction *, TemporalInterpolationMode>> mode_actions;
    if (prop && !indices.empty()) {
        QMenu *interpolation = menu.addMenu(bgl_tr("OBSTitles.TemporalInterpolation"));
        for (TemporalInterpolationMode mode : {TemporalInterpolationMode::Linear,
                 TemporalInterpolationMode::Hold, TemporalInterpolationMode::AutoBezier,
                 TemporalInterpolationMode::ContinuousBezier,
                 TemporalInterpolationMode::ManualBezier}) {
            QAction *action = interpolation->addAction(temporal_mode_label(mode));
            action->setCheckable(true);
            action->setChecked(std::all_of(indices.begin(), indices.end(), [&](int index) {
                return prop.keyframe_temporal_mode((size_t)index) == mode;
            }));
            mode_actions.push_back({action, mode});
        }
        interpolation->addSeparator();
        easy = interpolation->addAction(bgl_tr("OBSTitles.EasyEase"));
        easy_in = interpolation->addAction(bgl_tr("OBSTitles.EasyEaseIn"));
        easy_out = interpolation->addAction(bgl_tr("OBSTitles.EasyEaseOut"));
        velocity = menu.addAction(bgl_tr("OBSTitles.KeyframeVelocity"));
    }
    QAction *chosen = menu.exec(ev->globalPos());
    if (!chosen) return;
    if (chosen == value_graph) { set_graph_view_mode((int)GraphViewMode::Value); return; }
    if (chosen == speed_graph) { set_graph_view_mode((int)GraphViewMode::Speed); return; }
    if (chosen == fit_all) { fit_graph_to_view(); return; }
    if (chosen == fit_selection) { fit_graph_selection(); return; }
    if (chosen == velocity) { show_temporal_velocity_dialog(prop, indices); return; }
    for (const auto &[action, mode] : mode_actions) {
        if (chosen != action) continue;
        for (int index : indices) prop.apply_temporal_mode((size_t)index, mode);
        update(); emit keyframe_easing_changed(); return;
    }
    if (chosen == easy || chosen == easy_in || chosen == easy_out) {
        const bool in = chosen == easy || chosen == easy_in;
        const bool out = chosen == easy || chosen == easy_out;
        for (int index : indices) prop.apply_easy_ease((size_t)index, in, out);
        update(); emit keyframe_easing_changed();
    }
}
