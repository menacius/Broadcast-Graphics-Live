#include "bgl-modern-controls.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QBoxLayout>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStringList>
#include <QStyle>
#include <QTimer>
#include <QVariant>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kSwitchTrackWidth = 28;
constexpr int kSwitchTrackHeight = 14;
constexpr int kSwitchKnobDiameter = 10;
constexpr int kSwitchSpacing = 7;
constexpr int kPanelHeaderHeight = 28;
constexpr int kPanelHandleWidth = 23;
constexpr int kPanelCaretWidth = 24;
constexpr int kPanelContentLeft = 10;
constexpr int kPanelContentTop = 8;
constexpr int kPanelContentRight = 10;
constexpr int kPanelContentBottom = 10;
constexpr char kPanelMimeType[] = "application/x-bgl-collapsible-panel";
constexpr double kPi = 3.14159265358979323846;

QPointF mouse_position(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->localPos();
#endif
}

QPointF drag_position(QDragMoveEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->pos();
#endif
}

QPointF drop_position(QDropEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->pos();
#endif
}

QColor blended(const QColor &a, const QColor &b, int a_weight = 1, int b_weight = 1)
{
    const int total = std::max(1, a_weight + b_weight);
    return QColor((a.red() * a_weight + b.red() * b_weight) / total,
                  (a.green() * a_weight + b.green() * b_weight) / total,
                  (a.blue() * a_weight + b.blue() * b_weight) / total,
                  (a.alpha() * a_weight + b.alpha() * b_weight) / total);
}

BglCollapsiblePanel *panel_from_drag(const QMimeData *mime, const QObject *source)
{
    if (!mime || !source || !mime->hasFormat(kPanelMimeType))
        return nullptr;
    bool mime_ok = false;
    bool source_ok = false;
    const quintptr mime_address = mime->data(kPanelMimeType).toULongLong(&mime_ok, 16);
    const quintptr source_address = source->property("bglPanelAddress").toULongLong(&source_ok);
    if (!mime_ok || !source_ok || mime_address != source_address)
        return nullptr;
    return reinterpret_cast<BglCollapsiblePanel *>(mime_address);
}

QString inferred_panel_title(QWidget *section, const QString &requested)
{
    if (!requested.isEmpty())
        return requested;
    if (section) {
        const QString property_title = section->property("bglPanelTitle").toString();
        if (!property_title.isEmpty())
            return property_title;
        if (auto *group = qobject_cast<QGroupBox *>(section); group && !group->title().isEmpty())
            return group->title();
    }
    return QObject::tr("Panel");
}

void draw_caret(QPainter &painter, const QRectF &rect, int state, const QColor &color)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    const QPointF center = rect.center();
    QPolygonF triangle;
    if (state > 0) {
        triangle << QPointF(center.x() - 4.0, center.y() - 2.0)
                 << QPointF(center.x() + 4.0, center.y() - 2.0)
                 << QPointF(center.x(), center.y() + 3.0);
    } else {
        triangle << QPointF(center.x() - 2.0, center.y() - 4.0)
                 << QPointF(center.x() - 2.0, center.y() + 4.0)
                 << QPointF(center.x() + 3.0, center.y());
    }
    painter.drawPolygon(triangle);
    if (state == 1) {
        QColor marker = color;
        marker.setAlpha(std::min(255, color.alpha() + 35));
        painter.setBrush(marker);
        painter.drawEllipse(QPointF(center.x() + 6.0, center.y() + 5.0), 1.45, 1.45);
    }
    painter.restore();
}

QString normalized_panel_key(QString value)
{
    value = value.trimmed();
    for (int index = 0; index < value.size(); ++index) {
        const QChar ch = value.at(index);
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('_') && ch != QLatin1Char('-'))
            value[index] = QLatin1Char('_');
    }
    while (value.contains(QStringLiteral("__")))
        value.replace(QStringLiteral("__"), QStringLiteral("_"));
    return value.isEmpty() ? QStringLiteral("Panel") : value;
}

QString panel_group_for(QWidget *parent)
{
    QStringList path;
    for (QWidget *widget = parent; widget && path.size() < 5;
         widget = widget->parentWidget()) {
        QString segment = widget->objectName();
        if (segment.isEmpty()) {
            segment = QString::fromLatin1(widget->metaObject()->className());
            int sibling_index = 0;
            if (QWidget *container = widget->parentWidget()) {
                const auto siblings = container->findChildren<QWidget *>(
                    QString(), Qt::FindDirectChildrenOnly);
                for (QWidget *sibling : siblings) {
                    if (sibling == widget)
                        break;
                    if (QString::fromLatin1(sibling->metaObject()->className()) == segment)
                        ++sibling_index;
                }
            }
            segment += QStringLiteral("_%1").arg(sibling_index);
        }
        path.prepend(normalized_panel_key(segment));
        if (!widget->objectName().isEmpty())
            break;
    }
    return path.isEmpty() ? QStringLiteral("EditorInspector")
                          : path.join(QStringLiteral("_"));
}

QStringList persisted_panel_order(const QString &group)
{
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("EditorPanels"));
    return settings.value(QStringLiteral("order/%1").arg(group)).toStringList();
}

void save_panel_order(QBoxLayout *layout, const QString &group)
{
    if (!layout || group.isEmpty())
        return;
    QStringList order;
    for (int index = 0; index < layout->count(); ++index) {
        auto *panel = qobject_cast<BglCollapsiblePanel *>(layout->itemAt(index)->widget());
        if (panel && panel->property("bglPersistOrder").toBool() &&
            !panel->persistenceKey().isEmpty())
            order.push_back(panel->persistenceKey());
    }
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("EditorPanels"));
    settings.setValue(QStringLiteral("order/%1").arg(group), order);
}

void restore_panel_order(QBoxLayout *layout, const QString &group)
{
    if (!layout || group.isEmpty())
        return;
    const QStringList saved = persisted_panel_order(group);
    if (saved.isEmpty())
        return;
    int destination = 0;
    for (const QString &key : saved) {
        for (int index = destination; index < layout->count(); ++index) {
            auto *panel = qobject_cast<BglCollapsiblePanel *>(layout->itemAt(index)->widget());
            if (!panel || !panel->property("bglPersistOrder").toBool() ||
                panel->persistenceKey() != key)
                continue;
            if (index != destination) {
                const int stretch = layout->stretch(index);
                QLayoutItem *item = layout->takeAt(index);
                const Qt::Alignment alignment = item ? item->alignment() : Qt::Alignment();
                layout->insertWidget(destination, panel, stretch, alignment);
                delete item;
            }
            ++destination;
            break;
        }
    }
}
} // namespace

BglSwitch::BglSwitch(QWidget *parent)
    : QCheckBox(parent)
{
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setMinimumHeight(19);
}

BglSwitch::BglSwitch(const QString &text, QWidget *parent)
    : BglSwitch(parent)
{
    setText(text);
}

QSize BglSwitch::sizeHint() const
{
    const QFontMetrics metrics(font());
    const int text_width = text().isEmpty() ? 0 : metrics.horizontalAdvance(text()) + kSwitchSpacing;
    return QSize(text_width + kSwitchTrackWidth + 2,
                 std::max(metrics.height(), kSwitchTrackHeight) + 4);
}

QSize BglSwitch::minimumSizeHint() const
{
    return QSize(kSwitchTrackWidth + 2, kSwitchTrackHeight + 4);
}

bool BglSwitch::hitButton(const QPoint &position) const
{
    return rect().contains(position);
}

void BglSwitch::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPalette pal = palette();
    const bool rtl = layoutDirection() == Qt::RightToLeft;
    const QRect track_rect(rtl ? 1 : width() - kSwitchTrackWidth - 1,
                           (height() - kSwitchTrackHeight) / 2,
                           kSwitchTrackWidth, kSwitchTrackHeight);
    QRect text_rect = rect();
    if (rtl)
        text_rect.setLeft(track_rect.right() + kSwitchSpacing);
    else
        text_rect.setRight(track_rect.left() - kSwitchSpacing);

    if (!text().isEmpty()) {
        QColor text_color = pal.color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                      QPalette::WindowText);
        if (isEnabled())
            text_color.setAlpha(225);
        painter.setPen(text_color);
        painter.drawText(text_rect, (rtl ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                         text());
    }

    const Qt::CheckState state = checkState();
    const bool partially_checked = state == Qt::PartiallyChecked;
    const bool fully_checked = state == Qt::Checked;

    QColor track;
    QColor border;
    if (fully_checked || partially_checked) {
        track = pal.color(QPalette::Highlight);
        track.setAlpha(partially_checked ? 78 : 118);
        border = pal.color(QPalette::Highlight);
        border.setAlpha(hasFocus() ? 210 : 150);
    } else {
        track = pal.color(QPalette::Button);
        track.setAlpha(88);
        border = pal.color(QPalette::Mid);
        border.setAlpha(hasFocus() ? 210 : 118);
    }
    if (!isEnabled()) {
        track = pal.color(QPalette::Disabled, QPalette::Button);
        track.setAlpha(64);
        border = pal.color(QPalette::Disabled, QPalette::Mid);
        border.setAlpha(82);
    } else if (underMouse()) {
        track.setAlpha(std::min(165, track.alpha() + 18));
        border.setAlpha(std::min(220, border.alpha() + 18));
    }

    painter.setPen(QPen(border, hasFocus() ? 1.25 : 1.0));
    painter.setBrush(track);
    painter.drawRoundedRect(QRectF(track_rect), kSwitchTrackHeight / 2.0, kSwitchTrackHeight / 2.0);

    const int knob_y = track_rect.top() + (kSwitchTrackHeight - kSwitchKnobDiameter) / 2;
    int knob_x = track_rect.left() + 2;
    if (fully_checked)
        knob_x = track_rect.right() - kSwitchKnobDiameter - 1;
    else if (partially_checked)
        knob_x = track_rect.center().x() - kSwitchKnobDiameter / 2;
    if (rtl && !partially_checked)
        knob_x = fully_checked ? track_rect.left() + 2
                               : track_rect.right() - kSwitchKnobDiameter - 1;

    QColor knob = fully_checked || partially_checked
                      ? pal.color(QPalette::HighlightedText)
                      : pal.color(QPalette::ButtonText);
    knob.setAlpha(isEnabled() ? 205 : 105);
    painter.setPen(Qt::NoPen);
    painter.setBrush(knob);
    painter.drawEllipse(QRectF(knob_x, knob_y, kSwitchKnobDiameter, kSwitchKnobDiameter));
}

BglCaretButton::BglCaretButton(QWidget *parent)
    : QToolButton(parent)
{
    setAutoRaise(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(18, 20);
    setToolButtonStyle(Qt::ToolButtonIconOnly);
}

QSize BglCaretButton::sizeHint() const
{
    return QSize(18, 20);
}

void BglCaretButton::setCaretState(int state)
{
    state = std::clamp(state, 0, 2);
    if (state_ == state)
        return;
    state_ = state;
    update();
    emit caretStateChanged(state_);
}

void BglCaretButton::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    if (underMouse() && isEnabled()) {
        QColor hover = palette().color(QPalette::Button);
        hover.setAlpha(90);
        painter.fillRect(rect().adjusted(1, 1, -1, -1), hover);
    }
    QColor caret = palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                   QPalette::WindowText);
    caret.setAlpha(isEnabled() ? 175 : 85);
    draw_caret(painter, rect(), state_, caret);
}

BglAngleControl::BglAngleControl(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::CrossCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setAccessibleName(tr("Direction"));
    setToolTip(tr("Drag to set direction; use the numeric field for an exact angle"));
}

void BglAngleControl::setRange(double minimum, double maximum)
{
    if (minimum > maximum)
        std::swap(minimum, maximum);
    minimum_ = minimum;
    maximum_ = maximum;
    setValue(value_);
}

void BglAngleControl::setSingleStep(double step)
{
    step_ = std::max(0.01, std::abs(step));
}

QSize BglAngleControl::sizeHint() const
{
    return QSize(38, 38);
}

QSize BglAngleControl::minimumSizeHint() const
{
    return QSize(30, 30);
}

void BglAngleControl::setValue(double value)
{
    const double bounded = std::clamp(value, minimum_, maximum_);
    if (std::abs(value_ - bounded) < 0.000001)
        return;
    value_ = bounded;
    update();
    emit valueChanged(value_);
}

void BglAngleControl::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPalette pal = palette();
    const qreal side = std::min(width(), height()) - 5.0;
    const QRectF circle((width() - side) / 2.0, (height() - side) / 2.0, side, side);
    const QPointF center = circle.center();
    const qreal radius = circle.width() / 2.0;

    QColor fill = pal.color(QPalette::Button);
    if (underMouse() && isEnabled())
        fill = fill.lightness() < 128 ? fill.lighter(114) : fill.darker(105);
    painter.setBrush(fill);
    painter.setPen(QPen(hasFocus() ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid),
                        hasFocus() ? 2.0 : 1.0));
    painter.drawEllipse(circle);

    painter.setPen(QPen(pal.color(QPalette::Mid), 1.0));
    for (int angle = 0; angle < 360; angle += 45) {
        const double radians = angle * kPi / 180.0;
        const QPointF outer(center.x() + std::cos(radians) * (radius - 3.0),
                            center.y() + std::sin(radians) * (radius - 3.0));
        const QPointF inner(center.x() + std::cos(radians) * (radius - (angle % 90 == 0 ? 7.0 : 5.0)),
                            center.y() + std::sin(radians) * (radius - (angle % 90 == 0 ? 7.0 : 5.0)));
        painter.drawLine(inner, outer);
    }

    const double normalized = std::fmod(std::fmod(value_, 360.0) + 360.0, 360.0);
    const double radians = normalized * kPi / 180.0;
    const QPointF tip(center.x() + std::cos(radians) * (radius - 7.0),
                      center.y() + std::sin(radians) * (radius - 7.0));
    const QColor accent = isEnabled() ? pal.color(QPalette::Highlight)
                                      : pal.color(QPalette::Disabled, QPalette::Text);
    painter.setPen(QPen(accent, 2.4, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(center, tip);
    painter.setPen(Qt::NoPen);
    painter.setBrush(accent);
    painter.drawEllipse(center, 2.6, 2.6);
    painter.drawEllipse(tip, 3.2, 3.2);
}

double BglAngleControl::nearestEquivalent(double normalized) const
{
    double candidate = normalized;
    while (candidate - value_ > 180.0)
        candidate -= 360.0;
    while (value_ - candidate > 180.0)
        candidate += 360.0;
    while (candidate < minimum_ && candidate + 360.0 <= maximum_)
        candidate += 360.0;
    while (candidate > maximum_ && candidate - 360.0 >= minimum_)
        candidate -= 360.0;
    return std::clamp(candidate, minimum_, maximum_);
}

void BglAngleControl::setValueFromPosition(const QPointF &position)
{
    const QPointF delta = position - rect().center();
    if (std::abs(delta.x()) < 0.001 && std::abs(delta.y()) < 0.001)
        return;
    double angle = std::atan2(delta.y(), delta.x()) * 180.0 / kPi;
    if (angle < 0.0)
        angle += 360.0;
    setValue(nearestEquivalent(angle));
}

void BglAngleControl::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && isEnabled()) {
        dragging_ = true;
        setFocus(Qt::MouseFocusReason);
        setValueFromPosition(mouse_position(event));
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void BglAngleControl::mouseMoveEvent(QMouseEvent *event)
{
    if (dragging_) {
        setValueFromPosition(mouse_position(event));
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void BglAngleControl::mouseReleaseEvent(QMouseEvent *event)
{
    if (dragging_ && event->button() == Qt::LeftButton) {
        dragging_ = false;
        setValueFromPosition(mouse_position(event));
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void BglAngleControl::wheelEvent(QWheelEvent *event)
{
    if (!isEnabled()) {
        QWidget::wheelEvent(event);
        return;
    }
    const int direction = event->angleDelta().y() >= 0 ? 1 : -1;
    setValue(value_ + direction * step_);
    event->accept();
}

void BglAngleControl::keyPressEvent(QKeyEvent *event)
{
    const double multiplier = event->modifiers().testFlag(Qt::ShiftModifier) ? 10.0 : 1.0;
    if (event->key() == Qt::Key_Left || event->key() == Qt::Key_Down) {
        setValue(value_ - step_ * multiplier);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Right || event->key() == Qt::Key_Up) {
        setValue(value_ + step_ * multiplier);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Home) {
        setValue(0.0);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

class BglCollapsiblePanel::Header final : public QWidget {
public:
    explicit Header(BglCollapsiblePanel *owner)
        : QWidget(owner), owner_(owner)
    {
        setFixedHeight(kPanelHeaderHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        setAccessibleName(owner_->title_);
        setToolTip(tr("Click to collapse or expand. Drag the handle to reorder."));
        setProperty("bglPanelAddress",
                    QVariant::fromValue<qulonglong>(reinterpret_cast<quintptr>(owner_)));

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(kPanelHandleWidth + 5, 3, kPanelCaretWidth + 3, 3);
        layout->setSpacing(4);
        owner_->header_leading_ = new QHBoxLayout();
        owner_->header_leading_->setContentsMargins(0, 0, 0, 0);
        owner_->header_leading_->setSpacing(3);
        layout->addLayout(owner_->header_leading_);
        layout->addStretch(1);
        owner_->header_actions_ = new QHBoxLayout();
        owner_->header_actions_->setContentsMargins(0, 0, 0, 0);
        owner_->header_actions_->setSpacing(3);
        layout->addLayout(owner_->header_actions_);
    }

    QRect handleRect() const { return QRect(2, 2, kPanelHandleWidth - 3, height() - 4); }
    QRect caretRect() const { return QRect(width() - kPanelCaretWidth, 0, kPanelCaretWidth, height()); }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPalette pal = palette();

        QColor header = blended(pal.color(QPalette::Window), pal.color(QPalette::Button), 2, 1);
        if (underMouse())
            header = header.lightness() < 128 ? header.lighter(106) : header.darker(102);
        painter.fillRect(rect(), header);
        QColor accent = pal.color(QPalette::Highlight);
        accent.setAlpha(155);
        painter.fillRect(QRect(0, 0, width(), 2), accent);

        QColor border = pal.color(QPalette::Mid);
        border.setAlpha(155);
        painter.fillRect(QRect(0, height() - 1, width(), 1), border);

        QColor handle_color = pal.color(QPalette::WindowText);
        handle_color.setAlpha(handle_hovered_ || drag_armed_ ? 185 : 92);
        painter.setPen(Qt::NoPen);
        painter.setBrush(handle_color);
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 2; ++column)
                painter.drawEllipse(QPointF(8.0 + column * 5.0, 9.0 + row * 5.0), 1.25, 1.25);
        }

        const int action_left = owner_->header_actions_ && owner_->header_actions_->count() > 0
                                    ? owner_->header_actions_->geometry().left() - 6
                                    : width() - kPanelCaretWidth - 4;
        const int title_left = owner_->header_leading_ && owner_->header_leading_->count() > 0
                                   ? owner_->header_leading_->geometry().right() + 6
                                   : kPanelHandleWidth + 4;
        QRect title_rect(title_left, 0, std::max(0, action_left - title_left), height());
        QFont title_font = font();
        title_font.setBold(true);
        painter.setFont(title_font);
        QColor title_color = pal.color(QPalette::WindowText);
        title_color.setAlpha(225);
        painter.setPen(title_color);
        painter.drawText(title_rect, Qt::AlignLeft | Qt::AlignVCenter,
                         QFontMetrics(title_font).elidedText(owner_->title_, Qt::ElideRight,
                                                             title_rect.width()));

        QColor caret_color = pal.color(QPalette::WindowText);
        caret_color.setAlpha(175);
        draw_caret(painter, caretRect(), owner_->expanded_ ? 2 : 0, caret_color);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            press_position_ = mouse_position(event).toPoint();
            drag_armed_ = handleRect().contains(press_position_);
            toggle_armed_ = !drag_armed_;
            if (drag_armed_)
                setCursor(Qt::ClosedHandCursor);
            update();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPoint position = mouse_position(event).toPoint();
        const bool hovered = handleRect().contains(position);
        if (handle_hovered_ != hovered) {
            handle_hovered_ = hovered;
            if (!drag_armed_)
                setCursor(handle_hovered_ ? Qt::OpenHandCursor : Qt::PointingHandCursor);
            update();
        }
        if (drag_armed_ && (position - press_position_).manhattanLength() >= QApplication::startDragDistance()) {
            drag_armed_ = false;
            setCursor(Qt::OpenHandCursor);
            owner_->beginDrag();
            update();
            event->accept();
            return;
        }
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            const QPoint position = mouse_position(event).toPoint();
            const bool toggle = toggle_armed_ &&
                                (position - press_position_).manhattanLength() < QApplication::startDragDistance();
            drag_armed_ = false;
            toggle_armed_ = false;
            setCursor(handleRect().contains(position) ? Qt::OpenHandCursor : Qt::PointingHandCursor);
            update();
            if (toggle) {
                emit owner_->activated();
                owner_->toggleExpanded();
            }
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        handle_hovered_ = false;
        if (!drag_armed_)
            setCursor(Qt::PointingHandCursor);
        update();
        QWidget::leaveEvent(event);
    }

private:
    BglCollapsiblePanel *owner_ = nullptr;
    QPoint press_position_;
    bool drag_armed_ = false;
    bool toggle_armed_ = false;
    bool handle_hovered_ = false;
};

BglCollapsiblePanel::BglCollapsiblePanel(const QString &title, QWidget *content,
                                         QWidget *parent)
    : QWidget(parent), title_(title), content_(content)
{
    setAcceptDrops(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setAttribute(Qt::WA_StyledBackground, false);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    header_ = new Header(this);
    layout->addWidget(header_);

    body_ = new QWidget(this);
    body_->setObjectName(QStringLiteral("BglCollapsiblePanelBody"));
    auto *body_layout = new QVBoxLayout(body_);
    body_layout->setContentsMargins(0, 0, 0, 0);
    body_layout->setSpacing(0);
    layout->addWidget(body_);

    if (content_) {
        content_->setParent(body_);
        content_->installEventFilter(this);
        content_->setProperty("bglCollapsiblePanelAddress",
                              QVariant::fromValue<qulonglong>(reinterpret_cast<quintptr>(this)));
        if (!content_->property("bglPreservePanelMargins").toBool() &&
            !qobject_cast<QAbstractScrollArea *>(content_.data()) && content_->layout()) {
            content_->layout()->setContentsMargins(kPanelContentLeft, kPanelContentTop,
                                                   kPanelContentRight, kPanelContentBottom);
        }
        if (auto *group = qobject_cast<QGroupBox *>(content_.data())) {
            group->setTitle(QString());
            group->setCheckable(false);
            group->setFlat(true);
            if (group->objectName().isEmpty())
                group->setObjectName(QStringLiteral("BglPanelContent_%1")
                                         .arg(reinterpret_cast<quintptr>(group), 0, 16));
            const QString selector = QStringLiteral("QGroupBox#%1{border:none;margin:0;padding:0;background:transparent;}"
                                                    "QGroupBox#%1::title{height:0;padding:0;margin:0;}")
                                         .arg(group->objectName());
            group->setStyleSheet(group->styleSheet() + selector);
        }
        body_layout->addWidget(content_);
    }

    drop_indicator_widget_ = new QWidget(this);
    drop_indicator_widget_->setAttribute(Qt::WA_TransparentForMouseEvents);
    drop_indicator_widget_->setAutoFillBackground(true);
    QPalette indicator_palette = drop_indicator_widget_->palette();
    indicator_palette.setColor(QPalette::Window, palette().color(QPalette::Highlight));
    drop_indicator_widget_->setPalette(indicator_palette);
    drop_indicator_widget_->hide();
}

void BglCollapsiblePanel::setTitle(const QString &title)
{
    if (title_ == title)
        return;
    title_ = title;
    if (header_) {
        header_->setAccessibleName(title_);
        header_->update();
    }
}

void BglCollapsiblePanel::addHeaderWidget(QWidget *widget)
{
    if (!widget || !header_actions_)
        return;
    widget->setParent(header_);
    widget->setMaximumHeight(kPanelHeaderHeight - 6);
    header_actions_->addWidget(widget, 0, Qt::AlignVCenter);
    header_->updateGeometry();
    header_->update();
}

void BglCollapsiblePanel::addHeaderLeadingWidget(QWidget *widget)
{
    if (!widget || !header_leading_)
        return;
    widget->setParent(header_);
    widget->setMaximumHeight(kPanelHeaderHeight - 6);
    header_leading_->addWidget(widget, 0, Qt::AlignVCenter);
    header_->updateGeometry();
    header_->update();
}

void BglCollapsiblePanel::setOrderPersistenceEnabled(bool enabled)
{
    persist_order_ = enabled;
    setProperty("bglPersistOrder", persist_order_);
}

void BglCollapsiblePanel::setPersistenceKey(const QString &group, const QString &key)
{
    persistence_group_ = normalized_panel_key(group);
    persistence_key_ = normalized_panel_key(key);
    setProperty("bglPersistOrder", persist_order_);
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("EditorPanels"));
    const QString setting = QStringLiteral("expanded/%1/%2")
                                .arg(persistence_group_, persistence_key_);
    if (settings.contains(setting)) {
        expanded_ = settings.value(setting, true).toBool();
        applyExpandedState();
    }
}

void BglCollapsiblePanel::saveExpandedState() const
{
    if (persistence_group_.isEmpty() || persistence_key_.isEmpty())
        return;
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("EditorPanels"));
    settings.setValue(QStringLiteral("expanded/%1/%2")
                          .arg(persistence_group_, persistence_key_), expanded_);
}

void BglCollapsiblePanel::setExpanded(bool expanded)
{
    if (expanded_ == expanded)
        return;

    int scroll_value = -1;
    QPointer<QScrollBar> scroll_bar;
    for (QWidget *ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        if (auto *area = qobject_cast<QAbstractScrollArea *>(ancestor)) {
            scroll_bar = area->verticalScrollBar();
            if (scroll_bar)
                scroll_value = scroll_bar->value();
            break;
        }
    }

    expanded_ = expanded;
    applyExpandedState();
    saveExpandedState();
    emit expandedChanged(expanded_);

    if (scroll_bar && scroll_value >= 0) {
        QTimer::singleShot(0, this, [scroll_bar, scroll_value]() {
            if (scroll_bar)
                scroll_bar->setValue(std::min(scroll_value, scroll_bar->maximum()));
        });
    }
}

void BglCollapsiblePanel::toggleExpanded()
{
    setExpanded(!expanded_);
}

void BglCollapsiblePanel::applyExpandedState()
{
    if (body_)
        body_->setVisible(expanded_);
    if (expanded_) {
        setMinimumHeight(0);
        setMaximumHeight(QWIDGETSIZE_MAX);
    } else {
        const int collapsed_height = kPanelHeaderHeight + 2;
        setMinimumHeight(collapsed_height);
        setMaximumHeight(collapsed_height);
    }
    if (header_)
        header_->update();
    updateGeometry();
    update();
}

void BglCollapsiblePanel::beginDrag()
{
    emit activated();
    QDrag drag(header_);
    auto *mime = new QMimeData();
    mime->setData(kPanelMimeType,
                  QByteArray::number(reinterpret_cast<quintptr>(this), 16));
    drag.setMimeData(mime);
    const QPixmap preview = header_->grab();
    drag.setPixmap(preview);
    drag.setHotSpot(QPoint(10, preview.height() / 2));
    drag.exec(Qt::MoveAction);
}

bool BglCollapsiblePanel::canAcceptPanel(const BglCollapsiblePanel *panel) const
{
    return panel && panel != this && panel->parentWidget() == parentWidget() &&
           parentWidget() && parentWidget()->layout() &&
           parentWidget()->layout()->indexOf(const_cast<BglCollapsiblePanel *>(panel)) >= 0 &&
           parentWidget()->layout()->indexOf(const_cast<BglCollapsiblePanel *>(this)) >= 0;
}

void BglCollapsiblePanel::setDropIndicator(int edge)
{
    if (drop_indicator_edge_ == edge)
        return;
    drop_indicator_edge_ = edge;
    if (!drop_indicator_widget_)
        return;
    if (edge == 0) {
        drop_indicator_widget_->hide();
        return;
    }
    const int y = edge < 0 ? 0 : std::max(0, height() - 3);
    drop_indicator_widget_->setGeometry(0, y, width(), 3);
    QPalette indicator_palette = drop_indicator_widget_->palette();
    indicator_palette.setColor(QPalette::Window, palette().color(QPalette::Highlight));
    drop_indicator_widget_->setPalette(indicator_palette);
    drop_indicator_widget_->show();
    drop_indicator_widget_->raise();
}

void BglCollapsiblePanel::movePanelHere(BglCollapsiblePanel *source, bool before)
{
    if (!canAcceptPanel(source))
        return;
    auto *box_layout = qobject_cast<QBoxLayout *>(parentWidget()->layout());
    if (!box_layout)
        return;

    const int source_index = box_layout->indexOf(source);
    const int target_index = box_layout->indexOf(this);
    if (source_index < 0 || target_index < 0)
        return;

    int insertion_index = target_index + (before ? 0 : 1);
    const int source_stretch = box_layout->stretch(source_index);
    QLayoutItem *source_item = box_layout->takeAt(source_index);
    if (!source_item)
        return;
    const Qt::Alignment source_alignment = source_item->alignment();
    if (source_index < insertion_index)
        --insertion_index;
    insertion_index = std::clamp(insertion_index, 0, box_layout->count());
    box_layout->insertWidget(insertion_index, source, source_stretch, source_alignment);
    delete source_item;
    if (source->persist_order_ && persist_order_ &&
        !source->persistence_group_.isEmpty() &&
        source->persistence_group_ == persistence_group_)
        save_panel_order(box_layout, persistence_group_);
    emit source->orderChanged();
    emit orderChanged();
}

void BglCollapsiblePanel::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPalette pal = palette();

    QColor body = pal.color(QPalette::Window);
    body = body.lightness() < 128 ? body.lighter(112) : body.darker(104);
    QColor border = pal.color(QPalette::Mid);
    border.setAlpha(165);

    painter.setPen(QPen(border, 1.0));
    painter.setBrush(body);
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 2.0, 2.0);

    QColor accent = pal.color(QPalette::Highlight);
    accent.setAlpha(155);
    painter.fillRect(QRect(1, 0, std::max(0, width() - 2), 2), accent);

}

void BglCollapsiblePanel::dragEnterEvent(QDragEnterEvent *event)
{
    BglCollapsiblePanel *source = panel_from_drag(event ? event->mimeData() : nullptr, event ? event->source() : nullptr);
    if (event && canAcceptPanel(source)) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    if (event)
        event->ignore();
}

void BglCollapsiblePanel::dragMoveEvent(QDragMoveEvent *event)
{
    BglCollapsiblePanel *source = panel_from_drag(event ? event->mimeData() : nullptr, event ? event->source() : nullptr);
    if (event && canAcceptPanel(source)) {
        setDropIndicator(drag_position(event).y() < height() / 2.0 ? -1 : 1);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    setDropIndicator(0);
    if (event)
        event->ignore();
}

void BglCollapsiblePanel::dragLeaveEvent(QDragLeaveEvent *event)
{
    setDropIndicator(0);
    if (event)
        event->accept();
}

void BglCollapsiblePanel::dropEvent(QDropEvent *event)
{
    BglCollapsiblePanel *source = panel_from_drag(event ? event->mimeData() : nullptr, event ? event->source() : nullptr);
    if (event && canAcceptPanel(source)) {
        const bool before = drop_position(event).y() < height() / 2.0;
        movePanelHere(source, before);
        setDropIndicator(0);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    setDropIndicator(0);
    if (event)
        event->ignore();
}

void BglCollapsiblePanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (drop_indicator_edge_ != 0 && drop_indicator_widget_) {
        const int y = drop_indicator_edge_ < 0 ? 0 : std::max(0, height() - 3);
        drop_indicator_widget_->setGeometry(0, y, width(), 3);
        drop_indicator_widget_->raise();
    }
}

bool BglCollapsiblePanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == content_.data() && event) {
        if ((event->type() == QEvent::Hide || event->type() == QEvent::HideToParent) &&
            content_ && content_->isHidden()) {
            setVisible(false);
        } else if ((event->type() == QEvent::Show || event->type() == QEvent::ShowToParent) &&
                   content_ && !content_->isHidden()) {
            setVisible(true);
        }
    }
    return QWidget::eventFilter(watched, event);
}

QWidget *bgl_make_angle_field(QDoubleSpinBox *spin_box, QWidget *parent,
                              BglAngleControl **angle_control)
{
    if (!spin_box)
        return nullptr;

    auto *field = new QWidget(parent);
    auto *layout = new QHBoxLayout(field);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);

    auto *dial = new BglAngleControl(field);
    dial->setRange(spin_box->minimum(), spin_box->maximum());
    dial->setSingleStep(spin_box->singleStep());
    dial->setValue(spin_box->value());
    dial->setFixedSize(36, 36);
    spin_box->setParent(field);
    spin_box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    layout->addWidget(dial, 0, Qt::AlignVCenter);
    layout->addWidget(spin_box, 1, Qt::AlignVCenter);

    QObject::connect(spin_box, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                     dial, [dial](double value) {
        const QSignalBlocker blocker(dial);
        dial->setValue(value);
    });
    QObject::connect(dial, &BglAngleControl::valueChanged,
                     spin_box, [spin_box](double value) {
        spin_box->setValue(value);
    });

    if (angle_control)
        *angle_control = dial;
    return field;
}

BglCollapsiblePanel *bgl_add_panel_section(QVBoxLayout *layout, QWidget *section,
                                           const QString &title,
                                           QWidget *header_widget,
                                           int insert_index)
{
    if (!layout || !section)
        return nullptr;

    if (BglCollapsiblePanel *existing = bgl_panel_for_content(section))
        return existing;

    const QString panel_title = inferred_panel_title(section, title);
    QWidget *parent = layout->parentWidget();
    const QString persistence_group = section->property("bglPanelPersistenceGroup").toString().isEmpty()
                                          ? panel_group_for(parent)
                                          : section->property("bglPanelPersistenceGroup").toString();
    const QString persistence_key = section->property("bglPanelPersistenceKey").toString().isEmpty()
                                        ? (section->objectName().isEmpty() ? panel_title : section->objectName())
                                        : section->property("bglPanelPersistenceKey").toString();
    auto *panel = new BglCollapsiblePanel(panel_title, section, parent);
    panel->setObjectName(QStringLiteral("BglCollapsiblePanel_%1")
                             .arg(normalized_panel_key(persistence_key)));
    const QVariant persist_order_property = section->property("bglPersistPanelOrder");
    panel->setOrderPersistenceEnabled(!persist_order_property.isValid() ||
                                      persist_order_property.toBool());
    panel->setPersistenceKey(persistence_group, persistence_key);

    if (!header_widget) {
        QObject *property_widget = section->property("bglPanelHeaderWidget").value<QObject *>();
        header_widget = qobject_cast<QWidget *>(property_widget);
    }
    if (header_widget)
        panel->addHeaderWidget(header_widget);

    panel->setVisible(!section->isHidden());
    if (insert_index >= 0)
        layout->insertWidget(std::min(insert_index, layout->count()), panel);
    else
        layout->addWidget(panel);
    if (panel->property("bglPersistOrder").toBool())
        restore_panel_order(layout, normalized_panel_key(persistence_group));
    return panel;
}

BglCollapsiblePanel *bgl_panel_for_content(QWidget *section)
{
    if (!section)
        return nullptr;
    bool ok = false;
    const quintptr address = section->property("bglCollapsiblePanelAddress").toULongLong(&ok);
    return ok ? reinterpret_cast<BglCollapsiblePanel *>(address) : nullptr;
}
