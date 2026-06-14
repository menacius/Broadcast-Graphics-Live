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
 *  ToolsSidebar – Photoshop-style icon-only tool palette
 * ══════════════════════════════════════════════════════════════════ */
class ToolsSidebar : public QWidget {
    Q_OBJECT

public:
    explicit ToolsSidebar(QWidget *parent = nullptr);
    void set_selected_shape(ShapeType shape_type);
    ShapeType selected_shape() const { return selected_shape_; }
    void set_selected_text_layer_type(LayerType type);
    LayerType selected_text_layer_type() const { return selected_text_layer_type_; }

signals:
    void selection_tool_requested();
    void shape_tool_requested(ShapeType shape_type);
    void text_tool_requested(LayerType type);
    void color_picker_tool_requested();
    void gradient_tool_requested();

private:
    void rebuild_shape_menu();
    void rebuild_text_menu();
    QToolButton *selection_button_ = nullptr;
    QToolButton *shape_button_ = nullptr;
    QToolButton *text_button_ = nullptr;
    QToolButton *color_picker_button_ = nullptr;
    QToolButton *gradient_button_ = nullptr;
    QActionGroup *tool_group_ = nullptr;
    QMenu *shape_menu_ = nullptr;
    QMenu *text_menu_ = nullptr;
    ShapeType selected_shape_ = ShapeType::Rectangle;
    LayerType selected_text_layer_type_ = LayerType::Text;
};


