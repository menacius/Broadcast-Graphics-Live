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
#include <memory>
#include <string>
#include <vector>
#include <set>

class QEvent;
class QMouseEvent;
class QWheelEvent;
class QKeyEvent;
class QContextMenuEvent;
class QResizeEvent;
class QPaintEvent;
class QDragEnterEvent;
class QDropEvent;
class QDragMoveEvent;
class QMimeData;
class QPainter;
class QScrollBar;
/* ══════════════════════════════════════════════════════════════════
 *  CanvasPreview  – renders the title at the current playhead
 * ══════════════════════════════════════════════════════════════════ */
class CanvasPreview : public QWidget {
    Q_OBJECT

public:
    explicit CanvasPreview(QWidget *parent = nullptr);

    struct ViewState {
        int zoom_percent = 100;
        bool fit_zoom_active = true;
        bool fit_zoom_up_to_100 = false;
        QPointF pan_offset;
    };

    void set_title(std::shared_ptr<Title> t, bool preserve_view = false);
    ViewState view_state() const;
    void restore_view_state(const ViewState &state);
    void set_playhead(double t);
    void set_selected_layer(const std::string &lid);
    void set_selected_layers(const std::vector<std::string> &ids);
    void set_safe_guides_visible(bool visible);
    void set_rulers_visible(bool visible);
    void set_guides_visible(bool visible);
    void set_guides_locked(bool locked);
    void set_show_guide_coordinates(bool visible);
    void set_canvas_border_visible(bool visible);
    void clear_user_guides();
    bool rulers_visible() const { return rulers_visible_; }
    bool guides_visible() const { return guides_visible_; }
    bool guides_locked() const { return guides_locked_; }
    bool show_guide_coordinates() const { return show_guide_coordinates_; }
    bool canvas_border_visible() const { return canvas_border_visible_; }
    void set_snap_enabled(bool enabled);
    void set_snap_to_guides(bool enabled);
    void set_snap_to_grid(bool enabled);
    void set_snap_to_object_edges(bool enabled);
    void set_snap_to_object_centers(bool enabled);
    void set_snap_to_canvas_bounds(bool enabled);
    void set_snap_to_spacing(bool enabled);
    void refresh_preview();
    void clear_rendered_frame();
    void set_zoom_percent(int percent);
    int zoom_percent() const;
    void fit_canvas(bool up_to_100 = false);
    void set_checkerboard_pattern(int pattern);
    void set_selection_tool_active();
    void set_shape_tool_active(ShapeType shape_type);
    void set_text_tool_active(LayerType type);
    void set_color_picker_tool_active();
    void set_gradient_tool_active();
    void set_gradient_editor_active(bool active);
    void begin_text_edit_for_layer(const std::string &layer_id);
    void apply_active_text_char_format(const std::string &layer_id, const RichTextCharFormat &format, uint32_t mask);
    QPointF view_center_canvas_point() const;

signals:
    void layer_clicked(const std::string &layer_id);
    void layers_selected(const std::vector<std::string> &layer_ids);
    void layer_geometry_changed();
    void layer_structure_changed();
    void zoom_percent_changed(int percent);
    void shape_drawing_started(ShapeType shape_type, const QPointF &canvas_pt);
    void text_drawing_started(LayerType type, const QPointF &canvas_pt);
    void shape_drawing_changed(const QRectF &canvas_rect);
    void shape_drawing_finished(bool keep_layer);
    void text_edit_changed(const std::string &layer_id);
    void text_edit_cursor_changed(const std::string &layer_id);
    void text_edit_committed(const std::string &layer_id);
    void color_picked(const QColor &color);
    void external_image_layer_requested(const QString &image_path, const QPointF &canvas_pt);
    void external_text_layer_requested(const QString &text, const QPointF &canvas_pt);

protected:
    void paintEvent(QPaintEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void mouseDoubleClickEvent(QMouseEvent *ev) override;
    void keyPressEvent(QKeyEvent *ev) override;
    void dragEnterEvent(QDragEnterEvent *ev) override;
    void dragMoveEvent(QDragMoveEvent *ev) override;
    void dropEvent(QDropEvent *ev) override;
    void leaveEvent(QEvent *ev) override;
    void contextMenuEvent(QContextMenuEvent *ev) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void wheelEvent(QWheelEvent *ev) override;
    void resizeEvent(QResizeEvent *ev) override;

private:
    enum class DragMode {
        None,
        Marquee,
        Move,
        ResizeNW,
        ResizeN,
        ResizeNE,
        ResizeE,
        ResizeSE,
        ResizeS,
        ResizeSW,
        ResizeW,
        Origin,
        Rotate,
        GradientStart,
        GradientEnd,
        GradientCenter,
        GradientRadius,
        GradientFocal,
        CornerRadiusTL,
        CornerRadiusTR,
        CornerRadiusBR,
        CornerRadiusBL,
        GuideX,
        GuideY
    };
    enum class CanvasTool { Selection, Shape, Text, ColorPicker, Gradient };

    struct GradientHandleGeometry {
        bool valid = false;
        bool radial = false;
        QRectF local_rect;
        QPointF center;
        QPointF start;
        QPointF end;
        QPointF radius;
        QPointF focal;
    };

    void render_to_pixmap();
    void update_layer_panels(std::shared_ptr<Layer> layer, double playhead);
    std::shared_ptr<Layer> selected_layer() const;
    std::vector<std::shared_ptr<Layer>> selected_layers() const;
    QRectF layer_local_rect(const Layer &layer) const;
    double fit_scale() const;
    double view_scale() const;
    QPointF centered_view_origin() const;
    QPointF view_origin() const;
    QPointF view_to_canvas(const QPointF &view_pt) const;
    QPointF canvas_to_view(const QPointF &canvas_pt) const;
    QPointF canvas_to_layer(const Layer &layer, const QPointF &canvas_pt) const;
    QPointF layer_to_canvas(const Layer &layer, const QPointF &layer_pt) const;
    DragMode hit_test_selected(const QPointF &view_pt) const;
    bool active_draw_tool_can_manipulate_selected() const;
    void set_cursor_for_drag_mode(DragMode mode, bool dragging);
    bool gradient_handles_visible() const;
    bool layer_supports_gradient_handles(const Layer &layer) const;
    GradientHandleGeometry gradient_handle_geometry(const Layer &layer) const;
    DragMode hit_test_gradient_handles(const Layer &layer, const QPointF &view_pt) const;
    void draw_gradient_handles(QPainter &p, const Layer &layer);
    void begin_gradient_drag(const Layer &layer);
    bool apply_gradient_drag(const QPointF &view_pt, Qt::KeyboardModifiers modifiers);
    bool begin_gradient_tool_drag(const QPointF &view_pt, Qt::KeyboardModifiers modifiers);
    bool layer_supports_corner_radius_handles(const Layer &layer) const;
    QPointF corner_radius_handle_local_pos(const Layer &layer, const QRectF &box, DragMode mode) const;
    QPointF corner_radius_visual_offset_view(const Layer &layer, const QRectF &box, DragMode mode) const;
    QPointF corner_radius_handle_view_pos(const Layer &layer, const QRectF &box, DragMode mode) const;
    DragMode hit_test_corner_radius_handles(const Layer &layer, const QPointF &view_pt) const;
    void draw_corner_radius_handles(QPainter &p, const Layer &layer);
    void begin_corner_radius_drag(const Layer &layer);
    bool apply_corner_radius_drag(const QPointF &view_pt, Qt::KeyboardModifiers modifiers);
    QRectF layer_canvas_bounds(const Layer &layer) const;
    QRectF selected_canvas_bounds() const;
    void begin_marquee(const QPointF &view_pt, Qt::KeyboardModifiers modifiers);
    void update_marquee(const QPointF &view_pt, Qt::KeyboardModifiers modifiers);
    bool duplicate_selected_layers_for_drag();
    bool nudge_selected_layers(double dx, double dy);
    QPointF snap_delta_for_bounds(const QRectF &start_bounds, const QPointF &delta, bool snap_x, bool snap_y,
                                  bool allow_snap = true);
    QPointF snap_canvas_point(const QPointF &canvas_pt, bool snap_x, bool snap_y, bool allow_snap = true);
    void collect_snap_targets(bool x_axis, std::vector<double> &targets, std::vector<QString> &labels) const;
    QRectF canvas_view_rect() const;
    QRectF ruler_top_rect() const;
    QRectF ruler_left_rect() const;
    QRectF ruler_corner_rect() const;
    bool ruler_hit_test(const QPointF &view_pt, bool &vertical_guide) const;
    int guide_hit_test(const QPointF &view_pt, bool &x_axis, bool include_locked = false) const;
    double snap_guide_value_to_objects(bool x_axis, double raw_value);
    void draw_rulers(QPainter &p, const QRectF &canvas_rect, double scale, const QPointF &origin);
    void draw_user_guides(QPainter &p, const QRectF &canvas_rect);
    void draw_guide_coordinate(QPainter &p, const QPointF &view_pt, bool x_axis, double value) const;
    void save_ruler_guide_settings() const;
    void load_ruler_guide_settings();
    void collect_spacing_targets(bool x_axis, std::vector<double> &targets, std::vector<QString> &labels) const;
    void clear_snap_feedback();
    void add_snap_feedback(bool x_axis, double value, const QString &label);
    void apply_drag(const QPointF &view_pt, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    QRectF toolbar_draw_rect(const QPointF &canvas_pt, Qt::KeyboardModifiers modifiers) const;
    QRectF snapped_toolbar_draw_rect(const QRectF &raw_rect, bool allow_snap = true);
    double toolbar_draw_aspect_ratio() const;
    QRect toolbar_preview_update_rect() const;
    void draw_toolbar_preview(QPainter &p);
    bool sample_color_at_view(const QPointF &view_pt, QColor &color);
    void update_color_picker_tooltip(const QPointF &view_pt);
    void draw_color_picker_tooltip(QPainter &p);
    bool mime_has_external_canvas_content(const QMimeData *mime) const;
    bool handle_external_canvas_mime(const QMimeData *mime, const QPointF &canvas_pt);
    QString canvas_drag_tooltip_text() const;
    void draw_canvas_drag_tooltip(QPainter &p);
    QRect snap_cursor_update_rect() const;
    QRect final_snap_cursor_update_rect() const;
    QRect canvas_drag_tooltip_update_rect() const;
    void clear_draw_tool_snap_cursor();
    void update_draw_tool_snap_cursor(const QPointF &view_pt, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void draw_snap_cursor_indicator(QPainter &p);
    void update_shape_drawing(const QPointF &view_pt, Qt::KeyboardModifiers modifiers = Qt::NoModifier);
    void begin_text_edit(const std::shared_ptr<Layer> &layer);
    void commit_text_edit(bool accept_changes = true);
    void position_text_editor();
    void configure_inline_text_editor(const Layer &layer);
    bool sync_inline_text_layer(bool mark_dirty);
    void refresh_inline_text_edit(bool mark_dirty, bool emit_changed);
    double inline_text_visual_scale(const Layer &layer) const;
    QRectF inline_text_document_local_rect(const Layer &layer) const;
    std::shared_ptr<Layer> layer_at_view_pos(const QPointF &view_pt) const;
    std::shared_ptr<Layer> text_layer_at_view_pos(const QPointF &view_pt) const;
    void update_hover_layer(const QPointF &view_pt);
    void draw_hover_layer_box(QPainter &p);
    void draw_canvas_border(QPainter &p, const QRectF &canvas_rect);
    void draw_ruler_mouse_indicators(QPainter &p, const QRectF &canvas_rect);
    void invalidate_checkerboard_cache();
    void draw_static_checkerboard(QPainter &p, const QRect &canvas_rect_px);

    std::shared_ptr<Title> title_;
    std::string sel_layer_id_;
    std::vector<std::string> selected_layer_ids_;
    double playhead_ = 0.0;
    int zoom_percent_ = 100;
    bool fit_zoom_active_ = true;
    bool fit_zoom_up_to_100_ = false;
    QPointF pan_offset_;
    bool panning_ = false;
    QPointF pan_start_view_;
    QPointF pan_start_offset_;
    QPixmap frame_pixmap_;
    QPoint frame_pixmap_canvas_offset_;
    QSize frame_pixmap_canvas_size_;
    bool dirty_ = true;
    bool safe_guides_visible_ = false;
    bool rulers_visible_ = false;
    bool guides_visible_ = true;
    bool guides_locked_ = false;
    bool show_guide_coordinates_ = true;
    bool canvas_border_visible_ = true;
    bool mouse_inside_canvas_ = false;
    QPointF last_mouse_view_pos_;
    std::string hovered_layer_id_;
    std::vector<double> vertical_guides_;
    std::vector<double> horizontal_guides_;
    bool dragging_new_guide_ = false;
    bool dragging_guide_x_axis_ = true;
    int dragging_guide_index_ = -1;
    double dragging_guide_value_ = 0.0;
    int checkerboard_pattern_ = 1;
    QPixmap checkerboard_tile_;
    int checkerboard_tile_pattern_ = -1;
    CanvasTool active_tool_ = CanvasTool::Selection;
    ShapeType active_shape_type_ = ShapeType::Rectangle;
    LayerType active_text_layer_type_ = LayerType::Text;
    bool drawing_shape_ = false;
    bool drawing_shape_changed_ = false;
    QTextEdit *inline_text_editor_ = nullptr;
    std::string inline_text_layer_id_;
    double inline_text_last_visual_scale_ = 0.0;
    bool committing_inline_text_ = false;
    bool updating_inline_text_editor_ = false;
    bool refreshing_inline_text_ = false;
    QPointF shape_draw_start_canvas_;
    QPointF shape_draw_current_canvas_;
    QRectF shape_draw_current_rect_;
    Qt::KeyboardModifiers shape_draw_modifiers_ = Qt::NoModifier;
    bool snap_cursor_visible_ = false;
    QPointF snap_cursor_canvas_;
    bool final_snap_cursor_visible_ = false;
    QPointF final_snap_cursor_canvas_;
    QRect last_toolbar_preview_update_rect_;
    bool color_picker_tooltip_visible_ = false;
    bool gradient_tool_dragging_ = false;
    QPointF gradient_tool_start_local_;
    QPointF color_picker_tooltip_pos_;
    QColor color_picker_tooltip_color_;

    struct SnapSettings {
        bool enabled = true;
        bool guides = true;
        bool grid = false;
        bool object_edges = true;
        bool object_centers = true;
        bool canvas_bounds = true;
        bool spacing = true;
    };
    struct SnapFeedback {
        bool x_axis = true;
        double value = 0.0;
        QString label;
    };
    SnapSettings snap_settings_;
    std::vector<SnapFeedback> snap_feedback_;

    DragMode drag_mode_ = DragMode::None;
    bool drag_changed_ = false;
    bool alt_duplicate_pending_ = false;
    bool alt_duplicate_done_ = false;
    bool drag_text_object_scaling_ = false;
    bool marquee_active_ = false;
    QPointF drag_start_view_;
    QPointF drag_current_view_;
    std::vector<std::string> marquee_base_selection_;
    QPointF drag_start_canvas_;
    QPointF drag_rotation_pivot_canvas_;
    double drag_start_rotation_angle_ = 0.0;
    double drag_current_rotation_delta_ = 0.0;
    double drag_start_x_ = 0.0;
    double drag_start_y_ = 0.0;
    float drag_start_w_ = 1.0f;
    float drag_start_h_ = 1.0f;
    float drag_start_origin_x_ = 0.5f;
    float drag_start_origin_y_ = 0.5f;
    QRectF drag_start_selection_bounds_;
    struct GradientDragState {
        bool active = false;
        bool radial = false;
        QRectF local_rect;
        QPointF center;
        QPointF start;
        QPointF end;
        QPointF radius;
        QPointF focal;
        float center_x = 0.5f;
        float center_y = 0.5f;
        float focal_x = 0.5f;
        float focal_y = 0.5f;
        float scale = 1.0f;
        float angle = 0.0f;
    };
    GradientDragState gradient_drag_;
    bool gradient_editor_active_ = false;
    struct CornerRadiusDragState {
        bool active = false;
        QRectF local_rect;
        float top_left = 0.0f;
        float top_right = 0.0f;
        float bottom_right = 0.0f;
        float bottom_left = 0.0f;
        bool locked = true;
    };
    CornerRadiusDragState corner_radius_drag_;
    struct LayerDragState {
        std::string id;
        std::string resize_root_id;
        double x = 0.0;
        double y = 0.0;
        float w = 1.0f;
        float h = 1.0f;
        double scale_x = 1.0;
        double scale_y = 1.0;
        double rotation = 0.0;
        float stroke_width = 0.0f;
        float corner_radius_tl = 0.0f;
        float corner_radius_tr = 0.0f;
        float corner_radius_br = 0.0f;
        float corner_radius_bl = 0.0f;
    };
    std::vector<LayerDragState> drag_layer_states_;
    std::vector<LayerDragState> drag_child_layer_states_;
};
