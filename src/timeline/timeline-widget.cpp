#include "title-editor-internal.h"

TimelineWidget::TimelineWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
}

void TimelineWidget::set_title(std::shared_ptr<Title> t)
{
    const bool title_changed = t != title_;
    title_ = t;
    if (title_changed) {
        scroll_x_ = 0;
        fit_on_next_resize_ = true;
        selected_keyframes_.clear();
    } else {
        prune_keyframe_selection();
    }
    clamp_scroll();
    clamp_vertical_scroll();
    if (fit_on_next_resize_ && width() > 40) {
        fit_on_next_resize_ = false;
        fit_timeline();
        return;
    }
    update();
}

void TimelineWidget::set_selected_layer(const std::string &lid)
{
    sel_layer_id_ = lid; update();
}

void TimelineWidget::set_playhead(double t)
{
    playhead_ = snap_time(t);
    if (title_)
        keep_playhead_visible();
    update();
}

void TimelineWidget::set_vertical_scroll(int scroll_y)
{
    scroll_y_ = scroll_y;
    clamp_vertical_scroll();
    update();
}

void TimelineWidget::set_pixels_per_sec(double pixels_per_sec, double anchor_time, int anchor_x)
{
    pixels_per_sec_ = std::clamp(pixels_per_sec, 5.0, 1200.0);
    scroll_x_ = (int)std::round(anchor_time * pixels_per_sec_) - anchor_x;
    clamp_scroll();
    keep_playhead_visible();
    update();
    emit zoom_percent_changed(zoom_percent());
}

void TimelineWidget::set_zoom_percent(int percent)
{
    int clamped = std::clamp(percent, 5, 1200);
    double anchor_time = title_ ? std::clamp(playhead_, 0.0, title_->duration) : playhead_;
    int anchor_x = std::clamp(time_to_x(anchor_time), 24, std::max(24, width() - 24));
    set_pixels_per_sec((double)clamped, anchor_time, anchor_x);
}

int TimelineWidget::zoom_percent() const
{
    return (int)std::round(pixels_per_sec_);
}

void TimelineWidget::fit_timeline()
{
    double dur = title_ ? std::max(obs_frame_duration(), title_->duration) : 10.0;
    double fitted = (double)std::max(1, width() - 40) / dur;
    set_pixels_per_sec(fitted, 0.0, 0);
}

bool TimelineWidget::has_selected_keyframes() const
{
    return title_ && !selected_keyframes_.empty();
}

bool TimelineWidget::has_keyframe_clipboard() const
{
    return !keyframe_clipboard_.empty();
}

bool TimelineWidget::copy_keyframe_selection()
{
    return copy_selected_keyframes();
}

bool TimelineWidget::cut_keyframe_selection()
{
    if (!cut_selected_keyframes()) return false;
    emit keyframe_easing_changed();
    return true;
}

bool TimelineWidget::delete_keyframe_selection()
{
    if (!delete_selected_keyframes()) return false;
    emit keyframe_easing_changed();
    return true;
}

bool TimelineWidget::paste_keyframes_at_playhead()
{
    if (!paste_keyframes_at(playhead_)) return false;
    emit keyframe_easing_changed();
    return true;
}

bool TimelineWidget::keep_playhead_visible()
{
    if (!title_) return false;
    int phx = time_to_x(playhead_);
    int old_scroll = scroll_x_;
    if (phx < 24)
        scroll_x_ = std::max(0, (int)std::round(playhead_ * pixels_per_sec_) - 24);
    if (phx > width() - 24)
        scroll_x_ = std::max(0, (int)std::round(playhead_ * pixels_per_sec_) - width() + 24);
    clamp_scroll();
    return old_scroll != scroll_x_;
}

double TimelineWidget::x_to_time(int x) const
{
    return snap_time((x + scroll_x_) / pixels_per_sec_);
}

int TimelineWidget::time_to_x(double t) const
{
    return (int)std::round(t * pixels_per_sec_) - scroll_x_;
}

double TimelineWidget::snap_time(double t) const
{
    return snap_to_obs_frame(t);
}

void TimelineWidget::clamp_scroll()
{
    double dur = title_ ? title_->duration : 10.0;
    int max_scroll = std::max(0, (int)std::ceil(dur * pixels_per_sec_) - width() + 40);
    scroll_x_ = std::clamp(scroll_x_, 0, max_scroll);
}

int TimelineWidget::max_vertical_scroll() const
{
    int content_height = (int)timeline_rows(title_).size() * row_height();
    int viewport_height = std::max(0, height() - ruler_height());
    return std::max(0, content_height - viewport_height);
}

void TimelineWidget::clamp_vertical_scroll()
{
    scroll_y_ = std::clamp(scroll_y_, 0, max_vertical_scroll());
}


bool TimelineWidget::KeyframeRef::operator<(const KeyframeRef &other) const
{
    return std::tie(layer_id, prop_name, index) <
           std::tie(other.layer_id, other.prop_name, other.index);
}

AnimatedProperty *TimelineWidget::find_timeline_property(Layer &layer, const std::string &prop_name) const
{
    for (auto *prop : timeline_properties(layer)) {
        if (prop->name == prop_name)
            return prop;
    }
    return nullptr;
}

void TimelineWidget::clear_keyframe_selection()
{
    if (selected_keyframes_.empty()) return;
    selected_keyframes_.clear();
    update();
}

void TimelineWidget::prune_keyframe_selection()
{
    if (!title_) {
        selected_keyframes_.clear();
        return;
    }

    for (auto it = selected_keyframes_.begin(); it != selected_keyframes_.end();) {
        auto layer = title_->find_layer(it->layer_id);
        AnimatedProperty *prop = layer ? find_timeline_property(*layer, it->prop_name) : nullptr;
        if (!layer || !prop || it->index < 0 || it->index >= (int)prop->keyframes.size())
            it = selected_keyframes_.erase(it);
        else
            ++it;
    }
}

bool TimelineWidget::is_keyframe_selected(const std::string &layer_id, const std::string &prop_name, int kf_idx) const
{
    return selected_keyframes_.find({layer_id, prop_name, kf_idx}) != selected_keyframes_.end();
}

void TimelineWidget::select_keyframe(const std::string &layer_id, const std::string &prop_name,
                                     int kf_idx, bool additive, bool toggle)
{
    KeyframeRef ref{layer_id, prop_name, kf_idx};
    if (!additive)
        selected_keyframes_.clear();
    if (toggle && selected_keyframes_.find(ref) != selected_keyframes_.end())
        selected_keyframes_.erase(ref);
    else
        selected_keyframes_.insert(ref);
    update();
}

QRect TimelineWidget::marquee_rect() const
{
    return QRect(marquee_start_, marquee_current_).normalized()
        .intersected(QRect(0, ruler_height(), width(), std::max(0, height() - ruler_height())));
}

void TimelineWidget::select_keyframes_in_rect(const QRect &rect, bool additive)
{
    if (!title_) return;
    std::set<KeyframeRef> selection = additive ? selected_keyframes_ : std::set<KeyframeRef>{};
    auto rows = timeline_rows(title_);
    const QRect visible_timeline(0, ruler_height(), width(), std::max(0, height() - ruler_height()));
    QRect bounded = rect.normalized().intersected(visible_timeline);
    if (bounded.isEmpty()) {
        selected_keyframes_ = std::move(selection);
        update();
        return;
    }

    for (int row = 0; row < (int)rows.size(); ++row) {
        const auto &entry = rows[row];
        if (!entry.is_property || !entry.layer || !entry.prop) continue;
        if (!entry.layer->properties_expanded || entry.layer->locked) continue;
        int y = ruler_height() + row * row_height() - scroll_y_;
        int ky = y + row_height() / 2;
        if (ky < visible_timeline.top() || ky > visible_timeline.bottom()) continue;
        if (ky < bounded.top() || ky > bounded.bottom()) continue;
        for (int i = 0; i < (int)entry.prop->keyframes.size(); ++i) {
            const auto &kf = entry.prop->keyframes[i];
            int kx = time_to_x(entry.layer->in_time + kf.time);
            if (kx < visible_timeline.left() || kx > visible_timeline.right()) continue;
            if (bounded.contains(QPoint(kx, ky)))
                selection.insert({entry.layer->id, entry.prop->name, i});
        }
    }
    selected_keyframes_ = std::move(selection);
    update();
}

bool TimelineWidget::copy_selected_keyframes()
{
    if (!title_) return false;
    prune_keyframe_selection();
    if (selected_keyframes_.empty()) return false;

    struct PendingCopy {
        std::string layer_id;
        std::string prop_name;
        Keyframe keyframe;
        double timeline_time = 0.0;
    };
    std::vector<PendingCopy> pending;
    double origin = std::numeric_limits<double>::max();

    for (const auto &ref : selected_keyframes_) {
        auto layer = title_->find_layer(ref.layer_id);
        AnimatedProperty *prop = layer ? find_timeline_property(*layer, ref.prop_name) : nullptr;
        if (!layer || !prop || ref.index < 0 || ref.index >= (int)prop->keyframes.size()) continue;
        const Keyframe keyframe = prop->keyframes[ref.index];
        const double timeline_time = layer->in_time + keyframe.time;
        origin = std::min(origin, timeline_time);
        pending.push_back({ref.layer_id, ref.prop_name, keyframe, timeline_time});
    }

    if (pending.empty()) return false;
    std::sort(pending.begin(), pending.end(), [](const PendingCopy &a, const PendingCopy &b) {
        return std::tie(a.timeline_time, a.layer_id, a.prop_name) <
               std::tie(b.timeline_time, b.layer_id, b.prop_name);
    });

    keyframe_clipboard_.clear();
    keyframe_clipboard_.reserve(pending.size());
    for (const auto &entry : pending)
        keyframe_clipboard_.push_back({entry.layer_id, entry.prop_name, entry.keyframe, entry.timeline_time - origin});
    return true;
}

bool TimelineWidget::delete_selected_keyframes()
{
    if (!title_) return false;
    prune_keyframe_selection();
    if (selected_keyframes_.empty()) return false;

    std::map<std::pair<std::string, std::string>, std::vector<int>> grouped;
    for (const auto &ref : selected_keyframes_)
        grouped[{ref.layer_id, ref.prop_name}].push_back(ref.index);

    bool changed = false;
    for (auto &[prop_ref, indices] : grouped) {
        auto layer = title_->find_layer(prop_ref.first);
        if (!layer || layer->locked) continue;
        AnimatedProperty *prop = find_timeline_property(*layer, prop_ref.second);
        if (!prop) continue;
        std::sort(indices.begin(), indices.end(), std::greater<int>());
        for (int index : indices) {
            if (index < 0 || index >= (int)prop->keyframes.size()) continue;
            prop->keyframes.erase(prop->keyframes.begin() + index);
            changed = true;
        }
    }

    if (changed) {
        selected_keyframes_.clear();
        update();
    }
    return changed;
}

bool TimelineWidget::cut_selected_keyframes()
{
    if (!copy_selected_keyframes()) return false;
    return delete_selected_keyframes();
}

bool TimelineWidget::paste_keyframes_at(double timeline_time)
{
    if (!title_ || keyframe_clipboard_.empty()) return false;

    std::map<std::pair<std::string, std::string>, std::vector<double>> inserted_times;
    bool changed = false;
    const double paste_origin = std::clamp(snap_time(timeline_time), 0.0, title_->duration);

    for (const auto &entry : keyframe_clipboard_) {
        auto layer = title_->find_layer(entry.layer_id);
        if (!layer || layer->locked) continue;
        AnimatedProperty *prop = find_timeline_property(*layer, entry.prop_name);
        if (!prop) continue;

        Keyframe pasted = entry.keyframe;
        const double target_time = paste_origin + entry.offset;
        pasted.time = std::clamp(snap_time(target_time - layer->in_time),
                                 0.0, std::max(0.0, layer->out_time - layer->in_time));
        prop->keyframes.push_back(pasted);
        inserted_times[{entry.layer_id, entry.prop_name}].push_back(pasted.time);
        changed = true;
    }

    if (!changed) return false;

    selected_keyframes_.clear();
    for (auto &[prop_ref, times] : inserted_times) {
        auto layer = title_->find_layer(prop_ref.first);
        AnimatedProperty *prop = layer ? find_timeline_property(*layer, prop_ref.second) : nullptr;
        if (!prop) continue;
        std::sort(prop->keyframes.begin(), prop->keyframes.end(),
                  [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });

        std::set<int> used;
        for (double inserted_time : times) {
            int best = -1;
            double best_distance = std::numeric_limits<double>::max();
            for (int i = 0; i < (int)prop->keyframes.size(); ++i) {
                if (used.count(i)) continue;
                const double distance = std::abs(prop->keyframes[i].time - inserted_time);
                if (distance < best_distance) {
                    best = i;
                    best_distance = distance;
                }
            }
            if (best >= 0) {
                used.insert(best);
                selected_keyframes_.insert({prop_ref.first, prop_ref.second, best});
            }
        }
    }

    update();
    return true;
}

void TimelineWidget::begin_keyframe_drag(const std::string &layer_id, const std::string &prop_name,
                                         int kf_idx, double start_time)
{
    drag_mode_ = DragMode::Keyframe;
    drag_layer_id_ = layer_id;
    drag_prop_name_ = prop_name;
    drag_keyframe_index_ = kf_idx;
    drag_start_time_ = start_time;
    dragged_keyframes_.clear();
    prune_keyframe_selection();
    if (!is_keyframe_selected(layer_id, prop_name, kf_idx))
        selected_keyframes_ = {{layer_id, prop_name, kf_idx}};

    for (const auto &ref : selected_keyframes_) {
        auto layer = title_ ? title_->find_layer(ref.layer_id) : nullptr;
        if (!layer || layer->locked) continue;
        AnimatedProperty *prop = find_timeline_property(*layer, ref.prop_name);
        if (!prop || ref.index < 0 || ref.index >= (int)prop->keyframes.size()) continue;
        dragged_keyframes_.push_back({ref, prop->keyframes[ref.index].time});
    }
}

void TimelineWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    int W = width(), H = height();
    int rh = ruler_height(), rowh = row_height();
    const QPalette pal = palette();
    auto with_alpha = [](QColor color, int alpha) {
        color.setAlpha(alpha);
        return color;
    };
    auto subtle = [](const QColor &color) {
        return color.lightness() < 128 ? color.lighter(108) : color.darker(104);
    };

    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor disabled_text = pal.color(QPalette::Disabled, QPalette::WindowText);
    const QColor border = pal.color(QPalette::Mid);
    const QColor dark = pal.color(QPalette::Dark);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlighted_text = pal.color(QPalette::HighlightedText);
    const QColor ruler_bg = window.lightness() < 128 ? window.darker(116) : window.darker(106);
    const QColor property_bg = subtle(window);
    const QColor selected_row = with_alpha(highlight, window.lightness() < 128 ? 90 : 65);
    const QColor tick_major = with_alpha(text, 150);
    const QColor tick_minor = with_alpha(text, 80);
    const QColor label_text = with_alpha(text, 165);
    const QColor playhead_color = TitlePreferences::timeline_color(TitlePreferences::TimelineColorRole::Current);
    const QColor pause_color = TitlePreferences::timeline_color(TitlePreferences::TimelineColorRole::Pause);
    const QColor loop_color = TitlePreferences::timeline_color(TitlePreferences::TimelineColorRole::Loop);
    const QColor handle_color = with_alpha(text, 150);

    /* Background */
    p.fillRect(0, 0, W, H, window);

    /* Ruler */
    p.fillRect(0, 0, W, rh, ruler_bg);
    p.setPen(border);

    double dur = title_ ? title_->duration : 10.0;
    double fps = obs_frame_rate();
    double frame_step = obs_frame_duration();
    int first_frame = std::max(0, (int)std::floor(scroll_x_ / pixels_per_sec_ / frame_step) - 1);
    int last_frame = (int)std::ceil((scroll_x_ + W) / pixels_per_sec_ / frame_step) + 1;
    int label_every = std::max(1, (int)std::ceil(55.0 / (pixels_per_sec_ * frame_step)));

    for (int frame = first_frame; frame <= last_frame; ++frame) {
        double t = frame * frame_step;
        if (t > dur + frame_step) break;
        int x = time_to_x(t);
        if (x < 0 || x > W) continue;
        bool is_second = (frame % std::max(1, (int)std::round(fps)) == 0);
        bool label = (frame % label_every == 0) || is_second;
        p.setPen(is_second ? tick_major : tick_minor);
        p.drawLine(x, rh - (is_second ? 9 : label ? 6 : 3), x, rh);
        if (label) {
            p.setPen(label_text);
            int seconds = frame / std::max(1, (int)std::round(fps));
            int frame_in_second = frame % std::max(1, (int)std::round(fps));
            QString text = is_second
                ? QString("%1s").arg(seconds)
                : QString("+%1f").arg(frame_in_second, 2, 10, QChar('0'));
            p.drawText(x + 2, rh - 2, text);
        }
    }

    if (title_) {
        if (title_->playback_mode == 1) {
            int loop_x0 = time_to_x(std::clamp(title_->loop_start, 0.0, dur));
            int loop_x1 = time_to_x(std::clamp(title_->loop_end, title_->loop_start, dur));
            if (loop_x1 > loop_x0) {
                p.fillRect(loop_x0, 18, loop_x1 - loop_x0, rh - 18, with_alpha(loop_color, 45));
                p.setPen(QPen(loop_color, 2));
                p.drawLine(loop_x0, 18, loop_x0, H);
                p.drawLine(loop_x1, 18, loop_x1, H);
                p.setPen(loop_color.lightness() < 128 ? loop_color.lighter(170) : loop_color.darker(170));
                p.drawText(loop_x0 + 4, 20, 80, 16, Qt::AlignVCenter, obsgs_tr("OBSTitles.LoopIn"));
                p.drawText(loop_x1 + 4, 20, 80, 16, Qt::AlignVCenter, obsgs_tr("OBSTitles.LoopOut"));
            }
        }
        if (title_->playback_mode == 2) {
            int pause_x = time_to_x(std::clamp(title_->pause_time, 0.0, dur));
            p.setPen(QPen(pause_color, 2));
            p.drawLine(pause_x, 12, pause_x, rh);
            p.setBrush(pause_color);
            p.setPen(Qt::NoPen);
            QPolygon marker;
            marker << QPoint(pause_x - 6, 12) << QPoint(pause_x + 6, 12) << QPoint(pause_x, 22);
            p.drawPolygon(marker);
            p.setPen(pause_color.lightness() < 128 ? pause_color.lighter(170) : pause_color.darker(170));
            p.drawText(pause_x + 4, 22, 100, 16, Qt::AlignVCenter, obsgs_tr("OBSTitles.Pause"));
            p.setBrush(Qt::NoBrush);
        }
    }

    /* Layer/property rows.  This uses the same row model as LayerStack so
     * keyframed property rows stay vertically aligned with the layer list.
     */
    auto rows = timeline_rows(title_);
    for (int row = 0; row < (int)rows.size(); ++row) {
        auto &entry = rows[row];
        auto &layer = entry.layer;
        int y = rh + row * rowh - scroll_y_;
        if (y > H) break;
        if (y + rowh < rh) continue;
        bool sel = (layer->id == sel_layer_id_);

        p.fillRect(0, y, W, rowh,
                   entry.is_property ? property_bg :
                   sel ? selected_row : window);
        p.setPen(border);
        p.drawLine(0, y + rowh - 1, W, y + rowh - 1);

        int x0 = time_to_x(layer->in_time);
        int x1 = time_to_x(layer->out_time);
        if (!entry.is_property) {
            QRect strip_rect(std::min(x0, x1), y + 3, std::abs(x1 - x0), rowh - 6);
            QColor bar_col = layer_color(*layer, row);
            if (!layer->visible) {
                const int gray = qGray(bar_col.rgb());
                bar_col = QColor(gray, gray, gray).darker(135);
            }
            if (sel) bar_col = bar_col.lighter(125);
            p.fillRect(strip_rect, bar_col);
            if (layer->locked) {
                p.save();
                p.setClipRect(strip_rect);
                p.setPen(QPen(with_alpha(dark, 170), 2));
                for (int lx = strip_rect.left() - strip_rect.height(); lx < strip_rect.right() + strip_rect.height(); lx += 8)
                    p.drawLine(lx, strip_rect.bottom(), lx + strip_rect.height(), strip_rect.top());
                p.restore();
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(dark);
            p.drawRect(strip_rect);

            /* Trim handles for mouse resizing of unlocked layer in/out. */
            if (!layer->locked) {
                p.fillRect(x0, y + 3, 4, rowh - 6, handle_color);
                p.fillRect(x1 - 4, y + 3, 4, rowh - 6, handle_color);
            }

            p.setPen(layer->visible ? text : disabled_text);
            p.drawText(std::max(strip_rect.left(), 0) + 6, y, std::max(1, strip_rect.width() - 12), rowh,
                       Qt::AlignVCenter, QString::fromStdString(layer->name));
        } else {
            p.fillRect(x0, y + rowh / 2 - 1, x1 - x0, 2, border);
            p.setPen(disabled_text.isValid() ? disabled_text : with_alpha(text, 150));
            p.drawText(6, y, 150, rowh, Qt::AlignVCenter, property_label(entry.prop->name));
        }

        auto draw_kf = [&](const AnimatedProperty &prop) {
            for (int i = 0; i < (int)prop.keyframes.size(); ++i) {
                const auto &kf = prop.keyframes[i];
                int kx = time_to_x(layer->in_time + kf.time);
                if (kx < 0 || kx > W) continue;
                int ky = y + rowh / 2;
                QColor kf_fill = keyframe_color(kf.easing);
                if (!layer->visible)
                    kf_fill = kf_fill.darker(160);
                const bool selected = is_keyframe_selected(layer->id, prop.name, i);
                if (selected) {
                    draw_keyframe_marker(p, QPointF(kx, ky), kf.easing, 8.0,
                                         with_alpha(highlighted_text, 45),
                                         highlighted_text, 2.0);
                }
                draw_keyframe_marker(p, QPointF(kx, ky), kf.easing, 5.0,
                                     selected ? kf_fill.lighter(125) : kf_fill,
                                     selected ? highlighted_text : border,
                                     selected ? 2.0 : 1.0);
            }
        };

        if (entry.is_property)
            draw_kf(*entry.prop);
        else if (!layer->properties_expanded)
            for (auto *prop : timeline_properties(*layer)) draw_kf(*prop);
    }

    /* Playhead */
    int phx = time_to_x(playhead_);
    p.setPen(QPen(playhead_color, 1.5));
    p.drawLine(phx, 0, phx, H);
    /* Playhead head triangle */
    p.setBrush(playhead_color);
    p.setPen(Qt::NoPen);
    QPolygon tri;
    tri << QPoint(phx - 6, 0)
        << QPoint(phx + 6, 0)
        << QPoint(phx,     10);
    p.drawPolygon(tri);

    QString tc = format_timecode(playhead_);
    QRect tc_rect(phx + 8, 2, 96, 18);
    if (tc_rect.right() > W) tc_rect.moveRight(phx - 8);
    p.fillRect(tc_rect, highlight);
    p.setPen(highlighted_text);
    p.drawText(tc_rect.adjusted(4, 0, -4, 0), Qt::AlignVCenter, tc);

    if (drag_mode_ == DragMode::Marquee && marquee_moved_) {
        QRect rect = marquee_rect();
        p.fillRect(rect, with_alpha(highlight, 35));
        p.setPen(QPen(highlight, 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(rect.adjusted(0, 0, -1, -1));
    }
}

bool TimelineWidget::hit_keyframe(const QPoint &pos, std::shared_ptr<Layer> *hit_layer,
                                  AnimatedProperty **hit_prop, int *hit_kf_idx,
                                  int *hit_row_idx) const
{
    if (!title_ || pos.y() < ruler_height()) return false;
    auto rows = timeline_rows(title_);
    int row = (pos.y() - ruler_height() + scroll_y_) / row_height();
    if (row < 0 || row >= (int)rows.size()) return false;

    auto &entry = rows[row];
    constexpr int kHitRadius = 7;
    auto test_prop = [&](AnimatedProperty *prop) -> bool {
        for (int i = 0; i < (int)prop->keyframes.size(); ++i) {
            const auto &kf = prop->keyframes[i];
            int kx = time_to_x(entry.layer->in_time + kf.time);
            int ky = ruler_height() + row * row_height() - scroll_y_ + row_height() / 2;
            if (std::abs(pos.x() - kx) <= kHitRadius &&
                std::abs(pos.y() - ky) <= kHitRadius) {
                if (hit_layer) *hit_layer = entry.layer;
                if (hit_prop) *hit_prop = prop;
                if (hit_kf_idx) *hit_kf_idx = i;
                if (hit_row_idx) *hit_row_idx = row;
                return true;
            }
        }
        return false;
    };

    if (entry.is_property)
        return entry.prop && test_prop(entry.prop);
    if (!entry.layer->properties_expanded) {
        for (auto *prop : timeline_properties(*entry.layer)) {
            if (test_prop(prop)) return true;
        }
    }
    return false;
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent *ev)
{
    if (!title_) return;

    std::shared_ptr<Layer> layer;
    AnimatedProperty *hit_prop = nullptr;
    int hit_idx = -1;
    const bool has_hit = hit_keyframe(ev->pos(), &layer, &hit_prop, &hit_idx, nullptr);
    if (has_hit && layer && layer->locked) return;
    if (!has_hit && keyframe_clipboard_.empty()) return;

    if (has_hit && layer && hit_prop && !is_keyframe_selected(layer->id, hit_prop->name, hit_idx))
        select_keyframe(layer->id, hit_prop->name, hit_idx, false, false);
    prune_keyframe_selection();

    QMenu menu(this);
    menu.setTitle(has_hit ? obsgs_tr("OBSTitles.KeyframeEasing") : obsgs_tr("OBSTitles.Paste"));

    QAction *copy_action = menu.addAction(obsgs_tr("OBSTitles.Copy"));
    QAction *cut_action = menu.addAction(obsgs_tr("OBSTitles.Cut"));
    QAction *paste_action = menu.addAction(obsgs_tr("OBSTitles.Paste"));
    QAction *delete_action = menu.addAction(obsgs_tr("OBSTitles.Delete"));
    const bool has_selection = !selected_keyframes_.empty();
    copy_action->setEnabled(has_selection);
    cut_action->setEnabled(has_selection);
    paste_action->setEnabled(!keyframe_clipboard_.empty());
    delete_action->setEnabled(has_selection);

    struct EasingChoice {
        QAction *action = nullptr;
        AnimatedProperty *prop = nullptr;
        std::vector<int> target_indices;
        EasingType easing = EasingType::Linear;
    };
    std::vector<EasingChoice> choices;

    auto swatch_icon = [](EasingType easing) {
        QPixmap swatch(16, 16);
        swatch.fill(Qt::transparent);
        QPainter painter(&swatch);
        draw_keyframe_marker(painter, QPointF(8, 8), easing, 5.0,
                             keyframe_color(easing), QColor(0x7a, 0x5a, 0x00), 1.0);
        return QIcon(swatch);
    };

    auto add_easing_action = [&](QMenu *target_menu, const QString &label,
                                 AnimatedProperty *prop, EasingType easing,
                                 const std::vector<int> &indices) {
        QAction *action = target_menu->addAction(swatch_icon(easing), label);
        action->setToolTip(easing == EasingType::Hold
            ? obsgs_tr("OBSTitles.HoldEasingTooltip")
            : obsgs_tr("OBSTitles.EasingTooltip"));
        action->setCheckable(true);
        action->setChecked(prop && std::all_of(indices.begin(), indices.end(), [&](int idx) {
            return idx >= 0 && idx < (int)prop->keyframes.size() &&
                   prop->keyframes[idx].easing == easing;
        }));
        choices.push_back({action, prop, indices, easing});
        return action;
    };

    auto add_easing_group = [&](QMenu *target_menu, AnimatedProperty *prop, const std::vector<int> &indices) {
        auto *group = new QActionGroup(target_menu);
        group->setExclusive(true);
        for (auto [label, easing] : std::initializer_list<std::pair<QString, EasingType>>{
                 {obsgs_tr("OBSTitles.Linear"), EasingType::Linear},
                 {obsgs_tr("OBSTitles.EasyEase"), EasingType::EaseInOut},
                 {obsgs_tr("OBSTitles.EaseIn"), EasingType::EaseIn},
                 {obsgs_tr("OBSTitles.EaseOut"), EasingType::EaseOut},
                 {obsgs_tr("OBSTitles.Hold"), EasingType::Hold},
                 {obsgs_tr("OBSTitles.CustomBezier"), EasingType::Bezier},
             }) {
            add_easing_action(target_menu, label, prop, easing, indices)->setActionGroup(group);
        }
    };

    if (has_hit && layer && hit_prop) {
        menu.addSeparator();
        QMenu *easing_menu = menu.addMenu(obsgs_tr("OBSTitles.Easing"));
        QAction *header = easing_menu->addAction(QString("%1 · %2")
            .arg(QString::fromStdString(layer->name))
            .arg(property_label(hit_prop->name)));
        header->setEnabled(false);

        const bool has_previous_segment = hit_idx > 0;
        const bool has_next_segment = hit_idx + 1 < (int)hit_prop->keyframes.size();
        if (!has_previous_segment && !has_next_segment) {
            QAction *message = easing_menu->addAction(obsgs_tr("OBSTitles.AddKeyframeForEasing"));
            message->setEnabled(false);
        } else {
            QAction *scope = easing_menu->addAction(has_previous_segment && has_next_segment
                ? obsgs_tr("OBSTitles.EasingBothSegments")
                : has_next_segment ? obsgs_tr("OBSTitles.EasingNextSegment") : obsgs_tr("OBSTitles.EasingPreviousSegment"));
            scope->setEnabled(false);
            easing_menu->addSeparator();

            auto default_targets = [&]() {
                std::vector<int> indices;
                if (has_previous_segment && has_next_segment) {
                    indices = {hit_idx - 1, hit_idx};
                } else if (has_next_segment) {
                    indices = {hit_idx};
                } else {
                    indices = {hit_idx - 1};
                }
                return indices;
            };
            add_easing_group(easing_menu, hit_prop, default_targets());

            if (has_previous_segment && has_next_segment) {
                easing_menu->addSeparator();
                QMenu *advanced = easing_menu->addMenu(obsgs_tr("OBSTitles.ApplyOneSide"));
                QMenu *previous = advanced->addMenu(obsgs_tr("OBSTitles.PreviousSegment"));
                add_easing_group(previous, hit_prop, {hit_idx - 1});
                QMenu *next = advanced->addMenu(obsgs_tr("OBSTitles.NextSegment"));
                add_easing_group(next, hit_prop, {hit_idx});
            }
        }
    }

    QAction *chosen = menu.exec(ev->globalPos());
    if (!chosen) return;

    if (chosen == copy_action) {
        copy_selected_keyframes();
        return;
    }
    if (chosen == cut_action) {
        if (cut_selected_keyframes()) emit keyframe_easing_changed();
        return;
    }
    if (chosen == paste_action) {
        if (paste_keyframes_at(std::clamp(x_to_time(ev->pos().x()), 0.0, title_->duration)))
            emit keyframe_easing_changed();
        return;
    }
    if (chosen == delete_action) {
        if (delete_selected_keyframes()) emit keyframe_easing_changed();
        return;
    }

    auto choice = std::find_if(choices.begin(), choices.end(),
                               [&](const EasingChoice &candidate) { return candidate.action == chosen; });
    if (choice == choices.end() || !choice->prop) return;

    for (int idx : choice->target_indices) {
        if (idx >= 0 && idx < (int)choice->prop->keyframes.size())
            apply_easing_preset(choice->prop->keyframes[idx], choice->easing);
    }
    update();
    emit keyframe_easing_changed();
}

void TimelineWidget::wheelEvent(QWheelEvent *ev)
{
    if (!title_) return;

    const QPoint angle = ev->angleDelta();
    if (ev->modifiers() & Qt::ShiftModifier) {
        int delta = angle.x() != 0 ? angle.x() : angle.y();
        scroll_x_ -= delta;
        clamp_scroll();
        update();
        ev->accept();
        return;
    }

    if (ev->modifiers() & Qt::ControlModifier) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        int cursor_x = (int)std::round(ev->position().x());
#else
        int cursor_x = ev->pos().x();
#endif
        double anchor_time = (cursor_x + scroll_x_) / pixels_per_sec_;
        int delta = angle.y() != 0 ? angle.y() : angle.x();
        if (delta == 0) return;

        double factor = std::pow(1.0015, delta);
        set_pixels_per_sec(pixels_per_sec_ * factor, anchor_time, cursor_x);
        ev->accept();
        return;
    }

    int delta = angle.y() != 0 ? -angle.y() : -angle.x();
    if (delta == 0) return;
    emit vertical_scroll_delta_requested(delta);
    ev->accept();
}

void TimelineWidget::resizeEvent(QResizeEvent *ev)
{
    QWidget::resizeEvent(ev);
    if (fit_on_next_resize_ && title_ && width() > 40) {
        fit_on_next_resize_ = false;
        fit_timeline();
        return;
    }
    clamp_scroll();
    clamp_vertical_scroll();
}

void TimelineWidget::keyPressEvent(QKeyEvent *ev)
{
    if (!title_) {
        QWidget::keyPressEvent(ev);
        return;
    }

    if (ev->matches(QKeySequence::Copy) && has_selected_keyframes()) {
        copy_keyframe_selection();
        ev->accept();
        return;
    }
    if (ev->matches(QKeySequence::Cut) && has_selected_keyframes()) {
        cut_keyframe_selection();
        ev->accept();
        return;
    }
    if (ev->matches(QKeySequence::Paste) && has_keyframe_clipboard()) {
        paste_keyframes_at_playhead();
        ev->accept();
        return;
    }
    if ((ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) && has_selected_keyframes()) {
        delete_keyframe_selection();
        ev->accept();
        return;
    }

    QWidget::keyPressEvent(ev);
}

void TimelineWidget::mousePressEvent(QMouseEvent *ev)
{
    setFocus(Qt::MouseFocusReason);
    if (!title_) return;
    drag_mode_ = DragMode::None;
    drag_layer_id_.clear();
    drag_prop_name_.clear();
    drag_keyframe_index_ = -1;
    drag_start_time_ = 0.0;
    drag_start_in_ = 0.0;
    drag_start_out_ = 0.0;
    dragged_keyframes_.clear();
    marquee_moved_ = false;

    if (ev->pos().y() < ruler_height()) {
        if (title_->playback_mode == 2) {
            int pause_x = time_to_x(std::clamp(title_->pause_time, 0.0, title_->duration));
            if (std::abs(ev->pos().x() - pause_x) <= 8) {
                drag_mode_ = DragMode::PauseMarker;
                setCursor(Qt::SizeHorCursor);
                ev->accept();
                return;
            }
        }
        if (title_->playback_mode == 1) {
            int loop_x0 = time_to_x(std::clamp(title_->loop_start, 0.0, title_->duration));
            int loop_x1 = time_to_x(std::clamp(title_->loop_end, title_->loop_start, title_->duration));
            if (std::abs(ev->pos().x() - loop_x0) <= 8) {
                drag_mode_ = DragMode::LoopStart;
                setCursor(Qt::SizeHorCursor);
                ev->accept();
                return;
            }
            if (std::abs(ev->pos().x() - loop_x1) <= 8) {
                drag_mode_ = DragMode::LoopEnd;
                setCursor(Qt::SizeHorCursor);
                ev->accept();
                return;
            }
        }
        drag_mode_ = DragMode::Playhead;
        double t = std::clamp(x_to_time(ev->pos().x()), 0.0, title_->duration);
        emit playhead_changed(t);
        ev->accept();
        return;
    }

    std::shared_ptr<Layer> hit_layer;
    AnimatedProperty *hit_prop = nullptr;
    int hit_idx = -1;
    if (hit_keyframe(ev->pos(), &hit_layer, &hit_prop, &hit_idx, nullptr)) {
        if (hit_layer && hit_layer->locked) {
            ev->accept();
            return;
        }
        if (hit_layer) emit layer_selected(hit_layer->id);
        const bool shift = ev->modifiers() & Qt::ShiftModifier;
        if (shift) {
            select_keyframe(hit_layer->id, hit_prop->name, hit_idx, true, true);
            ev->accept();
            return;
        }
        if (!is_keyframe_selected(hit_layer->id, hit_prop->name, hit_idx))
            select_keyframe(hit_layer->id, hit_prop->name, hit_idx, false, false);
        begin_keyframe_drag(hit_layer->id, hit_prop->name, hit_idx,
                            std::clamp(x_to_time(ev->pos().x()), 0.0, title_->duration));
        setCursor(Qt::ClosedHandCursor);
        ev->accept();
        return;
    }

    auto rows = timeline_rows(title_);
    int row = (ev->pos().y() - ruler_height() + scroll_y_) / row_height();
    if (row >= 0 && row < (int)rows.size() && !rows[row].is_property) {
        auto layer = rows[row].layer;
        if (!layer) {
            ev->accept();
            return;
        }
        int x0 = time_to_x(layer->in_time);
        int x1 = time_to_x(layer->out_time);
        constexpr int kTrimHit = 7;
        const bool hit_strip = ev->pos().x() >= std::min(x0, x1) - kTrimHit &&
                               ev->pos().x() <= std::max(x0, x1) + kTrimHit;
        if (layer->locked && hit_strip) {
            ev->accept();
            return;
        }
        if (hit_strip) emit layer_selected(layer->id);
        if (std::abs(ev->pos().x() - x0) <= kTrimHit) {
            drag_mode_ = DragMode::TrimIn;
            drag_layer_id_ = layer->id;
            setCursor(Qt::SizeHorCursor);
            ev->accept();
            return;
        }
        if (std::abs(ev->pos().x() - x1) <= kTrimHit) {
            drag_mode_ = DragMode::TrimOut;
            drag_layer_id_ = layer->id;
            setCursor(Qt::SizeHorCursor);
            ev->accept();
            return;
        }
        if (ev->pos().x() >= std::min(x0, x1) && ev->pos().x() <= std::max(x0, x1)) {
            drag_mode_ = DragMode::Layer;
            drag_layer_id_ = layer->id;
            drag_start_time_ = x_to_time(ev->pos().x());
            drag_start_in_ = layer->in_time;
            drag_start_out_ = layer->out_time;
            setCursor(Qt::ClosedHandCursor);
            ev->accept();
            return;
        }
    }

    if (ev->button() == Qt::LeftButton && ev->pos().y() >= ruler_height()) {
        drag_mode_ = DragMode::Marquee;
        marquee_start_ = ev->pos();
        marquee_current_ = ev->pos();
        marquee_additive_ = ev->modifiers() & Qt::ShiftModifier;
        marquee_moved_ = false;
        if (!marquee_additive_)
            selected_keyframes_.clear();
        ev->accept();
        update();
        return;
    }

    emit layer_selected(std::string());
    ev->accept();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent *ev)
{
    if (!title_) return;
    double t = std::clamp(x_to_time(ev->pos().x()), 0.0, title_->duration);

    if (drag_mode_ == DragMode::Playhead) {
        emit playhead_changed(t);
        return;
    }

    if (drag_mode_ == DragMode::PauseMarker) {
        title_->pause_time = t;
        update();
        return;
    }

    if (drag_mode_ == DragMode::LoopStart) {
        title_->loop_start = std::clamp(t, 0.0, title_->loop_end);
        update();
        return;
    }

    if (drag_mode_ == DragMode::LoopEnd) {
        title_->loop_end = std::clamp(t, title_->loop_start, title_->duration);
        update();
        return;
    }

    if (drag_mode_ == DragMode::Keyframe) {
        double delta = t - drag_start_time_;
        for (const auto &dragged : dragged_keyframes_) {
            auto layer = title_->find_layer(dragged.ref.layer_id);
            if (!layer || layer->locked) continue;
            AnimatedProperty *prop = find_timeline_property(*layer, dragged.ref.prop_name);
            if (!prop || dragged.ref.index < 0 || dragged.ref.index >= (int)prop->keyframes.size()) continue;
            prop->keyframes[dragged.ref.index].time =
                std::clamp(dragged.start_time + delta, 0.0, std::max(0.0, layer->out_time - layer->in_time));
        }
        update();
        return;
    }

    if (drag_mode_ == DragMode::Marquee) {
        marquee_current_ = ev->pos();
        if ((marquee_current_ - marquee_start_).manhattanLength() >= 3)
            marquee_moved_ = true;
        update();
        return;
    }

    if (drag_mode_ == DragMode::TrimIn || drag_mode_ == DragMode::TrimOut) {
        auto layer = title_->find_layer(drag_layer_id_);
        if (!layer || layer->locked) return;
        if (drag_mode_ == DragMode::TrimIn)
            layer->in_time = std::clamp(t, 0.0, std::max(0.0, layer->out_time - obs_frame_duration()));
        else
            layer->out_time = std::clamp(t, layer->in_time + obs_frame_duration(), title_->duration);
        update();
        return;
    }

    if (drag_mode_ == DragMode::Layer) {
        auto layer = title_->find_layer(drag_layer_id_);
        if (!layer || layer->locked) return;
        double duration = std::max(obs_frame_duration(), drag_start_out_ - drag_start_in_);
        double new_in = drag_start_in_ + (t - drag_start_time_);
        new_in = std::clamp(new_in, 0.0, std::max(0.0, title_->duration - duration));
        layer->in_time = new_in;
        layer->out_time = std::min(title_->duration, new_in + duration);
        update();
        return;
    }

    auto rows = timeline_rows(title_);
    int row = (ev->pos().y() - ruler_height() + scroll_y_) / row_height();
    if (row >= 0 && row < (int)rows.size() && !rows[row].is_property) {
        if (!rows[row].layer || rows[row].layer->locked) {
            unsetCursor();
            return;
        }
        int x0 = time_to_x(rows[row].layer->in_time);
        int x1 = time_to_x(rows[row].layer->out_time);
        if (std::abs(ev->pos().x() - x0) <= 7 || std::abs(ev->pos().x() - x1) <= 7)
            setCursor(Qt::SizeHorCursor);
        else if (ev->pos().x() >= std::min(x0, x1) && ev->pos().x() <= std::max(x0, x1))
            setCursor(Qt::OpenHandCursor);
        else
            unsetCursor();
    } else {
        unsetCursor();
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent *)
{
    bool changed = drag_mode_ == DragMode::Keyframe ||
                   drag_mode_ == DragMode::TrimIn ||
                   drag_mode_ == DragMode::TrimOut ||
                   drag_mode_ == DragMode::Layer ||
                   drag_mode_ == DragMode::LoopStart ||
                   drag_mode_ == DragMode::LoopEnd ||
                   drag_mode_ == DragMode::PauseMarker;

    if (drag_mode_ == DragMode::Marquee) {
        if (marquee_moved_)
            select_keyframes_in_rect(marquee_rect(), marquee_additive_);
        else if (!marquee_additive_)
            clear_keyframe_selection();
    }

    if (drag_mode_ == DragMode::Keyframe && title_) {
        std::map<KeyframeRef, double> selected_times;
        for (const auto &ref : selected_keyframes_) {
            auto layer = title_->find_layer(ref.layer_id);
            AnimatedProperty *prop = layer ? find_timeline_property(*layer, ref.prop_name) : nullptr;
            if (prop && ref.index >= 0 && ref.index < (int)prop->keyframes.size())
                selected_times[ref] = prop->keyframes[ref.index].time;
        }

        std::set<std::pair<std::string, std::string>> props_to_sort;
        for (const auto &dragged : dragged_keyframes_)
            props_to_sort.insert({dragged.ref.layer_id, dragged.ref.prop_name});

        for (const auto &prop_ref : props_to_sort) {
            if (auto layer = title_->find_layer(prop_ref.first)) {
                if (layer->locked) continue;
                if (AnimatedProperty *prop = find_timeline_property(*layer, prop_ref.second)) {
                    std::sort(prop->keyframes.begin(), prop->keyframes.end(),
                              [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
                }
            }
        }

        std::set<KeyframeRef> remapped;
        std::map<std::pair<std::string, std::string>, std::set<int>> used_indices;
        for (const auto &[ref, selected_time] : selected_times) {
            auto layer = title_->find_layer(ref.layer_id);
            AnimatedProperty *prop = layer ? find_timeline_property(*layer, ref.prop_name) : nullptr;
            if (!prop) continue;
            int best = -1;
            double best_distance = std::numeric_limits<double>::max();
            auto key = std::make_pair(ref.layer_id, ref.prop_name);
            for (int i = 0; i < (int)prop->keyframes.size(); ++i) {
                if (used_indices[key].count(i)) continue;
                double distance = std::abs(prop->keyframes[i].time - selected_time);
                if (distance < best_distance) {
                    best = i;
                    best_distance = distance;
                }
            }
            if (best >= 0) {
                used_indices[key].insert(best);
                remapped.insert({ref.layer_id, ref.prop_name, best});
            }
        }
        selected_keyframes_ = std::move(remapped);
    }

    drag_mode_ = DragMode::None;
    drag_layer_id_.clear();
    drag_prop_name_.clear();
    drag_keyframe_index_ = -1;
    drag_start_time_ = 0.0;
    drag_start_in_ = 0.0;
    drag_start_out_ = 0.0;
    dragged_keyframes_.clear();
    marquee_additive_ = false;
    marquee_moved_ = false;
    unsetCursor();
    update();
    if (changed) emit keyframe_easing_changed();
}

/* ══════════════════════════════════════════════════════════════════
 *  TitlePropertiesPanel
 * ══════════════════════════════════════════════════════════════════ */

