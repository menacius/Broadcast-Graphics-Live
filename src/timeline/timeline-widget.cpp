#include "title-editor-internal.h"
#include "cache-manager.h"
#include "effect-preset-catalog.h"
#include "transition-preset-catalog.h"
#include "title-logger.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QDragLeaveEvent>

TimelineWidget::TimelineWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumHeight(100);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    setAutoFillBackground(false);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAcceptDrops(true);
}

void TimelineWidget::set_title(std::shared_ptr<Title> t)
{
    const bool title_changed = t != title_;
    title_ = t;
    if (title_changed) {
        BGL_LOG_INFO("Timeline", QStringLiteral(
            "Timeline title=%1 duration=%2 layers=%3")
            .arg(title_ ? QString::fromStdString(title_->id)
                        : QStringLiteral("<none>"))
            .arg(title_ ? title_->duration : 0.0, 0, 'f', 6)
            .arg(title_ ? static_cast<int>(title_->layers.size()) : 0));
        scroll_x_ = 0;
        fit_on_next_resize_ = true;
        selected_keyframes_.clear();
        transition_target_selected_ = false;
        selected_transition_layer_id_.clear();
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
    set_selected_layers(lid.empty() ? std::vector<std::string>()
                                    : std::vector<std::string>{lid});
}

void TimelineWidget::set_selected_layers(const std::vector<std::string> &layer_ids)
{
    selected_layer_ids_.clear();
    if (!title_) {
        sel_layer_id_.clear();
        transition_target_selected_ = false;
        selected_transition_layer_id_.clear();
        update();
        return;
    }

    std::set<std::string> seen;
    for (const auto &id : layer_ids) {
        if (id.empty() || !seen.insert(id).second) continue;
        if (title_->find_layer(id))
            selected_layer_ids_.push_back(id);
    }
    sel_layer_id_ = selected_layer_ids_.empty() ? std::string() : selected_layer_ids_.back();
    if (!sel_layer_id_.empty())
        selection_anchor_layer_id_ = sel_layer_id_;
    else
        selection_anchor_layer_id_.clear();
    if (transition_target_selected_ &&
        std::find(selected_layer_ids_.begin(), selected_layer_ids_.end(), selected_transition_layer_id_) == selected_layer_ids_.end()) {
        transition_target_selected_ = false;
        selected_transition_layer_id_.clear();
    }
    update();
}

void TimelineWidget::set_playhead(double t)
{
    const int old_x = time_to_x(playhead_);
    playhead_ = snap_time(t);
    bool scrolled = false;
    if (title_)
        scrolled = keep_playhead_visible();

    if (scrolled) {
        update();
        return;
    }

    const int new_x = time_to_x(playhead_);
    const QRect old_rect = playhead_dirty_rect(old_x);
    const QRect new_rect = playhead_dirty_rect(new_x);
    if (old_rect.intersects(new_rect) || old_rect.adjusted(-4, 0, 4, 0).intersects(new_rect))
        update(old_rect.united(new_rect));
    else {
        update(old_rect);
        update(new_rect);
    }
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

bool TimelineWidget::has_transition_target_selection() const
{
    return transition_target_selected_ && selected_transition_layer() != nullptr;
}

bool TimelineWidget::has_selected_transition() const
{
    return selected_transition() != nullptr;
}

bool TimelineWidget::has_transition_clipboard() const
{
    return transition_clipboard_valid_;
}

bool TimelineWidget::layer_accepts_transition(const Layer &layer,
                                               const LayerTransition &transition) const
{
    if (!layer_transition_type_is_text(transition.type))
        return true;
    return layer.type == LayerType::Text ||
           layer.type == LayerType::Clock ||
           layer.type == LayerType::Ticker;
}

bool TimelineWidget::can_paste_transition_to_selection() const
{
    const auto layer = selected_transition_layer();
    return transition_clipboard_valid_ && layer && !layer->locked &&
           layer_accepts_transition(*layer, transition_clipboard_);
}

std::shared_ptr<Layer> TimelineWidget::selected_transition_layer() const
{
    if (!title_ || !transition_target_selected_ || selected_transition_layer_id_.empty())
        return nullptr;
    return title_->find_layer(selected_transition_layer_id_);
}

const LayerTransition *TimelineWidget::selected_transition() const
{
    const auto layer = selected_transition_layer();
    return layer ? find_layer_transition(layer->transitions, selected_transition_edge_) : nullptr;
}

LayerTransition *TimelineWidget::selected_transition()
{
    auto layer = selected_transition_layer();
    return layer ? find_layer_transition(layer->transitions, selected_transition_edge_) : nullptr;
}

void TimelineWidget::select_transition_target(const std::string &layer_id,
                                               LayerTransitionEdge edge)
{
    transition_target_selected_ = !layer_id.empty();
    selected_transition_layer_id_ = layer_id;
    selected_transition_edge_ = edge;
    selected_keyframes_.clear();
    update();
}

void TimelineWidget::clear_transition_selection()
{
    if (!transition_target_selected_ && selected_transition_layer_id_.empty())
        return;
    transition_target_selected_ = false;
    selected_transition_layer_id_.clear();
    update();
}

bool TimelineWidget::copy_transition_selection()
{
    const LayerTransition *transition = selected_transition();
    if (!transition)
        return false;
    transition_clipboard_ = *transition;
    transition_clipboard_valid_ = true;
    return true;
}

bool TimelineWidget::delete_transition_selection()
{
    auto layer = selected_transition_layer();
    if (!layer || layer->locked || !selected_transition())
        return false;
    auto &transitions = layer->transitions;
    transitions.erase(std::remove_if(transitions.begin(), transitions.end(),
                                     [&](const LayerTransition &transition) {
                                         return transition.edge == selected_transition_edge_;
                                     }), transitions.end());
    update();
    BGL_LOG_DEBUG("Transitions", QStringLiteral(
        "Deleted transition title=%1 layer=%2 edge=%3")
        .arg(title_ ? QString::fromStdString(title_->id)
                    : QStringLiteral("<none>"))
        .arg(QString::fromStdString(layer->id))
        .arg(static_cast<int>(selected_transition_edge_)));
    emit transition_modified();
    return true;
}

bool TimelineWidget::cut_transition_selection()
{
    // Keyboard shortcuts reach this path even when the context-menu action is
    // disabled. Validate mutability before changing the clipboard so Ctrl+X
    // on a locked layer cannot behave like an unexpected Copy operation.
    const auto layer = selected_transition_layer();
    if (!layer || layer->locked || !selected_transition())
        return false;
    if (!copy_transition_selection())
        return false;
    return delete_transition_selection();
}

void TimelineWidget::clear_transition_target_selection()
{
    clear_transition_selection();
}

bool TimelineWidget::paste_transition_to_selection()
{
    auto layer = selected_transition_layer();
    if (!transition_clipboard_valid_ || !layer || layer->locked ||
        !layer_accepts_transition(*layer, transition_clipboard_))
        return false;

    LayerTransition pasted = transition_clipboard_;
    pasted.id = TitleDataStore::make_uuid();
    pasted.edge = selected_transition_edge_;
    pasted.kind = layer_transition_type_is_text(pasted.type)
        ? LayerTransitionKind::Text : LayerTransitionKind::General;

    const double frame = obs_frame_duration();
    const double layer_duration = std::max(frame, layer->out_time - layer->in_time);
    const LayerTransitionEdge other_edge = selected_transition_edge_ == LayerTransitionEdge::In
        ? LayerTransitionEdge::Out : LayerTransitionEdge::In;
    const LayerTransition *other = find_layer_transition(layer->transitions, other_edge);
    const double maximum_duration = std::max(frame, layer_duration - (other ? other->duration : 0.0));
    pasted.duration = std::clamp(pasted.duration, frame, maximum_duration);

    if (LayerTransition *existing = find_layer_transition(layer->transitions, selected_transition_edge_))
        *existing = pasted;
    else
        layer->transitions.push_back(std::move(pasted));

    update();
    BGL_LOG_DEBUG("Transitions", QStringLiteral(
        "Pasted transition title=%1 layer=%2 edge=%3 type=%4")
        .arg(title_ ? QString::fromStdString(title_->id)
                    : QStringLiteral("<none>"))
        .arg(QString::fromStdString(layer->id))
        .arg(static_cast<int>(selected_transition_edge_))
        .arg(static_cast<int>(transition_clipboard_.type)));
    emit transition_modified();
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

QRect TimelineWidget::playhead_dirty_rect(int playhead_x) const
{
    QRect line_rect(playhead_x - 10, 0, 20, height());
    QRect timecode_rect(playhead_x + 8, 2, 96, 18);
    if (timecode_rect.right() > width())
        timecode_rect.moveRight(playhead_x - 8);
    return line_rect.united(timecode_rect).adjusted(-2, 0, 2, 0)
        .intersected(rect());
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

TimelinePropertyRef TimelineWidget::find_timeline_property(Layer &layer, const std::string &prop_name) const
{
    for (auto prop : timeline_properties(layer)) {
        if (prop.name() == prop_name)
            return prop;
    }
    return {};
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
        auto prop = layer ? find_timeline_property(*layer, it->prop_name) : TimelinePropertyRef{};
        if (!layer || !prop || it->index < 0 || it->index >= (int)prop.keyframe_count())
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
    transition_target_selected_ = false;
    selected_transition_layer_id_.clear();
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
        for (int i = 0; i < (int)entry.prop.keyframe_count(); ++i) {
            int kx = time_to_x(entry.layer->in_time + entry.prop.keyframe_time((size_t)i));
            if (kx < visible_timeline.left() || kx > visible_timeline.right()) continue;
            if (bounded.contains(QPoint(kx, ky)))
                selection.insert({entry.layer->id, entry.prop.name(), i});
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
        VectorKeyframe vector_keyframe;
        bool is_vector = false;
        double timeline_time = 0.0;
    };
    std::vector<PendingCopy> pending;
    double origin = std::numeric_limits<double>::max();

    for (const auto &ref : selected_keyframes_) {
        auto layer = title_->find_layer(ref.layer_id);
        auto prop = layer ? find_timeline_property(*layer, ref.prop_name) : TimelinePropertyRef{};
        if (!layer || !prop || ref.index < 0 || ref.index >= (int)prop.keyframe_count()) continue;
        const double timeline_time = layer->in_time + prop.keyframe_time((size_t)ref.index);
        origin = std::min(origin, timeline_time);
        PendingCopy copy;
        copy.layer_id = ref.layer_id;
        copy.prop_name = ref.prop_name;
        copy.is_vector = prop.is_vector();
        copy.keyframe = prop.scalar_keyframe((size_t)ref.index);
        copy.vector_keyframe = prop.vector_keyframe((size_t)ref.index);
        copy.timeline_time = timeline_time;
        pending.push_back(copy);
    }

    if (pending.empty()) return false;
    std::sort(pending.begin(), pending.end(), [](const PendingCopy &a, const PendingCopy &b) {
        return std::tie(a.timeline_time, a.layer_id, a.prop_name) <
               std::tie(b.timeline_time, b.layer_id, b.prop_name);
    });

    keyframe_clipboard_.clear();
    keyframe_clipboard_.reserve(pending.size());
    for (const auto &entry : pending)
        keyframe_clipboard_.push_back({entry.layer_id, entry.prop_name, entry.keyframe,
                                       entry.vector_keyframe, entry.is_vector,
                                       entry.timeline_time - origin});
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
        auto prop = find_timeline_property(*layer, prop_ref.second);
        if (!prop) continue;
        std::sort(indices.begin(), indices.end(), std::greater<int>());
        for (int index : indices) {
            if (index < 0 || index >= (int)prop.keyframe_count()) continue;
            prop.erase_keyframe((size_t)index);
            changed = true;
        }
    }

    if (changed) {
        const int removed_count = static_cast<int>(selected_keyframes_.size());
        selected_keyframes_.clear();
        update();
        BGL_LOG_DEBUG("Animation", QStringLiteral(
            "Deleted keyframes title=%1 count=%2")
            .arg(title_ ? QString::fromStdString(title_->id)
                        : QStringLiteral("<none>"))
            .arg(removed_count));
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
        auto prop = find_timeline_property(*layer, entry.prop_name);
        if (!prop) continue;

        const double target_time = paste_origin + entry.offset;
        const double local_time = std::clamp(snap_time(target_time - layer->in_time),
                                             0.0, std::max(0.0, layer->out_time - layer->in_time));
        if (entry.is_vector) {
            VectorKeyframe pasted = entry.vector_keyframe;
            pasted.time = local_time;
            prop.push_keyframe(pasted);
        } else {
            Keyframe pasted = entry.keyframe;
            pasted.time = local_time;
            prop.push_keyframe(pasted);
        }
        inserted_times[{entry.layer_id, entry.prop_name}].push_back(local_time);
        changed = true;
    }

    if (!changed) return false;

    selected_keyframes_.clear();
    for (auto &[prop_ref, times] : inserted_times) {
        auto layer = title_->find_layer(prop_ref.first);
        auto prop = layer ? find_timeline_property(*layer, prop_ref.second) : TimelinePropertyRef{};
        if (!prop) continue;
        prop.sort_keyframes();

        std::set<int> used;
        for (double inserted_time : times) {
            int best = -1;
            double best_distance = std::numeric_limits<double>::max();
            for (int i = 0; i < (int)prop.keyframe_count(); ++i) {
                if (used.count(i)) continue;
                const double distance = std::abs(prop.keyframe_time(i) - inserted_time);
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
    BGL_LOG_DEBUG("Animation", QStringLiteral(
        "Pasted keyframes title=%1 count=%2 origin=%3")
        .arg(title_ ? QString::fromStdString(title_->id)
                    : QStringLiteral("<none>"))
        .arg(static_cast<int>(keyframe_clipboard_.size()))
        .arg(paste_origin, 0, 'f', 6));
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
        auto prop = find_timeline_property(*layer, ref.prop_name);
        if (!prop || ref.index < 0 || ref.index >= (int)prop.keyframe_count()) continue;
        dragged_keyframes_.push_back({ref, prop.keyframe_time(ref.index)});
    }
}

bool TimelineWidget::is_layer_selected(const std::string &layer_id) const
{
    return std::find(selected_layer_ids_.begin(), selected_layer_ids_.end(), layer_id) != selected_layer_ids_.end();
}

void TimelineWidget::select_layer_from_mouse(const std::string &layer_id, Qt::KeyboardModifiers modifiers)
{
    if (!title_ || layer_id.empty()) return;

    auto display_order = [&]() {
        std::vector<std::string> ids;
        for (auto it = title_->layers.rbegin(); it != title_->layers.rend(); ++it) {
            if (*it) ids.push_back((*it)->id);
        }
        return ids;
    };
    auto ordered_from_set = [&](const std::set<std::string> &selected) {
        std::vector<std::string> ids;
        for (const auto &layer : title_->layers) {
            if (layer && selected.find(layer->id) != selected.end())
                ids.push_back(layer->id);
        }
        return ids;
    };

    std::vector<std::string> next_ids;
    if (modifiers & Qt::ShiftModifier) {
        std::set<std::string> selected;
        if (modifiers & Qt::ControlModifier)
            selected.insert(selected_layer_ids_.begin(), selected_layer_ids_.end());

        const std::string anchor = selection_anchor_layer_id_.empty()
            ? (selected_layer_ids_.empty() ? layer_id : selected_layer_ids_.back())
            : selection_anchor_layer_id_;
        const auto order = display_order();
        auto anchor_it = std::find(order.begin(), order.end(), anchor);
        auto clicked_it = std::find(order.begin(), order.end(), layer_id);
        if (anchor_it != order.end() && clicked_it != order.end()) {
            const int a = (int)std::distance(order.begin(), anchor_it);
            const int b = (int)std::distance(order.begin(), clicked_it);
            const int first = std::min(a, b);
            const int last = std::max(a, b);
            for (int i = first; i <= last; ++i)
                selected.insert(order[(size_t)i]);
        } else {
            selected.insert(layer_id);
        }
        next_ids = ordered_from_set(selected);
    } else if (modifiers & Qt::ControlModifier) {
        next_ids = selected_layer_ids_;
        auto existing = std::find(next_ids.begin(), next_ids.end(), layer_id);
        if (existing == next_ids.end())
            next_ids.push_back(layer_id);
        else
            next_ids.erase(existing);
        selection_anchor_layer_id_ = layer_id;
    } else if (is_layer_selected(layer_id) && selected_layer_ids_.size() > 1) {
        next_ids = selected_layer_ids_;
    } else {
        next_ids.push_back(layer_id);
        selection_anchor_layer_id_ = layer_id;
    }

    selected_layer_ids_ = next_ids;
    sel_layer_id_ = selected_layer_ids_.empty() ? std::string() : selected_layer_ids_.back();
    if (selected_layer_ids_.empty())
        selection_anchor_layer_id_.clear();
    update();
    emit layers_selected(selected_layer_ids_);
}

void TimelineWidget::begin_layer_strip_drag(const std::string &layer_id, DragMode mode, double start_time)
{
    if (!title_) return;
    drag_mode_ = mode;
    drag_layer_id_ = layer_id;
    drag_start_time_ = start_time;
    dragged_layer_strips_.clear();

    std::set<std::string> ids;
    if (is_layer_selected(layer_id))
        ids.insert(selected_layer_ids_.begin(), selected_layer_ids_.end());
    ids.insert(layer_id);

    for (const auto &layer : title_->layers) {
        if (!layer || layer->locked || ids.find(layer->id) == ids.end()) continue;
        DraggedLayerStrip dragged;
        dragged.layer_id = layer->id;
        dragged.start_in = layer->in_time;
        dragged.start_out = layer->out_time;
        for (auto prop : timeline_properties(*layer)) {
            if (!prop) continue;
            for (int i = 0; i < (int)prop.keyframe_count(); ++i)
                dragged.keyframes.push_back({prop.name(), i, prop.keyframe_time((size_t)i)});
        }
        dragged_layer_strips_.push_back(std::move(dragged));
    }

    if (auto primary = title_->find_layer(layer_id)) {
        drag_start_in_ = primary->in_time;
        drag_start_out_ = primary->out_time;
    }
}

struct VisibleTimelineRow {
    int row = 0;
    TimelineRow entry;
};

static std::vector<VisibleTimelineRow> visible_timeline_rows(const std::shared_ptr<Title> &title,
                                                             int first_row, int last_row)
{
    std::vector<VisibleTimelineRow> rows;
    if (!title || last_row < first_row) return rows;

    int row = 0;
    for (auto it = title->layers.rbegin(); it != title->layers.rend(); ++it) {
        auto layer = *it;
        if (row >= first_row && row <= last_row)
            rows.push_back({row, {layer, TimelinePropertyRef{}, false}});
        ++row;
        if (row > last_row) break;

        if (!layer || !layer->properties_expanded) continue;
        std::set<std::string> seen;
        for (auto prop : timeline_properties(*layer)) {
            if (!prop.is_animated()) continue;
            QString label = property_label(prop.name());
            std::string key = label.toStdString();
            if (!seen.insert(key).second) continue;
            if (row >= first_row && row <= last_row)
                rows.push_back({row, {layer, prop, true}});
            ++row;
            if (row > last_row) break;
        }
        if (row > last_row) break;
    }
    return rows;
}

static QString timeline_blend_mode_short(EffectBlendMode mode)
{
    switch (mode) {
    case EffectBlendMode::Multiply: return bgl_tr("OBSTitles.BlendShortMultiply");
    case EffectBlendMode::Additive: return bgl_tr("OBSTitles.BlendShortAdditive");
    case EffectBlendMode::Screen: return bgl_tr("OBSTitles.BlendShortScreen");
    case EffectBlendMode::Overlay: return bgl_tr("OBSTitles.BlendShortOverlay");
    case EffectBlendMode::Color: return bgl_tr("OBSTitles.BlendShortColor");
    case EffectBlendMode::Normal:
    default: return bgl_tr("OBSTitles.BlendShortNormal");
    }
}

static QString timeline_layer_switches_text(const Title &title, const Layer &layer)
{
    QStringList tags;
    if (layer.mask_mode != MaskMode::None && !layer.mask_source_id.empty())
        tags << (layer.mask_mode == MaskMode::InvertedAlpha ? bgl_tr("OBSTitles.TrackMatteInvAlpha") :
                 layer.mask_mode == MaskMode::Luma ? bgl_tr("OBSTitles.TrackMatteLuma") :
                 layer.mask_mode == MaskMode::InvertedLuma ? bgl_tr("OBSTitles.TrackMatteInvLuma") :
                 bgl_tr("OBSTitles.TrackMatteAlpha"));
    if (!layer.parent_id.empty()) {
        QString parent_name = bgl_tr("OBSTitles.Parent");
        if (auto parent = title.find_layer(layer.parent_id))
            parent_name = QString::fromStdString(parent->name);
        tags << bgl_tr("OBSTitles.ParentNamed").arg(parent_name);
    }
    if (layer.blend_mode != EffectBlendMode::Normal)
        tags << bgl_tr("OBSTitles.ModeNamed").arg(timeline_blend_mode_short(layer.blend_mode));
    return tags.join(QStringLiteral("  ·  "));
}

void TimelineWidget::paintEvent(QPaintEvent *ev)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    int W = width(), H = height();
    const QRect dirty = ev ? ev->rect().intersected(rect()) : rect();
    if (dirty.isEmpty()) return;
    p.setClipRect(dirty);

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
    p.fillRect(dirty, window);

    double dur = title_ ? title_->duration : 10.0;
    double fps = obs_frame_rate();
    double frame_step = obs_frame_duration();
    int first_frame = std::max(0, (int)std::floor((scroll_x_ + dirty.left()) / pixels_per_sec_ / frame_step) - 1);
    int last_frame = (int)std::ceil((scroll_x_ + dirty.right()) / pixels_per_sec_ / frame_step) + 1;
    int label_every = std::max(1, (int)std::ceil(55.0 / (pixels_per_sec_ * frame_step)));

    auto draw_header = [&]() {
        const QRect header_dirty = dirty.intersected(QRect(0, 0, W, rh));
        if (header_dirty.isEmpty())
            return;

        p.save();
        p.setClipRect(header_dirty);

        /* Compact ruler/header. Keep this height matched with LayerStack's
         * column header so the first layer row starts at the same Y position
         * in both panes. */
        p.fillRect(QRect(0, 0, W, rh), ruler_bg);
        p.setPen(border);
        p.drawLine(0, rh - 1, W, rh - 1);

        for (int frame = first_frame; frame <= last_frame; ++frame) {
            double t = frame * frame_step;
            if (t > dur + frame_step) break;
            int x = time_to_x(t);
            if (x < dirty.left() - 64 || x > dirty.right() + 64) continue;
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
            const int cache_y = rh - 14;
            const int cache_h = 5;
            const int frame_w = std::max(1, (int)std::ceil(pixels_per_sec_ * frame_step));
            auto state_color = [](FrameCacheState state, bool static_frame) {
                switch (state) {
                case FrameCacheState::Queued: return QColor(96, 96, 96);
                case FrameCacheState::Rendering: return QColor(255, 202, 74);
                case FrameCacheState::CachedRam:
                    /* Dynamic RAM frames stay bright green; visually static/reused
                     * RAM frames are darker so the user can see cache reuse spans. */
                    return static_frame ? QColor(21, 112, 67) : QColor(39, 186, 103);
                case FrameCacheState::CachedDisk:
                    /* Disk-resident static frames are blue, distinct from RAM. */
                    return static_frame ? QColor(45, 105, 190) : QColor(74, 144, 226);
                case FrameCacheState::Stale: return QColor(214, 90, 90);
                case FrameCacheState::Disabled: return QColor(95, 95, 95);
                case FrameCacheState::NotCached:
                default: return QColor(0, 0, 0, 0);
                }
            };
            const bool cache_disabled = !CacheManager::instance().cacheEnabled() ||
                CacheManager::instance().titleCacheability(title_) == TitleCacheability::NonCacheable;
            if (cache_disabled) {
                p.fillRect(QRect(0, cache_y, W, cache_h), state_color(FrameCacheState::Disabled, false));
            } else {
                for (int frame = first_frame; frame <= last_frame; ++frame) {
                    const FrameCacheState state = CacheManager::instance().displayStateForFrame(title_, frame);
                    if (state == FrameCacheState::NotCached) continue;
                    const bool static_frame = (state == FrameCacheState::CachedRam || state == FrameCacheState::CachedDisk) &&
                        CacheManager::instance().displayFrameIsStatic(title_, frame);
                    const QColor color = state_color(state, static_frame);
                    if (color.alpha() == 0) continue;
                    const int x = time_to_x(frame * frame_step);
                    p.fillRect(QRect(x, cache_y, frame_w, cache_h), color);
                }
            }
            p.setPen(with_alpha(text, 70));
            p.drawLine(0, cache_y + cache_h, W, cache_y + cache_h);
        }

        if (title_ && title_->playback_mode == 1) {
            int loop_x0 = time_to_x(std::clamp(title_->loop_start, 0.0, dur));
            int loop_x1 = time_to_x(std::clamp(title_->loop_end, title_->loop_start, dur));
            if (loop_x1 > loop_x0) {
                p.fillRect(loop_x0, 18, loop_x1 - loop_x0, rh - 18, with_alpha(loop_color, 45));
                p.setPen(QPen(loop_color, 2));
                p.drawLine(loop_x0, 18, loop_x0, H);
                p.drawLine(loop_x1, 18, loop_x1, H);
                p.setPen(loop_color.lightness() < 128 ? loop_color.lighter(170) : loop_color.darker(170));
                p.drawText(loop_x0 + 4, 20, 80, 16, Qt::AlignVCenter, bgl_tr("OBSTitles.LoopIn"));
                p.drawText(loop_x1 + 4, 20, 80, 16, Qt::AlignVCenter, bgl_tr("OBSTitles.LoopOut"));
            }
        }
        if (title_ && title_->playback_mode == 2) {
            int pause_x = time_to_x(std::clamp(title_->pause_time, 0.0, dur));
            p.setPen(QPen(pause_color, 2));
            p.drawLine(pause_x, 12, pause_x, rh);
            p.setBrush(pause_color);
            p.setPen(Qt::NoPen);
            QPolygon marker;
            marker << QPoint(pause_x - 6, 12) << QPoint(pause_x + 6, 12) << QPoint(pause_x, 22);
            p.drawPolygon(marker);
            p.setPen(pause_color.lightness() < 128 ? pause_color.lighter(170) : pause_color.darker(170));
            p.drawText(pause_x + 4, 22, 100, 16, Qt::AlignVCenter, bgl_tr("OBSTitles.Pause"));
            p.setBrush(Qt::NoBrush);
        }
        p.restore();
    };

    /* Layer/property rows.  This uses the same row model as LayerStack so
     * keyframed property rows stay vertically aligned with the layer list.
     */
    const QRect body_dirty = dirty.intersected(QRect(0, rh, W, std::max(0, H - rh)));
    if (!body_dirty.isEmpty()) {
        p.save();
        p.setClipRect(body_dirty);
        const int first_dirty_row = std::max(0, (body_dirty.top() - rh + scroll_y_) / rowh);
        const int last_dirty_row = (body_dirty.bottom() - rh + scroll_y_) / rowh;
        const auto rows = visible_timeline_rows(title_, first_dirty_row, last_dirty_row);
        for (const auto &visible_row : rows) {
            const int row = visible_row.row;
            const auto &entry = visible_row.entry;
            auto &layer = entry.layer;
            int y = rh + row * rowh - scroll_y_;
            if (!body_dirty.intersects(QRect(0, y, W, rowh))) continue;
            bool sel = is_layer_selected(layer->id);

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

                /* Draw the normal layer label first so transition strips remain
                 * visually on top, like dedicated Premiere timeline items. */
                p.setPen(layer->visible ? text : disabled_text);
                const QString switches = title_ ? timeline_layer_switches_text(*title_, *layer) : QString();
                const QString layer_label = switches.isEmpty()
                    ? QString::fromStdString(layer->name)
                    : QStringLiteral("%1    [%2]").arg(QString::fromStdString(layer->name), switches);
                p.drawText(std::max(strip_rect.left(), 0) + 6, y, std::max(1, strip_rect.width() - 12), rowh,
                           Qt::AlignVCenter, layer_label);

                /* Premiere-style transition overlays live inside the layer
                 * strip. Their inner edge is the duration resize handle. */
                for (const auto &transition : layer->transitions) {
                    const QRect transition_bounds = transition_rect(*layer, transition, y);
                    const bool transition_selected = transition_target_selected_ &&
                        selected_transition_layer_id_ == layer->id &&
                        selected_transition_edge_ == transition.edge;
                    QColor transition_color = transition.kind == LayerTransitionKind::Text
                        ? QColor(112, 76, 156) : QColor(54, 111, 151);
                    if (!transition.enabled)
                        transition_color = transition_color.darker(155);
                    p.fillRect(transition_bounds, with_alpha(transition_color, 220));
                    p.save();
                    p.setClipRect(transition_bounds);
                    p.setPen(QPen(with_alpha(Qt::white, 45), 1));
                    for (int hx = transition_bounds.left() - transition_bounds.height();
                         hx < transition_bounds.right() + transition_bounds.height(); hx += 7)
                        p.drawLine(hx, transition_bounds.bottom(), hx + transition_bounds.height(), transition_bounds.top());
                    p.restore();
                    p.setPen(QPen(transition_selected ? highlighted_text : transition_color.lighter(170),
                                  transition_selected ? 2 : 1));
                    p.setBrush(Qt::NoBrush);
                    p.drawRect(transition_bounds.adjusted(0, 0, -1, -1));
                    if (!layer->locked) {
                        const int handle_x = transition.edge == LayerTransitionEdge::In
                            ? transition_bounds.right() - 2 : transition_bounds.left();
                        p.fillRect(handle_x, transition_bounds.top(), 3, transition_bounds.height(),
                                   with_alpha(Qt::white, 170));
                    }
                    if (transition_bounds.width() >= 56) {
                        p.setPen(Qt::white);
                        const QString transition_name = QString::fromStdString(transition.display_name);
                        p.drawText(transition_bounds.adjusted(5, 0, -5, 0),
                                   Qt::AlignVCenter | Qt::AlignHCenter,
                                   p.fontMetrics().elidedText(transition_name, Qt::ElideRight,
                                                              std::max(1, transition_bounds.width() - 10)));
                    }
                }

                if (transition_target_selected_ && selected_transition_layer_id_ == layer->id &&
                    !find_layer_transition(layer->transitions, selected_transition_edge_)) {
                    const QRect target_bounds = transition_edge_target_rect(*layer, selected_transition_edge_, y);
                    p.fillRect(target_bounds, with_alpha(highlight, 55));
                    p.setPen(QPen(highlighted_text, 2, Qt::DashLine));
                    p.setBrush(Qt::NoBrush);
                    p.drawRect(target_bounds.adjusted(1, 1, -2, -2));
                }

                if (transition_drop_preview_layer_id_ == layer->id) {
                    LayerTransition preview;
                    preview.edge = transition_drop_preview_edge_;
                    preview.duration = std::min(0.6, std::max(obs_frame_duration(),
                                                             (layer->out_time - layer->in_time) * 0.35));
                    const QRect preview_bounds = transition_rect(*layer, preview, y);
                    p.fillRect(preview_bounds, with_alpha(highlight, 80));
                    p.setPen(QPen(highlight, 2, Qt::DashLine));
                    p.setBrush(Qt::NoBrush);
                    p.drawRect(preview_bounds.adjusted(1, 1, -2, -2));
                }

                /* Trim handles for mouse resizing of unlocked layer in/out. */
                if (!layer->locked) {
                    p.fillRect(x0, y + 3, 4, rowh - 6, handle_color);
                    p.fillRect(x1 - 4, y + 3, 4, rowh - 6, handle_color);
                }

            } else {
                p.fillRect(x0, y + rowh / 2 - 1, x1 - x0, 2, border);
                p.setPen(disabled_text.isValid() ? disabled_text : with_alpha(text, 150));
                p.drawText(6, y, 150, rowh, Qt::AlignVCenter, property_label(entry.prop.name()));
            }

            auto draw_kf = [&](const TimelinePropertyRef &prop) {
                for (int i = 0; i < (int)prop.keyframe_count(); ++i) {
                    int kx = time_to_x(layer->in_time + prop.keyframe_time((size_t)i));
                    if (kx < body_dirty.left() - 10 || kx > body_dirty.right() + 10) continue;
                    int ky = y + rowh / 2;
                    const EasingType easing = prop.keyframe_easing((size_t)i);
                    QColor kf_fill = keyframe_color(easing);
                    if (!layer->visible)
                        kf_fill = kf_fill.darker(160);
                    const bool selected = is_keyframe_selected(layer->id, prop.name(), i);
                    if (selected) {
                        draw_keyframe_marker(p, QPointF(kx, ky), easing, 8.0,
                                             with_alpha(highlighted_text, 45),
                                             highlighted_text, 2.0);
                    }
                    draw_keyframe_marker(p, QPointF(kx, ky), easing, 5.0,
                                         selected ? kf_fill.lighter(125) : kf_fill,
                                         selected ? highlighted_text : border,
                                         selected ? 2.0 : 1.0);
                }
            };

            if (entry.is_property)
                draw_kf(entry.prop);
            else if (!layer->properties_expanded)
                for (auto prop : timeline_properties(*layer)) draw_kf(prop);
        }
        p.restore();
    }

    draw_header();

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
                                  TimelinePropertyRef *hit_prop, int *hit_kf_idx,
                                  int *hit_row_idx) const
{
    if (!title_ || pos.y() < ruler_height()) return false;
    auto rows = timeline_rows(title_);
    int row = (pos.y() - ruler_height() + scroll_y_) / row_height();
    if (row < 0 || row >= (int)rows.size()) return false;

    auto &entry = rows[row];
    constexpr int kHitRadius = 7;
    auto test_prop = [&](const TimelinePropertyRef &prop) -> bool {
        for (int i = 0; i < (int)prop.keyframe_count(); ++i) {
            int kx = time_to_x(entry.layer->in_time + prop.keyframe_time((size_t)i));
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
        for (auto prop : timeline_properties(*entry.layer)) {
            if (test_prop(prop)) return true;
        }
    }
    return false;
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent *ev)
{
    if (!title_) return;

    auto show_transition_menu = [&](const std::shared_ptr<Layer> &layer,
                                    LayerTransitionEdge edge) {
        if (!layer)
            return;
        select_layer_from_mouse(layer->id, Qt::NoModifier);
        select_transition_target(layer->id, edge);
        const bool has_transition = selected_transition() != nullptr;
        const bool editable = has_transition && !layer->locked;

        QMenu transition_menu(this);
        QAction *edit_action = transition_menu.addAction(bgl_tr("OBSTitles.EditTransition"));
        transition_menu.addSeparator();
        QAction *copy_action = transition_menu.addAction(bgl_tr("OBSTitles.Copy"));
        QAction *cut_action = transition_menu.addAction(bgl_tr("OBSTitles.Cut"));
        QAction *paste_action = transition_menu.addAction(bgl_tr("OBSTitles.Paste"));
        QAction *delete_action = transition_menu.addAction(bgl_tr("OBSTitles.Delete"));
        edit_action->setEnabled(editable);
        copy_action->setEnabled(has_transition);
        cut_action->setEnabled(editable);
        paste_action->setEnabled(can_paste_transition_to_selection());
        delete_action->setEnabled(editable);

        QAction *picked = transition_menu.exec(ev->globalPos());
        if (picked == edit_action) {
            emit transition_edit_requested(layer->id, static_cast<int>(edge));
        } else if (picked == copy_action) {
            copy_transition_selection();
        } else if (picked == cut_action) {
            cut_transition_selection();
        } else if (picked == paste_action) {
            paste_transition_to_selection();
        } else if (picked == delete_action) {
            delete_transition_selection();
        }
    };

    TransitionHit transition_hit;
    if (transition_hit_at_pos(ev->pos(), &transition_hit)) {
        show_transition_menu(transition_hit.layer, transition_hit.edge);
        return;
    }

    std::shared_ptr<Layer> transition_target_layer;
    LayerTransitionEdge transition_target_edge = LayerTransitionEdge::In;
    if (transition_edge_target_at_pos(ev->pos(), &transition_target_layer, &transition_target_edge)) {
        show_transition_menu(transition_target_layer, transition_target_edge);
        return;
    }

    std::shared_ptr<Layer> layer;
    TimelinePropertyRef hit_prop;
    int hit_idx = -1;
    const bool has_hit = hit_keyframe(ev->pos(), &layer, &hit_prop, &hit_idx, nullptr);
    if (has_hit && layer && layer->locked) return;
    if (!has_hit && keyframe_clipboard_.empty()) return;

    if (has_hit && layer && hit_prop && !is_keyframe_selected(layer->id, hit_prop.name(), hit_idx))
        select_keyframe(layer->id, hit_prop.name(), hit_idx, false, false);
    prune_keyframe_selection();

    QMenu menu(this);
    menu.setTitle(has_hit ? bgl_tr("OBSTitles.KeyframeEasing") : bgl_tr("OBSTitles.Paste"));

    QAction *copy_action = menu.addAction(bgl_tr("OBSTitles.Copy"));
    QAction *cut_action = menu.addAction(bgl_tr("OBSTitles.Cut"));
    QAction *paste_action = menu.addAction(bgl_tr("OBSTitles.Paste"));
    QAction *delete_action = menu.addAction(bgl_tr("OBSTitles.Delete"));
    const bool has_selection = !selected_keyframes_.empty();
    copy_action->setEnabled(has_selection);
    cut_action->setEnabled(has_selection);
    paste_action->setEnabled(!keyframe_clipboard_.empty());
    delete_action->setEnabled(has_selection);

    struct EasingChoice {
        QAction *action = nullptr;
        TimelinePropertyRef prop;
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
                                 TimelinePropertyRef prop, EasingType easing,
                                 const std::vector<int> &indices) {
        QAction *action = target_menu->addAction(swatch_icon(easing), label);
        action->setToolTip(easing == EasingType::Hold
            ? bgl_tr("OBSTitles.HoldEasingTooltip")
            : bgl_tr("OBSTitles.EasingTooltip"));
        action->setCheckable(true);
        action->setChecked(prop && std::all_of(indices.begin(), indices.end(), [&](int idx) {
            return idx >= 0 && idx < (int)prop.keyframe_count() &&
                   prop.keyframe_easing((size_t)idx) == easing;
        }));
        choices.push_back({action, prop, indices, easing});
        return action;
    };

    auto add_easing_group = [&](QMenu *target_menu, TimelinePropertyRef prop, const std::vector<int> &indices) {
        auto *group = new QActionGroup(target_menu);
        group->setExclusive(true);
        for (auto [label, easing] : std::initializer_list<std::pair<QString, EasingType>>{
                 {bgl_tr("OBSTitles.Linear"), EasingType::Linear},
                 {bgl_tr("OBSTitles.EasyEase"), EasingType::EaseInOut},
                 {bgl_tr("OBSTitles.EaseIn"), EasingType::EaseIn},
                 {bgl_tr("OBSTitles.EaseOut"), EasingType::EaseOut},
                 {bgl_tr("OBSTitles.Hold"), EasingType::Hold},
                 {bgl_tr("OBSTitles.CustomBezier"), EasingType::Bezier},
             }) {
            add_easing_action(target_menu, label, prop, easing, indices)->setActionGroup(group);
        }
    };

    if (has_hit && layer && hit_prop) {
        menu.addSeparator();
        QMenu *easing_menu = menu.addMenu(bgl_tr("OBSTitles.Easing"));
        QAction *header = easing_menu->addAction(QString("%1 · %2")
            .arg(QString::fromStdString(layer->name))
            .arg(property_label(hit_prop.name())));
        header->setEnabled(false);

        const bool has_previous_segment = hit_idx > 0;
        const bool has_next_segment = hit_idx + 1 < (int)hit_prop.keyframe_count();
        if (!has_previous_segment && !has_next_segment) {
            QAction *message = easing_menu->addAction(bgl_tr("OBSTitles.AddKeyframeForEasing"));
            message->setEnabled(false);
        } else {
            QAction *scope = easing_menu->addAction(has_previous_segment && has_next_segment
                ? bgl_tr("OBSTitles.EasingBothSegments")
                : has_next_segment ? bgl_tr("OBSTitles.EasingNextSegment") : bgl_tr("OBSTitles.EasingPreviousSegment"));
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
                QMenu *advanced = easing_menu->addMenu(bgl_tr("OBSTitles.ApplyOneSide"));
                QMenu *previous = advanced->addMenu(bgl_tr("OBSTitles.PreviousSegment"));
                add_easing_group(previous, hit_prop, {hit_idx - 1});
                QMenu *next = advanced->addMenu(bgl_tr("OBSTitles.NextSegment"));
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
        if (idx >= 0 && idx < (int)choice->prop.keyframe_count())
            choice->prop.apply_easing((size_t)idx, choice->easing);
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

    if (has_transition_target_selection()) {
        if (ev->matches(QKeySequence::Copy)) {
            copy_transition_selection();
            ev->accept();
            return;
        }
        if (ev->matches(QKeySequence::Cut)) {
            cut_transition_selection();
            ev->accept();
            return;
        }
        if (ev->matches(QKeySequence::Paste)) {
            paste_transition_to_selection();
            ev->accept();
            return;
        }
        if (ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) {
            delete_transition_selection();
            ev->accept();
            return;
        }
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

std::shared_ptr<Layer> TimelineWidget::layer_strip_at_pos(const QPoint &pos) const
{
    if (!title_ || pos.y() < ruler_height())
        return nullptr;

    const auto rows = timeline_rows(title_);
    const int row = (pos.y() - ruler_height() + scroll_y_) / row_height();
    if (row < 0 || row >= static_cast<int>(rows.size()) || rows[row].is_property)
        return nullptr;

    const auto layer = rows[row].layer;
    if (!layer || layer->locked)
        return nullptr;

    const int x0 = time_to_x(layer->in_time);
    const int x1 = time_to_x(layer->out_time);
    if (pos.x() < std::min(x0, x1) || pos.x() > std::max(x0, x1))
        return nullptr;
    return layer;
}


QRect TimelineWidget::transition_rect(const Layer &layer, const LayerTransition &transition, int row_y) const
{
    const double layer_duration = std::max(0.0, layer.out_time - layer.in_time);
    const double duration = std::clamp(transition.duration, 0.0, layer_duration);
    const double start = transition.edge == LayerTransitionEdge::In
        ? layer.in_time : layer.out_time - duration;
    const double end = transition.edge == LayerTransitionEdge::In
        ? layer.in_time + duration : layer.out_time;
    const int x0 = time_to_x(start);
    const int x1 = time_to_x(end);
    return QRect(std::min(x0, x1), row_y + 3, std::max(1, std::abs(x1 - x0)), row_height() - 6);
}

QRect TimelineWidget::transition_edge_target_rect(const Layer &layer,
                                                      LayerTransitionEdge edge,
                                                      int row_y) const
{
    const int x0 = time_to_x(layer.in_time);
    const int x1 = time_to_x(layer.out_time);
    const int left = std::min(x0, x1);
    const int right = std::max(x0, x1);
    const int strip_width = std::max(1, right - left);
    const int zone = std::min(strip_width, std::clamp(strip_width / 5, 14, 24));
    const int trim_reserve = std::min(7, std::max(0, zone - 1));
    const int target_width = std::max(1, zone - trim_reserve);
    const int x = edge == LayerTransitionEdge::In
        ? left + trim_reserve : right - zone;
    return QRect(x, row_y + 3, target_width, row_height() - 6);
}

bool TimelineWidget::transition_edge_target_at_pos(const QPoint &pos,
                                                  std::shared_ptr<Layer> *layer_out,
                                                  LayerTransitionEdge *edge_out) const
{
    if (!title_ || pos.y() < ruler_height())
        return false;
    const auto rows = timeline_rows(title_);
    const int row = (pos.y() - ruler_height() + scroll_y_) / row_height();
    if (row < 0 || row >= static_cast<int>(rows.size()) || rows[row].is_property)
        return false;
    const auto layer = rows[row].layer;
    if (!layer)
        return false;
    const int row_y = ruler_height() + row * row_height() - scroll_y_;
    const QRect in_rect = transition_edge_target_rect(*layer, LayerTransitionEdge::In, row_y);
    const QRect out_rect = transition_edge_target_rect(*layer, LayerTransitionEdge::Out, row_y);
    if (!in_rect.contains(pos) && !out_rect.contains(pos))
        return false;

    LayerTransitionEdge edge = LayerTransitionEdge::In;
    if (in_rect.contains(pos) && out_rect.contains(pos)) {
        const int in_distance = std::abs(pos.x() - in_rect.left());
        const int out_distance = std::abs(pos.x() - out_rect.right());
        edge = out_distance < in_distance ? LayerTransitionEdge::Out : LayerTransitionEdge::In;
    } else if (out_rect.contains(pos)) {
        edge = LayerTransitionEdge::Out;
    }
    if (layer_out) *layer_out = layer;
    if (edge_out) *edge_out = edge;
    return true;
}

bool TimelineWidget::transition_hit_at_pos(const QPoint &pos, TransitionHit *hit) const
{
    if (!title_ || pos.y() < ruler_height())
        return false;
    const auto rows = timeline_rows(title_);
    const int row = (pos.y() - ruler_height() + scroll_y_) / row_height();
    if (row < 0 || row >= static_cast<int>(rows.size()) || rows[row].is_property)
        return false;
    const auto layer = rows[row].layer;
    if (!layer)
        return false;
    const int row_y = ruler_height() + row * row_height() - scroll_y_;
    for (const auto &transition : layer->transitions) {
        const QRect rect = transition_rect(*layer, transition, row_y);
        if (!rect.adjusted(-2, 0, 2, 0).contains(pos))
            continue;
        const int handle_x = transition.edge == LayerTransitionEdge::In ? rect.right() : rect.left();
        if (hit) {
            hit->layer = layer;
            hit->edge = transition.edge;
            hit->rect = rect;
            hit->duration_handle = std::abs(pos.x() - handle_x) <= 7;
        }
        return true;
    }
    return false;
}

bool TimelineWidget::transition_drop_target_at_pos(const QPoint &pos,
                                                   std::shared_ptr<Layer> *layer_out,
                                                   LayerTransitionEdge *edge_out) const
{
    const auto layer = layer_strip_at_pos(pos);
    if (!layer || layer->locked)
        return false;
    const int x0 = time_to_x(layer->in_time);
    const int x1 = time_to_x(layer->out_time);
    const int left = std::min(x0, x1);
    const int right = std::max(x0, x1);
    const int strip_width = std::max(1, right - left);
    const int zone = std::clamp(strip_width / 3, 18, 110);
    LayerTransitionEdge edge;
    if (pos.x() <= left + zone)
        edge = LayerTransitionEdge::In;
    else if (pos.x() >= right - zone)
        edge = LayerTransitionEdge::Out;
    else
        return false;
    if (layer_out) *layer_out = layer;
    if (edge_out) *edge_out = edge;
    return true;
}

void TimelineWidget::normalize_transition_durations(Layer &layer)
{
    const double frame = obs_frame_duration();
    const double layer_duration = std::max(frame, layer.out_time - layer.in_time);
    LayerTransition *in_transition = find_layer_transition(layer.transitions, LayerTransitionEdge::In);
    LayerTransition *out_transition = find_layer_transition(layer.transitions, LayerTransitionEdge::Out);
    if (in_transition)
        in_transition->duration = std::clamp(in_transition->duration, frame, layer_duration);
    if (out_transition)
        out_transition->duration = std::clamp(out_transition->duration, frame, layer_duration);
    if (in_transition && out_transition && in_transition->duration + out_transition->duration > layer_duration) {
        if (drag_mode_ == DragMode::TransitionDuration && drag_transition_edge_ == LayerTransitionEdge::Out)
            out_transition->duration = std::max(frame, layer_duration - in_transition->duration);
        else
            in_transition->duration = std::max(frame, layer_duration - out_transition->duration);
    }
}

void TimelineWidget::clear_transition_drop_preview()
{
    if (transition_drop_preview_layer_id_.empty())
        return;
    transition_drop_preview_layer_id_.clear();
    update();
}

void TimelineWidget::dragEnterEvent(QDragEnterEvent *ev)
{
    if (ev && bgs::transitions::mime_has_transition_preset(ev->mimeData())) {
        ev->setDropAction(Qt::CopyAction);
        ev->accept();
        return;
    }
    if (ev && bgs::effects::mime_has_effect_preset(ev->mimeData())) {
        ev->setDropAction(Qt::CopyAction);
        ev->accept();
        return;
    }
    QWidget::dragEnterEvent(ev);
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent *ev)
{
    if (ev && bgs::transitions::mime_has_transition_preset(ev->mimeData())) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPoint pos = ev->position().toPoint();
#else
        const QPoint pos = ev->pos();
#endif
        std::shared_ptr<Layer> layer;
        LayerTransitionEdge edge = LayerTransitionEdge::In;
        if (transition_drop_target_at_pos(pos, &layer, &edge)) {
            const bool changed = transition_drop_preview_layer_id_ != layer->id ||
                                 transition_drop_preview_edge_ != edge;
            transition_drop_preview_layer_id_ = layer->id;
            transition_drop_preview_edge_ = edge;
            if (changed) update();
            ev->setDropAction(Qt::CopyAction);
            ev->accept();
        } else {
            clear_transition_drop_preview();
            ev->ignore();
        }
        return;
    }
    if (ev && bgs::effects::mime_has_effect_preset(ev->mimeData())) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPoint pos = ev->position().toPoint();
#else
        const QPoint pos = ev->pos();
#endif
        if (layer_strip_at_pos(pos)) {
            ev->setDropAction(Qt::CopyAction);
            ev->accept();
        } else {
            ev->ignore();
        }
        return;
    }
    QWidget::dragMoveEvent(ev);
}

void TimelineWidget::dropEvent(QDropEvent *ev)
{
    if (ev && bgs::transitions::mime_has_transition_preset(ev->mimeData())) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPoint pos = ev->position().toPoint();
#else
        const QPoint pos = ev->pos();
#endif
        std::shared_ptr<Layer> layer;
        LayerTransitionEdge edge = LayerTransitionEdge::In;
        const QString file_path = bgs::transitions::transition_preset_path_from_mime(ev->mimeData());
        if (transition_drop_target_at_pos(pos, &layer, &edge) && !file_path.isEmpty()) {
            clear_transition_drop_preview();
            select_layer_from_mouse(layer->id, Qt::NoModifier);
            select_transition_target(layer->id, edge);
            emit transition_preset_dropped(file_path, layer->id, static_cast<int>(edge));
            ev->setDropAction(Qt::CopyAction);
            ev->accept();
            return;
        }
        clear_transition_drop_preview();
        ev->ignore();
        return;
    }
    if (ev && bgs::effects::mime_has_effect_preset(ev->mimeData())) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPoint pos = ev->position().toPoint();
#else
        const QPoint pos = ev->pos();
#endif
        const auto layer = layer_strip_at_pos(pos);
        const QString file_path = bgs::effects::effect_preset_path_from_mime(ev->mimeData());
        if (layer && !file_path.isEmpty()) {
            emit effect_preset_dropped(file_path, layer->id);
            ev->setDropAction(Qt::CopyAction);
            ev->accept();
            return;
        }
        ev->ignore();
        return;
    }
    QWidget::dropEvent(ev);
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent *ev)
{
    clear_transition_drop_preview();
    QWidget::dragLeaveEvent(ev);
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
    dragged_layer_strips_.clear();
    marquee_moved_ = false;

    TransitionHit transition_hit;
    if (ev->button() == Qt::LeftButton && transition_hit_at_pos(ev->pos(), &transition_hit)) {
        if (!transition_hit.layer) {
            ev->accept();
            return;
        }
        select_layer_from_mouse(transition_hit.layer->id, ev->modifiers());
        select_transition_target(transition_hit.layer->id, transition_hit.edge);
        if (transition_hit.layer->locked) {
            ev->accept();
            return;
        }
        if (transition_hit.duration_handle) {
            const LayerTransition *transition = find_layer_transition(transition_hit.layer->transitions,
                                                                      transition_hit.edge);
            if (transition) {
                drag_mode_ = DragMode::TransitionDuration;
                drag_layer_id_ = transition_hit.layer->id;
                drag_transition_edge_ = transition_hit.edge;
                setCursor(Qt::SizeHorCursor);
                ev->accept();
                return;
            }
        }
        /* The transition overlay is its own timeline item. A normal click
         * selects it/layer but must not fall through into moving the layer strip. */
        ev->accept();
        return;
    }

    if (ev->pos().y() < ruler_height()) {
        clear_transition_selection();
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
    TimelinePropertyRef hit_prop;
    int hit_idx = -1;
    if (hit_keyframe(ev->pos(), &hit_layer, &hit_prop, &hit_idx, nullptr)) {
        clear_transition_selection();
        if (hit_layer && hit_layer->locked) {
            ev->accept();
            return;
        }
        if (hit_layer) select_layer_from_mouse(hit_layer->id, ev->modifiers());
        const bool shift = ev->modifiers() & Qt::ShiftModifier;
        if (shift) {
            select_keyframe(hit_layer->id, hit_prop.name(), hit_idx, true, true);
            ev->accept();
            return;
        }
        if (!is_keyframe_selected(hit_layer->id, hit_prop.name(), hit_idx))
            select_keyframe(hit_layer->id, hit_prop.name(), hit_idx, false, false);
        begin_keyframe_drag(hit_layer->id, hit_prop.name(), hit_idx,
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
        std::shared_ptr<Layer> edge_layer;
        LayerTransitionEdge edge = LayerTransitionEdge::In;
        if (ev->button() == Qt::LeftButton &&
            transition_edge_target_at_pos(ev->pos(), &edge_layer, &edge) && edge_layer == layer) {
            select_layer_from_mouse(layer->id, ev->modifiers());
            select_transition_target(layer->id, edge);
            if (layer->locked) {
                ev->accept();
                return;
            }
            if (std::abs(ev->pos().x() - x0) <= kTrimHit) {
                begin_layer_strip_drag(layer->id, DragMode::TrimIn, x_to_time(ev->pos().x()));
                setCursor(Qt::SizeHorCursor);
            } else if (std::abs(ev->pos().x() - x1) <= kTrimHit) {
                begin_layer_strip_drag(layer->id, DragMode::TrimOut, x_to_time(ev->pos().x()));
                setCursor(Qt::SizeHorCursor);
            }
            ev->accept();
            return;
        }
        const bool hit_strip = ev->pos().x() >= std::min(x0, x1) - kTrimHit &&
                               ev->pos().x() <= std::max(x0, x1) + kTrimHit;
        if (layer->locked && hit_strip) {
            ev->accept();
            return;
        }
        if (hit_strip) {
            clear_transition_selection();
            select_layer_from_mouse(layer->id, ev->modifiers());
        }
        if (std::abs(ev->pos().x() - x0) <= kTrimHit) {
            begin_layer_strip_drag(layer->id, DragMode::TrimIn, x_to_time(ev->pos().x()));
            setCursor(Qt::SizeHorCursor);
            ev->accept();
            return;
        }
        if (std::abs(ev->pos().x() - x1) <= kTrimHit) {
            begin_layer_strip_drag(layer->id, DragMode::TrimOut, x_to_time(ev->pos().x()));
            setCursor(Qt::SizeHorCursor);
            ev->accept();
            return;
        }
        if (ev->pos().x() >= std::min(x0, x1) && ev->pos().x() <= std::max(x0, x1)) {
            begin_layer_strip_drag(layer->id, DragMode::Layer, x_to_time(ev->pos().x()));
            setCursor(Qt::ClosedHandCursor);
            ev->accept();
            return;
        }
    }

    if (ev->button() == Qt::LeftButton && ev->pos().y() >= ruler_height()) {
        clear_transition_selection();
        if (!selected_layer_ids_.empty()) {
            set_selected_layers({});
            emit layers_selected({});
        }
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

    clear_transition_selection();
    set_selected_layers({});
    emit layers_selected({});
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
            auto prop = find_timeline_property(*layer, dragged.ref.prop_name);
            if (!prop || dragged.ref.index < 0 || dragged.ref.index >= (int)prop.keyframe_count()) continue;
            prop.set_keyframe_time((size_t)dragged.ref.index,
                                   std::clamp(dragged.start_time + delta, 0.0,
                                              std::max(0.0, layer->out_time - layer->in_time)));
        }
        update();
        return;
    }

    if (drag_mode_ == DragMode::TransitionDuration) {
        if (auto layer = title_->find_layer(drag_layer_id_)) {
            if (!layer->locked) {
                if (auto *transition = find_layer_transition(layer->transitions, drag_transition_edge_)) {
                    transition->duration = drag_transition_edge_ == LayerTransitionEdge::In
                        ? t - layer->in_time : layer->out_time - t;
                    normalize_transition_durations(*layer);
                    update();
                }
            }
        }
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
        const double delta = t - drag_start_time_;
        if (dragged_layer_strips_.empty()) {
            if (auto layer = title_->find_layer(drag_layer_id_)) {
                DraggedLayerStrip dragged;
                dragged.layer_id = layer->id;
                dragged.start_in = layer->in_time;
                dragged.start_out = layer->out_time;
                for (auto prop : timeline_properties(*layer)) {
                    if (!prop) continue;
                    for (int i = 0; i < (int)prop.keyframe_count(); ++i)
                        dragged.keyframes.push_back({prop.name(), i, prop.keyframe_time((size_t)i)});
                }
                dragged_layer_strips_.push_back(std::move(dragged));
            }
        }
        for (const auto &dragged : dragged_layer_strips_) {
            auto layer = title_->find_layer(dragged.layer_id);
            if (!layer || layer->locked) continue;
            if (drag_mode_ == DragMode::TrimIn) {
                const double new_in = std::clamp(dragged.start_in + delta,
                                                 0.0,
                                                 std::max(0.0, dragged.start_out - obs_frame_duration()));
                const double keyframe_offset = dragged.start_in - new_in;
                layer->in_time = new_in;
                layer->out_time = dragged.start_out;
                for (const auto &keyframe : dragged.keyframes) {
                    auto prop = find_timeline_property(*layer, keyframe.prop_name);
                    if (!prop || keyframe.index < 0 || keyframe.index >= (int)prop.keyframe_count()) continue;
                    prop.set_keyframe_time((size_t)keyframe.index,
                                           std::clamp(keyframe.start_time + keyframe_offset,
                                                      0.0,
                                                      std::max(0.0, layer->out_time - layer->in_time)));
                }
                normalize_transition_durations(*layer);
            } else {
                layer->in_time = dragged.start_in;
                layer->out_time = std::clamp(dragged.start_out + delta,
                                             dragged.start_in + obs_frame_duration(),
                                             title_->duration);
                normalize_transition_durations(*layer);
            }
        }
        update();
        return;
    }

    if (drag_mode_ == DragMode::Layer) {
        double delta = t - drag_start_time_;
        if (dragged_layer_strips_.empty()) {
            if (auto layer = title_->find_layer(drag_layer_id_)) {
                DraggedLayerStrip dragged;
                dragged.layer_id = layer->id;
                dragged.start_in = layer->in_time;
                dragged.start_out = layer->out_time;
                dragged_layer_strips_.push_back(std::move(dragged));
            }
        }
        double min_delta = -std::numeric_limits<double>::max();
        double max_delta = std::numeric_limits<double>::max();
        for (const auto &dragged : dragged_layer_strips_) {
            const double duration = std::max(obs_frame_duration(), dragged.start_out - dragged.start_in);
            min_delta = std::max(min_delta, -dragged.start_in);
            max_delta = std::min(max_delta, std::max(0.0, title_->duration - duration) - dragged.start_in);
        }
        if (std::isfinite(min_delta) && std::isfinite(max_delta))
            delta = std::clamp(delta, min_delta, max_delta);
        for (const auto &dragged : dragged_layer_strips_) {
            auto layer = title_->find_layer(dragged.layer_id);
            if (!layer || layer->locked) continue;
            const double duration = std::max(obs_frame_duration(), dragged.start_out - dragged.start_in);
            const double new_in = std::clamp(dragged.start_in + delta,
                                             0.0,
                                             std::max(0.0, title_->duration - duration));
            layer->in_time = new_in;
            layer->out_time = std::min(title_->duration, new_in + duration);
        }
        update();
        return;
    }

    auto rows = timeline_rows(title_);
    int row = (ev->pos().y() - ruler_height() + scroll_y_) / row_height();
    if (row >= 0 && row < (int)rows.size() && !rows[row].is_property) {
        TransitionHit transition_hit;
        if (transition_hit_at_pos(ev->pos(), &transition_hit) && transition_hit.layer) {
            if (!transition_hit.layer->locked && transition_hit.duration_handle)
                setCursor(Qt::SizeHorCursor);
            else
                setCursor(Qt::ArrowCursor);
            return;
        }
        std::shared_ptr<Layer> transition_target_layer;
        LayerTransitionEdge transition_target_edge = LayerTransitionEdge::In;
        if (transition_edge_target_at_pos(ev->pos(), &transition_target_layer,
                                          &transition_target_edge)) {
            setCursor(Qt::ArrowCursor);
            return;
        }
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
                   drag_mode_ == DragMode::TransitionDuration ||
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
            auto prop = layer ? find_timeline_property(*layer, ref.prop_name) : TimelinePropertyRef{};
            if (prop && ref.index >= 0 && ref.index < (int)prop.keyframe_count())
                selected_times[ref] = prop.keyframe_time(ref.index);
        }

        std::set<std::pair<std::string, std::string>> props_to_sort;
        for (const auto &dragged : dragged_keyframes_)
            props_to_sort.insert({dragged.ref.layer_id, dragged.ref.prop_name});

        for (const auto &prop_ref : props_to_sort) {
            if (auto layer = title_->find_layer(prop_ref.first)) {
                if (layer->locked) continue;
                if (auto prop = find_timeline_property(*layer, prop_ref.second)) {
                    prop.sort_keyframes();
                }
            }
        }

        std::set<KeyframeRef> remapped;
        std::map<std::pair<std::string, std::string>, std::set<int>> used_indices;
        for (const auto &[ref, selected_time] : selected_times) {
            auto layer = title_->find_layer(ref.layer_id);
            auto prop = layer ? find_timeline_property(*layer, ref.prop_name) : TimelinePropertyRef{};
            if (!prop) continue;
            int best = -1;
            double best_distance = std::numeric_limits<double>::max();
            auto key = std::make_pair(ref.layer_id, ref.prop_name);
            for (int i = 0; i < (int)prop.keyframe_count(); ++i) {
                if (used_indices[key].count(i)) continue;
                double distance = std::abs(prop.keyframe_time(i) - selected_time);
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

    const bool transition_changed = drag_mode_ == DragMode::TransitionDuration;
    drag_mode_ = DragMode::None;
    drag_layer_id_.clear();
    drag_prop_name_.clear();
    drag_keyframe_index_ = -1;
    drag_start_time_ = 0.0;
    drag_start_in_ = 0.0;
    drag_start_out_ = 0.0;
    dragged_keyframes_.clear();
    dragged_layer_strips_.clear();
    marquee_additive_ = false;
    marquee_moved_ = false;
    unsetCursor();
    update();
    if (transition_changed)
        emit transition_modified();
    else if (changed)
        emit keyframe_easing_changed();
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent *ev)
{
    TransitionHit hit;
    if (ev && transition_hit_at_pos(ev->pos(), &hit) && hit.layer && !hit.layer->locked) {
        emit transition_edit_requested(hit.layer->id, static_cast<int>(hit.edge));
        ev->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(ev);
}

/* ══════════════════════════════════════════════════════════════════
 *  TitlePropertiesPanel
 * ══════════════════════════════════════════════════════════════════ */
