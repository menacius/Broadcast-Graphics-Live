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
 *  TitlePropertiesPanel – global title inspector
 * ══════════════════════════════════════════════════════════════════ */
class TitlePropertiesPanel : public QGroupBox {
    Q_OBJECT

public:
    explicit TitlePropertiesPanel(QWidget *parent = nullptr);
    void set_title(std::shared_ptr<Title> t);

signals:
    void title_changed(bool push_undo_snapshot = true);

protected:
    bool event(QEvent *event) override;

private:
    void apply_theme_style();
    void load_values();

    std::shared_ptr<Title> title_;
    bool loading_values_ = false;
    bool numeric_label_dragging_ = false;
    bool applying_theme_style_ = false;
    QButtonGroup   *grp_playback_mode_ = nullptr;
    QWidget        *loop_area_row_ = nullptr;
    QDoubleSpinBox *spn_pause_frame_ = nullptr;
    QDoubleSpinBox *spn_duration_ = nullptr;
    QDoubleSpinBox *spn_loop_start_ = nullptr;
    QDoubleSpinBox *spn_loop_end_ = nullptr;
};


