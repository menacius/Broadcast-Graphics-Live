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
class QPainter;
class QScrollBar;
class QTimer;

class PropertiesPanel : public QScrollArea {
    Q_OBJECT

public:
    explicit PropertiesPanel(QWidget *parent = nullptr);

    void set_layer(std::shared_ptr<Layer> layer, double playhead);
    void update_playhead(double playhead);
    void set_title(std::shared_ptr<Title> t);
    void set_active_text_edit_layer(const std::string &layer_id);

public slots:
    void open_foreground_color_selector();
    void open_background_color_selector();
    void swap_foreground_background_colors();
    void remember_next_color_popup_position(const QPoint &global_pos);
    bool apply_external_picked_color(const QColor &color, bool commit);

signals:
    void property_changed(bool push_undo_snapshot = true);
    void live_visual_changed();
    void runtime_visual_changed();
    void text_char_format_changed(const std::string &layer_id, const RichTextCharFormat &format, uint32_t mask);
    void gradient_editor_active_changed(bool active);
    void gradient_model_refresh_requested();
    void color_picker_tool_requested();
    void recent_colors_changed();
    void color_library_add_requested(const QColor &color);

private:
    void build_text_section(QWidget *w, QFormLayout *fl);
    void build_rect_section(QWidget *w, QFormLayout *fl);
    void build_transform_section(QWidget *w, QFormLayout *fl);

    void load_values();
    void update_ticker_runtime_button();

    std::shared_ptr<Layer> layer_;
    std::shared_ptr<Title> title_;
    bool pending_color_popup_position_valid_ = false;
    QPoint pending_color_popup_position_;
    double playhead_ = 0.0;
    bool loading_values_ = false;
    QTimer *ticker_status_timer_ = nullptr;
    bool numeric_label_dragging_ = false;
    std::string active_text_edit_layer_id_;
    std::string external_gradient_layer_id_;
    int external_gradient_stop_index_ = -1;
    bool external_gradient_stroke_ = false;

    QGroupBox       *text_box_     = nullptr;
    QGroupBox       *type_options_box_ = nullptr;
    QGroupBox       *paragraph_box_ = nullptr;
    QGroupBox       *dynamic_text_box_ = nullptr;
    QGroupBox       *live_edit_box_ = nullptr;
    QGroupBox       *bullets_box_ = nullptr;
    QGroupBox       *rect_box_     = nullptr;
    QWidget         *image_box_    = nullptr;
    QWidget         *image_box_size_box_ = nullptr;

    /* Text controls */
    QTextEdit       *txt_content_  = nullptr;
    QComboBox       *cmb_font_     = nullptr;
    QComboBox       *cmb_font_style_ = nullptr;
    QSpinBox        *spn_size_     = nullptr;
    QToolButton     *chk_bold_     = nullptr;
    QToolButton     *chk_italic_   = nullptr;
    QToolButton     *chk_font_kerning_ = nullptr;
    QComboBox       *cmb_kerning_mode_ = nullptr;
    QDoubleSpinBox  *spn_kerning_value_ = nullptr;
    QDoubleSpinBox  *spn_text_leading_ = nullptr;
    QDoubleSpinBox  *spn_char_tracking_ = nullptr;
    QDoubleSpinBox  *spn_char_scale_x_ = nullptr;
    QDoubleSpinBox  *spn_char_scale_y_ = nullptr;
    QDoubleSpinBox  *spn_baseline_shift_ = nullptr;
    QComboBox       *cmb_text_style_ = nullptr;
    QWidget         *row_text_color_ = nullptr;
    QToolButton     *btn_all_caps_ = nullptr;
    QToolButton     *btn_small_caps_ = nullptr;
    QToolButton     *btn_superscript_ = nullptr;
    QToolButton     *btn_subscript_ = nullptr;
    QToolButton     *btn_underline_ = nullptr;
    QToolButton     *btn_strikethrough_ = nullptr;
    QToolButton     *btn_ligatures_ = nullptr;
    QToolButton     *btn_stylistic_alternates_ = nullptr;
    QToolButton     *btn_fractions_ = nullptr;
    QToolButton     *btn_opentype_features_ = nullptr;
    QComboBox       *cmb_text_overflow_ = nullptr;
    QDoubleSpinBox  *spn_text_fit_min_scale_ = nullptr;
    QWidget         *row_ticker_playback_ = nullptr;
    QToolButton     *btn_ticker_pause_ = nullptr;
    QComboBox       *cmb_ticker_playback_mode_ = nullptr;
    QWidget         *row_ticker_completion_ = nullptr;
    QDoubleSpinBox  *spn_ticker_completion_ = nullptr;
    QPushButton     *btn_kf_ticker_completion_ = nullptr;
    QComboBox       *cmb_ticker_style_ = nullptr;
    QDoubleSpinBox  *spn_ticker_speed_ = nullptr;
    QDoubleSpinBox  *spn_ticker_line_hold_ = nullptr;
    QComboBox       *cmb_ticker_direction_ = nullptr;
    QLabel          *lbl_text_fit_scale_ = nullptr;
    QCheckBox       *chk_text_box_width_to_text_ = nullptr;
    QCheckBox       *chk_text_box_height_to_text_ = nullptr;
    QDoubleSpinBox  *spn_max_text_box_width_ = nullptr;
    QDoubleSpinBox  *spn_max_text_box_height_ = nullptr;
    QCheckBox       *chk_expose_text_ = nullptr;
    QCheckBox       *chk_exposed_hide_if_empty_ = nullptr;
    QCheckBox       *chk_exposed_single_value_ = nullptr;
    QCheckBox       *chk_ignore_persistence_ = nullptr;
    QButtonGroup    *grp_text_align_ = nullptr;
    QButtonGroup    *grp_text_valign_ = nullptr;
    QDoubleSpinBox  *spn_paragraph_indent_left_ = nullptr;
    QDoubleSpinBox  *spn_paragraph_indent_right_ = nullptr;
    QDoubleSpinBox  *spn_paragraph_indent_first_line_ = nullptr;
    QDoubleSpinBox  *spn_paragraph_space_before_ = nullptr;
    QDoubleSpinBox  *spn_paragraph_space_after_ = nullptr;
    QCheckBox       *chk_paragraph_hyphenate_ = nullptr;
    QPushButton     *btn_text_color_ = nullptr;
    QGroupBox       *auto_style_box_ = nullptr;
    QCheckBox       *chk_auto_style_enabled_ = nullptr;
    QComboBox       *cmb_auto_default_style_ = nullptr;
    QListWidget     *lst_auto_style_rules_ = nullptr;
    QComboBox       *cmb_auto_rule_style_ = nullptr;
    QLineEdit       *edt_auto_rule_name_ = nullptr;
    QCheckBox       *chk_auto_rule_enabled_ = nullptr;
    QComboBox       *cmb_auto_rule_start_condition_ = nullptr;
    QSpinBox        *spn_auto_rule_start_offset_ = nullptr;
    QLineEdit       *edt_auto_rule_start_chars_ = nullptr;
    QComboBox       *cmb_auto_rule_end_condition_ = nullptr;
    QSpinBox        *spn_auto_rule_end_offset_ = nullptr;
    QLineEdit       *edt_auto_rule_end_chars_ = nullptr;
    QComboBox       *cmb_auto_rule_conflict_mode_ = nullptr;
    QComboBox       *cmb_auto_rule_match_mode_ = nullptr;
    QLineEdit       *edt_auto_rule_excludes_ = nullptr;
    QCheckBox       *chk_auto_rule_stop_processing_ = nullptr;
    QCheckBox       *chk_auto_rule_require_stop_match_ = nullptr;
    QCheckBox       *chk_auto_rule_include_start_marker_ = nullptr;
    QCheckBox       *chk_auto_rule_include_end_marker_ = nullptr;
    QSpinBox        *spn_auto_rule_chars_ = nullptr; // legacy hidden fallback
    QPushButton     *btn_auto_rule_add_ = nullptr;
    QPushButton     *btn_auto_rule_update_ = nullptr;
    QPushButton     *btn_auto_rule_delete_ = nullptr;
    QPushButton     *btn_auto_rule_up_ = nullptr;
    QPushButton     *btn_auto_rule_down_ = nullptr;

    /* Text/shape outline controls */
    QGroupBox       *outline_box_ = nullptr;
    QCheckBox       *chk_outline_enabled_ = nullptr;
    QComboBox       *cmb_stroke_fill_type_ = nullptr;
    QDoubleSpinBox  *spn_outline_width_ = nullptr;
    QPushButton     *btn_outline_color_ = nullptr;
    QWidget         *row_outline_color_ = nullptr;
    QDoubleSpinBox  *spn_outline_opacity_ = nullptr;
    QComboBox       *cmb_outline_join_ = nullptr;
    QComboBox       *cmb_outline_position_ = nullptr;
    QCheckBox       *chk_outline_antialias_ = nullptr;
    QComboBox       *cmb_stroke_gradient_type_ = nullptr;
    QPushButton     *btn_stroke_gradient_start_color_ = nullptr;
    QPushButton     *btn_stroke_gradient_end_color_ = nullptr;
    QDoubleSpinBox  *spn_stroke_gradient_start_pos_ = nullptr;
    QDoubleSpinBox  *spn_stroke_gradient_end_pos_ = nullptr;
    QDoubleSpinBox  *spn_stroke_gradient_start_opacity_ = nullptr;
    QDoubleSpinBox  *spn_stroke_gradient_end_opacity_ = nullptr;
    QDoubleSpinBox  *spn_stroke_gradient_opacity_ = nullptr;
    QDoubleSpinBox  *spn_stroke_gradient_angle_ = nullptr;
    QDoubleSpinBox  *spn_stroke_gradient_center_x_ = nullptr;
    QDoubleSpinBox  *spn_stroke_gradient_center_y_ = nullptr;
    QDoubleSpinBox  *spn_stroke_gradient_scale_ = nullptr;
    QDoubleSpinBox  *spn_stroke_gradient_focal_x_ = nullptr;
    QDoubleSpinBox  *spn_stroke_gradient_focal_y_ = nullptr;

    /* Rectangle/Image geometry controls */
    QDoubleSpinBox  *spn_layer_w_   = nullptr;
    QDoubleSpinBox  *spn_layer_h_   = nullptr;
    QDoubleSpinBox  *spn_rect_corner_tl_ = nullptr;
    QDoubleSpinBox  *spn_rect_corner_tr_ = nullptr;
    QDoubleSpinBox  *spn_rect_corner_br_ = nullptr;
    QDoubleSpinBox  *spn_rect_corner_bl_ = nullptr;
    QCheckBox       *chk_corner_lock_ = nullptr;
    QWidget         *row_rect_corners_ = nullptr;
    QDoubleSpinBox  *spn_corner_bevel_roundness_ = nullptr;
    QComboBox       *cmb_shape_type_ = nullptr;
    QButtonGroup    *grp_shape_type_ = nullptr;
    QPushButton     *btn_shape_defaults_ = nullptr;
    QCheckBox       *chk_size_lock_ = nullptr;
    QSpinBox        *spn_shape_points_ = nullptr;
    QSpinBox        *spn_shape_sides_ = nullptr;
    QDoubleSpinBox  *spn_shape_inner_radius_ = nullptr;
    QDoubleSpinBox  *spn_shape_outer_radius_ = nullptr;
    QDoubleSpinBox  *spn_shape_roundness_ = nullptr;
    QPushButton     *btn_fill_color_ = nullptr;
    QWidget         *row_fill_color_ = nullptr;
    QComboBox       *cmb_fill_type_ = nullptr;
    QWidget         *row_fill_type_ = nullptr;
    QGroupBox       *gradient_box_ = nullptr;
    QComboBox       *cmb_gradient_type_ = nullptr;
    QComboBox       *cmb_gradient_spread_ = nullptr;
    QPushButton     *btn_gradient_start_color_ = nullptr;
    QPushButton     *btn_gradient_end_color_ = nullptr;
    QDoubleSpinBox  *spn_gradient_start_pos_ = nullptr;
    QDoubleSpinBox  *spn_gradient_end_pos_ = nullptr;
    QDoubleSpinBox  *spn_gradient_start_opacity_ = nullptr;
    QDoubleSpinBox  *spn_gradient_end_opacity_ = nullptr;
    QDoubleSpinBox  *spn_gradient_opacity_ = nullptr;
    QDoubleSpinBox  *spn_gradient_angle_ = nullptr;
    QDoubleSpinBox  *spn_gradient_center_x_ = nullptr;
    QDoubleSpinBox  *spn_gradient_center_y_ = nullptr;
    QDoubleSpinBox  *spn_gradient_scale_ = nullptr;
    QDoubleSpinBox  *spn_gradient_focal_x_ = nullptr;
    QDoubleSpinBox  *spn_gradient_focal_y_ = nullptr;

    /* Text/image background controls */
    QGroupBox       *background_gradient_box_ = nullptr;
    QComboBox       *cmb_background_gradient_type_ = nullptr;
    QPushButton     *btn_background_gradient_start_color_ = nullptr;
    QPushButton     *btn_background_gradient_end_color_ = nullptr;
    QDoubleSpinBox  *spn_background_gradient_start_pos_ = nullptr;
    QDoubleSpinBox  *spn_background_gradient_end_pos_ = nullptr;
    QDoubleSpinBox  *spn_background_gradient_start_opacity_ = nullptr;
    QDoubleSpinBox  *spn_background_gradient_end_opacity_ = nullptr;
    QDoubleSpinBox  *spn_background_gradient_opacity_ = nullptr;
    QDoubleSpinBox  *spn_background_gradient_angle_ = nullptr;
    QDoubleSpinBox  *spn_background_gradient_center_x_ = nullptr;
    QDoubleSpinBox  *spn_background_gradient_center_y_ = nullptr;
    QDoubleSpinBox  *spn_background_gradient_scale_ = nullptr;
    QDoubleSpinBox  *spn_background_gradient_focal_x_ = nullptr;
    QDoubleSpinBox  *spn_background_gradient_focal_y_ = nullptr;

    /* Image controls */
    QLabel          *lbl_image_preview_ = nullptr;
    QLineEdit       *edit_image_path_ = nullptr;
    QPushButton     *btn_pick_image_ = nullptr;
    QComboBox       *cmb_image_scale_filter_ = nullptr;
    QComboBox       *cmb_image_box_mode_ = nullptr;
    QCheckBox       *chk_image_crop_when_outside_box_ = nullptr;
    QPushButton     *btn_image_anchor_grid_ = nullptr;
    QDoubleSpinBox  *spn_image_box_w_ = nullptr;
    QDoubleSpinBox  *spn_image_box_h_ = nullptr;
    QCheckBox       *chk_image_box_size_lock_ = nullptr;
    QPushButton     *btn_kf_image_box_size_ = nullptr;

    /* Transform controls (static) */
    QDoubleSpinBox  *spn_px_       = nullptr;
    QDoubleSpinBox  *spn_py_       = nullptr;
    QDoubleSpinBox  *spn_scale_x_  = nullptr;
    QDoubleSpinBox  *spn_scale_y_  = nullptr;
    QDoubleSpinBox  *spn_transform_size_w_ = nullptr;
    QDoubleSpinBox  *spn_transform_size_h_ = nullptr;
    QDoubleSpinBox  *spn_rot_      = nullptr;
    QDoubleSpinBox  *spn_opacity_  = nullptr;
    QCheckBox       *chk_scene_mask_ = nullptr;
    QDoubleSpinBox  *spn_origin_x_ = nullptr;
    QDoubleSpinBox  *spn_origin_y_ = nullptr;
    QCheckBox       *chk_scale_lock_ = nullptr;
    QCheckBox       *chk_transform_size_lock_ = nullptr;
    QCheckBox       *chk_shape_scale_stroke_ = nullptr;
    QCheckBox       *chk_shape_scale_corners_ = nullptr;
    QComboBox       *cmb_anchor_ = nullptr;
    QWidget         *transform_box_ = nullptr;
    QWidget         *transform_scale_label_ = nullptr;
    QWidget         *transform_scale_field_x_ = nullptr;
    QWidget         *transform_scale_field_y_ = nullptr;
    QWidget         *transform_size_label_ = nullptr;
    QWidget         *transform_size_field_w_ = nullptr;
    QWidget         *transform_size_field_h_ = nullptr;
    QWidget         *row_shape_scale_options_ = nullptr;
    QPushButton     *btn_kf_transform_size_ = nullptr;
    QWidget         *shape_size_label_ = nullptr;
    QWidget         *shape_size_field_w_ = nullptr;
    QWidget         *shape_size_field_h_ = nullptr;
    QWidget         *appearance_box_ = nullptr;
    QPushButton     *btn_appearance_fill_color_ = nullptr;
    QPushButton     *btn_appearance_stroke_color_ = nullptr;
    QLabel          *btn_appearance_stroke_label_ = nullptr;
    QDoubleSpinBox  *spn_appearance_stroke_width_ = nullptr;
    QDoubleSpinBox  *spn_appearance_opacity_ = nullptr;
    QPushButton     *btn_kf_appearance_fill_ = nullptr;
    QPushButton     *btn_kf_appearance_stroke_ = nullptr;
    QPushButton     *btn_kf_appearance_opacity_ = nullptr;
    QPushButton     *btn_anchor_grid_ = nullptr;
    QPushButton     *btn_transform_defaults_ = nullptr;
    QGroupBox       *shadow_box_ = nullptr;
    QCheckBox       *chk_shadow_enabled_ = nullptr;
    QComboBox       *cmb_shadow_preset_ = nullptr;
    QComboBox       *cmb_shadow_blur_type_ = nullptr;
    QPushButton     *btn_shadow_color_ = nullptr;
    QDoubleSpinBox  *spn_shadow_opacity_ = nullptr;
    QDoubleSpinBox  *spn_shadow_distance_ = nullptr;
    QDoubleSpinBox  *spn_shadow_angle_ = nullptr;
    QDoubleSpinBox  *spn_shadow_blur_ = nullptr;
    QDoubleSpinBox  *spn_shadow_spread_ = nullptr;
    QCheckBox       *chk_long_shadow_enabled_ = nullptr;
    QPushButton     *btn_long_shadow_color_ = nullptr;
    QDoubleSpinBox  *spn_long_shadow_opacity_ = nullptr;
    QDoubleSpinBox  *spn_long_shadow_length_ = nullptr;
    QDoubleSpinBox  *spn_long_shadow_angle_ = nullptr;
    QDoubleSpinBox  *spn_long_shadow_falloff_ = nullptr;
    QComboBox       *cmb_long_shadow_blur_type_ = nullptr;
    QDoubleSpinBox  *spn_long_shadow_blur_ = nullptr;
    QPushButton     *btn_kf_shadow_enabled_ = nullptr;
    QPushButton     *btn_kf_shadow_color_ = nullptr;
    QPushButton     *btn_kf_shadow_opacity_ = nullptr;
    QPushButton     *btn_kf_shadow_distance_ = nullptr;
    QPushButton     *btn_kf_shadow_angle_ = nullptr;
    QPushButton     *btn_kf_shadow_blur_ = nullptr;
    QPushButton     *btn_kf_shadow_spread_ = nullptr;
    QPushButton     *btn_kf_pos_x_ = nullptr;
    QPushButton     *btn_kf_pos_y_ = nullptr;
    QPushButton     *btn_kf_scale_x_ = nullptr;
    QPushButton     *btn_kf_scale_y_ = nullptr;
    QPushButton     *btn_kf_rotation_ = nullptr;
    QPushButton     *btn_kf_opacity_ = nullptr;
    QPushButton     *btn_kf_origin_x_ = nullptr;
    QPushButton     *btn_kf_origin_y_ = nullptr;
    QPushButton     *btn_kf_paragraph_indent_left_ = nullptr;
    QPushButton     *btn_kf_paragraph_indent_right_ = nullptr;
    QPushButton     *btn_kf_paragraph_indent_first_line_ = nullptr;
    QPushButton     *btn_kf_font_size_ = nullptr;
    QPushButton     *btn_kf_char_scale_x_ = nullptr;
    QPushButton     *btn_kf_char_scale_y_ = nullptr;
    QPushButton     *btn_kf_char_tracking_ = nullptr;
    QPushButton     *btn_kf_baseline_shift_ = nullptr;
    QPushButton     *btn_kf_paragraph_space_before_ = nullptr;
    QPushButton     *btn_kf_paragraph_space_after_ = nullptr;
    QPushButton     *btn_kf_width_ = nullptr;
    QPushButton     *btn_kf_text_color_ = nullptr;
    QPushButton     *btn_kf_fill_color_ = nullptr;
};
