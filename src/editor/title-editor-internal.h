/* Internal implementation support shared by extracted editor widgets. */
#pragma once

#include "title-editor.h"
#include "tools-sidebar.h"
#include "canvas-preview.h"
#include "layer-stack-widget.h"
#include "timeline-widget.h"
#include "title-properties-panel.h"
#include "effects-panel.h"
#include "properties-panel.h"

#include "title-editor.h"
#include "title-data.h"
#include "title-source.h"
#include "title-assets.h"
#include "timecode-spinbox.h"
#include "title-localization.h"
#include "plugin-main.h"
#include "title-preferences.h"
#include "cache-manager.h"
#include "prerender-dock.h"

#include <obs-module.h>

#include <QApplication>

#include <QBuffer>
#include <QIODevice>
#include <QPainter>
#include <QBrush>
#include <QPainterPath>
#include <QPolygonF>
#include <QLineF>
#include <QCursor>
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QSvgRenderer>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QToolBar>
#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QIcon>
#include <QPixmap>
#include <QStringList>
#include <QVariant>
#include <QLocale>
#include <QStyle>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QColorDialog>
#include <QFileDialog>
#include <QInputDialog>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QScrollArea>
#include <QScrollBar>
#include <QListWidget>
#include <QStackedWidget>
#include <QSizePolicy>
#include <QFrame>
#include <QSignalBlocker>
#include <QKeyEvent>
#include <QEvent>
#include <QKeySequence>
#include <QAbstractSpinBox>
#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QTextEdit>
#include <QTextDocument>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextLayout>
#include <QTextOption>
#include <QAbstractTextDocumentLayout>
#include <QDateTime>
#include <QTransform>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QConicalGradient>
#include <QToolButton>
#include <QMenu>
#include <QMenuBar>
#include <QTabWidget>
#include <QTabBar>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QMainWindow>
#include <QScreen>
#include <QSettings>
#include <QWindow>
#include <QContextMenuEvent>
#include <cmath>
#include <algorithm>
#include <vector>
#include <initializer_list>
#include <set>
#include <map>
#include <limits>
#include <tuple>
#include <functional>


constexpr const char *kEditorLayoutSettingsGroup = "TitleEditorLayout";
constexpr const char *kEditorGeometryKey = "geometry";
constexpr const char *kEditorWindowStateKey = "windowState";
constexpr const char *kEditorPanelsLockedKey = "panelsLocked";
constexpr const char *kEditorCanvasTransparencyKey = "canvasTransparency";
constexpr const char *kEditorSafeGuidesVisibleKey = "safeGuidesVisible";
constexpr const char *kGraphicPropertiesDockObjectName = "OBSGraphicsStudioProGraphicPropertiesDock";
constexpr const char *kLayerPropertiesDockObjectName = "OBSGraphicsStudioProLayerPropertiesDock";
constexpr const char *kEffectsDockObjectName = "OBSGraphicsStudioProEffectsDock";
constexpr const char *kStylesDockObjectName = "OBSGraphicsStudioProStylesDock";
constexpr const char *kColorSwatchesDockObjectName = "OBSGraphicsStudioProColorSwatchesDock";
constexpr const char *kTimelineDockObjectName = "OBSGraphicsStudioProTimelineDock";
constexpr const char *kPrerenderDockObjectName = "OBSGraphicsStudioProPrerenderDock";

static QPoint clamp_popup_position_to_screen(const QPoint &desired, const QSize &popup_size, QWidget *anchor)
{
    QScreen *screen = QApplication::screenAt(desired);
    if (!screen && anchor && anchor->window() && anchor->window()->windowHandle())
        screen = anchor->window()->windowHandle()->screen();
    if (!screen)
        screen = QApplication::primaryScreen();
    if (!screen)
        return desired;

    const QRect available = screen->availableGeometry().adjusted(6, 6, -6, -6);
    QPoint pos = desired;
    pos.setX(std::clamp(pos.x(), available.left(), std::max(available.left(), available.right() - popup_size.width() + 1)));
    pos.setY(std::clamp(pos.y(), available.top(), std::max(available.top(), available.bottom() - popup_size.height() + 1)));
    return pos;
}

class NumericDragLabel : public QLabel {
public:
    NumericDragLabel(const QString &text, QWidget *field, QWidget *parent = nullptr,
                     std::function<void()> drag_started = {},
                     std::function<void()> drag_finished = {})
        : QLabel(text, parent), spin_box_(find_spin_box(field)),
          drag_started_(std::move(drag_started)), drag_finished_(std::move(drag_finished))
    {
        if (!spin_box_) return;
        setToolTip(obsgs_tr("OBSTitles.DragNumericLabelTooltip"));
        setCursor(Qt::SizeHorCursor);
    }

    ~NumericDragLabel() override
    {
        if (dragging_)
            QApplication::restoreOverrideCursor();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !can_drag()) {
            QLabel::mousePressEvent(event);
            return;
        }

        dragging_ = true;
        drag_start_x_ = event->globalPosition().x();
        drag_start_value_ = spin_value();
        grabMouse(Qt::SizeHorCursor);
        QApplication::setOverrideCursor(Qt::SizeHorCursor);
        if (drag_started_)
            drag_started_();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!dragging_) {
            QLabel::mouseMoveEvent(event);
            return;
        }
        if (!can_drag()) {
            finish_drag();
            event->accept();
            return;
        }

        const double delta = event->globalPosition().x() - drag_start_x_;
        set_spin_value(drag_start_value_ + delta * spin_step());
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!dragging_ || event->button() != Qt::LeftButton) {
            QLabel::mouseReleaseEvent(event);
            return;
        }

        finish_drag();
        event->accept();
    }

    void leaveEvent(QEvent *event) override
    {
        if (!dragging_)
            QLabel::leaveEvent(event);
    }

private:
    static QAbstractSpinBox *find_spin_box(QWidget *field)
    {
        if (!field) return nullptr;
        if (auto *spin = qobject_cast<QAbstractSpinBox *>(field)) return spin;
        return field->findChild<QAbstractSpinBox *>();
    }

    bool can_drag() const
    {
        return spin_box_ && spin_box_->isEnabled() && spin_box_->isVisible() && isEnabled();
    }

    double spin_value() const
    {
        if (auto *spin = qobject_cast<QDoubleSpinBox *>(spin_box_)) return spin->value();
        if (auto *spin = qobject_cast<QSpinBox *>(spin_box_)) return spin->value();
        return 0.0;
    }

    double spin_step() const
    {
        if (auto *spin = qobject_cast<QDoubleSpinBox *>(spin_box_)) return spin->singleStep();
        if (auto *spin = qobject_cast<QSpinBox *>(spin_box_)) return spin->singleStep();
        return 1.0;
    }

    void set_spin_value(double value)
    {
        if (auto *spin = qobject_cast<QDoubleSpinBox *>(spin_box_)) {
            spin->setValue(std::clamp(value, spin->minimum(), spin->maximum()));
        } else if (auto *spin = qobject_cast<QSpinBox *>(spin_box_)) {
            spin->setValue(std::clamp((int)std::round(value), spin->minimum(), spin->maximum()));
        }
    }

    void finish_drag()
    {
        dragging_ = false;
        releaseMouse();
        QApplication::restoreOverrideCursor();
        if (drag_finished_)
            drag_finished_();
    }

    QAbstractSpinBox *spin_box_ = nullptr;
    std::function<void()> drag_started_;
    std::function<void()> drag_finished_;
    bool dragging_ = false;
    double drag_start_x_ = 0.0;
    double drag_start_value_ = 0.0;
};

class StrokeOptionsLabel : public QLabel {
public:
    StrokeOptionsLabel(const QString &text, QDoubleSpinBox *spin, QWidget *parent = nullptr,
                       std::function<void()> clicked = {},
                       std::function<void()> drag_started = {},
                       std::function<void()> drag_finished = {})
        : QLabel(text, parent), spin_(spin), clicked_(std::move(clicked)),
          drag_started_(std::move(drag_started)), drag_finished_(std::move(drag_finished))
    {
        setCursor(Qt::PointingHandCursor);
        setToolTip(obsgs_tr("OBSTitles.DragNumericLabelTooltip"));
    }

    ~StrokeOptionsLabel() override
    {
        if (dragging_)
            QApplication::restoreOverrideCursor();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !spin_ || !spin_->isEnabled()) {
            QLabel::mousePressEvent(event);
            return;
        }
        dragging_ = true;
        moved_ = false;
        drag_start_x_ = event->globalPosition().x();
        drag_start_value_ = spin_->value();
        grabMouse(Qt::SizeHorCursor);
        QApplication::setOverrideCursor(Qt::SizeHorCursor);
        if (drag_started_)
            drag_started_();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!dragging_ || !spin_) {
            QLabel::mouseMoveEvent(event);
            return;
        }
        const double delta = event->globalPosition().x() - drag_start_x_;
        if (std::abs(delta) >= 2.0)
            moved_ = true;
        spin_->setValue(std::clamp(drag_start_value_ + delta * spin_->singleStep(),
                                   spin_->minimum(), spin_->maximum()));
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!dragging_ || event->button() != Qt::LeftButton) {
            QLabel::mouseReleaseEvent(event);
            return;
        }
        dragging_ = false;
        releaseMouse();
        QApplication::restoreOverrideCursor();
        if (drag_finished_)
            drag_finished_();
        if (!moved_ && clicked_)
            clicked_();
        event->accept();
    }

private:
    QDoubleSpinBox *spin_ = nullptr;
    std::function<void()> clicked_;
    std::function<void()> drag_started_;
    std::function<void()> drag_finished_;
    bool dragging_ = false;
    bool moved_ = false;
    double drag_start_x_ = 0.0;
    double drag_start_value_ = 0.0;
};

class TransformLockCheckBox : public QCheckBox {
public:
    explicit TransformLockCheckBox(QWidget *parent = nullptr) : QCheckBox(parent)
    {
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF r = rect().adjusted(1, 1, -1, -1);
        const QColor bg = underMouse() ? QColor(48, 48, 48) : QColor(36, 36, 36);
        const QColor border = isChecked() ? QColor(111, 143, 196) : QColor(58, 58, 58);
        const QColor fg = isChecked() ? QColor(215, 226, 248) : QColor(170, 170, 170);
        p.setPen(QPen(border, 1));
        p.setBrush(bg);
        p.drawRoundedRect(r, 2, 2);

        p.setPen(QPen(fg, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        if (isChecked())
            p.drawArc(QRectF(width() / 2.0 - 5.0, height() / 2.0 - 8.0, 10.0, 12.0), 0, 180 * 16);
        else
            p.drawArc(QRectF(width() / 2.0 - 1.0, height() / 2.0 - 8.0, 10.0, 12.0), 30 * 16, 150 * 16);

        const QRectF body(width() / 2.0 - 5.0, height() / 2.0 - 1.0, 10.0, 8.0);
        p.setBrush(fg);
        p.drawRoundedRect(body, 1.5, 1.5);
        p.setPen(bg);
        p.drawPoint(QPointF(width() / 2.0, height() / 2.0 + 3.0));
    }
};

class AnchorGridButton : public QPushButton {
public:
    explicit AnchorGridButton(QWidget *parent = nullptr) : QPushButton(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setProperty("active_index", 4);
    }

    std::function<void(int)> anchor_selected;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QRectF outer = rect().adjusted(1, 1, -1, -1);
        p.setPen(QPen(underMouse() ? QColor(74, 74, 74) : QColor(55, 55, 55), 1));
        p.setBrush(underMouse() ? QColor(48, 48, 48) : QColor(36, 36, 36));
        p.drawRoundedRect(outer, 2, 2);

        const int active = property("active_index").toInt();
        constexpr int cell = 7;
        constexpr int gap = 3;
        const int grid = cell * 3 + gap * 2;
        const int origin_x = (width() - grid) / 2;
        const int origin_y = (height() - grid) / 2;
        for (int i = 0; i < 9; ++i) {
            QRectF cell_rect(origin_x + (i % 3) * (cell + gap),
                             origin_y + (i / 3) * (cell + gap),
                             cell, cell);
            p.setPen(QPen(i == active ? QColor(111, 143, 196) : QColor(160, 160, 160), 1.2));
            p.setBrush(i == active ? QColor(75, 110, 168) : Qt::NoBrush);
            p.drawRect(cell_rect);
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QPushButton::mouseReleaseEvent(event);
            return;
        }

        constexpr int cell = 7;
        constexpr int gap = 3;
        const int grid = cell * 3 + gap * 2;
        const int origin_x = (width() - grid) / 2;
        const int origin_y = (height() - grid) / 2;
        const QPoint pos = event->pos();
        for (int i = 0; i < 9; ++i) {
            QRect cell_rect(origin_x + (i % 3) * (cell + gap),
                            origin_y + (i / 3) * (cell + gap),
                            cell + gap, cell + gap);
            if (cell_rect.contains(pos)) {
                setProperty("active_index", i);
                update();
                if (anchor_selected)
                    anchor_selected(i);
                break;
            }
        }
        event->accept();
    }
};

class HsvColorPicker : public QWidget {
public:
    explicit HsvColorPicker(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedSize(250, 250);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setMouseTracking(true);
    }

    void set_color(const QColor &color)
    {
        current_ = color.isValid() ? color : QColor(Qt::white);
        const int h = current_.hsvHue();
        hue_ = h < 0 ? 0.0 : (double)h;
        saturation_ = std::clamp((double)current_.hsvSaturationF(), 0.0, 1.0);
        value_ = std::clamp((double)current_.valueF(), 0.0, 1.0);
        alpha_ = std::clamp((double)current_.alphaF(), 0.0, 1.0);
        update();
    }

    QColor color() const
    {
        QColor color;
        color.setHsvF(std::clamp(hue_ / 359.0, 0.0, 1.0), saturation_, value_, alpha_);
        return color;
    }

    std::function<void(const QColor &)> color_changed;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const QPointF c = wheel_center();
        const double outer = wheel_outer_radius();
        const double inner = wheel_inner_radius();

        QImage wheel(size(), QImage::Format_ARGB32_Premultiplied);
        wheel.fill(Qt::transparent);
        for (int y = 0; y < wheel.height(); ++y) {
            for (int x = 0; x < wheel.width(); ++x) {
                const double dx = x + 0.5 - c.x();
                const double dy = y + 0.5 - c.y();
                const double r = std::sqrt(dx * dx + dy * dy);
                if (r < inner || r > outer)
                    continue;
                double deg = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
                if (deg < 0.0)
                    deg += 360.0;
                QColor hc;
                hc.setHsvF(deg / 359.0, 1.0, 1.0, 1.0);
                wheel.setPixelColor(x, y, hc);
            }
        }
        p.drawImage(0, 0, wheel);

        p.setPen(QPen(QColor(0, 0, 0, 90), 1));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, outer, outer);
        p.drawEllipse(c, inner, inner);

        const double angle = hue_ * 3.14159265358979323846 / 180.0;
        const QPointF hue_handle(c.x() + std::cos(angle) * ((inner + outer) * 0.5),
                                 c.y() + std::sin(angle) * ((inner + outer) * 0.5));
        p.setPen(QPen(Qt::white, 3));
        p.drawEllipse(hue_handle, 10, 10);
        p.setPen(QPen(QColor(20, 20, 20), 2));
        p.drawEllipse(hue_handle, 11, 11);

        const QRect sv = sv_rect();
        QImage image(sv.size(), QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y) {
            const double v = 1.0 - (double)y / std::max(1, image.height() - 1);
            for (int x = 0; x < image.width(); ++x) {
                const double s = (double)x / std::max(1, image.width() - 1);
                QColor col;
                col.setHsvF(std::clamp(hue_ / 359.0, 0.0, 1.0), s, v, 1.0);
                image.setPixelColor(x, y, col);
            }
        }
        p.drawImage(sv.topLeft(), image);
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.drawRect(sv.adjusted(0, 0, -1, -1));

        const QPoint sv_handle(sv.left() + (int)std::round(saturation_ * (sv.width() - 1)),
                               sv.top() + (int)std::round((1.0 - value_) * (sv.height() - 1)));
        p.setPen(QPen(Qt::white, 2));
        p.drawEllipse(sv_handle, 7, 7);
        p.setPen(QPen(QColor(20, 20, 20), 1));
        p.drawEllipse(sv_handle, 8, 8);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        update_from_pos(event->pos());
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons() & Qt::LeftButton) {
            update_from_pos(event->pos());
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

private:
    QPointF wheel_center() const { return QPointF(width() * 0.5, height() * 0.48); }
    double wheel_outer_radius() const { return std::min(width(), height()) * 0.44; }
    double wheel_inner_radius() const { return std::min(width(), height()) * 0.32; }
    QRect sv_rect() const
    {
        const int side = (int)std::round(wheel_inner_radius() * 1.28);
        const QPointF c = wheel_center();
        return QRect((int)std::round(c.x() - side * 0.5),
                     (int)std::round(c.y() - side * 0.5),
                     side, side);
    }

    void update_from_pos(const QPoint &pos)
    {
        const QRect sv = sv_rect();
        bool changed = false;
        if (sv.contains(pos)) {
            saturation_ = std::clamp((double)(pos.x() - sv.left()) / std::max(1, sv.width() - 1), 0.0, 1.0);
            value_ = std::clamp(1.0 - (double)(pos.y() - sv.top()) / std::max(1, sv.height() - 1), 0.0, 1.0);
            changed = true;
        } else {
            const QPointF c = wheel_center();
            const double dx = pos.x() - c.x();
            const double dy = pos.y() - c.y();
            const double r = std::sqrt(dx * dx + dy * dy);
            if (r >= wheel_inner_radius() && r <= wheel_outer_radius()) {
                double deg = std::atan2(dy, dx) * 180.0 / 3.14159265358979323846;
                if (deg < 0.0)
                    deg += 360.0;
                hue_ = std::clamp(deg, 0.0, 359.0);
                changed = true;
            }
        }

        if (!changed)
            return;
        current_ = color();
        update();
        if (color_changed)
            color_changed(current_);
    }

    QColor current_ = Qt::white;
    double hue_ = 0.0;
    double saturation_ = 0.0;
    double value_ = 1.0;
    double alpha_ = 1.0;
};

static QString gradient_editor_hex(const QColor &color)
{
    return color.alpha() < 255
        ? QStringLiteral("#%1%2%3%4")
              .arg(color.red(), 2, 16, QLatin1Char('0'))
              .arg(color.green(), 2, 16, QLatin1Char('0'))
              .arg(color.blue(), 2, 16, QLatin1Char('0'))
              .arg(color.alpha(), 2, 16, QLatin1Char('0'))
              .toUpper()
        : QStringLiteral("#%1%2%3")
              .arg(color.red(), 2, 16, QLatin1Char('0'))
              .arg(color.green(), 2, 16, QLatin1Char('0'))
              .arg(color.blue(), 2, 16, QLatin1Char('0'))
              .toUpper();
}

static bool gradient_editor_parse_hex(QString text, QColor &color)
{
    text = text.trimmed();
    if (text.startsWith(QStringLiteral("#"))) text.remove(0, 1);
    if (text.size() != 6 && text.size() != 8) return false;
    bool ok = false;
    const uint value = text.toUInt(&ok, 16);
    if (!ok) return false;
    color = text.size() == 6
        ? QColor((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF, 255)
        : QColor((value >> 24) & 0xFF, (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return color.isValid();
}

static QColor gradient_editor_color_from_argb(uint32_t argb)
{
    return QColor((argb >> 16) & 0xFF,
                  (argb >> 8) & 0xFF,
                  argb & 0xFF,
                  (argb >> 24) & 0xFF);
}

static uint32_t gradient_editor_argb_from_color(const QColor &color)
{
    return ((uint32_t)color.alpha() << 24) |
           ((uint32_t)color.red() << 16) |
           ((uint32_t)color.green() << 8) |
           (uint32_t)color.blue();
}

class GradientEditorPreview : public QWidget {
public:
    explicit GradientEditorPreview(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(440, 128);
        setMouseTracking(true);
        setFocusPolicy(Qt::StrongFocus);
    }

    void set_gradient(int type, uint32_t start_color, uint32_t end_color,
                      double start_pos, double end_pos, double start_opacity, double end_opacity,
                      double opacity, double angle, double center_x, double center_y, double scale)
    {
        type_ = std::clamp(type, 0, 4);
        colors_.assign({gradient_editor_color_from_argb(start_color), gradient_editor_color_from_argb(end_color)});
        for (auto &color : colors_) color.setAlpha(255);
        positions_.assign({std::clamp(start_pos, 0.0, 1.0), std::clamp(end_pos, 0.0, 1.0)});
        stop_opacities_.assign({std::clamp(start_opacity, 0.0, 1.0), std::clamp(end_opacity, 0.0, 1.0)});
        if (selected_stop_ >= stop_count()) selected_stop_ = -1;
        opacity_ = std::clamp(opacity, 0.0, 1.0);
        angle_ = angle;
        center_x_ = center_x;
        center_y_ = center_y;
        scale_ = std::clamp(scale, 0.01, 100.0);
        update();
    }

    void set_extra_stops(const std::vector<GradientStop> &stops)
    {
        while (colors_.size() > 2) {
            colors_.pop_back();
            positions_.pop_back();
            stop_opacities_.pop_back();
        }
        for (const auto &stop : stops) {
            QColor c = gradient_editor_color_from_argb(stop.color);
            c.setAlpha(255);
            colors_.push_back(c);
            positions_.push_back(std::clamp((double)stop.position, 0.0, 1.0));
            stop_opacities_.push_back(std::clamp((double)stop.opacity, 0.0, 1.0));
        }
        if (selected_stop_ >= stop_count()) selected_stop_ = -1;
        update();
    }

    std::vector<GradientStop> extra_stops() const
    {
        std::vector<GradientStop> stops;
        for (int i = 2; i < stop_count(); ++i) {
            GradientStop stop;
            stop.color = gradient_editor_argb_from_color(colors_[i]);
            stop.position = (float)std::clamp(positions_[i], 0.0, 1.0);
            stop.opacity = (float)std::clamp(stop_opacities_[i], 0.0, 1.0);
            stops.push_back(stop);
        }
        std::sort(stops.begin(), stops.end(), [](const GradientStop &a, const GradientStop &b) {
            return a.position < b.position;
        });
        return stops;
    }

    int gradient_type() const { return type_; }
    int selected_stop() const { return selected_stop_; }
    int stop_count() const { return (int)std::min({colors_.size(), positions_.size(), stop_opacities_.size()}); }
    uint32_t stop_color_argb(int index) const { return gradient_editor_argb_from_color(colors_[clamp_stop_index(index)]); }
    QColor stop_color(int index) const
    {
        const int i = clamp_stop_index(index);
        QColor color = colors_[i];
        color.setAlphaF(std::clamp((double)color.alphaF() * stop_opacities_[i] * opacity_, 0.0, 1.0));
        return color;
    }
    double stop_position(int index) const { return positions_[clamp_stop_index(index)]; }
    double stop_opacity(int index) const { return stop_opacities_[clamp_stop_index(index)]; }
    double gradient_opacity() const { return opacity_; }
    double angle() const { return angle_; }
    double center_x() const { return center_x_; }
    double center_y() const { return center_y_; }
    double scale() const { return scale_; }

    void set_gradient_type(int type) { type_ = std::clamp(type, 0, 4); changed(); }
    void set_angle(double value) { angle_ = value; changed(); }
    void set_center_x(double value) { center_x_ = value; changed(); }
    void set_center_y(double value) { center_y_ = value; changed(); }
    void set_scale(double value) { scale_ = std::clamp(value, 0.01, 100.0); changed(); }
    void set_stop_position(int index, double value) { positions_[clamp_stop_index(index)] = std::clamp(value, 0.0, 1.0); changed(); }
    void set_stop_opacity(int index, double value) { stop_opacities_[clamp_stop_index(index)] = std::clamp(value, 0.0, 1.0); changed(); }
    void set_stop_color(int index, const QColor &color)
    {
        if (!color.isValid()) return;
        const int i = clamp_stop_index(index);
        colors_[i] = color;
        colors_[i].setAlpha(255);
        stop_opacities_[i] = color.alphaF();
        changed();
    }
    int add_stop(double position, QColor color = QColor())
    {
        const double p = std::clamp(position, 0.0, 1.0);
        if (!color.isValid()) color = interpolated_color(p);
        color.setAlpha(255);
        colors_.push_back(color);
        positions_.push_back(p);
        stop_opacities_.push_back(std::clamp((double)color.alphaF(), 0.0, 1.0));
        selected_stop_ = stop_count() - 1;
        changed();
        if (selection_changed) selection_changed(selected_stop_);
        return selected_stop_;
    }
    void remove_stop(int index)
    {
        const int i = clamp_stop_index(index);
        if (i < 0 || stop_count() <= 2) return;
        colors_.erase(colors_.begin() + i);
        positions_.erase(positions_.begin() + i);
        stop_opacities_.erase(stop_opacities_.begin() + i);
        selected_stop_ = stop_count() > 0 ? std::min(selected_stop_, stop_count() - 1) : -1;
        changed();
        if (selection_changed) selection_changed(selected_stop_);
    }
    void set_selected_stop(int index)
    {
        selected_stop_ = (index >= 0 && index < stop_count()) ? index : -1;
        update();
        if (selection_changed) selection_changed(selected_stop_);
    }

    QPoint stop_global_anchor(int index) const
    {
        const QPointF p = stop_point(clamp_stop_index(index));
        return mapToGlobal(QPoint((int)std::round(p.x()), (int)std::round(p.y() + 14.0)));
    }

    std::function<void()> gradient_changed;
    std::function<void(int)> selection_changed;
    std::function<void(int, const QPoint &)> color_popup_requested;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(43, 43, 43));

        const QRectF ramp = gradient_line_rect();
        draw_checkerboard(p, ramp.toAlignedRect());
        p.fillRect(ramp, preview_brush(ramp));
        p.setPen(QPen(QColor(24, 24, 24), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(ramp);

        // Photoshop-style midpoint diamonds between adjacent color stops.
        auto sorted = sorted_stop_indices();
        p.setPen(QPen(QColor(145, 145, 145), 1));
        for (int i = 0; i + 1 < (int)sorted.size(); ++i) {
            double mid = (positions_[sorted[i]] + positions_[sorted[i + 1]]) * 0.5;
            QPointF m(ramp.left() + mid * ramp.width(), ramp.bottom() + 8.0);
            QPolygonF diamond;
            diamond << QPointF(m.x(), m.y() - 5) << QPointF(m.x() + 5, m.y())
                    << QPointF(m.x(), m.y() + 5) << QPointF(m.x() - 5, m.y());
            p.setBrush(QColor(75, 75, 75));
            p.drawPolygon(diamond);
        }

        for (int i = 0; i < stop_count(); ++i) {
            const QPointF pos(ramp.left() + positions_[i] * ramp.width(), 0.0);

            // Opacity stop handle above the ramp.
            QPolygonF opacity_handle;
            opacity_handle << QPointF(pos.x() - 6, ramp.top() - 18)
                           << QPointF(pos.x() + 6, ramp.top() - 18)
                           << QPointF(pos.x() + 6, ramp.top() - 7)
                           << QPointF(pos.x(), ramp.top() - 2)
                           << QPointF(pos.x() - 6, ramp.top() - 7);
            p.setPen(QPen(i == selected_stop_ ? QColor(120, 170, 230) : QColor(18, 18, 18), i == selected_stop_ ? 2 : 1));
            p.setBrush(QColor(210, 210, 210, (int)std::round(stop_opacities_[i] * 255.0)));
            p.drawPolygon(opacity_handle);

            // Color stop handle below the ramp.
            QPolygonF color_handle;
            color_handle << QPointF(pos.x(), ramp.bottom() + 2)
                         << QPointF(pos.x() + 7, ramp.bottom() + 8)
                         << QPointF(pos.x() + 7, ramp.bottom() + 23)
                         << QPointF(pos.x() - 7, ramp.bottom() + 23)
                         << QPointF(pos.x() - 7, ramp.bottom() + 8);
            p.setPen(QPen(i == selected_stop_ ? QColor(120, 170, 230) : QColor(18, 18, 18), i == selected_stop_ ? 2 : 1));
            p.setBrush(QColor(36, 36, 36));
            p.drawPolygon(color_handle);
            QRectF swatch(pos.x() - 5, ramp.bottom() + 10, 10, 10);
            draw_checkerboard(p, swatch.toAlignedRect());
            p.fillRect(swatch, stop_color(i));
            p.setPen(QColor(18, 18, 18));
            p.drawRect(swatch.adjusted(0, 0, -1, -1));
        }

        p.setPen(QColor(210, 210, 210));
        p.drawText(QRectF(ramp.left(), 6, ramp.width(), 20),
                   QStringLiteral("Opacity stops above, color stops below — %1 stops")
                       .arg(stop_count()));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        int hit = hit_stop(event->pos());
        if (hit >= 0) {
            set_selected_stop(hit);
            dragging_stop_ = true;
            dragging_stop_index_ = hit;
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (dragging_stop_ && (event->buttons() & Qt::LeftButton)) {
            const int drag_index = clamp_stop_index(dragging_stop_index_);
            if (drag_index < 0 || drag_index >= stop_count()) {
                dragging_stop_ = false;
                dragging_stop_index_ = -1;
                QWidget::mouseMoveEvent(event);
                return;
            }
            selected_stop_ = drag_index;
            const QRectF line = gradient_line_rect();
            positions_[drag_index] = std::clamp((double)(event->position().x() - line.left()) / std::max(1.0, (double)line.width()), 0.0, 1.0);
            changed();
            event->accept();
            return;
        }
        setCursor(hit_stop(event->pos()) >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        dragging_stop_ = false;
        dragging_stop_index_ = -1;
        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        int hit = hit_stop(event->pos());
        if (hit >= 0) {
            set_selected_stop(hit);
            if (stop_count() > 2) {
                remove_stop(hit);
            }
            event->accept();
            return;
        }
        if (gradient_line_rect().adjusted(-6, -12, 6, 12).contains(event->position())) {
            const QRectF line = gradient_line_rect();
            const double pos = std::clamp((double)(event->position().x() - line.left()) / std::max(1.0, (double)line.width()), 0.0, 1.0);
            add_stop(pos);
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

private:
    int clamp_stop_index(int index) const { return std::clamp(index, 0, std::max(0, stop_count() - 1)); }
    QRectF preview_rect() const { return gradient_line_rect(); }
    QRectF gradient_line_rect() const
    {
        return QRectF(24, 42, std::max(1, width() - 48), 28);
    }
    QPointF stop_point(int index) const
    {
        const int i = clamp_stop_index(index);
        const QRectF line = gradient_line_rect();
        return QPointF(line.left() + positions_[i] * line.width(), line.center().y());
    }
    QRectF stop_handle_rect(int index) const
    {
        QPointF p = stop_point(index);
        return QRectF(p.x() - 10, p.y() - 20, 20, 48);
    }
    int hit_stop(const QPointF &pos) const
    {
        int best = -1;
        double best_distance = std::numeric_limits<double>::max();
        for (int i = 0; i < stop_count(); ++i) {
            if (!stop_handle_rect(i).adjusted(-4, -4, 4, 4).contains(pos))
                continue;
            const double distance = std::abs(pos.x() - stop_point(i).x());
            const bool prefer_selected = (i == selected_stop_ && best != selected_stop_);
            if (prefer_selected || distance < best_distance - 0.5 ||
                (std::abs(distance - best_distance) <= 0.5 &&
                 (best < 0 || (best != selected_stop_ && i < best)))) {
                best = i;
                best_distance = distance;
            }
        }
        return best;
    }

    std::vector<int> sorted_stop_indices() const
    {
        std::vector<int> indices;
        for (int i = 0; i < stop_count(); ++i) indices.push_back(i);
        std::sort(indices.begin(), indices.end(), [&](int a, int b) { return positions_[a] < positions_[b]; });
        return indices;
    }

    QColor interpolated_color(double pos) const
    {
        auto sorted = sorted_stop_indices();
        if (sorted.empty()) return QColor(255, 255, 255, 255);
        int left = sorted.front();
        int right = sorted.back();
        for (int i = 0; i + 1 < (int)sorted.size(); ++i) {
            if (positions_[sorted[i]] <= pos && pos <= positions_[sorted[i + 1]]) {
                left = sorted[i];
                right = sorted[i + 1];
                break;
            }
        }
        const double span = std::max(0.0001, positions_[right] - positions_[left]);
        const double t = std::clamp((pos - positions_[left]) / span, 0.0, 1.0);
        QColor a = colors_[left];
        QColor b = colors_[right];
        QColor out((int)std::round(a.red() + (b.red() - a.red()) * t),
                   (int)std::round(a.green() + (b.green() - a.green()) * t),
                   (int)std::round(a.blue() + (b.blue() - a.blue()) * t),
                   (int)std::round((stop_opacities_[left] + (stop_opacities_[right] - stop_opacities_[left]) * t) * 255.0));
        return out;
    }

    void changed()
    {
        update();
        if (gradient_changed) gradient_changed();
    }

    void set_all_stops(QGradient &gradient) const
    {
        for (int i = 0; i < stop_count(); ++i)
            gradient.setColorAt(std::clamp(positions_[i], 0.0, 1.0), stop_color(i));
    }

    QBrush preview_brush(const QRectF &r) const
    {
        // The editor ramp is always a left-to-right linear preview, like Photoshop.
        // The selected gradient type still controls the actual object/canvas rendering.
        QLinearGradient gradient(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        set_all_stops(gradient);
        return QBrush(gradient);
    }

    static QString type_name(int type)
    {
        switch (type) {
        case 1: return QStringLiteral("Radial");
        case 2: return QStringLiteral("Angle");
        case 3: return QStringLiteral("Reflected");
        case 4: return QStringLiteral("Diamond");
        case 0:
        default: return QStringLiteral("Linear");
        }
    }

    static void draw_checkerboard(QPainter &p, const QRect &rect)
    {
        const int cell = 12;
        for (int y = rect.top(); y < rect.bottom(); y += cell)
            for (int x = rect.left(); x < rect.right(); x += cell)
                p.fillRect(QRect(x, y, cell, cell).intersected(rect),
                           ((x / cell) + (y / cell)) % 2 ? QColor(62, 62, 62) : QColor(82, 82, 82));
    }

    int type_ = 0;
    std::vector<QColor> colors_ = {QColor(75, 110, 168), QColor(27, 27, 27)};
    std::vector<double> positions_ = {0.0, 1.0};
    std::vector<double> stop_opacities_ = {1.0, 1.0};
    double opacity_ = 1.0;
    double angle_ = 0.0;
    double center_x_ = 0.5;
    double center_y_ = 0.5;
    double scale_ = 1.0;
    int selected_stop_ = -1;
    bool dragging_stop_ = false;
    int dragging_stop_index_ = -1;
};



static QColor rich_text_color_from_argb(uint32_t argb)
{
    return QColor((argb >> 16) & 0xFF,
                  (argb >> 8) & 0xFF,
                  argb & 0xFF,
                  (argb >> 24) & 0xFF);
}

static uint32_t rich_text_argb_from_color(const QColor &color)
{
    return ((uint32_t)color.alpha() << 24) |
           ((uint32_t)color.red() << 16) |
           ((uint32_t)color.green() << 8) |
           (uint32_t)color.blue();
}

static std::vector<QRectF> text_edit_selection_viewport_rects(const QTextEdit *editor)
{
    std::vector<QRectF> rects;
    if (!editor || !editor->textCursor().hasSelection())
        return rects;

    const QTextCursor cursor = editor->textCursor();
    const int selection_start = cursor.selectionStart();
    const int selection_end = cursor.selectionEnd();
    if (selection_start >= selection_end)
        return rects;

    const QTextDocument *doc = editor->document();
    const QAbstractTextDocumentLayout *doc_layout = doc ? doc->documentLayout() : nullptr;
    if (!doc || !doc_layout)
        return rects;

    const QPointF scroll_offset(editor->horizontalScrollBar() ? editor->horizontalScrollBar()->value() : 0.0,
                                editor->verticalScrollBar() ? editor->verticalScrollBar()->value() : 0.0);

    for (QTextBlock block = doc->findBlock(selection_start);
         block.isValid() && block.position() < selection_end;
         block = block.next()) {
        const QTextLayout *layout = block.layout();
        if (!layout)
            continue;

        const int block_start = block.position();
        const int block_text_end = block_start + std::max(0, block.length() - 1);
        const int block_selection_start = std::max(selection_start, block_start);
        const int block_selection_end = std::min(selection_end, block_text_end);
        if (block_selection_start >= block_selection_end)
            continue;

        const QRectF block_rect = doc_layout->blockBoundingRect(block);
        for (int line_index = 0; line_index < layout->lineCount(); ++line_index) {
            const QTextLine line = layout->lineAt(line_index);
            if (!line.isValid())
                continue;

            const int line_start = block_start + line.textStart();
            const int line_end = line_start + line.textLength();
            const int line_selection_start = std::max(block_selection_start, line_start);
            const int line_selection_end = std::min(block_selection_end, line_end);
            if (line_selection_start >= line_selection_end)
                continue;

            const qreal x1 = line.cursorToX(line_selection_start - block_start);
            const qreal x2 = line.cursorToX(line_selection_end - block_start);
            QRectF rect(block_rect.left() + std::min(x1, x2),
                        block_rect.top() + line.y(),
                        std::max<qreal>(1.0, std::abs(x2 - x1)),
                        std::max<qreal>(1.0, line.height()));
            rect.translate(-scroll_offset);
            rects.push_back(rect.adjusted(-1.0, 0.0, 1.0, 0.0));
        }
    }

    return rects;
}

enum RichTextCharFormatMask : uint32_t {
    RichTextCharFontFamily = 1u << 0,
    RichTextCharFontSize = 1u << 1,
    RichTextCharBold = 1u << 2,
    RichTextCharItalic = 1u << 3,
    RichTextCharUnderline = 1u << 4,
    RichTextCharStrikethrough = 1u << 5,
    RichTextCharTracking = 1u << 6,
    RichTextCharScaleX = 1u << 7,
    RichTextCharScaleY = 1u << 8,
    RichTextCharBaselineShift = 1u << 9,
    RichTextCharFillColor = 1u << 10,
    RichTextCharFontStyle = 1u << 11,
    RichTextCharKerning = 1u << 12,
    RichTextCharTextStyle = 1u << 13,
    RichTextCharLigatures = 1u << 14,
    RichTextCharStylisticAlternates = 1u << 15,
    RichTextCharFractions = 1u << 16,
    RichTextCharOpenTypeFeatures = 1u << 17,
    RichTextCharLanguage = 1u << 18,
};

enum RichTextFormatProperty {
    RichTextPropFontFamily = QTextFormat::UserProperty + 100,
    RichTextPropFontSize,
    RichTextPropBold,
    RichTextPropItalic,
    RichTextPropUnderline,
    RichTextPropStrikethrough,
    RichTextPropTracking,
    RichTextPropScaleX,
    RichTextPropScaleY,
    RichTextPropBaselineShift,
    RichTextPropFillType,
    RichTextPropFillColor,
    RichTextPropGradientType,
    RichTextPropGradientStartColor,
    RichTextPropGradientEndColor,
    RichTextPropGradientStartPos,
    RichTextPropGradientEndPos,
    RichTextPropGradientStartOpacity,
    RichTextPropGradientEndOpacity,
    RichTextPropGradientOpacity,
    RichTextPropGradientAngle,
    RichTextPropGradientCenterX,
    RichTextPropGradientCenterY,
    RichTextPropGradientScale,
    RichTextPropGradientFocalX,
    RichTextPropGradientFocalY,
    RichTextPropFontStyle,
    RichTextPropKerning,
    RichTextPropKerningMode,
    RichTextPropManualKerning,
    RichTextPropTextStyle,
    RichTextPropLigatures,
    RichTextPropStylisticAlternates,
    RichTextPropFractions,
    RichTextPropOpenTypeFeatures,
    RichTextPropLanguage,
};

static void store_rich_text_format_properties(QTextCharFormat &out, const RichTextCharFormat &format)
{
    out.setProperty(RichTextPropFontFamily, QString::fromStdString(format.font_family));
    out.setProperty(RichTextPropFontStyle, QString::fromStdString(format.font_style));
    out.setProperty(RichTextPropFontSize, format.font_size);
    out.setProperty(RichTextPropBold, format.bold);
    out.setProperty(RichTextPropItalic, format.italic);
    out.setProperty(RichTextPropUnderline, format.underline);
    out.setProperty(RichTextPropStrikethrough, format.strikethrough);
    out.setProperty(RichTextPropKerning, format.kerning);
    out.setProperty(RichTextPropKerningMode, format.kerning_mode);
    out.setProperty(RichTextPropManualKerning, (double)format.manual_kerning);
    out.setProperty(RichTextPropTracking, (double)format.tracking);
    out.setProperty(RichTextPropScaleX, (double)format.scale_x);
    out.setProperty(RichTextPropScaleY, (double)format.scale_y);
    out.setProperty(RichTextPropBaselineShift, (double)format.baseline_shift);
    out.setProperty(RichTextPropTextStyle, format.text_style);
    out.setProperty(RichTextPropLigatures, format.ligatures);
    out.setProperty(RichTextPropStylisticAlternates, format.stylistic_alternates);
    out.setProperty(RichTextPropFractions, format.fractions);
    out.setProperty(RichTextPropOpenTypeFeatures, format.opentype_features);
    out.setProperty(RichTextPropLanguage, QString::fromStdString(format.language));
    out.setProperty(RichTextPropFillType, format.fill.type);
    out.setProperty(RichTextPropFillColor, (uint)format.fill.color);
    out.setProperty(RichTextPropGradientType, format.fill.gradient_type);
    out.setProperty(RichTextPropGradientStartColor, (uint)format.fill.gradient_start_color);
    out.setProperty(RichTextPropGradientEndColor, (uint)format.fill.gradient_end_color);
    out.setProperty(RichTextPropGradientStartPos, (double)format.fill.gradient_start_pos);
    out.setProperty(RichTextPropGradientEndPos, (double)format.fill.gradient_end_pos);
    out.setProperty(RichTextPropGradientStartOpacity, (double)format.fill.gradient_start_opacity);
    out.setProperty(RichTextPropGradientEndOpacity, (double)format.fill.gradient_end_opacity);
    out.setProperty(RichTextPropGradientOpacity, (double)format.fill.gradient_opacity);
    out.setProperty(RichTextPropGradientAngle, (double)format.fill.gradient_angle);
    out.setProperty(RichTextPropGradientCenterX, (double)format.fill.gradient_center_x);
    out.setProperty(RichTextPropGradientCenterY, (double)format.fill.gradient_center_y);
    out.setProperty(RichTextPropGradientScale, (double)format.fill.gradient_scale);
    out.setProperty(RichTextPropGradientFocalX, (double)format.fill.gradient_focal_x);
    out.setProperty(RichTextPropGradientFocalY, (double)format.fill.gradient_focal_y);
}

static void store_editor_rich_text_format_properties_masked(QTextCharFormat &out, const RichTextCharFormat &format, uint32_t mask)
{
    if (mask & RichTextCharFontFamily)
        out.setProperty(RichTextPropFontFamily, QString::fromStdString(format.font_family));
    if (mask & RichTextCharFontStyle)
        out.setProperty(RichTextPropFontStyle, QString::fromStdString(format.font_style));
    if (mask & RichTextCharFontSize)
        out.setProperty(RichTextPropFontSize, format.font_size);
    if (mask & RichTextCharBold) out.setProperty(RichTextPropBold, format.bold);
    if (mask & RichTextCharItalic) out.setProperty(RichTextPropItalic, format.italic);
    if (mask & RichTextCharUnderline) out.setProperty(RichTextPropUnderline, format.underline);
    if (mask & RichTextCharStrikethrough) out.setProperty(RichTextPropStrikethrough, format.strikethrough);
    if (mask & RichTextCharKerning) {
        out.setProperty(RichTextPropKerning, format.kerning);
        out.setProperty(RichTextPropKerningMode, format.kerning_mode);
        out.setProperty(RichTextPropManualKerning, (double)format.manual_kerning);
    }
    if (mask & RichTextCharTracking) out.setProperty(RichTextPropTracking, (double)format.tracking);
    if (mask & RichTextCharScaleX) out.setProperty(RichTextPropScaleX, (double)format.scale_x);
    if (mask & RichTextCharScaleY) out.setProperty(RichTextPropScaleY, (double)format.scale_y);
    if (mask & RichTextCharBaselineShift) out.setProperty(RichTextPropBaselineShift, (double)format.baseline_shift);
    if (mask & RichTextCharTextStyle) out.setProperty(RichTextPropTextStyle, format.text_style);
    if (mask & RichTextCharLigatures) out.setProperty(RichTextPropLigatures, format.ligatures);
    if (mask & RichTextCharStylisticAlternates)
        out.setProperty(RichTextPropStylisticAlternates, format.stylistic_alternates);
    if (mask & RichTextCharFractions) out.setProperty(RichTextPropFractions, format.fractions);
    if (mask & RichTextCharOpenTypeFeatures) out.setProperty(RichTextPropOpenTypeFeatures, format.opentype_features);
    if (mask & RichTextCharLanguage)
        out.setProperty(RichTextPropLanguage, QString::fromStdString(format.language));
    if (mask & RichTextCharFillColor) {
        out.setProperty(RichTextPropFillType, format.fill.type);
        out.setProperty(RichTextPropFillColor, (uint)format.fill.color);
        out.setProperty(RichTextPropGradientType, format.fill.gradient_type);
        out.setProperty(RichTextPropGradientStartColor, (uint)format.fill.gradient_start_color);
        out.setProperty(RichTextPropGradientEndColor, (uint)format.fill.gradient_end_color);
        out.setProperty(RichTextPropGradientStartPos, (double)format.fill.gradient_start_pos);
        out.setProperty(RichTextPropGradientEndPos, (double)format.fill.gradient_end_pos);
        out.setProperty(RichTextPropGradientStartOpacity, (double)format.fill.gradient_start_opacity);
        out.setProperty(RichTextPropGradientEndOpacity, (double)format.fill.gradient_end_opacity);
        out.setProperty(RichTextPropGradientOpacity, (double)format.fill.gradient_opacity);
        out.setProperty(RichTextPropGradientAngle, (double)format.fill.gradient_angle);
        out.setProperty(RichTextPropGradientCenterX, (double)format.fill.gradient_center_x);
        out.setProperty(RichTextPropGradientCenterY, (double)format.fill.gradient_center_y);
        out.setProperty(RichTextPropGradientScale, (double)format.fill.gradient_scale);
        out.setProperty(RichTextPropGradientFocalX, (double)format.fill.gradient_focal_x);
        out.setProperty(RichTextPropGradientFocalY, (double)format.fill.gradient_focal_y);
    }
}

static void apply_editor_rich_text_extended_font_properties(QFont &font, const RichTextCharFormat &format)
{
    if (!format.font_style.empty())
        font.setStyleName(QString::fromStdString(format.font_style));
    font.setKerning(format.kerning_mode != 2 && format.kerning);
    font.setLetterSpacing(QFont::AbsoluteSpacing,
                          format.tracking + (format.kerning_mode == 2 ? format.manual_kerning : 0.0f));
    font.setStretch(std::clamp((int)std::round(format.scale_x * 100.0f), 1, 4000));
    font.setCapitalization(QFont::MixedCase);
    if (format.text_style == 1)
        font.setCapitalization(QFont::AllUppercase);
    else if (format.text_style == 2)
        font.setCapitalization(QFont::SmallCaps);
    if (format.text_style == 3 || format.text_style == 4)
        font.setPixelSize(std::max(1, (int)std::round(font.pixelSize() * 0.65)));
    if (!format.ligatures)
        font.setStyleStrategy((QFont::StyleStrategy)(font.styleStrategy() | QFont::PreferNoShaping));
}

static void apply_editor_rich_text_extended_char_format(QTextCharFormat &out, const RichTextCharFormat &format)
{
    if (format.text_style == 3)
        out.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
    else if (format.text_style == 4)
        out.setVerticalAlignment(QTextCharFormat::AlignSubScript);
    else
        out.setVerticalAlignment(QTextCharFormat::AlignNormal);
    out.setFontKerning(format.kerning_mode != 2 && format.kerning);
    out.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    out.setFontLetterSpacing(format.tracking + (format.kerning_mode == 2 ? format.manual_kerning : 0.0f));
}

static RichTextCharFormat rich_text_format_from_qtext_format(const QTextCharFormat &fmt,
                                                             const RichTextCharFormat &fallback,
                                                             double visual_scale)
{
    RichTextCharFormat out = fallback;
    QFont font = fmt.font();
    out.font_family = fmt.property(RichTextPropFontFamily).toString().isEmpty()
                          ? (!font.family().isEmpty() ? font.family().toStdString() : out.font_family)
                          : fmt.property(RichTextPropFontFamily).toString().toStdString();
    if (fmt.hasProperty(RichTextPropFontStyle))
        out.font_style = fmt.property(RichTextPropFontStyle).toString().toStdString();
    else if (!font.styleName().isEmpty())
        out.font_style = font.styleName().toStdString();
    int px = font.pixelSize();
    if (px <= 0 && font.pointSizeF() > 0.0) px = (int)std::round(font.pointSizeF());
    out.font_size = fmt.hasProperty(RichTextPropFontSize)
                        ? fmt.property(RichTextPropFontSize).toInt()
                        : (px > 0 ? std::max(1, (int)std::round(px / std::max(0.0001, visual_scale))) : out.font_size);
    out.bold = fmt.hasProperty(RichTextPropBold) ? fmt.property(RichTextPropBold).toBool() : (fmt.fontWeight() >= QFont::Bold);
    out.italic = fmt.hasProperty(RichTextPropItalic) ? fmt.property(RichTextPropItalic).toBool() : fmt.fontItalic();
    out.underline = fmt.hasProperty(RichTextPropUnderline) ? fmt.property(RichTextPropUnderline).toBool() : fmt.fontUnderline();
    out.strikethrough = fmt.hasProperty(RichTextPropStrikethrough) ? fmt.property(RichTextPropStrikethrough).toBool() : fmt.fontStrikeOut();
    if (fmt.hasProperty(RichTextPropKerning)) out.kerning = fmt.property(RichTextPropKerning).toBool();
    else out.kerning = font.kerning();
    if (fmt.hasProperty(RichTextPropKerningMode)) out.kerning_mode = fmt.property(RichTextPropKerningMode).toInt();
    if (fmt.hasProperty(RichTextPropManualKerning)) out.manual_kerning = (float)fmt.property(RichTextPropManualKerning).toDouble();
    if (fmt.hasProperty(RichTextPropTracking)) out.tracking = (float)fmt.property(RichTextPropTracking).toDouble();
    if (fmt.hasProperty(RichTextPropScaleX)) out.scale_x = (float)fmt.property(RichTextPropScaleX).toDouble();
    if (fmt.hasProperty(RichTextPropScaleY)) out.scale_y = (float)fmt.property(RichTextPropScaleY).toDouble();
    if (fmt.hasProperty(RichTextPropBaselineShift)) out.baseline_shift = (float)fmt.property(RichTextPropBaselineShift).toDouble();
    if (fmt.hasProperty(RichTextPropTextStyle)) out.text_style = fmt.property(RichTextPropTextStyle).toInt();
    else if (font.capitalization() == QFont::AllUppercase) out.text_style = 1;
    else if (font.capitalization() == QFont::SmallCaps) out.text_style = 2;
    else if (fmt.verticalAlignment() == QTextCharFormat::AlignSuperScript) out.text_style = 3;
    else if (fmt.verticalAlignment() == QTextCharFormat::AlignSubScript) out.text_style = 4;
    if (fmt.hasProperty(RichTextPropLigatures)) out.ligatures = fmt.property(RichTextPropLigatures).toBool();
    if (fmt.hasProperty(RichTextPropStylisticAlternates)) out.stylistic_alternates = fmt.property(RichTextPropStylisticAlternates).toBool();
    if (fmt.hasProperty(RichTextPropFractions)) out.fractions = fmt.property(RichTextPropFractions).toBool();
    if (fmt.hasProperty(RichTextPropOpenTypeFeatures)) out.opentype_features = fmt.property(RichTextPropOpenTypeFeatures).toBool();
    if (fmt.hasProperty(RichTextPropLanguage)) out.language = fmt.property(RichTextPropLanguage).toString().toStdString();
    if (fmt.hasProperty(RichTextPropFillType)) out.fill.type = fmt.property(RichTextPropFillType).toInt();
    if (fmt.hasProperty(RichTextPropFillColor)) out.fill.color = fmt.property(RichTextPropFillColor).toUInt();
    else if (fmt.foreground().style() != Qt::NoBrush) {
        const QColor c = fmt.foreground().color();
        if (c.isValid() && c.alpha() > 0)
            out.fill.color = rich_text_argb_from_color(c);
    }
    if (fmt.hasProperty(RichTextPropGradientType)) out.fill.gradient_type = fmt.property(RichTextPropGradientType).toInt();
    if (fmt.hasProperty(RichTextPropGradientStartColor)) out.fill.gradient_start_color = fmt.property(RichTextPropGradientStartColor).toUInt();
    if (fmt.hasProperty(RichTextPropGradientEndColor)) out.fill.gradient_end_color = fmt.property(RichTextPropGradientEndColor).toUInt();
    if (fmt.hasProperty(RichTextPropGradientStartPos)) out.fill.gradient_start_pos = (float)fmt.property(RichTextPropGradientStartPos).toDouble();
    if (fmt.hasProperty(RichTextPropGradientEndPos)) out.fill.gradient_end_pos = (float)fmt.property(RichTextPropGradientEndPos).toDouble();
    if (fmt.hasProperty(RichTextPropGradientStartOpacity)) out.fill.gradient_start_opacity = (float)fmt.property(RichTextPropGradientStartOpacity).toDouble();
    if (fmt.hasProperty(RichTextPropGradientEndOpacity)) out.fill.gradient_end_opacity = (float)fmt.property(RichTextPropGradientEndOpacity).toDouble();
    if (fmt.hasProperty(RichTextPropGradientOpacity)) out.fill.gradient_opacity = (float)fmt.property(RichTextPropGradientOpacity).toDouble();
    if (fmt.hasProperty(RichTextPropGradientAngle)) out.fill.gradient_angle = (float)fmt.property(RichTextPropGradientAngle).toDouble();
    if (fmt.hasProperty(RichTextPropGradientCenterX)) out.fill.gradient_center_x = (float)fmt.property(RichTextPropGradientCenterX).toDouble();
    if (fmt.hasProperty(RichTextPropGradientCenterY)) out.fill.gradient_center_y = (float)fmt.property(RichTextPropGradientCenterY).toDouble();
    if (fmt.hasProperty(RichTextPropGradientScale)) out.fill.gradient_scale = (float)fmt.property(RichTextPropGradientScale).toDouble();
    if (fmt.hasProperty(RichTextPropGradientFocalX)) out.fill.gradient_focal_x = (float)fmt.property(RichTextPropGradientFocalX).toDouble();
    if (fmt.hasProperty(RichTextPropGradientFocalY)) out.fill.gradient_focal_y = (float)fmt.property(RichTextPropGradientFocalY).toDouble();
    return out;
}

static QTextCharFormat qtext_format_from_rich_text_format(const RichTextCharFormat &format, double visual_scale)
{
    QTextCharFormat out;
    QFont font(QString::fromStdString(format.font_family));
    font.setPixelSize(std::max(1, (int)std::round(format.font_size * visual_scale)));
    font.setBold(format.bold);
    font.setItalic(format.italic);
    font.setUnderline(format.underline);
    font.setStrikeOut(format.strikethrough);
    apply_editor_rich_text_extended_font_properties(font, format);
    out.setFont(font);
    out.setFontUnderline(format.underline);
    out.setFontStrikeOut(format.strikethrough);
    apply_editor_rich_text_extended_char_format(out, format);
    store_rich_text_format_properties(out, format);
    QColor color = rich_text_color_from_argb(format.fill.color);
    color.setAlpha(0);
    out.setForeground(color);
    return out;
}

static void populate_qtext_document_from_rich_text(QTextDocument *doc, const RichTextDocument &model, double visual_scale)
{
    if (!doc) return;
    QSignalBlocker blocker(doc);
    doc->setPlainText(QString::fromStdString(model.plain_text));
    QTextCursor all(doc);
    all.select(QTextCursor::Document);
    all.mergeCharFormat(qtext_format_from_rich_text_format(model.default_format, visual_scale));
    for (const auto &range : model.ranges) {
        if (range.length == 0 || range.start >= model.plain_text.size()) continue;
        QTextCursor cursor(doc);
        cursor.setPosition((int)std::min(range.start, model.plain_text.size()));
        cursor.setPosition((int)std::min(range.start + range.length, model.plain_text.size()), QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(qtext_format_from_rich_text_format(range.format, visual_scale));
    }
}

static void populate_qtext_document_from_plain_layer_text(QTextDocument *doc, const std::string &text,
                                                          const RichTextCharFormat &format, double visual_scale)
{
    if (!doc) return;
    QSignalBlocker blocker(doc);
    doc->setPlainText(QString::fromStdString(text));
    QTextCursor all(doc);
    all.select(QTextCursor::Document);
    all.mergeCharFormat(qtext_format_from_rich_text_format(format, visual_scale));
}


static bool rich_text_fills_equal(const RichTextFill &a, const RichTextFill &b)
{
    return a.type == b.type && a.color == b.color && a.gradient_type == b.gradient_type &&
           a.gradient_start_color == b.gradient_start_color &&
           a.gradient_end_color == b.gradient_end_color &&
           std::abs(a.gradient_start_pos - b.gradient_start_pos) < 0.0001f &&
           std::abs(a.gradient_end_pos - b.gradient_end_pos) < 0.0001f &&
           std::abs(a.gradient_start_opacity - b.gradient_start_opacity) < 0.0001f &&
           std::abs(a.gradient_end_opacity - b.gradient_end_opacity) < 0.0001f &&
           std::abs(a.gradient_opacity - b.gradient_opacity) < 0.0001f &&
           std::abs(a.gradient_angle - b.gradient_angle) < 0.0001f &&
           std::abs(a.gradient_center_x - b.gradient_center_x) < 0.0001f &&
           std::abs(a.gradient_center_y - b.gradient_center_y) < 0.0001f &&
           std::abs(a.gradient_scale - b.gradient_scale) < 0.0001f &&
           std::abs(a.gradient_focal_x - b.gradient_focal_x) < 0.0001f &&
           std::abs(a.gradient_focal_y - b.gradient_focal_y) < 0.0001f;
}

static bool rich_text_char_formats_equal(const RichTextCharFormat &a, const RichTextCharFormat &b)
{
    return a.font_family == b.font_family && a.font_style == b.font_style &&
           a.font_size == b.font_size && a.bold == b.bold && a.italic == b.italic &&
           a.underline == b.underline && a.strikethrough == b.strikethrough &&
           a.kerning == b.kerning && a.kerning_mode == b.kerning_mode &&
           std::abs(a.manual_kerning - b.manual_kerning) < 0.0001f &&
           std::abs(a.tracking - b.tracking) < 0.0001f &&
           std::abs(a.scale_x - b.scale_x) < 0.0001f &&
           std::abs(a.scale_y - b.scale_y) < 0.0001f &&
           std::abs(a.baseline_shift - b.baseline_shift) < 0.0001f &&
           a.text_style == b.text_style && a.ligatures == b.ligatures &&
           a.stylistic_alternates == b.stylistic_alternates && a.fractions == b.fractions &&
           a.opentype_features == b.opentype_features && a.language == b.language &&
           rich_text_fills_equal(a.fill, b.fill);
}

static bool rich_text_ranges_equal(const std::vector<RichTextRange> &a, const std::vector<RichTextRange> &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const RichTextRange &x = a[i];
        const RichTextRange &y = b[i];
        if (x.start != y.start || x.length != y.length) return false;
        if (!rich_text_char_formats_equal(x.format, y.format)) return false;
    }
    return true;
}

static RichTextParagraphFormat layer_paragraph_format_for_editor(const Layer &layer)
{
    if (!layer.rich_text.empty())
        return layer.rich_text.default_paragraph_format;
    RichTextParagraphFormat f;
    f.align_h = layer.align_h;
    f.align_v = layer.align_v;
    f.indent_left = layer.paragraph_indent_left;
    f.indent_right = layer.paragraph_indent_right;
    f.indent_first_line = layer.paragraph_indent_first_line;
    f.line_spacing = layer.text_leading;
    f.space_before = layer.paragraph_space_before;
    f.space_after = layer.paragraph_space_after;
    f.hyphenate = layer.paragraph_hyphenate;
    return f;
}

static RichTextDocument rich_text_document_from_qtext_document(const QTextDocument *doc, const Layer &layer, double visual_scale, const QTextCursor &text_cursor = QTextCursor())
{
    RichTextDocument model = layer.rich_text.empty() ? rich_text_document_from_layer_defaults(layer) : layer.rich_text;
    if (!doc) return model;
    model.plain_text = doc->toPlainText().toStdString();
    model.default_paragraph_format = layer_paragraph_format_for_editor(layer);
    model.ranges.clear();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.text().isEmpty()) continue;
            RichTextRange range;
            range.start = (size_t)std::max(0, fragment.position());
            range.length = (size_t)fragment.text().size();
            range.format = rich_text_format_from_qtext_format(fragment.charFormat(), model.default_format, visual_scale);
            model.ranges.push_back(range);
        }
    }
    if (!text_cursor.isNull()) {
        model.selection.anchor = (size_t)std::max(0, text_cursor.anchor());
        model.selection.head = (size_t)std::max(0, text_cursor.position());
        if (!text_cursor.hasSelection()) {
            model.typing_format = rich_text_format_from_qtext_format(text_cursor.charFormat(), model.default_format, visual_scale);
            model.has_typing_format = true;
        } else {
            model.has_typing_format = false;
        }
    } else {
        model.selection.anchor = 0;
        model.selection.head = 0;
        model.has_typing_format = false;
    }
    model.normalize();
    return model;
}



struct RichTextCharFormatSummary {
    RichTextCharFormat format;
    uint32_t mixed = 0;
    bool valid = false;
};

static RichTextCharFormat layer_char_format_for_editor(const Layer &layer)
{
    if (!layer.rich_text.empty())
        return layer.rich_text.default_format;
    RichTextCharFormat f;
    f.font_family = layer.font_family;
    f.font_style = layer.font_style;
    f.font_size = layer.font_size;
    f.bold = layer.font_bold;
    f.italic = layer.font_italic;
    f.underline = layer.text_underline;
    f.strikethrough = layer.text_strikethrough;
    f.kerning = layer.font_kerning;
    f.kerning_mode = layer.kerning_mode;
    f.manual_kerning = layer.manual_kerning;
    f.tracking = layer.char_tracking;
    f.scale_x = layer.char_scale_x;
    f.scale_y = layer.char_scale_y;
    f.baseline_shift = layer.baseline_shift;
    f.text_style = layer.text_style;
    f.ligatures = layer.text_ligatures;
    f.stylistic_alternates = layer.text_stylistic_alternates;
    f.fractions = layer.text_fractions;
    f.opentype_features = layer.text_opentype_features;
    f.language = layer.text_language;
    f.fill.type = layer.fill_type;
    f.fill.color = layer.text_color;
    f.fill.gradient_type = layer.gradient_type;
    f.fill.gradient_start_color = layer.gradient_start_color;
    f.fill.gradient_end_color = layer.gradient_end_color;
    f.fill.gradient_start_pos = layer.gradient_start_pos;
    f.fill.gradient_end_pos = layer.gradient_end_pos;
    f.fill.gradient_start_opacity = layer.gradient_start_opacity;
    f.fill.gradient_end_opacity = layer.gradient_end_opacity;
    f.fill.gradient_opacity = layer.gradient_opacity;
    f.fill.gradient_angle = layer.gradient_angle;
    f.fill.gradient_center_x = layer.gradient_center_x;
    f.fill.gradient_center_y = layer.gradient_center_y;
    f.fill.gradient_scale = layer.gradient_scale;
    f.fill.gradient_focal_x = layer.gradient_focal_x;
    f.fill.gradient_focal_y = layer.gradient_focal_y;
    return f;
}

static bool format_value_differs(const RichTextCharFormat &a, const RichTextCharFormat &b, uint32_t bit)
{
    switch (bit) {
    case RichTextCharFontFamily: return a.font_family != b.font_family;
    case RichTextCharFontStyle: return a.font_style != b.font_style;
    case RichTextCharFontSize: return a.font_size != b.font_size;
    case RichTextCharBold: return a.bold != b.bold;
    case RichTextCharItalic: return a.italic != b.italic;
    case RichTextCharUnderline: return a.underline != b.underline;
    case RichTextCharStrikethrough: return a.strikethrough != b.strikethrough;
    case RichTextCharKerning:
        return a.kerning != b.kerning || a.kerning_mode != b.kerning_mode ||
               std::abs(a.manual_kerning - b.manual_kerning) >= 0.0001f;
    case RichTextCharTracking: return a.tracking != b.tracking;
    case RichTextCharScaleX: return a.scale_x != b.scale_x;
    case RichTextCharScaleY: return a.scale_y != b.scale_y;
    case RichTextCharBaselineShift: return a.baseline_shift != b.baseline_shift;
    case RichTextCharFillColor: return !rich_text_fills_equal(a.fill, b.fill);
    case RichTextCharTextStyle: return a.text_style != b.text_style;
    case RichTextCharLigatures: return a.ligatures != b.ligatures;
    case RichTextCharStylisticAlternates: return a.stylistic_alternates != b.stylistic_alternates;
    case RichTextCharFractions: return a.fractions != b.fractions;
    case RichTextCharOpenTypeFeatures: return a.opentype_features != b.opentype_features;
    case RichTextCharLanguage: return a.language != b.language;
    default: return false;
    }
}

static RichTextCharFormat format_at_offset(const RichTextDocument &doc, size_t offset)
{
    RichTextCharFormat f = doc.default_format;
    for (const auto &range : doc.ranges) {
        if (offset >= range.start && offset < range.start + range.length)
            f = range.format;
    }
    return f;
}

static RichTextCharFormat rich_text_cursor_format(const RichTextDocument &doc)
{
    if (doc.has_typing_format)
        return doc.typing_format;
    if (doc.plain_text.empty())
        return doc.default_format;
    const size_t pos = std::min(doc.selection.head, doc.plain_text.size());
    const size_t sample = pos < doc.plain_text.size() ? pos : (pos > 0 ? pos - 1 : 0);
    return format_at_offset(doc, sample);
}

static RichTextCharFormat insertion_format_for_text_replace(const RichTextDocument &doc)
{
    if (doc.plain_text.empty())
        return doc.default_format;
    const size_t start = std::min(doc.selection.anchor, doc.selection.head);
    const size_t sample = std::min(start, doc.plain_text.size() - 1);
    return format_at_offset(doc, sample);
}

static RichTextCharFormatSummary summarize_rich_text_char_format(const Layer &layer, bool active_selection)
{
    RichTextDocument doc = layer.rich_text.empty() ? rich_text_document_from_layer_defaults(layer) : layer.rich_text;
    /*
     * The layer scalar text fields can represent the most recently applied
     * cursor style while editing.  Do not sync them into an existing rich text
     * document for inspection, or uncovered/default spans would be reported as
     * that cached style instead of the document's actual stored style.
     */
    doc.normalize();
    RichTextCharFormatSummary summary;
    const size_t text_len = doc.plain_text.size();
    if (!active_selection) {
        summary.format = doc.default_format;
        summary.valid = true;
        return summary;
    }
    size_t start = 0;
    size_t end = text_len;
    start = std::min(doc.selection.anchor, doc.selection.head);
    end = std::max(doc.selection.anchor, doc.selection.head);
    if (start == end) {
        summary.format = rich_text_cursor_format(doc);
        summary.valid = true;
        return summary;
    }
    if (text_len == 0) {
        summary.format = doc.has_typing_format ? doc.typing_format : doc.default_format;
        summary.valid = true;
        return summary;
    }
    start = std::min(start, text_len);
    end = std::min(end, text_len);
    if (start >= end) {
        summary.format = doc.default_format;
        summary.valid = true;
        return summary;
    }
    summary.format = format_at_offset(doc, start);
    summary.valid = true;
    constexpr uint32_t bits[] = {RichTextCharFontFamily, RichTextCharFontStyle, RichTextCharFontSize,
        RichTextCharBold, RichTextCharItalic, RichTextCharUnderline, RichTextCharStrikethrough,
        RichTextCharKerning, RichTextCharTracking, RichTextCharScaleX, RichTextCharScaleY,
        RichTextCharBaselineShift, RichTextCharFillColor, RichTextCharTextStyle, RichTextCharLigatures,
        RichTextCharStylisticAlternates, RichTextCharFractions, RichTextCharOpenTypeFeatures,
        RichTextCharLanguage};
    for (size_t i = start + 1; i < end; ++i) {
        RichTextCharFormat f = format_at_offset(doc, i);
        for (uint32_t bit : bits)
            if (format_value_differs(summary.format, f, bit)) summary.mixed |= bit;
    }
    return summary;
}

static void merge_format_bits(RichTextCharFormat &dst, const RichTextCharFormat &src, uint32_t mask)
{
    if (mask & RichTextCharFontFamily) dst.font_family = src.font_family;
    if (mask & RichTextCharFontStyle) dst.font_style = src.font_style;
    if (mask & RichTextCharFontSize) dst.font_size = src.font_size;
    if (mask & RichTextCharBold) dst.bold = src.bold;
    if (mask & RichTextCharItalic) dst.italic = src.italic;
    if (mask & RichTextCharUnderline) dst.underline = src.underline;
    if (mask & RichTextCharStrikethrough) dst.strikethrough = src.strikethrough;
    if (mask & RichTextCharKerning) {
        dst.kerning = src.kerning;
        dst.kerning_mode = src.kerning_mode;
        dst.manual_kerning = src.manual_kerning;
    }
    if (mask & RichTextCharTracking) dst.tracking = src.tracking;
    if (mask & RichTextCharScaleX) dst.scale_x = src.scale_x;
    if (mask & RichTextCharScaleY) dst.scale_y = src.scale_y;
    if (mask & RichTextCharBaselineShift) dst.baseline_shift = src.baseline_shift;
    if (mask & RichTextCharTextStyle) dst.text_style = src.text_style;
    if (mask & RichTextCharLigatures) dst.ligatures = src.ligatures;
    if (mask & RichTextCharStylisticAlternates) dst.stylistic_alternates = src.stylistic_alternates;
    if (mask & RichTextCharFractions) dst.fractions = src.fractions;
    if (mask & RichTextCharOpenTypeFeatures) dst.opentype_features = src.opentype_features;
    if (mask & RichTextCharLanguage) dst.language = src.language;
    if (mask & RichTextCharFillColor) dst.fill = src.fill;
}

static void materialize_rich_text_default_spans(RichTextDocument &doc)
{
    doc.normalize();
    if (doc.plain_text.empty()) return;
    std::vector<RichTextRange> materialized;
    materialized.reserve(doc.plain_text.size());
    size_t start = 0;
    RichTextCharFormat current = format_at_offset(doc, 0);
    for (size_t i = 1; i < doc.plain_text.size(); ++i) {
        RichTextCharFormat next = format_at_offset(doc, i);
        if (!rich_text_char_formats_equal(current, next)) {
            materialized.push_back({start, i - start, current});
            start = i;
            current = next;
        }
    }
    materialized.push_back({start, doc.plain_text.size() - start, current});
    doc.ranges = std::move(materialized);
}

static void apply_rich_text_format_to_layer_range(Layer &layer, const RichTextCharFormat &format, uint32_t mask, bool active_selection)
{
    if (layer.type != LayerType::Text && layer.type != LayerType::Ticker && layer.type != LayerType::Clock) return;
    if (layer.rich_text.empty())
        layer.rich_text = rich_text_document_from_layer_defaults(layer);
    RichTextDocument &doc = layer.rich_text;
    doc.normalize();
    const size_t text_len = doc.plain_text.size();
    size_t start = active_selection ? std::min(doc.selection.anchor, doc.selection.head) : 0;
    size_t end = active_selection ? std::max(doc.selection.anchor, doc.selection.head) : 0;
    start = std::min(start, text_len);
    end = std::min(end, text_len);
    if (!active_selection) {
        merge_format_bits(doc.default_format, format, mask);
        doc.has_typing_format = false;
        doc.normalize();
        layer.rich_text_html.clear();
        rich_text_document_sync_layer_mirrors(layer);
        return;
    }
    if (start == end) {
        RichTextCharFormat cursor_format = rich_text_cursor_format(doc);
        merge_format_bits(cursor_format, format, mask);
        doc.typing_format = cursor_format;
        doc.has_typing_format = true;
        doc.normalize();
        layer.rich_text_html.clear();
        rich_text_document_sync_layer_mirrors(layer);
        return;
    }
    std::vector<RichTextRange> next;
    next.reserve(doc.ranges.size() + 3);
    size_t cursor = start;
    auto append_range = [&next](size_t s, size_t len, const RichTextCharFormat &fmt) {
        if (len > 0) next.push_back({s, len, fmt});
    };
    for (const auto &range : doc.ranges) {
        const size_t rs = range.start;
        const size_t re = std::min(text_len, range.start + range.length);
        if (re <= start || rs >= end) {
            next.push_back(range);
            continue;
        }
        append_range(rs, start > rs ? start - rs : 0, range.format);
        if (cursor < std::max(start, rs)) {
            RichTextCharFormat gap = doc.default_format;
            merge_format_bits(gap, format, mask);
            append_range(cursor, std::max(start, rs) - cursor, gap);
        }
        RichTextCharFormat changed = range.format;
        merge_format_bits(changed, format, mask);
        const size_t is = std::max(start, rs);
        const size_t ie = std::min(end, re);
        append_range(is, ie - is, changed);
        cursor = std::max(cursor, ie);
        append_range(end, re > end ? re - end : 0, range.format);
    }
    if (cursor < end) {
        RichTextCharFormat changed = doc.default_format;
        merge_format_bits(changed, format, mask);
        append_range(cursor, end - cursor, changed);
    }
    doc.ranges = std::move(next);
    doc.has_typing_format = false;
    doc.normalize();
    layer.rich_text_html.clear();
    rich_text_document_sync_layer_mirrors(layer);
}

static void apply_rich_text_paragraph_format_to_layer(Layer &layer, const RichTextParagraphFormat &format)
{
    if (layer.type != LayerType::Text && layer.type != LayerType::Ticker && layer.type != LayerType::Clock) return;
    if (layer.rich_text.empty())
        layer.rich_text = rich_text_document_from_layer_defaults(layer);
    layer.rich_text.default_paragraph_format = format;
    layer.rich_text.normalize();
    layer.rich_text_html.clear();
    rich_text_document_sync_layer_mirrors(layer);
}

static QString rich_text_plain_text(const std::string &html)
{
    if (html.empty()) return QString();
    QTextDocument doc;
    doc.setHtml(QString::fromStdString(html));
    return doc.toPlainText();
}

static QString rich_text_html_with_replaced_plain_text(const std::string &html, const QString &plain)
{
    if (html.empty() || plain.isEmpty())
        return QString();

    QTextDocument source;
    source.setHtml(QString::fromStdString(html));
    QTextDocument replacement;
    replacement.setDocumentMargin(source.documentMargin());
    replacement.setDefaultFont(source.defaultFont());
    replacement.setDefaultStyleSheet(source.defaultStyleSheet());
    replacement.setDefaultTextOption(source.defaultTextOption());
    replacement.setTextWidth(source.textWidth());

    QTextCursor out(&replacement);
    QTextCursor source_cursor(&source);
    QTextBlock source_block = source.begin();
    QTextBlockFormat current_block_format = source_block.isValid() ? source_block.blockFormat() : QTextBlockFormat();
    const int max_source_pos = std::max(0, source.characterCount() - 1);
    int source_pos = 0;
    bool at_block_start = true;

    for (const QChar ch : plain) {
        if (ch == QLatin1Char('\r'))
            continue;
        source_cursor.setPosition(std::min(source_pos, max_source_pos));
        if (ch == QLatin1Char('\n')) {
            out.insertBlock(current_block_format);
            if (source_block.isValid()) {
                source_block = source_block.next();
                if (source_block.isValid())
                    current_block_format = source_block.blockFormat();
            }
            at_block_start = true;
            ++source_pos;
            continue;
        }

        QTextCharFormat fmt = source_cursor.charFormat();
        if (at_block_start) {
            out.setBlockFormat(current_block_format);
            at_block_start = false;
        }
        out.insertText(QString(ch), fmt);
        ++source_pos;
    }

    return replacement.toHtml();
}

static double title_manual_screenshot_time(const Title &title)
{
    if (title.playback_mode == 1)
        return std::clamp(title.loop_start, 0.0, title.duration);
    if (title.playback_mode == 2)
        return std::clamp(title.pause_time, 0.0, title.duration);
    return std::clamp(title.duration * 0.5, 0.0, title.duration);
}

static std::string title_manual_screenshot_png_base64(const Title &title)
{
    const QImage screenshot = CacheManager::instance().requestFrame(std::make_shared<Title>(title),
                                                                    title_manual_screenshot_time(title));
    if (screenshot.isNull())
        return {};

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!screenshot.save(&buffer, "PNG"))
        return {};

    const QByteArray encoded = png.toBase64();
    return std::string(encoded.constData(), (size_t)encoded.size());
}


/* ────────────────────────────────────────────────────────────────── */
/*  Dark AE-style palette constants                                   */
/* ────────────────────────────────────────────────────────────────── */
static const QColor C_BG_DARK  { 0x1a1a1a };
static const QColor C_BG_MID   { 0x252525 };
static const QColor C_BG_LIGHT { 0x2e2e2e };
static const QColor C_ACCENT   { 0x0078d4 };

static bool editor_focus_accepts_text(QWidget *widget);

/* OBS safe area margins: Rec. ITU-R BT.1848-1 / EBU R 95. */
static constexpr double OBS_ACTION_SAFE_PERCENT = 0.035;
static constexpr double OBS_GRAPHICS_SAFE_PERCENT = 0.05;

/* Canvas editing controls are measured in view pixels so object scale never
 * changes their apparent size or proportions. */
static constexpr double CANVAS_CONTROL_SIZE_PX = 8.0;
static constexpr double CANVAS_CONTROL_HIT_RADIUS_PX = 8.0;
static constexpr double CANVAS_ROTATE_HIT_RADIUS_PX = 24.0;
static constexpr double CANVAS_ORIGIN_RADIUS_PX = 3.6;
static constexpr double CANVAS_GRADIENT_HANDLE_RADIUS_PX = 5.0;
static constexpr double CANVAS_ROTATION_SNAP_DEGREES = 15.0;

static bool editor_image_path_is_svg(const QString &path)
{
    return path.endsWith(QStringLiteral(".svg"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".svgz"), Qt::CaseInsensitive);
}

static double radians_to_degrees(double radians)
{
    return radians * 180.0 / 3.14159265358979323846;
}

static double degrees_to_radians(double degrees)
{
    return degrees * 3.14159265358979323846 / 180.0;
}

static double normalize_degrees(double degrees)
{
    while (degrees > 180.0) degrees -= 360.0;
    while (degrees <= -180.0) degrees += 360.0;
    return degrees;
}

static QPointF rotate_point_around(const QPointF &point, const QPointF &pivot, double degrees)
{
    double radians = degrees_to_radians(degrees);
    double c = std::cos(radians);
    double ss = std::sin(radians);
    double dx = point.x() - pivot.x();
    double dy = point.y() - pivot.y();
    return QPointF(pivot.x() + dx * c - dy * ss,
                   pivot.y() + dx * ss + dy * c);
}

static QCursor canvas_rotation_cursor()
{
    static const QCursor cursor = []() {
        QPixmap pixmap(24, 24);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(255, 255, 255), 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawArc(QRectF(5.0, 4.0, 14.0, 14.0), 35 * 16, 285 * 16);
        QPolygonF arrow;
        arrow << QPointF(17.0, 4.0) << QPointF(22.0, 5.5) << QPointF(18.8, 10.0);
        painter.setBrush(QColor(255, 255, 255));
        painter.drawPolygon(arrow);
        painter.setPen(QPen(QColor(0, 0, 0, 170), 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(5.0, 4.0, 14.0, 14.0), 35 * 16, 285 * 16);
        return QCursor(pixmap, 12, 12);
    }();
    return cursor;
}

static QSize editor_image_intrinsic_size(const QString &path)
{
    if (editor_image_path_is_svg(path)) {
        QSvgRenderer renderer(path);
        if (!renderer.isValid()) return QSize();
        QSize size = renderer.defaultSize();
        if (!size.isValid() || size.isEmpty())
            size = renderer.viewBox().size();
        return size;
    }

    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize size = reader.size();
    if (size.isValid() && !size.isEmpty())
        return size;

    QImage image = reader.read();
    return image.isNull() ? QSize() : image.size();
}

static QImage editor_load_layer_image(const QString &path, const QSize &fallback_size = QSize())
{
    if (editor_image_path_is_svg(path)) {
        QSvgRenderer renderer(path);
        if (!renderer.isValid()) return QImage();

        QSize size = fallback_size.isValid() && !fallback_size.isEmpty()
            ? fallback_size
            : renderer.defaultSize();
        if (!size.isValid() || size.isEmpty())
            size = renderer.viewBox().size();
        if (!size.isValid() || size.isEmpty())
            size = QSize(256, 256);

        QImage image(size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        renderer.render(&painter);
        return image;
    }

    QImageReader reader(path);
    reader.setAutoTransform(true);
    return reader.read();
}

static QPixmap editor_image_preview_pixmap(const QString &path, const QSize &size)
{
    const QSize target = size.isValid() && !size.isEmpty() ? size : QSize(104, 104);
    QPixmap pixmap(target);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    const QPalette pal = qApp ? qApp->palette() : QPalette();
    const bool dark = pal.color(QPalette::Window).lightness() < 128;
    const QColor checker_a = dark ? QColor(62, 62, 62) : QColor(224, 224, 224);
    const QColor checker_b = dark ? QColor(42, 42, 42) : QColor(188, 188, 188);
    constexpr int checker = 8;
    for (int y = 0; y < target.height(); y += checker) {
        for (int x = 0; x < target.width(); x += checker) {
            painter.fillRect(QRect(x, y, checker, checker),
                             (((x / checker) + (y / checker)) & 1) ? checker_b : checker_a);
        }
    }

    const QString trimmed_path = path.trimmed();
    if (!trimmed_path.isEmpty()) {
        const QSize image_target(std::max(1, target.width() - 8), std::max(1, target.height() - 8));
        QImage image = editor_load_layer_image(trimmed_path, image_target);
        if (!image.isNull()) {
            image = image.scaled(image_target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            const QPoint top_left((target.width() - image.width()) / 2,
                                  (target.height() - image.height()) / 2);
            painter.drawImage(top_left, image);
        }
    }

    painter.setPen(pal.color(QPalette::Mid));
    painter.drawRect(QRect(QPoint(0, 0), target).adjusted(0, 0, -1, -1));
    return pixmap;
}

static void set_image_preview_label(QLabel *label, const QString &path)
{
    if (!label) return;
    const QSize size = label->size().isValid() && !label->size().isEmpty() ? label->size() : QSize(104, 104);
    label->setPixmap(editor_image_preview_pixmap(path, size));
}

static const QColor C_TEXT     { 0xcccccc };
static const QColor C_RULER    { 0x1e1e1e };
static const QColor C_KF_DOT   { 0xffd23f };

static QIcon keyframe_diamond_icon(bool active, bool outlined = false)
{
    if (active)
        return obsgs_icon("keyframe-active.svg", C_KF_DOT);
    if (outlined)
        return obsgs_icon("keyframe-outline.svg", C_KF_DOT);
    return obsgs_icon("keyframe-inactive.svg");
}





static double obs_frame_rate()
{
    struct obs_video_info ovi = {};
    if (obs_get_video_info(&ovi) && ovi.fps_den > 0 && ovi.fps_num > 0)
        return (double)ovi.fps_num / (double)ovi.fps_den;
    return 30.0;
}

static double obs_frame_duration()
{
    return 1.0 / std::max(1.0, obs_frame_rate());
}

static double snap_to_obs_frame(double t)
{
    double fd = obs_frame_duration();
    return std::round(t / fd) * fd;
}

static QString format_timecode(double t)
{
    double fps_d = obs_frame_rate();
    int fps = std::max(1, (int)std::round(fps_d));
    int total_frames = std::max(0, (int)std::round(t * fps_d));
    int frames = total_frames % fps;
    int total_seconds = total_frames / fps;
    int seconds = total_seconds % 60;
    int minutes = (total_seconds / 60) % 60;
    int hours = total_seconds / 3600;
    return QString("%1:%2:%3:%4")
        .arg(hours, 2, 10, QChar('0'))
        .arg(minutes, 2, 10, QChar('0'))
        .arg(seconds, 2, 10, QChar('0'))
        .arg(frames, 2, 10, QChar('0'));
}

static QColor layer_type_color(LayerType type)
{
    switch (type) {
    case LayerType::Text:
        return TitlePreferences::timeline_color(TitlePreferences::TimelineColorRole::TextLayer);
    case LayerType::Clock:
        return TitlePreferences::timeline_color(TitlePreferences::TimelineColorRole::ClockLayer);
    case LayerType::Ticker:
        return TitlePreferences::timeline_color(TitlePreferences::TimelineColorRole::TickerLayer);
    case LayerType::SolidRect:
    case LayerType::Shape:
        return TitlePreferences::timeline_color(TitlePreferences::TimelineColorRole::ObjectLayer);
    case LayerType::Image:
        return TitlePreferences::timeline_color(TitlePreferences::TimelineColorRole::ImageLayer);
    }
    return TitlePreferences::timeline_color(TitlePreferences::TimelineColorRole::ObjectLayer);
}

static QColor layer_color(const Layer &layer, int /*row*/)
{
    return layer_type_color(layer.type);
}

static QString layer_type_short(LayerType type)
{
    switch (type) {
    case LayerType::Text: return "T";
    case LayerType::Clock: return "⏱";
    case LayerType::Ticker: return "↔";
    case LayerType::SolidRect: return "■";
    case LayerType::Image: return "▧";
    case LayerType::Shape: return "◆";
    }
    return "•";
}

static QIcon obs_icon(const char *file_name)
{
    return obsgs_icon(file_name);
}

constexpr double kToolIconPi = 3.14159265358979323846;

static QString shape_display_name(ShapeType shape_type)
{
    switch (shape_type) {
    case ShapeType::RoundedRectangle: return QStringLiteral("Rounded Rectangle");
    case ShapeType::Ellipse: return QStringLiteral("Ellipse");
    case ShapeType::Triangle: return QStringLiteral("Triangle");
    case ShapeType::Star: return QStringLiteral("Star");
    case ShapeType::Polygon: return QStringLiteral("Polygon");
    case ShapeType::Diamond: return QStringLiteral("Diamond");
    case ShapeType::Line: return QStringLiteral("Line");
    case ShapeType::Rectangle:
    default: return QStringLiteral("Rectangle");
    }
}

static QPainterPath tool_shape_path(ShapeType shape_type, const QRectF &rect)
{
    QPainterPath path;
    switch (shape_type) {
    case ShapeType::RoundedRectangle:
        path.addRoundedRect(rect, 3.0, 3.0);
        break;
    case ShapeType::Ellipse:
        path.addEllipse(rect);
        break;
    case ShapeType::Triangle: {
        path.moveTo(rect.center().x(), rect.top());
        path.lineTo(rect.right(), rect.bottom());
        path.lineTo(rect.left(), rect.bottom());
        path.closeSubpath();
        break;
    }
    case ShapeType::Star: {
        const QPointF c = rect.center();
        const double rx = rect.width() / 2.0;
        const double ry = rect.height() / 2.0;
        for (int i = 0; i < 10; ++i) {
            const double r = (i % 2 == 0) ? 1.0 : 0.45;
            const double a = -kToolIconPi / 2.0 + kToolIconPi * i / 5.0;
            const QPointF pt(c.x() + std::cos(a) * rx * r, c.y() + std::sin(a) * ry * r);
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        path.closeSubpath();
        break;
    }
    case ShapeType::Polygon: {
        const QPointF c = rect.center();
        const double rx = rect.width() / 2.0;
        const double ry = rect.height() / 2.0;
        for (int i = 0; i < 6; ++i) {
            const double a = -kToolIconPi / 2.0 + 2.0 * kToolIconPi * i / 6.0;
            const QPointF pt(c.x() + std::cos(a) * rx, c.y() + std::sin(a) * ry);
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        path.closeSubpath();
        break;
    }
    case ShapeType::Diamond:
        path.moveTo(rect.center().x(), rect.top());
        path.lineTo(rect.right(), rect.center().y());
        path.lineTo(rect.center().x(), rect.bottom());
        path.lineTo(rect.left(), rect.center().y());
        path.closeSubpath();
        break;
    case ShapeType::Line:
        path.moveTo(rect.left(), rect.center().y());
        path.lineTo(rect.right(), rect.center().y());
        break;
    case ShapeType::Rectangle:
    default:
        path.addRect(rect);
        break;
    }
    return path;
}

static QPainterPath editor_layer_rounded_rect_path(const Layer &layer, const QRectF &rect);

static QPainterPath editor_scene_mask_shape_path(const Layer &layer, const QRectF &rect)
{
    QPainterPath path;
    const ShapeType shape_type = layer.type == LayerType::Shape ? layer.shape_type : ShapeType::RoundedRectangle;
    switch (shape_type) {
    case ShapeType::Ellipse:
        path.addEllipse(rect);
        break;
    case ShapeType::Triangle:
    case ShapeType::Polygon:
    case ShapeType::Diamond: {
        const int sides = shape_type == ShapeType::Triangle
            ? 3
            : (shape_type == ShapeType::Diamond ? 4 : std::clamp(layer.shape_sides, 3, 64));
        const QPointF center = rect.center();
        const double rx = rect.width() / 2.0;
        const double ry = rect.height() / 2.0;
        for (int i = 0; i < sides; ++i) {
            const double a = -kToolIconPi / 2.0 + 2.0 * kToolIconPi * i / sides;
            const QPointF pt(center.x() + std::cos(a) * rx, center.y() + std::sin(a) * ry);
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        path.closeSubpath();
        break;
    }
    case ShapeType::Star: {
        const QPointF center = rect.center();
        const double rx = rect.width() / 2.0;
        const double ry = rect.height() / 2.0;
        const int points = std::clamp(layer.shape_points, 3, 64);
        const double inner = std::clamp((double)layer.shape_inner_radius, 0.0, 1.0) * 2.0;
        const double outer = std::clamp((double)layer.shape_outer_radius, 0.0, 1.0) * 2.0;
        for (int i = 0; i < points * 2; ++i) {
            const double factor = (i % 2 == 0) ? outer : inner;
            const double a = -kToolIconPi / 2.0 + kToolIconPi * i / points;
            const QPointF pt(center.x() + std::cos(a) * rx * factor,
                             center.y() + std::sin(a) * ry * factor);
            if (i == 0) path.moveTo(pt); else path.lineTo(pt);
        }
        path.closeSubpath();
        break;
    }
    case ShapeType::Line:
        path.moveTo(rect.left(), rect.center().y());
        path.lineTo(rect.right(), rect.center().y());
        break;
    case ShapeType::RoundedRectangle:
        path = editor_layer_rounded_rect_path(layer, rect);
        break;
    case ShapeType::Rectangle:
    default:
        path = editor_layer_rounded_rect_path(layer, rect);
        break;
    }
    return path;
}

static QPainterPath editor_corner_rect_path(const QRectF &rect, double top_left, double top_right,
                                            double bottom_right, double bottom_left, CornerType corner_type)
{
    const double max_radius = std::max(0.0, std::min(rect.width(), rect.height()) / 2.0);
    const double tl = std::clamp(top_left, 0.0, max_radius);
    const double tr = std::clamp(top_right, 0.0, max_radius);
    const double br = std::clamp(bottom_right, 0.0, max_radius);
    const double bl = std::clamp(bottom_left, 0.0, max_radius);
    QPainterPath path;
    if (tl <= 0.0 && tr <= 0.0 && br <= 0.0 && bl <= 0.0) {
        path.addRect(rect);
        return path;
    }
    auto add_corner = [&](const QPointF &corner, const QPointF &from, const QPointF &to,
                          const QPointF &cutout, double radius) {
        if (radius <= 0.0) {
            path.lineTo(corner);
            return;
        }
        switch (corner_type) {
        case CornerType::Straight:
            path.lineTo(to);
            break;
        case CornerType::Cutout:
            path.lineTo(cutout);
            path.lineTo(to);
            break;
        case CornerType::Concave:
            path.cubicTo(cutout, cutout, to);
            break;
        case CornerType::Round:
        default:
            path.quadTo(corner, to);
            break;
        }
    };
    path.moveTo(rect.left() + tl, rect.top());
    path.lineTo(rect.right() - tr, rect.top());
    add_corner(rect.topRight(), QPointF(rect.right() - tr, rect.top()), QPointF(rect.right(), rect.top() + tr),
               QPointF(rect.right() - tr, rect.top() + tr), tr);
    path.lineTo(rect.right(), rect.bottom() - br);
    add_corner(rect.bottomRight(), QPointF(rect.right(), rect.bottom() - br), QPointF(rect.right() - br, rect.bottom()),
               QPointF(rect.right() - br, rect.bottom() - br), br);
    path.lineTo(rect.left() + bl, rect.bottom());
    add_corner(rect.bottomLeft(), QPointF(rect.left() + bl, rect.bottom()), QPointF(rect.left(), rect.bottom() - bl),
               QPointF(rect.left() + bl, rect.bottom() - bl), bl);
    path.lineTo(rect.left(), rect.top() + tl);
    add_corner(rect.topLeft(), QPointF(rect.left(), rect.top() + tl), QPointF(rect.left() + tl, rect.top()),
               QPointF(rect.left() + tl, rect.top() + tl), tl);
    path.closeSubpath();
    return path;
}

static QPainterPath editor_layer_rounded_rect_path(const Layer &layer, const QRectF &rect)
{
    return editor_corner_rect_path(rect, layer.corner_radius_tl, layer.corner_radius_tr,
                                   layer.corner_radius_br, layer.corner_radius_bl, layer.corner_type);
}

static void set_layer_corner_radii(Layer &layer, float top_left, float top_right,
                                   float bottom_right, float bottom_left)
{
    layer.corner_radius_tl = std::max(0.0f, top_left);
    layer.corner_radius_tr = std::max(0.0f, top_right);
    layer.corner_radius_br = std::max(0.0f, bottom_right);
    layer.corner_radius_bl = std::max(0.0f, bottom_left);
    layer.corner_radius = layer.corner_radius_tl;
    if (layer.corner_radius_tl == layer.corner_radius_tr &&
        layer.corner_radius_tl == layer.corner_radius_br &&
        layer.corner_radius_tl == layer.corner_radius_bl)
        layer.shape_roundness = layer.corner_radius_tl;
}

static void set_layer_all_corner_radii(Layer &layer, float radius)
{
    set_layer_corner_radii(layer, radius, radius, radius, radius);
}

static QBrush scene_mask_hatch_brush()
{
    QPixmap pattern(12, 12);
    pattern.fill(Qt::transparent);
    QPainter painter(&pattern);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(255, 0, 200, 180), 2.0));
    painter.drawLine(QPointF(-2.0, 12.0), QPointF(12.0, -2.0));
    painter.drawLine(QPointF(4.0, 14.0), QPointF(14.0, 4.0));
    painter.end();
    return QBrush(pattern);
}

static QIcon shape_tool_icon(ShapeType shape_type)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(220, 220, 220), shape_type == ShapeType::Line ? 2.4 : 1.8));
    painter.setBrush(shape_type == ShapeType::Line ? Qt::NoBrush : QBrush(QColor(120, 120, 120, 60)));
    painter.drawPath(tool_shape_path(shape_type, QRectF(5, 5, 14, 14)));
    return QIcon(pixmap);
}

static QIcon cursor_tool_icon()
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.moveTo(6, 4);
    path.lineTo(6, 19);
    path.lineTo(10, 15);
    path.lineTo(13, 21);
    path.lineTo(16, 19);
    path.lineTo(13, 13);
    path.lineTo(18, 13);
    path.closeSubpath();
    painter.setPen(QPen(QColor(230, 230, 230), 1.4));
    painter.setBrush(QColor(65, 65, 65));
    painter.drawPath(path);
    return QIcon(pixmap);
}

static QIcon gradient_tool_icon()
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QRectF r(4, 5, 16, 14);
    QLinearGradient g(r.topLeft(), r.topRight());
    g.setColorAt(0.0, QColor(255, 80, 80));
    g.setColorAt(0.5, QColor(255, 230, 80));
    g.setColorAt(1.0, QColor(80, 160, 255));
    painter.setBrush(g);
    painter.setPen(QPen(QColor(230, 230, 230), 1.2));
    painter.drawRoundedRect(r, 2, 2);
    painter.setPen(QPen(QColor(230, 230, 230), 1.5));
    painter.drawLine(QPointF(5, 20), QPointF(19, 20));
    painter.setBrush(QColor(230, 230, 230));
    painter.drawEllipse(QPointF(5, 20), 2.0, 2.0);
    painter.drawEllipse(QPointF(19, 20), 2.0, 2.0);
    return QIcon(pixmap);
}

static QString text_tool_display_name(LayerType type)
{
    if (type == LayerType::Clock) return obsgs_tr("OBSTitles.Clock");
    if (type == LayerType::Ticker) return obsgs_tr("OBSTitles.Ticker");
    return obsgs_tr("OBSTitles.Text");
}

static QIcon text_tool_icon(LayerType type)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(230, 230, 230), 1.7));
    painter.setBrush(Qt::NoBrush);

    if (type == LayerType::Clock) {
        painter.drawEllipse(QRectF(5.0, 5.0, 14.0, 14.0));
        painter.drawLine(QPointF(12.0, 12.0), QPointF(12.0, 7.5));
        painter.drawLine(QPointF(12.0, 12.0), QPointF(15.8, 14.2));
    } else if (type == LayerType::Ticker) {
        painter.setPen(QPen(QColor(230, 230, 230), 1.6));
        painter.drawText(QRectF(4.0, 2.0, 16.0, 12.0), Qt::AlignCenter, QStringLiteral("T"));
        painter.drawLine(QPointF(4.0, 15.0), QPointF(20.0, 15.0));
        painter.drawLine(QPointF(7.0, 19.0), QPointF(20.0, 19.0));
        painter.drawLine(QPointF(4.0, 15.0), QPointF(7.0, 12.0));
        painter.drawLine(QPointF(4.0, 15.0), QPointF(7.0, 18.0));
    } else {
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(18);
        painter.setFont(font);
        painter.drawText(QRectF(3.0, 2.0, 18.0, 20.0), Qt::AlignCenter, QStringLiteral("T"));
    }
    return QIcon(pixmap);
}

class HoldMenuToolButton : public QToolButton {
public:
    explicit HoldMenuToolButton(QWidget *parent = nullptr) : QToolButton(parent)
    {
        hold_timer_.setSingleShot(true);
        hold_timer_.setInterval(500);
        QObject::connect(&hold_timer_, &QTimer::timeout, this, [this]() {
            if (!menu() || !isDown() || !underMouse()) return;
            menu_opened_by_hold_ = true;
            menu()->popup(mapToGlobal(QPoint(width() + 2, 0)));
        });
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        menu_opened_by_hold_ = false;
        if (event->button() == Qt::LeftButton && menu())
            hold_timer_.start();
        QToolButton::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        hold_timer_.stop();
        if (menu_opened_by_hold_) {
            setDown(false);
            event->accept();
            return;
        }
        QToolButton::mouseReleaseEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        if (!isDown()) hold_timer_.stop();
        QToolButton::leaveEvent(event);
    }

private:
    QTimer hold_timer_;
    bool menu_opened_by_hold_ = false;
};

static std::string editor_text_std(const char *key)
{
    return obsgs_tr(key).toStdString();
}

static QLocale locale_for_text_transform(const QString &text)
{
    QLocale locale;
    for (const QChar ch : text) {
        uint u = ch.unicode();
        if (u >= 0x0370 && u <= 0x03FF)
            return QLocale(QLocale::Greek, QLocale::Greece);
        if (QStringLiteral("ıİşŞğĞçÇ").contains(ch))
            return QLocale(QLocale::Turkish, QLocale::Turkey);
        if (ch == QChar(0x00DF))
            return QLocale(QLocale::German, QLocale::Germany);
    }
    return locale;
}


static QString php_date_format(const QString &format, const QDateTime &date_time)
{
    QString out;
    const QDate date = date_time.date();
    const QTime time = date_time.time();
    for (int i = 0; i < format.size(); ++i) {
        const QChar token = format.at(i);
        if (token == QLatin1Char('\\') && i + 1 < format.size()) {
            out.append(format.at(++i));
            continue;
        }
        switch (token.unicode()) {
        case 'd': out += QString("%1").arg(date.day(), 2, 10, QChar('0')); break;
        case 'D': out += date_time.toString("ddd"); break;
        case 'j': out += QString::number(date.day()); break;
        case 'l': out += date_time.toString("dddd"); break;
        case 'F': out += date_time.toString("MMMM"); break;
        case 'm': out += QString("%1").arg(date.month(), 2, 10, QChar('0')); break;
        case 'M': out += date_time.toString("MMM"); break;
        case 'n': out += QString::number(date.month()); break;
        case 'Y': out += QString::number(date.year()); break;
        case 'y': out += QString("%1").arg(date.year() % 100, 2, 10, QChar('0')); break;
        case 'a': out += (time.hour() < 12 ? "am" : "pm"); break;
        case 'A': out += (time.hour() < 12 ? "AM" : "PM"); break;
        case 'g': { int h = time.hour() % 12; out += QString::number(h == 0 ? 12 : h); break; }
        case 'G': out += QString::number(time.hour()); break;
        case 'h': { int h = time.hour() % 12; out += QString("%1").arg(h == 0 ? 12 : h, 2, 10, QChar('0')); break; }
        case 'H': out += QString("%1").arg(time.hour(), 2, 10, QChar('0')); break;
        case 'i': out += QString("%1").arg(time.minute(), 2, 10, QChar('0')); break;
        case 's': out += QString("%1").arg(time.second(), 2, 10, QChar('0')); break;
        case 'U': out += QString::number(date_time.toSecsSinceEpoch()); break;
        default: out.append(token); break;
        }
    }
    return out;
}

static QString clock_text_for_layer(const Layer &layer)
{
    QString format = QString::fromStdString(layer.clock_format);
    if (format.isEmpty()) format = QStringLiteral("H:i:s");
    return php_date_format(format, QDateTime::currentDateTime());
}

static QString display_text_for_style(const Layer &layer)
{
    QString text = layer.type == LayerType::Clock
        ? clock_text_for_layer(layer)
        : QString::fromStdString(layer.text_content);
    if (layer.text_style == 1)
        return locale_for_text_transform(text).toUpper(text);
    return text;
}

static void apply_text_style_to_font(QFont &font, const Layer &layer)
{
    if (layer.text_style == 2)
        font.setCapitalization(QFont::SmallCaps);
    if (layer.text_style == 3 || layer.text_style == 4)
        font.setPixelSize(std::max(1, (int)std::round(font.pixelSize() * 0.65)));
}

static double eval_text_font_size(const Layer &layer, double t);
static double eval_char_tracking(const Layer &layer, double t);
static double eval_char_scale_x(const Layer &layer, double t);
static double eval_char_scale_y(const Layer &layer, double t);
static double eval_baseline_shift(const Layer &layer, double t);
static double eval_paragraph_space_before(const Layer &layer, double t);
static double eval_paragraph_space_after(const Layer &layer, double t);

static QFont font_for_layer(const Layer &layer, double t = 0.0)
{
    const QString family = QString::fromStdString(layer.font_family);
    const QString style = QString::fromStdString(layer.font_style);
    QFontDatabase fdb;
    QFont font = !style.isEmpty()
        ? fdb.font(family, style, (int)std::round(eval_text_font_size(layer, t)))
        : QFont(family);
    font.setFamily(family);
    font.setPixelSize((int)std::round(eval_text_font_size(layer, t)));
    if (!style.isEmpty())
        font.setStyleName(style);
    font.setBold(layer.font_bold);
    font.setItalic(layer.font_italic);
    font.setKerning(layer.font_kerning);
    font.setLetterSpacing(QFont::AbsoluteSpacing, eval_char_tracking(layer, t));
    font.setStretch(std::clamp((int)std::round(eval_char_scale_x(layer, t) * 100.0), 1, 4000));
    apply_text_style_to_font(font, layer);
    return font;
}

static QPainterPath apply_vertical_character_scale(const QPainterPath &path, const QRectF &rect,
                                                   Qt::Alignment alignment, const Layer &layer, double t = 0.0)
{
    double scale_y = std::clamp(eval_char_scale_y(layer, t), 0.1, 5.0);
    if (std::abs(scale_y - 1.0) < 0.0001)
        return path;

    QRectF bounds = path.boundingRect();
    double anchor_y = bounds.top();
    if (alignment & Qt::AlignVCenter)
        anchor_y = bounds.center().y();
    else if (alignment & Qt::AlignBottom)
        anchor_y = bounds.bottom();
    else if (!bounds.isEmpty())
        anchor_y = rect.top();

    QTransform xf;
    xf.translate(0.0, anchor_y);
    xf.scale(1.0, scale_y);
    xf.translate(0.0, -anchor_y);
    return xf.map(path);
}

static QRectF text_rect_for_style(const QRectF &rect, const Layer &layer)
{
    if (layer.text_style == 3)
        return rect.adjusted(0.0, 0.0, 0.0, -rect.height() * 0.28);
    if (layer.text_style == 4)
        return rect.adjusted(0.0, rect.height() * 0.28, 0.0, 0.0);
    return rect;
}

static QString overflow_layout_text(const QString &text, const Layer &layer)
{
    if (layer.text_overflow_mode == 2) {
        QString single = text;
        single.replace('\r', ' ');
        single.replace('\n', ' ');
        return single;
    }
    return text;
}

static double eval_text_font_size(const Layer &layer, double t)
{
    return std::clamp(layer.font_size_prop.is_animated()
                          ? layer.font_size_prop.evaluate(t)
                          : (double)layer.font_size,
                      1.0, 512.0);
}

static double eval_char_tracking(const Layer &layer, double t)
{
    return std::clamp(layer.char_tracking_prop.is_animated()
                          ? layer.char_tracking_prop.evaluate(t)
                          : (double)layer.char_tracking,
                      -1000.0, 1000.0);
}

static double eval_char_scale_x(const Layer &layer, double t)
{
    return std::clamp(layer.char_scale_x_prop.is_animated()
                          ? layer.char_scale_x_prop.evaluate(t)
                          : (double)layer.char_scale_x,
                      0.01, 100.0);
}

static double eval_char_scale_y(const Layer &layer, double t)
{
    return std::clamp(layer.char_scale_y_prop.is_animated()
                          ? layer.char_scale_y_prop.evaluate(t)
                          : (double)layer.char_scale_y,
                      0.01, 100.0);
}

static double eval_baseline_shift(const Layer &layer, double t)
{
    return std::clamp(layer.baseline_shift_prop.is_animated()
                          ? layer.baseline_shift_prop.evaluate(t)
                          : (double)layer.baseline_shift,
                      -1000.0, 1000.0);
}

static double eval_paragraph_space_before(const Layer &layer, double t)
{
    return std::clamp(layer.paragraph_space_before_prop.is_animated()
                          ? layer.paragraph_space_before_prop.evaluate(t)
                          : (double)layer.paragraph_space_before,
                      -10000.0, 10000.0);
}

static double eval_paragraph_space_after(const Layer &layer, double t)
{
    return std::clamp(layer.paragraph_space_after_prop.is_animated()
                          ? layer.paragraph_space_after_prop.evaluate(t)
                          : (double)layer.paragraph_space_after,
                      -10000.0, 10000.0);
}

static double eval_paragraph_indent_left(const Layer &layer, double t)
{
    return std::clamp(layer.paragraph_indent_left_prop.is_animated()
                          ? layer.paragraph_indent_left_prop.evaluate(t)
                          : (double)layer.paragraph_indent_left,
                      -10000.0, 10000.0);
}

static double eval_paragraph_indent_right(const Layer &layer, double t)
{
    return std::clamp(layer.paragraph_indent_right_prop.is_animated()
                          ? layer.paragraph_indent_right_prop.evaluate(t)
                          : (double)layer.paragraph_indent_right,
                      -10000.0, 10000.0);
}

static double eval_paragraph_indent_first_line(const Layer &layer, double t)
{
    return std::clamp(layer.paragraph_indent_first_line_prop.is_animated()
                          ? layer.paragraph_indent_first_line_prop.evaluate(t)
                          : (double)layer.paragraph_indent_first_line,
                      -10000.0, 10000.0);
}

static double horizontal_fit_scale(const QFont &font, const QRectF &rect,
                                   const QString &text, const Layer &layer, double)
{
    if (layer.text_overflow_mode != 2) return 1.0;
    QFontMetricsF metrics(font);
    const double text_width = static_cast<double>(metrics.horizontalAdvance(overflow_layout_text(text, layer)));
    double natural_width = std::max(1.0, text_width);
    if (natural_width <= rect.width()) return 1.0;
    return std::clamp(rect.width() / natural_width,
                      std::clamp((double)layer.text_fit_min_scale, 0.05, 1.0),
                      1.0);
}

static QPainterPath text_overflow_path(const QFont &font, const QRectF &rect,
                                       Qt::Alignment alignment, const QString &text,
                                       const Layer &layer, double t, double *fit_scale = nullptr)
{
    QPainterPath path;
    QFontMetricsF metrics(font);
    const double indent_left = eval_paragraph_indent_left(layer, t);
    const double indent_right = eval_paragraph_indent_right(layer, t);
    const double first_indent = eval_paragraph_indent_first_line(layer, t);
    const double space_before = eval_paragraph_space_before(layer, t);
    const double space_after = eval_paragraph_space_after(layer, t);
    const double paragraph_left = rect.left() + indent_left;
    const double paragraph_right = rect.right() - indent_right;
    const double paragraph_width = std::max(1.0, paragraph_right - paragraph_left);
    const int align_h = std::clamp(layer.align_h, 0, 6);

    auto aligned_x = [&](double line_left, double line_width, double text_width, int mode) {
        if (mode == 1 || mode == 4)
            return line_left + (line_width - text_width) / 2.0;
        if (mode == 2 || mode == 5)
            return line_left + line_width - text_width;
        return line_left;
    };

    if (layer.text_overflow_mode == 2) {
        QString single = overflow_layout_text(text, layer);
        QRectF bounds = metrics.boundingRect(single);
        QRectF fit_rect(paragraph_left + first_indent, rect.top(),
                        std::max(1.0, paragraph_width - first_indent), rect.height());
        double scale = horizontal_fit_scale(font, fit_rect, text, layer, t);
        if (fit_scale) *fit_scale = scale;
        double visual_width = bounds.width() * scale;
        double x = aligned_x(fit_rect.left(), fit_rect.width(), visual_width, align_h);
        double y = rect.top() - bounds.top();
        if (alignment & Qt::AlignVCenter) y = rect.top() + (rect.height() - bounds.height()) / 2.0 - bounds.top();
        else if (alignment & Qt::AlignBottom) y = rect.bottom() - bounds.height() - bounds.top();
        path.addText(QPointF(0, y), font, single);
        QTransform xf;
        xf.translate(x, 0.0);
        xf.scale(scale, 1.0);
        return xf.map(path);
    }
    if (fit_scale) *fit_scale = 1.0;

    struct Line {
        QString text;
        double width = 0.0;
        double ascent = 0.0;
        double height = 0.0;
        int paragraph = 0;
        bool first_in_paragraph = false;
        bool last_in_paragraph = false;
    };
    std::vector<Line> lines;
    const QStringList paragraphs = text.split('\n');
    QTextOption option;
    option.setWrapMode(layer.text_overflow_mode == 0
                           ? (layer.paragraph_hyphenate ? QTextOption::WrapAnywhere : QTextOption::WrapAtWordBoundaryOrAnywhere)
                           : QTextOption::NoWrap);
    for (int paragraph_index = 0; paragraph_index < paragraphs.size(); ++paragraph_index) {
        const QString &paragraph = paragraphs[paragraph_index];
        const size_t first_line_index = lines.size();
        if (paragraph.isEmpty()) {
            lines.push_back({QString(), 0.0, metrics.ascent(), metrics.lineSpacing(), paragraph_index, true, true});
            continue;
        }
        QTextLayout layout(paragraph, font);
        layout.setTextOption(option);
        layout.beginLayout();
        int line_index = 0;
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            const double line_indent = line_index == 0 ? first_indent : 0.0;
            const double line_width = std::max(1.0, paragraph_width - line_indent);
            line.setLineWidth(layer.text_overflow_mode == 0 ? line_width : 1000000.0);
            int start = line.textStart();
            int len = line.textLength();
            lines.push_back({paragraph.mid(start, len), line.naturalTextWidth(), line.ascent(), line.height(),
                             paragraph_index, line_index == 0, false});
            ++line_index;
            if (layer.text_overflow_mode != 0) break;
        }
        layout.endLayout();
        if (lines.size() > first_line_index)
            lines.back().last_in_paragraph = true;
    }
    double total_height = 0.0;
    const double leading = std::clamp((double)layer.text_leading, -200.0, 500.0);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].first_in_paragraph) total_height += space_before;
        total_height += lines[i].height;
        if (lines[i].last_in_paragraph)
            total_height += space_after;
        else if (i + 1 < lines.size())
            total_height += leading;
    }
    double y = rect.top();
    if (alignment & Qt::AlignVCenter) y = rect.top() + (rect.height() - total_height) / 2.0;
    else if (alignment & Qt::AlignBottom) y = rect.bottom() - total_height;

    auto add_justified_line = [&](const Line &line, double line_left, double line_width, double baseline_y) {
        QStringList words = line.text.simplified().split(' ', Qt::SkipEmptyParts);
        if (words.size() < 2 || line.width >= line_width) {
            path.addText(QPointF(line_left, baseline_y), font, line.text);
            return;
        }
        double words_width = 0.0;
        for (const QString &word : words)
            words_width += metrics.horizontalAdvance(word);
        const double extra_space = std::max(0.0, (line_width - words_width) / (words.size() - 1));
        double word_x = line_left;
        for (int i = 0; i < words.size(); ++i) {
            path.addText(QPointF(word_x, baseline_y), font, words[i]);
            word_x += metrics.horizontalAdvance(words[i]) + extra_space;
        }
    };

    for (const auto &line : lines) {
        if (line.first_in_paragraph) y += space_before;
        const double line_indent = line.first_in_paragraph ? first_indent : 0.0;
        const double line_left = paragraph_left + line_indent;
        const double line_width = std::max(1.0, paragraph_width - line_indent);
        const bool justify_line = align_h == 6 || (align_h >= 3 && align_h <= 5 && !line.last_in_paragraph);
        if (justify_line) {
            add_justified_line(line, line_left, line_width, y + line.ascent);
        } else {
            double x = aligned_x(line_left, line_width, line.width, align_h);
            path.addText(QPointF(x, y + line.ascent), font, line.text);
        }
        y += line.height;
        if (line.last_in_paragraph)
            y += space_after;
        else
            y += leading;
    }
    return path;
}


static double ticker_time_seconds()
{
    return QDateTime::currentMSecsSinceEpoch() / 1000.0;
}

static QStringList ticker_lines(const QString &text)
{
    QString normalized = text;
    normalized.replace('\r', '\n');
    QStringList raw_lines = normalized.split('\n');
    QStringList lines;
    for (const QString &line : raw_lines) {
        if (!line.trimmed().isEmpty())
            lines << line;
    }
    if (lines.isEmpty()) lines << QString();
    return lines;
}

static QPainterPath ticker_text_path(const QFont &font, const QRectF &rect,
                                     Qt::Alignment alignment, const QString &text,
                                     const Layer &layer)
{
    QPainterPath path;
    QFontMetricsF metrics(font);
    const double speed = std::max(1.0, layer.ticker_speed);
    const double now = ticker_time_seconds();

    if (layer.ticker_style == 0) {
        QString single = text;
        single.replace('\r', ' ');
        single.replace('\n', QStringLiteral("     •     "));
        QRectF bounds = metrics.boundingRect(single);
        const double text_w = std::max(1.0, bounds.width());
        const double travel = rect.width() + text_w;
        const double progress = std::fmod(now * speed, travel);
        const double x = layer.ticker_direction == 0
            ? rect.left() - text_w + progress
            : rect.right() - progress;
        double y = rect.top() - bounds.top();
        if (alignment & Qt::AlignVCenter) y = rect.top() + (rect.height() - bounds.height()) / 2.0 - bounds.top();
        else if (alignment & Qt::AlignBottom) y = rect.bottom() - bounds.height() - bounds.top();
        path.addText(QPointF(x, y), font, single);
        return path;
    }

    const QStringList lines = ticker_lines(text);
    const int line_count = std::max(1, static_cast<int>(lines.size()));
    const double line_h = std::max(1.0, metrics.lineSpacing() + std::clamp((double)layer.text_leading, -200.0, 500.0));
    if (layer.ticker_style == 1) {
        const double hold = std::max(0.1, layer.ticker_line_hold);
        int idx = (int)std::floor(now / hold) % line_count;
        if (layer.ticker_direction == 0) idx = line_count - 1 - idx;
        QString line = lines.at(idx);
        double line_w = metrics.horizontalAdvance(line);
        double x = rect.left();
        if (alignment & Qt::AlignHCenter) x = rect.left() + (rect.width() - line_w) / 2.0;
        else if (alignment & Qt::AlignRight) x = rect.right() - line_w;
        QRectF bounds = metrics.boundingRect(line);
        double y = rect.top() + (rect.height() - bounds.height()) / 2.0 - bounds.top();
        path.addText(QPointF(x, y), font, line);
        return path;
    }

    const double content_h = line_count * line_h;
    const double travel = rect.height() + content_h;
    const double progress = std::fmod(now * speed, travel);
    double y = layer.ticker_direction == 0
        ? rect.top() - content_h + progress
        : rect.bottom() - progress;
    for (const QString &line : lines) {
        double line_w = metrics.horizontalAdvance(line);
        double x = rect.left();
        if (alignment & Qt::AlignHCenter) x = rect.left() + (rect.width() - line_w) / 2.0;
        else if (alignment & Qt::AlignRight) x = rect.right() - line_w;
        path.addText(QPointF(x, y + metrics.ascent()), font, line);
        y += line_h;
    }
    return path;
}

static QPainterPath editor_scene_mask_layer_path(const Layer &layer, const QRectF &local_rect, double local_time)
{
    if (layer.type == LayerType::Text || layer.type == LayerType::Clock || layer.type == LayerType::Ticker) {
        QFont font = font_for_layer(layer, local_time);
        QRectF text_rect = text_rect_for_style(local_rect, layer);
        Qt::Alignment align = Qt::AlignLeft;
        if (layer.align_h == 1 || layer.align_h == 4) align = Qt::AlignHCenter;
        else if (layer.align_h == 2 || layer.align_h == 5) align = Qt::AlignRight;
        else if (layer.align_h >= 3) align = Qt::AlignJustify;
        if (layer.align_v == 1) align |= Qt::AlignVCenter;
        else if (layer.align_v == 2) align |= Qt::AlignBottom;
        else align |= Qt::AlignTop;

        QString text = layer.type == LayerType::Ticker
            ? QString::fromStdString(layer.text_content)
            : display_text_for_style(layer);
        if (layer.type == LayerType::Text && !layer.rich_text.plain_text.empty())
            text = QString::fromStdString(layer.rich_text.plain_text);

        QPainterPath text_path = layer.type == LayerType::Ticker
            ? ticker_text_path(font, text_rect, align, text, layer)
            : text_overflow_path(font, text_rect, align, text, layer, local_time);
        text_path = apply_vertical_character_scale(text_path, text_rect, align, layer, local_time);
        if (std::abs(eval_baseline_shift(layer, local_time)) > 0.0001)
            text_path.translate(0.0, -eval_baseline_shift(layer, local_time));
        if (!text_path.isEmpty())
            return text_path;
    }

    return editor_scene_mask_shape_path(layer, local_rect);
}

static bool title_has_dynamic_text_layer(const std::shared_ptr<Title> &title)
{
    if (!title) return false;
    return std::any_of(title->layers.begin(), title->layers.end(), [](const std::shared_ptr<Layer> &layer) {
        return layer && (layer->type == LayerType::Clock || layer->type == LayerType::Ticker);
    });
}

static QColor color_from_argb(uint32_t argb)
{
    return QColor((argb >> 16) & 0xFF,
                  (argb >> 8) & 0xFF,
                  argb & 0xFF,
                  (argb >> 24) & 0xFF);
}

static uint32_t argb_from_color(const QColor &color)
{
    return ((uint32_t)color.alpha() << 24) |
           ((uint32_t)color.red() << 16) |
           ((uint32_t)color.green() << 8) |
           (uint32_t)color.blue();
}

static QPointF anchor_point_from_index(int index)
{
    static const QPointF anchors[] = {
        {0.0, 0.0}, {0.5, 0.0}, {1.0, 0.0},
        {0.0, 0.5}, {0.5, 0.5}, {1.0, 0.5},
        {0.0, 1.0}, {0.5, 1.0}, {1.0, 1.0},
    };
    if (index < 0 || index >= 9) return anchors[4];
    return anchors[index];
}

static int anchor_index_from_layer(const Layer &layer)
{
    int x = layer.origin_x < 0.25f ? 0 : (layer.origin_x > 0.75f ? 2 : 1);
    int y = layer.origin_y < 0.25f ? 0 : (layer.origin_y > 0.75f ? 2 : 1);
    return y * 3 + x;
}

static QPointF rotated_scaled_delta(double dx, double dy, double rot_deg, double sx, double sy)
{
    double rot = rot_deg * 3.14159265358979323846 / 180.0;
    double x = dx * sx;
    double y = dy * sy;
    double c = std::cos(rot);
    double s = std::sin(rot);
    return QPointF(x * c - y * s, x * s + y * c);
}


static bool is_text_box_auto_size_layer(const Layer &layer)
{
    return layer.type == LayerType::Text || layer.type == LayerType::Clock;
}

static bool is_canvas_text_layer(const Layer &layer)
{
    return layer.type == LayerType::Text || layer.type == LayerType::Clock || layer.type == LayerType::Ticker;
}

static double natural_text_width(const Layer &layer)
{
    if (!is_text_box_auto_size_layer(layer)) return 1.0;
    QFontMetricsF metrics(font_for_layer(layer));
    QString text = display_text_for_style(layer);
    if (layer.text_overflow_mode == 2)
        text = overflow_layout_text(text, layer);

    double width = 1.0;
    for (const QString &line : text.split('\n'))
        width = std::max(width, static_cast<double>(metrics.horizontalAdvance(line)));
    return std::ceil(width);
}

static double natural_text_height(const Layer &layer, double width)
{
    if (!is_text_box_auto_size_layer(layer)) return 1.0;
    QFont font = font_for_layer(layer);
    QFontMetricsF metrics(font);
    QString text = display_text_for_style(layer);
    if (layer.text_overflow_mode == 2)
        text = overflow_layout_text(text, layer);

    QTextOption option;
    option.setWrapMode(layer.text_overflow_mode == 0
                           ? QTextOption::WrapAtWordBoundaryOrAnywhere
                           : QTextOption::NoWrap);

    double total_height = 0.0;
    const double leading = std::clamp((double)layer.text_leading, -200.0, 500.0);
    bool first_line = true;
    for (const QString &paragraph : text.split('\n')) {
        if (paragraph.isEmpty()) {
            if (!first_line) total_height += leading;
            total_height += metrics.lineSpacing();
            first_line = false;
            continue;
        }
        QTextLayout layout(paragraph, font);
        layout.setTextOption(option);
        layout.beginLayout();
        while (true) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(layer.text_overflow_mode == 0 ? std::max(1.0, width) : 1000000.0);
            if (!first_line) total_height += leading;
            total_height += line.height();
            first_line = false;
            if (layer.text_overflow_mode != 0) break;
        }
        layout.endLayout();
    }
    return std::ceil(std::max(1.0, total_height));
}

static double eval_box_width(const Layer &layer, double t)
{
    double width = layer.box_width.is_animated()
        ? layer.box_width.evaluate(t)
        : static_cast<double>(layer.rect_width);
    if (layer.text_box_width_to_text && is_text_box_auto_size_layer(layer))
        width = std::min(natural_text_width(layer), std::max(1.0, (double)layer.max_text_box_width));
    return std::max(0.0, width);
}

static double eval_box_height(const Layer &layer, double t)
{
    double height = layer.box_height.is_animated()
        ? layer.box_height.evaluate(t)
        : static_cast<double>(layer.rect_height);
    if (layer.text_box_height_to_text && is_text_box_auto_size_layer(layer)) {
        const double width = eval_box_width(layer, t);
        height = std::min(natural_text_height(layer, width), std::max(1.0, (double)layer.max_text_box_height));
    }
    return std::max(0.0, height);
}

static int shadow_pass_count(double blur)
{
    const int passes = static_cast<int>(std::ceil(blur / 3.0));
    return passes < 1 ? 1 : passes;
}

static double eval_origin_x(const Layer &layer, double t)
{
    return std::clamp(layer.origin_x_prop.is_animated()
                          ? layer.origin_x_prop.evaluate(t)
                          : (double)layer.origin_x,
                      0.0, 1.0);
}

static double eval_origin_y(const Layer &layer, double t)
{
    return std::clamp(layer.origin_y_prop.is_animated()
                          ? layer.origin_y_prop.evaluate(t)
                          : (double)layer.origin_y,
                      0.0, 1.0);
}

static int eval_channel(const AnimatedProperty &prop, double fallback, double t)
{
    return (int)std::clamp(std::round(prop.is_animated() ? prop.evaluate(t) : fallback),
                           0.0, 255.0);
}

static uint32_t eval_text_color(const Layer &layer, double t)
{
    return ((uint32_t)eval_channel(layer.text_color_a, (layer.text_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(layer.text_color_r, (layer.text_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(layer.text_color_g, (layer.text_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(layer.text_color_b, layer.text_color & 0xFF, t);
}

static uint32_t eval_fill_color(const Layer &layer, double t)
{
    return ((uint32_t)eval_channel(layer.fill_color_a, (layer.fill_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(layer.fill_color_r, (layer.fill_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(layer.fill_color_g, (layer.fill_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(layer.fill_color_b, layer.fill_color & 0xFF, t);
}

static QColor gradient_color_with_opacity(uint32_t argb, double gradient_opacity, double stop_opacity)
{
    QColor color = color_from_argb(argb);
    color.setAlphaF(std::clamp((double)color.alphaF() * gradient_opacity * stop_opacity, 0.0, 1.0));
    return color;
}


static void apply_extra_gradient_stops(QGradient &gradient, const std::vector<GradientStop> &stops, double gradient_opacity)
{
    for (const auto &stop : stops) {
        const double pos = std::clamp((double)stop.position, 0.0, 1.0);
        gradient.setColorAt(pos, gradient_color_with_opacity(stop.color, gradient_opacity, stop.opacity));
    }
}

static void apply_base_gradient_stops(QGradient &gradient,
                                      double start_pos, uint32_t start_color, double start_opacity,
                                      double end_pos, uint32_t end_color, double end_opacity,
                                      const std::vector<GradientStop> &extra_stops,
                                      double opacity)
{
    gradient.setColorAt(start_pos, gradient_color_with_opacity(start_color, opacity, start_opacity));
    gradient.setColorAt(end_pos, gradient_color_with_opacity(end_color, opacity, end_opacity));
    apply_extra_gradient_stops(gradient, extra_stops, opacity);
}

static QBrush make_gradient_brush(int gradient_type,
                                  const QRectF &box,
                                  double opacity,
                                  double center_x, double center_y,
                                  double focal_x, double focal_y,
                                  double scale, double angle_degrees,
                                  double start_pos, uint32_t start_color, double start_opacity,
                                  double end_pos, uint32_t end_color, double end_opacity,
                                  const std::vector<GradientStop> &extra_stops)
{
    const int type = std::clamp(gradient_type, 0, 4);
    const double cx = box.left() + center_x * box.width();
    const double cy = box.top() + center_y * box.height();
    const double safe_scale = std::clamp(scale, 0.01, 100.0);
    const double length = std::max(1.0, std::hypot(box.width(), box.height()) * 0.5 * safe_scale);
    const double angle = angle_degrees * std::acos(-1.0) / 180.0;
    const double dx = std::cos(angle) * length;
    const double dy = std::sin(angle) * length;

    if (type == 1 || type == 4) {
        const double radius = std::max(box.width(), box.height()) * 0.5 * safe_scale;
        // Qt has no native diamond gradient. Use a centered radial brush as a safe
        // rendering fallback while preserving the distinct Diamond type in data/UI.
        QRadialGradient gradient(QPointF(cx, cy), std::max(1.0, radius),
                                 type == 1 ? QPointF(box.left() + focal_x * box.width(),
                                                     box.top() + focal_y * box.height())
                                           : QPointF(cx, cy));
        apply_base_gradient_stops(gradient, start_pos, start_color, start_opacity,
                                  end_pos, end_color, end_opacity, extra_stops, opacity);
        return QBrush(gradient);
    }

    if (type == 2) {
        QConicalGradient gradient(QPointF(cx, cy), -angle_degrees);
        apply_base_gradient_stops(gradient, start_pos, start_color, start_opacity,
                                  end_pos, end_color, end_opacity, extra_stops, opacity);
        return QBrush(gradient);
    }

    QLinearGradient gradient(QPointF(cx - dx, cy - dy), QPointF(cx + dx, cy + dy));
    if (type == 3)
        gradient.setSpread(QGradient::ReflectSpread);
    apply_base_gradient_stops(gradient, start_pos, start_color, start_opacity,
                              end_pos, end_color, end_opacity, extra_stops, opacity);
    return QBrush(gradient);
}

static QBrush gradient_fill_brush(const Layer &layer, const QRectF &box, double layer_opacity = 1.0)
{
    return make_gradient_brush(layer.gradient_type, box,
                               std::clamp((double)layer.gradient_opacity * layer_opacity, 0.0, 1.0),
                               layer.gradient_center_x, layer.gradient_center_y,
                               layer.gradient_focal_x, layer.gradient_focal_y,
                               std::clamp((double)layer.gradient_scale, 0.01, 100.0),
                               layer.gradient_angle,
                               std::clamp((double)layer.gradient_start_pos, 0.0, 1.0),
                               layer.gradient_start_color, layer.gradient_start_opacity,
                               std::clamp((double)layer.gradient_end_pos, 0.0, 1.0),
                               layer.gradient_end_color, layer.gradient_end_opacity,
                               layer.gradient_stops);
}

static const LayerEffect *find_layer_effect(const Layer &layer, LayerEffectType type);

static QBrush background_gradient_fill_brush(const Layer &layer, const QRectF &box, double layer_opacity = 1.0)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    if (!effect) return QBrush(Qt::NoBrush);
    return make_gradient_brush(effect->effect_gradient_type, box,
                               std::clamp((double)effect->effect_gradient_opacity * layer_opacity, 0.0, 1.0),
                               effect->effect_gradient_center_x, effect->effect_gradient_center_y,
                               effect->effect_gradient_focal_x, effect->effect_gradient_focal_y,
                               std::clamp((double)effect->effect_gradient_scale, 0.01, 100.0),
                               effect->effect_gradient_angle,
                               std::clamp((double)effect->effect_gradient_start_pos, 0.0, 1.0),
                               effect->effect_gradient_start_color, effect->effect_gradient_start_opacity,
                               std::clamp((double)effect->effect_gradient_end_pos, 0.0, 1.0),
                               effect->effect_gradient_end_color, effect->effect_gradient_end_opacity,
                               {});
}

static QString effect_type_name(LayerEffectType type)
{
    switch (type) {
    case LayerEffectType::BackgroundColor: return obsgs_tr("OBSTitles.BackgroundColor");
    case LayerEffectType::Outline: return obsgs_tr("OBSTitles.Outline");
    case LayerEffectType::DropShadow: return obsgs_tr("OBSTitles.DropShadow");
    case LayerEffectType::LongShadow: return obsgs_tr("OBSTitles.LongShadow");
    case LayerEffectType::BrightnessContrast: return obsgs_tr("OBSTitles.BrightnessContrast");
    case LayerEffectType::Saturation: return obsgs_tr("OBSTitles.Saturation");
    case LayerEffectType::ColorOverlay: return obsgs_tr("OBSTitles.ColorOverlay");
    case LayerEffectType::Glow: return obsgs_tr("OBSTitles.Glow");
    case LayerEffectType::InnerGlow: return obsgs_tr("OBSTitles.InnerGlow");
    case LayerEffectType::InnerShadow: return obsgs_tr("OBSTitles.InnerShadow");
    case LayerEffectType::Blur: return obsgs_tr("OBSTitles.Blur");
    case LayerEffectType::MotionBlur: return obsgs_tr("OBSTitles.MotionBlur");
    }
    return QStringLiteral("Effect");
}

static void add_shadow_blur_items(QComboBox *combo)
{
    if (!combo) return;
    combo->addItem(obsgs_tr("OBSTitles.BoxBlur"), (int)ShadowBlurType::Box);
    combo->addItem(obsgs_tr("OBSTitles.GaussianBlur"), (int)ShadowBlurType::Gaussian);
    combo->addItem(obsgs_tr("OBSTitles.StackFastBlur"), (int)ShadowBlurType::StackFast);
    combo->addItem(obsgs_tr("OBSTitles.TriangularBlur"), (int)ShadowBlurType::Triangular);
    combo->addItem(obsgs_tr("OBSTitles.DualKawaseBlur"), (int)ShadowBlurType::DualKawase);
    combo->addItem(obsgs_tr("OBSTitles.AlphaMaskBlur"), (int)ShadowBlurType::AlphaMask);
}

static void add_blend_mode_items(QComboBox *combo)
{
    if (!combo) return;
    combo->addItem(obsgs_tr("OBSTitles.BlendModeNormal"), (int)EffectBlendMode::Normal);
    combo->addItem(obsgs_tr("OBSTitles.BlendModeMultiply"), (int)EffectBlendMode::Multiply);
    combo->addItem(obsgs_tr("OBSTitles.BlendModeAdditive"), (int)EffectBlendMode::Additive);
    combo->addItem(obsgs_tr("OBSTitles.BlendModeScreen"), (int)EffectBlendMode::Screen);
    combo->addItem(obsgs_tr("OBSTitles.BlendModeOverlay"), (int)EffectBlendMode::Overlay);
    combo->addItem(obsgs_tr("OBSTitles.BlendModeColor"), (int)EffectBlendMode::Color);
}

static const LayerEffect *find_layer_effect(const Layer &layer, LayerEffectType type)
{
    for (const auto &effect : layer.effects) {
        if (effect.type == type)
            return &effect;
    }
    return nullptr;
}

static bool eval_effect_enabled(const LayerEffect &effect, double t)
{
    return effect.enabled && (!effect.enabled_prop.is_animated() || effect.enabled_prop.evaluate(t) >= 0.5);
}

static uint32_t eval_effect_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.color_a, (effect.effect_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.color_r, (effect.effect_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.color_g, (effect.effect_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.color_b, effect.effect_color & 0xFF, t);
}

static uint32_t eval_effect_stroke_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.stroke_color_a, (effect.effect_stroke_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.stroke_color_r, (effect.effect_stroke_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.stroke_color_g, (effect.effect_stroke_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.stroke_color_b, effect.effect_stroke_color & 0xFF, t);
}

static bool eval_outline_enabled(const Layer &layer, double t)
{
    if (layer.outline_enabled && layer.stroke_fill_type != 0 && layer.stroke_width > 0.0f)
        return true;
    const auto *effect = find_layer_effect(layer, LayerEffectType::Outline);
    return effect && effect->effect_fill_type != 0 && eval_effect_enabled(*effect, t);
}

static uint32_t eval_outline_color(const Layer &layer, double t)
{
    if (layer.outline_enabled && layer.stroke_fill_type != 0 && layer.stroke_width > 0.0f)
        return layer.stroke_color;
    const auto *effect = find_layer_effect(layer, LayerEffectType::Outline);
    return effect ? eval_effect_color(*effect, t) : 0x00000000;
}

static double eval_outline_width(const Layer &layer, double t)
{
    if (layer.outline_enabled && layer.stroke_fill_type != 0 && layer.stroke_width > 0.0f)
        return std::max(0.0f, layer.stroke_width);
    const auto *effect = find_layer_effect(layer, LayerEffectType::Outline);
    return effect && eval_outline_enabled(layer, t)
               ? std::max(0.0, effect->size_prop.is_animated() ? effect->size_prop.evaluate(t) : (double)effect->effect_size)
               : 0.0;
}

static double eval_outline_opacity(const Layer &layer, double t)
{
    if (layer.outline_enabled && layer.stroke_fill_type != 0 && layer.stroke_width > 0.0f)
        return std::clamp((double)layer.outline_opacity, 0.0, 1.0);
    const auto *effect = find_layer_effect(layer, LayerEffectType::Outline);
    return effect ? std::clamp(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(t) : (double)effect->effect_opacity, 0.0, 1.0) : 1.0;
}

static bool eval_outline_on_front(const Layer &layer, double)
{
    if (layer.outline_enabled && layer.stroke_fill_type != 0 && layer.stroke_width > 0.0f)
        return layer.outline_on_front;
    const auto *effect = find_layer_effect(layer, LayerEffectType::Outline);
    return effect ? effect->effect_on_front : true;
}

static bool eval_outline_antialias(const Layer &layer, double)
{
    if (layer.outline_enabled && layer.stroke_fill_type != 0 && layer.stroke_width > 0.0f)
        return layer.outline_antialias;
    const auto *effect = find_layer_effect(layer, LayerEffectType::Outline);
    return effect ? effect->effect_antialias : true;
}

static Qt::PenJoinStyle outline_pen_join_style(const Layer &layer)
{
    const int join_style = (layer.outline_enabled && layer.stroke_fill_type != 0 && layer.stroke_width > 0.0f)
                               ? layer.outline_join_style
                               : 1;
    const auto *effect = find_layer_effect(layer, LayerEffectType::Outline);
    switch (effect && !(layer.outline_enabled && layer.stroke_fill_type != 0 && layer.stroke_width > 0.0f)
                ? effect->effect_join_style
                : join_style) {
    case 0: return Qt::MiterJoin;
    case 2: return Qt::BevelJoin;
    case 1:
    default: return Qt::RoundJoin;
    }
}

static bool eval_shadow_enabled(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect && eval_effect_enabled(*effect, t);
}

static double eval_shadow_opacity(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect ? std::clamp(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(t) : (double)effect->effect_opacity, 0.0, 1.0) : 0.0;
}

static double eval_shadow_distance(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect ? std::max(0.0, effect->distance_prop.is_animated() ? effect->distance_prop.evaluate(t) : (double)effect->effect_distance) : 0.0;
}

static double eval_shadow_angle(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect ? (effect->angle_prop.is_animated() ? effect->angle_prop.evaluate(t) : (double)effect->effect_angle) : 0.0;
}

static double eval_shadow_blur(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect ? std::max(0.0, effect->size_prop.is_animated() ? effect->size_prop.evaluate(t) : (double)effect->effect_size) : 0.0;
}

static double eval_shadow_spread(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect ? std::max(0.0, effect->spread_prop.is_animated() ? effect->spread_prop.evaluate(t) : (double)effect->effect_spread) : 0.0;
}

static uint32_t eval_shadow_color(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::DropShadow);
    return effect ? eval_effect_color(*effect, t) : 0x00000000;
}

static QPointF shadow_offset(const Layer &layer, double t)
{
    double radians = eval_shadow_angle(layer, t) * 3.14159265358979323846 / 180.0;
    double distance = eval_shadow_distance(layer, t);
    return QPointF(std::cos(radians) * distance, std::sin(radians) * distance);
}

static bool eval_background_enabled(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect && eval_effect_enabled(*effect, t);
}

static double eval_background_opacity(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::clamp(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(t) : (double)effect->effect_opacity, 0.0, 1.0) : 0.0;
}

static double eval_background_padding_x(const Layer &layer, double t)
{
    return 0.0;
}

static double eval_background_padding_y(const Layer &layer, double t)
{
    return 0.0;
}

static double eval_background_padding_left(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? (effect->padding_left_prop.is_animated() ? effect->padding_left_prop.evaluate(t) : (double)effect->effect_padding_left) : 0.0;
}

static double eval_background_padding_right(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? (effect->padding_right_prop.is_animated() ? effect->padding_right_prop.evaluate(t) : (double)effect->effect_padding_right) : 0.0;
}

static double eval_background_padding_top(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? (effect->padding_top_prop.is_animated() ? effect->padding_top_prop.evaluate(t) : (double)effect->effect_padding_top) : 0.0;
}

static double eval_background_padding_bottom(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? (effect->padding_bottom_prop.is_animated() ? effect->padding_bottom_prop.evaluate(t) : (double)effect->effect_padding_bottom) : 0.0;
}

static double eval_background_corner_radius(const Layer &layer, double t)
{
    return 0.0;
}

static double eval_background_corner_radius_tl(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::max(0.0, effect->corner_radius_tl_prop.is_animated() ? effect->corner_radius_tl_prop.evaluate(t) : (double)effect->effect_corner_radius_tl) : 0.0;
}

static double eval_background_corner_radius_tr(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::max(0.0, effect->corner_radius_tr_prop.is_animated() ? effect->corner_radius_tr_prop.evaluate(t) : (double)effect->effect_corner_radius_tr) : 0.0;
}

static double eval_background_corner_radius_br(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::max(0.0, effect->corner_radius_br_prop.is_animated() ? effect->corner_radius_br_prop.evaluate(t) : (double)effect->effect_corner_radius_br) : 0.0;
}

static double eval_background_corner_radius_bl(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::max(0.0, effect->corner_radius_bl_prop.is_animated() ? effect->corner_radius_bl_prop.evaluate(t) : (double)effect->effect_corner_radius_bl) : 0.0;
}

static double eval_background_stroke_width(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::max(0.0, effect->stroke_width_prop.is_animated() ? effect->stroke_width_prop.evaluate(t) : (double)effect->effect_stroke_width) : 0.0;
}

static double eval_background_stroke_opacity(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? std::clamp(effect->stroke_opacity_prop.is_animated() ? effect->stroke_opacity_prop.evaluate(t) : (double)effect->effect_stroke_opacity, 0.0, 1.0) : 1.0;
}

static uint32_t eval_background_color(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? eval_effect_color(*effect, t) : 0x00000000;
}

static uint32_t eval_background_stroke_color(const Layer &layer, double t)
{
    const auto *effect = find_layer_effect(layer, LayerEffectType::BackgroundColor);
    return effect ? eval_effect_stroke_color(*effect, t) : 0x00000000;
}

static QColor evaluated_background_color(const Layer &layer, double t)
{
    QColor color = color_from_argb(eval_background_color(layer, t));
    color.setAlphaF(std::clamp((double)color.alphaF() * eval_background_opacity(layer, t), 0.0, 1.0));
    return color;
}

static void set_channel_statics(Layer &layer, bool text, uint32_t argb)
{
    auto &a = text ? layer.text_color_a : layer.fill_color_a;
    auto &r = text ? layer.text_color_r : layer.fill_color_r;
    auto &g = text ? layer.text_color_g : layer.fill_color_g;
    auto &b = text ? layer.text_color_b : layer.fill_color_b;
    a.static_value = (argb >> 24) & 0xFF;
    r.static_value = (argb >> 16) & 0xFF;
    g.static_value = (argb >> 8) & 0xFF;
    b.static_value = argb & 0xFF;
}

static void apply_easing_preset(Keyframe &keyframe, EasingType easing)
{
    keyframe.easing = easing;
    switch (easing) {
    case EasingType::Bezier:
        keyframe.cx1 = 0.33f; keyframe.cy1 = 0.0f;
        keyframe.cx2 = 0.67f; keyframe.cy2 = 1.0f;
        break;
    case EasingType::EaseIn:
        keyframe.cx1 = 0.42f; keyframe.cy1 = 0.0f;
        keyframe.cx2 = 1.0f; keyframe.cy2 = 1.0f;
        break;
    case EasingType::EaseOut:
        keyframe.cx1 = 0.0f; keyframe.cy1 = 0.0f;
        keyframe.cx2 = 0.58f; keyframe.cy2 = 1.0f;
        break;
    case EasingType::EaseInOut:
        keyframe.cx1 = 0.42f; keyframe.cy1 = 0.0f;
        keyframe.cx2 = 0.58f; keyframe.cy2 = 1.0f;
        break;
    case EasingType::Linear:
    case EasingType::Hold:
    default:
        keyframe.cx1 = 0.333f; keyframe.cy1 = 0.0f;
        keyframe.cx2 = 0.667f; keyframe.cy2 = 1.0f;
        break;
    }
}

static void add_or_replace_keyframe(AnimatedProperty &prop, double time, double value)
{
    constexpr double kEpsilon = 1.0 / 240.0;
    prop.static_value = value;
    for (auto &kf : prop.keyframes) {
        if (std::abs(kf.time - time) <= kEpsilon) {
            kf.time = time;
            kf.value = value;
            return;
        }
    }
    Keyframe kf;
    kf.time = time;
    kf.value = value;
    apply_easing_preset(kf, EasingType::Linear);
    prop.keyframes.push_back(kf);
    std::sort(prop.keyframes.begin(), prop.keyframes.end(),
              [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
}

static void set_animated_value(AnimatedProperty &prop, double time, double value)
{
    if (prop.is_animated())
        add_or_replace_keyframe(prop, time, value);
    else
        prop.static_value = value;
}

static void set_color_channels_at(Layer &layer, bool text, double time, uint32_t argb)
{
    auto &a = text ? layer.text_color_a : layer.fill_color_a;
    auto &r = text ? layer.text_color_r : layer.fill_color_r;
    auto &g = text ? layer.text_color_g : layer.fill_color_g;
    auto &b = text ? layer.text_color_b : layer.fill_color_b;
    set_animated_value(a, time, (argb >> 24) & 0xFF);
    set_animated_value(r, time, (argb >> 16) & 0xFF);
    set_animated_value(g, time, (argb >> 8) & 0xFF);
    set_animated_value(b, time, argb & 0xFF);
}

static void set_shadow_color_channels_at(Layer &layer, double time, uint32_t argb)
{
    set_animated_value(layer.shadow_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(layer.shadow_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(layer.shadow_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(layer.shadow_color_b, time, argb & 0xFF);
}

static void set_background_color_channels_at(Layer &layer, double time, uint32_t argb)
{
    set_animated_value(layer.background_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(layer.background_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(layer.background_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(layer.background_color_b, time, argb & 0xFF);
}

static void set_background_stroke_color_channels_at(Layer &layer, double time, uint32_t argb)
{
    set_animated_value(layer.background_stroke_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(layer.background_stroke_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(layer.background_stroke_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(layer.background_stroke_color_b, time, argb & 0xFF);
}

static bool keyframe_at_time(const AnimatedProperty &prop, double time)
{
    constexpr double kEpsilon = 1.0 / 240.0;
    for (const auto &kf : prop.keyframes)
        if (std::abs(kf.time - time) <= kEpsilon) return true;
    return false;
}

static void remove_keyframe_at(AnimatedProperty &prop, double time)
{
    constexpr double kEpsilon = 1.0 / 240.0;
    prop.keyframes.erase(
        std::remove_if(prop.keyframes.begin(), prop.keyframes.end(),
                       [&](const Keyframe &kf) { return std::abs(kf.time - time) <= kEpsilon; }),
        prop.keyframes.end());
}

static void toggle_keyframe(AnimatedProperty &prop, double time, double value)
{
    if (keyframe_at_time(prop, time))
        remove_keyframe_at(prop, time);
    else
        add_or_replace_keyframe(prop, time, value);
}

static bool any_keyframe_at_time(std::initializer_list<const AnimatedProperty *> props, double time)
{
    for (const auto *prop : props)
        if (prop && keyframe_at_time(*prop, time)) return true;
    return false;
}

static bool any_keyframes(std::initializer_list<const AnimatedProperty *> props)
{
    for (const auto *prop : props)
        if (prop && prop->is_animated()) return true;
    return false;
}

static QStringList font_styles_for_family(const QString &family)
{
    QFontDatabase fdb;
    QStringList styles = fdb.styles(family);
    if (styles.isEmpty()) {
        styles << QStringLiteral("Regular");
    } else {
        styles.removeDuplicates();
        styles.sort(Qt::CaseInsensitive);
        int regular = styles.indexOf(QStringLiteral("Regular"));
        if (regular > 0) {
            QString item = styles.takeAt(regular);
            styles.prepend(item);
        }
    }
    return styles;
}

static void populate_font_style_combo(QComboBox *combo, const QString &family, const QString &preferred)
{
    if (!combo) return;
    QSignalBlocker blocker(combo);
    combo->clear();
    QStringList styles = font_styles_for_family(family);
    for (const QString &style : styles)
        combo->addItem(style, style);
    int idx = preferred.isEmpty() ? -1 : combo->findText(preferred);
    if (idx < 0) idx = combo->findText(QStringLiteral("Regular"));
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
}

static void style_color_button(QPushButton *button, uint32_t argb)
{
    if (!button) return;
    QColor c = color_from_argb(argb);
    button->setText(c.name(QColor::HexArgb));
    button->setToolTip(QString());
    button->setStyleSheet(QString(
        "QPushButton{color:%1;background:%2;border:1px solid #555;"
        "border-radius:3px;padding:3px 8px;}")
        .arg(c.lightness() < 128 ? "#fff" : "#000")
        .arg(c.name(QColor::HexArgb)));
}

static void style_gradient_button(QPushButton *button, uint32_t start_argb, uint32_t end_argb, int gradient_type)
{
    if (!button) return;
    QColor start = color_from_argb(start_argb);
    QColor end = color_from_argb(end_argb);
    button->setText(QString());
    const bool radial = gradient_type == 1 || gradient_type == 4;
    QString type_name = QStringLiteral("Linear Gradient");
    if (gradient_type == 1) type_name = QStringLiteral("Radial Gradient");
    else if (gradient_type == 2) type_name = QStringLiteral("Angle Gradient");
    else if (gradient_type == 3) type_name = QStringLiteral("Reflected Gradient");
    else if (gradient_type == 4) type_name = QStringLiteral("Diamond Gradient");
    button->setToolTip(type_name);
    const QString fill = radial
        ? QStringLiteral("qradialgradient(cx:0.5,cy:0.5,radius:0.65,fx:0.5,fy:0.5,stop:0 %1,stop:1 %2)")
              .arg(start.name(QColor::HexArgb), end.name(QColor::HexArgb))
        : QStringLiteral("qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 %1,stop:1 %2)")
              .arg(start.name(QColor::HexArgb), end.name(QColor::HexArgb));
    button->setStyleSheet(QStringLiteral(
        "QPushButton{background:%1;border:1px solid #555;border-radius:3px;padding:3px 8px;}").arg(fill));
}

static void style_color_button_mixed(QPushButton *button)
{
    if (!button) return;
    button->setText(QString());
    button->setToolTip(QStringLiteral("Mixed values"));
    button->setStyleSheet(
        "QPushButton{color:#d8d8d8;background:#252525;border:1px dashed #777;"
        "border-radius:3px;padding:3px 8px;}");
}

static void set_spin_mixed(QAbstractSpinBox *spin, bool mixed)
{
    if (!spin) return;
    if (auto *edit = spin->findChild<QLineEdit *>()) {
        if (mixed) edit->clear();
    }
    spin->setToolTip(mixed ? QStringLiteral("Mixed values") : QString());
}

static void set_combo_mixed(QComboBox *combo, bool mixed)
{
    if (!combo) return;
    if (mixed) combo->setCurrentIndex(-1);
    combo->setToolTip(mixed ? QStringLiteral("Mixed values") : QString());
}

static void set_button_mixed(QAbstractButton *button, bool mixed)
{
    if (!button) return;
    if (mixed)
        button->setChecked(false);
    button->setToolTip(mixed ? QStringLiteral("Mixed values") : QString());
    button->setStyleSheet(mixed
        ? QStringLiteral("QToolButton{color:#d8d8d8;background:#252525;border:1px dashed #777;"
                         "border-radius:2px;font-size:10px;font-weight:bold;padding:0;}"
                         "QToolButton:hover{background:#303030;border-color:#888;}")
        : QStringLiteral("QToolButton{color:#d8d8d8;background:#242424;border:1px solid #373737;border-radius:2px;"
                         "font-size:10px;font-weight:bold;padding:0;}"
                         "QToolButton:hover{background:#303030;border-color:#4a4a4a;}"
                         "QToolButton:checked{background:#4b6ea8;color:white;border-color:#6f8fc4;}"));
}

static QColor keyframe_color(EasingType)
{
    return C_KF_DOT;
}

static QPainterPath keyframe_shape_path(EasingType easing, const QPointF &center, qreal radius)
{
    const qreal x = center.x();
    const qreal y = center.y();
    QPainterPath path;

    switch (easing) {
    case EasingType::Linear:
        path.moveTo(x, y - radius);
        path.lineTo(x + radius, y);
        path.lineTo(x, y + radius);
        path.lineTo(x - radius, y);
        path.closeSubpath();
        break;
    case EasingType::EaseIn:
        path.moveTo(x - radius, y);
        path.lineTo(x, y - radius);
        path.lineTo(x + radius, y - radius);
        path.lineTo(x + radius, y + radius);
        path.lineTo(x, y + radius);
        path.closeSubpath();
        break;
    case EasingType::EaseOut:
        path.moveTo(x + radius, y);
        path.lineTo(x, y - radius);
        path.lineTo(x - radius, y - radius);
        path.lineTo(x - radius, y + radius);
        path.lineTo(x, y + radius);
        path.closeSubpath();
        break;
    case EasingType::EaseInOut:
        path.moveTo(x - radius, y - radius);
        path.lineTo(x + radius, y - radius);
        path.lineTo(x - radius, y + radius);
        path.lineTo(x + radius, y + radius);
        path.closeSubpath();
        break;
    case EasingType::Bezier:
        path.addEllipse(center, radius, radius);
        break;
    case EasingType::Hold:
        path.addRect(QRectF(x - radius, y - radius, radius * 2.0, radius * 2.0));
        break;
    default:
        path.moveTo(x, y - radius);
        path.lineTo(x + radius, y);
        path.lineTo(x, y + radius);
        path.lineTo(x - radius, y);
        path.closeSubpath();
        break;
    }

    return path;
}

static void draw_keyframe_marker(QPainter &painter, const QPointF &center, EasingType easing,
                                 qreal radius, const QColor &fill,
                                 const QColor &stroke, qreal stroke_width)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(fill);
    painter.setPen(QPen(stroke, stroke_width, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    painter.drawPath(keyframe_shape_path(easing, center, radius));

    if (easing == EasingType::Bezier) {
        painter.setPen(QPen(stroke, std::max<qreal>(1.0, stroke_width - 0.5)));
        painter.drawLine(QPointF(center.x() - radius - 3.0, center.y()),
                         QPointF(center.x() - radius, center.y()));
        painter.drawLine(QPointF(center.x() + radius, center.y()),
                         QPointF(center.x() + radius + 3.0, center.y()));
    }

    painter.restore();
}

static QString easing_label(EasingType easing)
{
    switch (easing) {
    case EasingType::Linear: return obsgs_tr("OBSTitles.Linear");
    case EasingType::EaseIn: return obsgs_tr("OBSTitles.EaseIn");
    case EasingType::EaseOut: return obsgs_tr("OBSTitles.EaseOut");
    case EasingType::EaseInOut: return obsgs_tr("OBSTitles.EasyEase");
    case EasingType::Bezier: return obsgs_tr("OBSTitles.CustomBezier");
    case EasingType::Hold: return obsgs_tr("OBSTitles.Hold");
    default: return obsgs_tr("OBSTitles.Linear");
    }
}

static std::vector<AnimatedProperty *> timeline_properties(Layer &layer)
{
    /* Position, Scale, Size and Origin are edited/keyframed as 2D vector
     * properties in the UI. Keep the scalar AnimatedProperty storage for
     * backward-compatible rendering/JSON, but expose one timeline lane per
     * vector by using the X/W property as the group representative.
     */
    std::vector<AnimatedProperty *> props {
        &layer.pos_x,
        &layer.scale_x,
        &layer.rotation, &layer.opacity,
        &layer.box_width,
        &layer.origin_x_prop,
        &layer.paragraph_indent_left_prop, &layer.paragraph_indent_right_prop,
        &layer.paragraph_indent_first_line_prop,
        &layer.font_size_prop, &layer.char_scale_x_prop, &layer.char_scale_y_prop,
        &layer.char_tracking_prop, &layer.baseline_shift_prop,
        &layer.paragraph_space_before_prop, &layer.paragraph_space_after_prop,
        &layer.text_color_a, &layer.text_color_r,
        &layer.text_color_g, &layer.text_color_b,
        &layer.fill_color_a, &layer.fill_color_r,
        &layer.fill_color_g, &layer.fill_color_b
    };

    auto add_effect_props = [&](LayerEffect &effect) {
        QString prefix = effect_type_name(effect.type);
        prefix.replace(QStringLiteral(" "), QStringLiteral("_"));
        prefix = prefix.toLower();
        auto name = [&](AnimatedProperty &prop, const char *suffix) {
            prop.name = (prefix + QStringLiteral("_") + QString::fromLatin1(suffix)).toStdString();
            props.push_back(&prop);
        };
        name(effect.enabled_prop, "enabled");
        switch (effect.type) {
        case LayerEffectType::BackgroundColor:
            name(effect.opacity_prop, "opacity");
            name(effect.padding_left_prop, "padding_left");
            name(effect.padding_right_prop, "padding_right");
            name(effect.padding_top_prop, "padding_top");
            name(effect.padding_bottom_prop, "padding_bottom");
            name(effect.corner_radius_tl_prop, "corner_tl");
            name(effect.corner_radius_tr_prop, "corner_tr");
            name(effect.corner_radius_br_prop, "corner_br");
            name(effect.corner_radius_bl_prop, "corner_bl");
            name(effect.stroke_width_prop, "stroke_width");
            name(effect.stroke_opacity_prop, "stroke_opacity");
            name(effect.color_a, "color_a");
            name(effect.color_r, "color_r");
            name(effect.color_g, "color_g");
            name(effect.color_b, "color_b");
            name(effect.stroke_color_a, "stroke_color_a");
            name(effect.stroke_color_r, "stroke_color_r");
            name(effect.stroke_color_g, "stroke_color_g");
            name(effect.stroke_color_b, "stroke_color_b");
            break;
        case LayerEffectType::Outline:
            name(effect.size_prop, "width");
            name(effect.opacity_prop, "opacity");
            name(effect.color_a, "color_a");
            name(effect.color_r, "color_r");
            name(effect.color_g, "color_g");
            name(effect.color_b, "color_b");
            break;
        case LayerEffectType::DropShadow:
        case LayerEffectType::InnerShadow:
            name(effect.opacity_prop, "opacity");
            name(effect.distance_prop, "distance");
            name(effect.angle_prop, "angle");
            name(effect.size_prop, "blur");
            name(effect.spread_prop, "spread");
            name(effect.color_a, "color_a");
            name(effect.color_r, "color_r");
            name(effect.color_g, "color_g");
            name(effect.color_b, "color_b");
            break;
        case LayerEffectType::LongShadow:
            name(effect.opacity_prop, "opacity");
            name(effect.distance_prop, "length");
            name(effect.angle_prop, "angle");
            name(effect.falloff_prop, "falloff");
            name(effect.size_prop, "blur");
            name(effect.color_a, "color_a");
            name(effect.color_r, "color_r");
            name(effect.color_g, "color_g");
            name(effect.color_b, "color_b");
            break;
        default:
            name(effect.opacity_prop, "amount");
            name(effect.size_prop, "size");
            name(effect.angle_prop, "angle");
            break;
        }
    };
    for (auto &effect : layer.effects)
        add_effect_props(effect);
    return props;
}

static QString property_label(const std::string &name)
{
    QString qname = QString::fromStdString(name);
    auto effect_label = [&](const QString &prefix, const QString &label) -> QString {
        QString title = prefix;
        title.replace(QStringLiteral("_"), QStringLiteral(" "));
        for (int i = 0; i < title.size(); ++i)
            if (i == 0 || title.at(i - 1).isSpace())
                title[i] = title.at(i).toUpper();
        return title + QStringLiteral(" ") + label;
    };
    if (qname.startsWith(QStringLiteral("background_color_"))) {
        QString suffix = qname.mid(QStringLiteral("background_color_").size());
        if (suffix == QStringLiteral("enabled")) return effect_label(QStringLiteral("Background Color"), QStringLiteral("Enabled"));
        if (suffix == QStringLiteral("opacity")) return effect_label(QStringLiteral("Background Color"), QStringLiteral("Opacity"));
        if (suffix.startsWith(QStringLiteral("padding_"))) return effect_label(QStringLiteral("Background Color"), suffix.replace(QStringLiteral("_"), QStringLiteral(" ")));
        if (suffix.startsWith(QStringLiteral("corner_"))) return effect_label(QStringLiteral("Background Color"), suffix.replace(QStringLiteral("_"), QStringLiteral(" ")));
        if (suffix.startsWith(QStringLiteral("stroke_"))) return effect_label(QStringLiteral("Background Color"), suffix.replace(QStringLiteral("_"), QStringLiteral(" ")));
        if (suffix.startsWith(QStringLiteral("color_"))) return effect_label(QStringLiteral("Background Color"), QStringLiteral("Color"));
    }
    if (qname.startsWith(QStringLiteral("outline_"))) {
        QString suffix = qname.mid(QStringLiteral("outline_").size());
        if (suffix == QStringLiteral("enabled")) return effect_label(QStringLiteral("Outline"), QStringLiteral("Enabled"));
        if (suffix == QStringLiteral("width")) return effect_label(QStringLiteral("Outline"), QStringLiteral("Width"));
        if (suffix == QStringLiteral("opacity")) return effect_label(QStringLiteral("Outline"), QStringLiteral("Opacity"));
        if (suffix.startsWith(QStringLiteral("color_"))) return effect_label(QStringLiteral("Outline"), QStringLiteral("Color"));
    }
    for (const QString &prefix : {QStringLiteral("drop_shadow_"), QStringLiteral("long_shadow_"), QStringLiteral("inner_shadow_")}) {
        if (!qname.startsWith(prefix)) continue;
        QString suffix = qname.mid(prefix.size()).replace(QStringLiteral("_"), QStringLiteral(" "));
        QString title = prefix.left(prefix.size() - 1);
        title.replace(QStringLiteral("_"), QStringLiteral(" "));
        return effect_label(title, suffix);
    }
    if (name == "pos_x" || name == "pos_y") return obsgs_tr("OBSTitles.Position");
    if (name == "scale_x" || name == "scale_y") return obsgs_tr("OBSTitles.Scale");
    if (name == "box_width" || name == "box_height") return obsgs_tr("OBSTitles.Size");
    if (name == "origin_x" || name == "origin_y") return obsgs_tr("OBSTitles.Origin");
    if (name == "paragraph_indent_left") return obsgs_tr("OBSTitles.ParagraphIndentLeft");
    if (name == "paragraph_indent_right") return obsgs_tr("OBSTitles.ParagraphIndentRight");
    if (name == "paragraph_indent_first_line") return obsgs_tr("OBSTitles.ParagraphIndentFirstLine");
    if (name == "font_size") return QStringLiteral("Size");
    if (name == "char_scale_x") return QStringLiteral("H Scale");
    if (name == "char_scale_y") return QStringLiteral("V Scale");
    if (name == "char_tracking") return obsgs_tr("OBSTitles.Tracking");
    if (name == "baseline_shift") return QStringLiteral("Baseline");
    if (name == "paragraph_space_before") return obsgs_tr("OBSTitles.ParagraphSpaceBefore");
    if (name == "paragraph_space_after") return obsgs_tr("OBSTitles.ParagraphSpaceAfter");
    if (name == "text_color_a" || name == "text_color_r" ||
        name == "text_color_g" || name == "text_color_b") return obsgs_tr("OBSTitles.TextColor");
    if (name == "fill_color_a" || name == "fill_color_r" ||
        name == "fill_color_g" || name == "fill_color_b") return obsgs_tr("OBSTitles.FillColor");
    if (name == "shadow_color_a" || name == "shadow_color_r" ||
        name == "shadow_color_g" || name == "shadow_color_b") return obsgs_tr("OBSTitles.ShadowColor");
    if (name == "background_color_a" || name == "background_color_r" ||
        name == "background_color_g" || name == "background_color_b") return obsgs_tr("OBSTitles.BackgroundColor");
    if (name == "background_enabled") return obsgs_tr("OBSTitles.EnableColorBackground");
    if (name == "background_opacity") return obsgs_tr("OBSTitles.BackgroundOpacityLabel");
    if (name == "background_padding_x") return obsgs_tr("OBSTitles.BackgroundHorizontalPaddingLabel");
    if (name == "background_padding_y") return obsgs_tr("OBSTitles.BackgroundVerticalPaddingLabel");
    if (name == "background_padding_left") return QStringLiteral("Left Padding");
    if (name == "background_padding_right") return QStringLiteral("Right Padding");
    if (name == "background_padding_top") return QStringLiteral("Top Padding");
    if (name == "background_padding_bottom") return QStringLiteral("Bottom Padding");
    if (name == "background_corner_radius") return obsgs_tr("OBSTitles.BackgroundCornerLabel");
    if (name == "background_corner_radius_tl") return QStringLiteral("Corner TL");
    if (name == "background_corner_radius_tr") return QStringLiteral("Corner TR");
    if (name == "background_corner_radius_br") return QStringLiteral("Corner BR");
    if (name == "background_corner_radius_bl") return QStringLiteral("Corner BL");
    if (name == "background_stroke_width") return QStringLiteral("Background Stroke Width");
    if (name == "background_stroke_opacity") return QStringLiteral("Background Stroke Opacity");
    if (name == "background_stroke_color_a" || name == "background_stroke_color_r" || name == "background_stroke_color_g" || name == "background_stroke_color_b") return QStringLiteral("Background Stroke");
    if (name == "shadow_enabled") return obsgs_tr("OBSTitles.ShadowEnable");
    if (name == "shadow_opacity") return obsgs_tr("OBSTitles.ShadowOpacity");
    if (name == "shadow_distance") return obsgs_tr("OBSTitles.ShadowDistance");
    if (name == "shadow_angle") return obsgs_tr("OBSTitles.ShadowAngle");
    if (name == "shadow_blur") return obsgs_tr("OBSTitles.ShadowBlur");
    if (name == "shadow_spread") return obsgs_tr("OBSTitles.ShadowSpread");
    if (name == "rotation") return obsgs_tr("OBSTitles.Rotation");
    if (name == "opacity") return obsgs_tr("OBSTitles.Opacity");
    return QString::fromStdString(name);
}

static QString property_value_text(const AnimatedProperty &prop, const Layer &layer)
{
    double value = prop.static_value;
    if (prop.name == "pos_x")
        return QString("%1,%2").arg(layer.pos_x.static_value, 0, 'f', 1)
                                .arg(layer.pos_y.static_value, 0, 'f', 1);
    if (prop.name == "scale_x")
        return QString("%1,%2%").arg(layer.scale_x.static_value * 100.0, 0, 'f', 1)
                                 .arg(layer.scale_y.static_value * 100.0, 0, 'f', 1);
    if (prop.name == "box_width")
        return QString("%1 × %2").arg(layer.box_width.static_value, 0, 'f', 0)
                                  .arg(layer.box_height.static_value, 0, 'f', 0);
    if (prop.name == "origin_x")
        return QString("%1,%2").arg(layer.origin_x_prop.static_value, 0, 'f', 2)
                                .arg(layer.origin_y_prop.static_value, 0, 'f', 2);
    if (prop.name == "char_scale_x" || prop.name == "char_scale_y") value *= 100.0;
    if (prop.name == "opacity" || prop.name == "shadow_opacity" || prop.name == "background_opacity") value *= 100.0;
    if (prop.name == "shadow_enabled" || prop.name == "background_enabled") return value >= 0.5 ? obsgs_tr("OBSTitles.On") : obsgs_tr("OBSTitles.Off");
    return QString::number(value, 'f', (prop.name == "opacity" || prop.name == "shadow_opacity" || prop.name == "background_opacity") ? 1 : 2);
}

struct TimelineRow {
    std::shared_ptr<Layer> layer;
    AnimatedProperty *prop = nullptr;
    bool is_property = false;
};

static std::vector<TimelineRow> timeline_rows(const std::shared_ptr<Title> &title)
{
    std::vector<TimelineRow> rows;
    if (!title) return rows;
    for (auto it = title->layers.rbegin(); it != title->layers.rend(); ++it) {
        auto layer = *it;
        rows.push_back({layer, nullptr, false});
        if (!layer->properties_expanded) continue;
        std::set<std::string> seen;
        for (auto *prop : timeline_properties(*layer)) {
            if (!prop->is_animated()) continue;
            QString label = property_label(prop->name);
            std::string key = label.toStdString();
            if (seen.insert(key).second)
                rows.push_back({layer, prop, true});
        }
    }
    return rows;
}

/* ══════════════════════════════════════════════════════════════════
 *  TitleEditor
 * ══════════════════════════════════════════════════════════════════ */
