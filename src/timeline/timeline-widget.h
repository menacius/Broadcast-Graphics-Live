#pragma once

#include "title-data.h"
#include "title-rich-text.h"

#include <QWidget>
#include <QGroupBox>
#include <QScrollArea>
#include <QListWidget>
#include <QToolButton>
#include <QActionGroup>
#include <QButtonGroup>
#include <QMenu>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QTextEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QSlider>
#include <QPointF>
#include <QPoint>
#include <QRectF>
#include <QColor>
#include <QPixmap>
#include <QElapsedTimer>
#include <QJsonObject>
#include <memory>
#include <string>
#include <vector>
#include <set>

class QEvent;
class QMouseEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDragLeaveEvent;
class QDropEvent;
class QWheelEvent;
class QKeyEvent;
class QContextMenuEvent;
class QResizeEvent;
class QPaintEvent;
class QPainter;
class QScrollBar;
struct TimelinePropertyRef;
/* ══════════════════════════════════════════════════════════════════
 *  TimelineWidget  – keyframe timeline
 * ══════════════════════════════════════════════════════════════════ */
class TimelineWidget : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget *parent = nullptr);

    void set_title(std::shared_ptr<Title> t);
    void set_selected_layer(const std::string &lid);
    void set_selected_layers(const std::vector<std::string> &layer_ids);
    void set_playhead(double t);
    void set_vertical_scroll(int scroll_y);
    void set_zoom_percent(int percent);
    int zoom_percent() const;
    void fit_timeline();
    bool has_selected_keyframes() const;
    bool has_keyframe_clipboard() const;
    bool copy_keyframe_selection();
    bool cut_keyframe_selection();
    bool delete_keyframe_selection();
    bool paste_keyframes_at_playhead();
    bool has_transition_target_selection() const;
    bool has_selected_transition() const;
    bool has_transition_clipboard() const;
    bool can_paste_transition_to_selection() const;
    bool copy_transition_selection();
    bool cut_transition_selection();
    bool delete_transition_selection();
    bool paste_transition_to_selection();
    void clear_transition_target_selection();

signals:
    void playhead_changed(double t);
    void keyframe_added(const std::string &layer_id,
                        const std::string &prop_name, double t);
    void keyframe_moved(const std::string &layer_id,
                        const std::string &prop_name, int kf_idx, double new_t);
    void keyframe_easing_changed();
    void vertical_scroll_delta_requested(int delta);
    void zoom_percent_changed(int percent);
    void layer_selected(const std::string &layer_id);
    void layers_selected(const std::vector<std::string> &layer_ids);
    void effect_preset_dropped(const QString &file_path, const std::string &layer_id);
    void transition_preset_dropped(const QString &file_path, const std::string &layer_id, int edge);
    void transition_edit_requested(const std::string &layer_id, int edge);
    void transition_modified();

protected:
    void paintEvent(QPaintEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void mouseDoubleClickEvent(QMouseEvent *ev) override;
    void dragEnterEvent(QDragEnterEvent *ev) override;
    void dragMoveEvent(QDragMoveEvent *ev) override;
    void dragLeaveEvent(QDragLeaveEvent *ev) override;
    void dropEvent(QDropEvent *ev) override;
    void keyPressEvent(QKeyEvent *ev) override;
    void contextMenuEvent(QContextMenuEvent *ev) override;
    void wheelEvent(QWheelEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;

private:
    double x_to_time(int x) const;
    int    time_to_x(double t) const;
    int    ruler_height() const { return 44; }
    int    row_height()   const { return 28; }
    double snap_time(double t) const;
    void   clamp_scroll();
    void   clamp_vertical_scroll();
    int    max_vertical_scroll() const;
    bool   hit_keyframe(const QPoint &pos, std::shared_ptr<Layer> *layer,
                        TimelinePropertyRef *prop, int *kf_idx, int *row_idx) const;
    struct KeyframeRef {
        std::string layer_id;
        std::string prop_name;
        int index = -1;
        bool operator<(const KeyframeRef &other) const;
    };
    struct DraggedKeyframe {
        KeyframeRef ref;
        double start_time = 0.0;
    };
    struct ClipboardKeyframe {
        std::string layer_id;
        std::string prop_name;
        Keyframe keyframe;
        VectorKeyframe vector_keyframe;
        std::vector<Keyframe> scalar_group_keyframes;
        QJsonObject extension_keyframe;
        bool is_vector = false;
        bool is_scalar_group = false;
        bool is_extension = false;
        double offset = 0.0;
    };
    struct DraggedLayerStrip {
        struct KeyframeTime {
            std::string prop_name;
            int index = -1;
            double start_time = 0.0;
        };
        std::string layer_id;
        double start_in = 0.0;
        double start_out = 0.0;
        std::vector<KeyframeTime> keyframes;
    };
    enum class DragMode { None, Playhead, Keyframe, Marquee, TrimIn, TrimOut, Layer, TransitionDuration, LoopStart, LoopEnd, PauseMarker };
    struct TransitionHit {
        std::shared_ptr<Layer> layer;
        LayerTransitionEdge edge = LayerTransitionEdge::In;
        QRect rect;
        bool duration_handle = false;
    };

    void   clear_keyframe_selection();
    void   prune_keyframe_selection();
    bool   is_keyframe_selected(const std::string &layer_id, const std::string &prop_name, int kf_idx) const;
    void   select_keyframe(const std::string &layer_id, const std::string &prop_name, int kf_idx, bool additive, bool toggle);
    void   select_keyframes_in_rect(const QRect &rect, bool additive);
    bool   copy_selected_keyframes();
    bool   delete_selected_keyframes();
    bool   cut_selected_keyframes();
    bool   paste_keyframes_at(double timeline_time);
    QRect  marquee_rect() const;
    void   begin_keyframe_drag(const std::string &layer_id, const std::string &prop_name, int kf_idx, double start_time);
    TimelinePropertyRef find_timeline_property(Layer &layer, const std::string &prop_name) const;
    bool   keep_playhead_visible();
    void   set_pixels_per_sec(double pixels_per_sec, double anchor_time, int anchor_x);
    bool   is_layer_selected(const std::string &layer_id) const;
    void   select_layer_from_mouse(const std::string &layer_id, Qt::KeyboardModifiers modifiers);
    void   begin_layer_strip_drag(const std::string &layer_id, DragMode mode, double start_time);
    QRect  playhead_dirty_rect(int playhead_x) const;
    std::shared_ptr<Layer> layer_strip_at_pos(const QPoint &pos) const;
    bool transition_hit_at_pos(const QPoint &pos, TransitionHit *hit) const;
    bool transition_drop_target_at_pos(const QPoint &pos, std::shared_ptr<Layer> *layer,
                                       LayerTransitionEdge *edge) const;
    bool transition_edge_target_at_pos(const QPoint &pos, std::shared_ptr<Layer> *layer,
                                       LayerTransitionEdge *edge) const;
    QRect transition_rect(const Layer &layer, const LayerTransition &transition, int row_y) const;
    QRect transition_edge_target_rect(const Layer &layer, LayerTransitionEdge edge, int row_y) const;
    void normalize_transition_durations(Layer &layer);
    void clear_transition_drop_preview();
    void select_transition_target(const std::string &layer_id, LayerTransitionEdge edge);
    void clear_transition_selection();
    std::shared_ptr<Layer> selected_transition_layer() const;
    const LayerTransition *selected_transition() const;
    LayerTransition *selected_transition();
    bool layer_accepts_transition(const Layer &layer, const LayerTransition &transition) const;

    std::shared_ptr<Title> title_;
    std::string sel_layer_id_;
    std::vector<std::string> selected_layer_ids_;
    std::string selection_anchor_layer_id_;
    bool fit_on_next_resize_ = false;
    double playhead_  = 0.0;
    DragMode drag_mode_ = DragMode::None;
    std::string drag_layer_id_;
    std::string drag_prop_name_;
    int drag_keyframe_index_ = -1;
    double drag_start_time_ = 0.0;
    double drag_start_in_ = 0.0;
    double drag_start_out_ = 0.0;
    LayerTransitionEdge drag_transition_edge_ = LayerTransitionEdge::In;
    std::set<KeyframeRef> selected_keyframes_;
    std::vector<DraggedKeyframe> dragged_keyframes_;
    std::vector<DraggedLayerStrip> dragged_layer_strips_;
    std::vector<ClipboardKeyframe> keyframe_clipboard_;
    QPoint marquee_start_;
    QPoint marquee_current_;
    bool marquee_additive_ = false;
    bool marquee_moved_ = false;
    double pixels_per_sec_ = 80.0;
    int    scroll_x_       = 0;
    int    scroll_y_       = 0;
    std::string transition_drop_preview_layer_id_;
    LayerTransitionEdge transition_drop_preview_edge_ = LayerTransitionEdge::In;
    bool transition_target_selected_ = false;
    std::string selected_transition_layer_id_;
    LayerTransitionEdge selected_transition_edge_ = LayerTransitionEdge::In;
    bool transition_clipboard_valid_ = false;
    LayerTransition transition_clipboard_;
};
