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
/* ══════════════════════════════════════════════════════════════════
 *  PropertiesPanel  – right-side inspector
 * ══════════════════════════════════════════════════════════════════ */

class EffectsPanel : public QWidget {
    Q_OBJECT

public:
    explicit EffectsPanel(QWidget *parent = nullptr);
    void set_layer(std::shared_ptr<Layer> layer, double playhead);

signals:
    void property_changed(bool push_undo_snapshot = true);

private:
    void rebuild_stack();
    void load_settings();
    void build_settings();
    LayerEffect *selected_effect();
    const LayerEffect *selected_effect() const;
    void sync_legacy_enabled_flags();
    void emit_effect_changed();

    std::shared_ptr<Layer> layer_;
    double playhead_ = 0.0;
    bool loading_values_ = false;
    bool numeric_label_dragging_ = false;
    int selected_index_ = -1;

    QListWidget *effect_list_ = nullptr;
    QWidget *settings_container_ = nullptr;
    QVBoxLayout *settings_layout_ = nullptr;
    QToolButton *btn_remove_ = nullptr;
    QToolButton *btn_duplicate_ = nullptr;
    QToolButton *btn_move_up_ = nullptr;
    QToolButton *btn_move_down_ = nullptr;
    QToolButton *btn_respect_masks_ = nullptr;
};

