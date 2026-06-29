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
#include <QJsonArray>
#include <QPointer>
#include <QVariant>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <set>

class QEvent;
class QMouseEvent;
class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QWheelEvent;
class QKeyEvent;
class QContextMenuEvent;
class QResizeEvent;
class QPaintEvent;
class QPainter;
class QScrollBar;
class BglCollapsiblePanel;
/* ══════════════════════════════════════════════════════════════════
 *  PropertiesPanel  – right-side inspector
 * ══════════════════════════════════════════════════════════════════ */

class EffectsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EffectsPanel(QWidget *parent = nullptr);
    ~EffectsPanel() override;
    void set_layer(std::shared_ptr<Layer> layer, double playhead);
    void update_playhead(double playhead);
    void begin_shutdown();
    bool add_effect_from_preset_file(const QString &file_path);
    QJsonArray extension_canvas_handles() const;
    void set_extension_canvas_handle_position(const QString &path, const QPointF &normalized_position, bool final_change);

signals:
    void property_changed(bool push_undo_snapshot = true);
    void extension_canvas_handles_changed(const QJsonArray &handles);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuild_stack();
    void load_settings();
    void build_settings();
    void build_effect_settings_panel(int effect_index);
    LayerEffect *selected_effect();
    const LayerEffect *selected_effect() const;
    int effect_index_for_object(const QObject *object) const;
    void set_active_effect_index(int effect_index);
    void apply_effect_panel_order();
    void duplicate_effect(int effect_index);
    void delete_effect(int effect_index);
    void move_effect(int effect_index, int delta);
    void sync_legacy_enabled_flags();
    void emit_effect_changed();
    bool settings_editor_has_focus() const;
    void update_bound_controls();
    void publish_canvas_handles(bool force = false);
    double current_local_time() const;

    struct NumericBinding {
        QPointer<QDoubleSpinBox> spin;
        std::function<double(const LayerEffect &, double)> value;
        int effect_index = -1;
    };
    struct ColorBinding {
        QPointer<QPushButton> button;
        std::function<uint32_t(const LayerEffect &, double)> value;
        int effect_index = -1;
    };
    struct BoolBinding {
        QPointer<QCheckBox> checkbox;
        std::function<bool(const LayerEffect &, double)> value;
        int effect_index = -1;
    };
    struct ComboBinding {
        QPointer<QComboBox> combo;
        std::function<QVariant(const LayerEffect &, double)> value;
        int effect_index = -1;
    };
    struct KeyframeBinding {
        QPointer<QPushButton> button;
        std::function<bool(const LayerEffect &, double)> has_keyframe;
        std::function<bool(const LayerEffect &)> has_keyframes;
        std::function<void(LayerEffect &)> clear_keyframes;
        int effect_index = -1;
    };

    std::shared_ptr<Layer> layer_;
    double playhead_ = 0.0;
    bool loading_values_ = false;
    bool numeric_label_dragging_ = false;
    bool shutting_down_ = false;
    int selected_index_ = -1;
    int building_effect_index_ = -1;
    bool applying_panel_order_ = false;
    bool panel_rebuild_pending_ = false;
    QJsonArray last_published_canvas_handles_;
    std::vector<NumericBinding> numeric_bindings_;
    std::vector<ColorBinding> color_bindings_;
    std::vector<BoolBinding> bool_bindings_;
    std::vector<ComboBinding> combo_bindings_;
    std::vector<KeyframeBinding> keyframe_bindings_;

    QWidget *settings_container_ = nullptr;
    QVBoxLayout *settings_layout_ = nullptr;
    QToolButton *btn_respect_masks_ = nullptr;
    std::vector<QPointer<BglCollapsiblePanel>> effect_panels_;
};
