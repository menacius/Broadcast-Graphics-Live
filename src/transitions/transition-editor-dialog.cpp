#include "transition-editor-dialog.h"

#include "title-localization.h"
#include "transition-preset-catalog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFontMetricsF>
#include <QLabel>
#include <QHideEvent>
#include <QShowEvent>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QTimer>
#include <QVector>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace {

class NumericDragLabel final : public QLabel {
public:
    NumericDragLabel(const QString &text, QDoubleSpinBox *spin, QWidget *parent = nullptr)
        : QLabel(text, parent), spin_(spin)
    {
        if (!spin_)
            return;
        setToolTip(bgl_tr("OBSTitles.DragNumericLabelTooltip"));
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
        drag_start_value_ = spin_->value();
        grabMouse(Qt::SizeHorCursor);
        QApplication::setOverrideCursor(Qt::SizeHorCursor);
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
        const double next = drag_start_value_ + delta * spin_->singleStep();
        spin_->setValue(std::clamp(next, spin_->minimum(), spin_->maximum()));
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
    bool can_drag() const
    {
        return spin_ && spin_->isEnabled() && spin_->isVisible() && isEnabled();
    }

    void finish_drag()
    {
        dragging_ = false;
        releaseMouse();
        QApplication::restoreOverrideCursor();
    }

    QDoubleSpinBox *spin_ = nullptr;
    bool dragging_ = false;
    double drag_start_x_ = 0.0;
    double drag_start_value_ = 0.0;
};

class TransitionPreviewWidget final : public QWidget {
public:
    explicit TransitionPreviewWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(360, 190);
        // A 30 FPS preview is visually smooth enough for this small demo and
        // avoids driving an otherwise idle modal dialog at 60 repaints/sec.
        timer_.setInterval(33);
        connect(&timer_, &QTimer::timeout, this, [this]() {
            phase_ += (timer_.interval() / 1000.0) /
                      std::max(0.2, transition_.duration);
            if (phase_ > 1.35)
                phase_ = 0.0;
            update();
        });
    }

    void set_transition(const LayerTransition &transition)
    {
        transition_ = transition;
        update();
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        timer_.start();
    }

    void hideEvent(QHideEvent *event) override
    {
        timer_.stop();
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF area = rect().adjusted(12, 12, -12, -12);
        painter.fillRect(rect(), palette().color(QPalette::Base));
        painter.setPen(QPen(palette().color(QPalette::Mid), 1));
        painter.drawRoundedRect(area, 5, 5);

        const double raw = std::clamp(phase_, 0.0, 1.0);
        if (transition_.kind == LayerTransitionKind::Text) {
            draw_text_preview(painter, area, raw);
        } else {
            draw_general_preview(painter, area, raw);
        }
    }

private:
    void draw_general_preview(QPainter &painter, const QRectF &area, double phase)
    {
        const QRectF frame = area.adjusted(14, 14, -14, -14);
        painter.save();
        painter.setClipRect(frame);
        painter.fillRect(frame, QColor(45, 72, 118));
        painter.setPen(Qt::white);
        QFont font = painter.font();
        font.setPixelSize(58);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(frame, Qt::AlignCenter, QStringLiteral("A"));

        LayerTransition copy = transition_;
        copy.duration = 1.0;
        const LayerTransitionVisualState state = evaluate_general_layer_transition(copy, 0.0, 1.0, phase);
        painter.setOpacity(std::clamp(state.opacity, 0.0, 1.0));
        painter.translate(state.translate_x * 0.5, state.translate_y * 0.5);
        const QPointF center = frame.center();
        painter.translate(center);
        painter.scale(state.scale, state.scale);
        painter.translate(-center);

        if (state.wipe < 0.999) {
            QRectF reveal = frame;
            switch (state.wipe_direction) {
            case LayerTransitionDirection::Right:
                reveal.setLeft(frame.right() - frame.width() * state.wipe);
                break;
            case LayerTransitionDirection::Up:
                reveal.setTop(frame.bottom() - frame.height() * state.wipe);
                break;
            case LayerTransitionDirection::Down:
                reveal.setHeight(frame.height() * state.wipe);
                break;
            case LayerTransitionDirection::Left:
            case LayerTransitionDirection::None:
            default:
                reveal.setWidth(frame.width() * state.wipe);
                break;
            }
            painter.setClipRect(reveal, Qt::IntersectClip);
        }

        auto draw_b = [&](const QPointF &offset, double opacity) {
            painter.save();
            painter.translate(offset);
            painter.setOpacity(std::clamp(state.opacity * opacity, 0.0, 1.0));
            painter.fillRect(frame, QColor(134, 65, 93));
            painter.setPen(Qt::white);
            painter.drawText(frame, Qt::AlignCenter, QStringLiteral("B"));
            painter.restore();
        };
        if (state.blur > 0.01) {
            const double radius = std::clamp(state.blur * 0.18, 1.0, 8.0);
            for (const QPointF &offset : {QPointF(-radius, 0.0), QPointF(radius, 0.0),
                                          QPointF(0.0, -radius), QPointF(0.0, radius),
                                          QPointF(-radius * 0.7, -radius * 0.7),
                                          QPointF(radius * 0.7, radius * 0.7)})
                draw_b(offset, 0.11);
        }
        draw_b(QPointF(), 1.0);
        painter.restore();
    }

    void draw_text_preview(QPainter &painter, const QRectF &area, double phase)
    {
        const QString text = QStringLiteral("Abc De");
        QFont font = painter.font();
        font.setPixelSize(48);
        font.setBold(true);
        painter.setFont(font);
        QFontMetricsF metrics(font);
        const qreal total_width = metrics.horizontalAdvance(text);
        qreal x = area.center().x() - total_width / 2.0;
        const qreal baseline = area.center().y() + metrics.ascent() / 2.0;

        // Keep unit positions anchored to advances from the complete string.
        // Summing isolated glyph widths loses pair kerning (for example A/V),
        // which made the preview disagree with the renderer.
        QVector<QPair<int, int>> unit_ranges;
        if (transition_.unit == LayerTransitionUnit::Word) {
            unit_ranges = {{0, 3}, {3, 1}, {4, 2}};
        } else if (transition_.unit == LayerTransitionUnit::Sentence) {
            unit_ranges = {{0, text.size()}};
        } else {
            for (int i = 0; i < text.size(); ++i)
                unit_ranges.push_back({i, 1});
        }

        int animated_count = 0;
        for (const auto &range : unit_ranges) {
            if (!text.mid(range.first, range.second).trimmed().isEmpty())
                ++animated_count;
        }
        int animated_index = 0;
        const qreal text_left = x;
        for (const auto &range : unit_ranges) {
            const QString unit = text.mid(range.first, range.second);
            const qreal unit_left = text_left + metrics.horizontalAdvance(text.left(range.first));
            const qreal unit_right = text_left + metrics.horizontalAdvance(
                text.left(range.first + range.second));
            x = unit_left;
            const qreal width = std::max<qreal>(0.0, unit_right - unit_left);
            if (unit.trimmed().isEmpty())
                continue;
            int order = transition_.reverse_order ? animated_count - 1 - animated_index : animated_index;
            const double delay = animated_count <= 1 ? 0.0 : transition_.stagger * order / (animated_count - 1.0);
            const double global_progress = layer_transition_ease(
                transition_.edge == LayerTransitionEdge::In ? phase : 1.0 - phase,
                transition_.easing);
            const double span = std::max(0.05, 1.0 - transition_.stagger);
            double local = transition_.edge == LayerTransitionEdge::In
                ? std::clamp((global_progress - delay) / span, 0.0, 1.0)
                : 1.0 - std::clamp(((1.0 - global_progress) - delay) / span, 0.0, 1.0);
            local = layer_transition_ease(local, transition_.easing);
            const double hidden = 1.0 - local;
            double dx = 0.0, dy = 0.0;
            if (transition_.type == LayerTransitionType::TextSlide ||
                transition_.type == LayerTransitionType::TextBlurSlide) {
                switch (transition_.direction) {
                case LayerTransitionDirection::Right: dx = transition_.offset * hidden * 0.45; break;
                case LayerTransitionDirection::Up: dy = -transition_.offset * hidden * 0.45; break;
                case LayerTransitionDirection::Down: dy = transition_.offset * hidden * 0.45; break;
                case LayerTransitionDirection::Left:
                case LayerTransitionDirection::None:
                default: dx = -transition_.offset * hidden * 0.45; break;
                }
            }
            const double scale = transition_.type == LayerTransitionType::TextScale
                ? transition_.scale_from + (1.0 - transition_.scale_from) * local : 1.0;
            painter.save();
            painter.setOpacity(local);
            QRectF unit_clip(x, baseline - metrics.ascent(), width, metrics.height());
            if (transition_.type == LayerTransitionType::TextWipe && local < 0.999) {
                switch (transition_.direction) {
                case LayerTransitionDirection::Right:
                    unit_clip.setLeft(unit_clip.right() - unit_clip.width() * local);
                    break;
                case LayerTransitionDirection::Up:
                    unit_clip.setTop(unit_clip.bottom() - unit_clip.height() * local);
                    break;
                case LayerTransitionDirection::Down:
                    unit_clip.setHeight(unit_clip.height() * local);
                    break;
                case LayerTransitionDirection::Left:
                case LayerTransitionDirection::None:
                default:
                    unit_clip.setWidth(unit_clip.width() * local);
                    break;
                }
                painter.setClipRect(unit_clip, Qt::IntersectClip);
            }
            const QPointF center(x + width / 2.0, baseline - metrics.ascent() / 2.0);
            painter.translate(center.x() + dx, center.y() + dy);
            painter.scale(scale, scale);
            painter.translate(-center.x(), -center.y());
            painter.setPen(palette().color(QPalette::Text));
            bool drew_blurred_unit = false;
            if ((transition_.type == LayerTransitionType::TextBlur ||
                 transition_.type == LayerTransitionType::TextBlurSlide) && hidden > 0.001) {
                // Preview the same radius-driven behavior as the renderer: the
                // unit is represented by a progressively smaller blur, not by
                // sharp text with a low-opacity halo drawn around it.
                const double radius = std::clamp(
                    transition_.blur_amount * hidden * 0.16, 0.5, 6.0);
                const double diagonal = radius * 0.70710678118;
                const QVector<QPointF> samples = {
                    QPointF(0.0, 0.0),
                    QPointF(-radius, 0.0), QPointF(radius, 0.0),
                    QPointF(0.0, -radius), QPointF(0.0, radius),
                    QPointF(-diagonal, -diagonal), QPointF(diagonal, -diagonal),
                    QPointF(-diagonal, diagonal), QPointF(diagonal, diagonal),
                };
                painter.save();
                painter.setOpacity(local / static_cast<double>(samples.size()));
                for (const QPointF &offset : samples)
                    painter.drawText(QPointF(x, baseline) + offset, unit);
                painter.restore();
                drew_blurred_unit = true;
            }
            if (!drew_blurred_unit)
                painter.drawText(QPointF(x, baseline), unit);
            painter.restore();
            ++animated_index;
        }
    }

    QTimer timer_;
    LayerTransition transition_;
    double phase_ = 0.0;
};

QComboBox *make_combo(QWidget *parent)
{
    auto *combo = new QComboBox(parent);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    return combo;
}

QLabel *make_numeric_drag_label(const QString &text, QDoubleSpinBox *spin, QWidget *parent)
{
    return new NumericDragLabel(text, spin, parent);
}

QString transition_settings_display_name(const LayerTransition &transition)
{
    QString name = QString::fromStdString(transition.display_name);
    if (name.endsWith(QStringLiteral(" In"), Qt::CaseInsensitive))
        name = name.left(name.size() - 3) + QStringLiteral(" In/Out");
    return name;
}

} // namespace

TransitionEditorDialog::TransitionEditorDialog(const LayerTransition &transition,
                                               double maximum_duration,
                                               QWidget *parent)
    : QDialog(parent), transition_(transition)
{
    const QString display_name = transition_settings_display_name(transition);
    setWindowTitle(bgl_tr("OBSTitles.TransitionSettingsNamed").arg(display_name));
    setModal(true);
    resize(470, 560);

    auto *layout = new QVBoxLayout(this);
    const QString safe_display_name = display_name.toHtmlEscaped();
    auto *title = new QLabel(QStringLiteral("<b>%1</b>").arg(safe_display_name), this);
    layout->addWidget(title);

    preview_ = new TransitionPreviewWidget(this);
    static_cast<TransitionPreviewWidget *>(preview_)->set_transition(transition_);
    layout->addWidget(preview_);

    auto *form = new QFormLayout;
    enabled_ = new QCheckBox(bgl_tr("OBSTitles.Enabled"), this);
    enabled_->setChecked(transition.enabled);
    form->addRow(QString(), enabled_);

    duration_ = new QDoubleSpinBox(this);
    duration_->setRange(1.0 / 240.0, std::max(1.0 / 240.0, maximum_duration));
    duration_->setDecimals(3);
    duration_->setSingleStep(0.05);
    duration_->setSuffix(QStringLiteral(" s"));
    duration_->setValue(std::clamp(transition.duration, duration_->minimum(), duration_->maximum()));
    form->addRow(make_numeric_drag_label(bgl_tr("OBSTitles.Duration"), duration_, this), duration_);

    easing_ = make_combo(this);
    easing_->addItem(bgl_tr("OBSTitles.Linear"), (int)EasingType::Linear);
    easing_->addItem(bgl_tr("OBSTitles.EaseIn"), (int)EasingType::EaseIn);
    easing_->addItem(bgl_tr("OBSTitles.EaseOut"), (int)EasingType::EaseOut);
    easing_->addItem(bgl_tr("OBSTitles.EaseInOut"), (int)EasingType::EaseInOut);
    easing_->setCurrentIndex(std::max(0, easing_->findData((int)transition.easing)));
    form->addRow(bgl_tr("OBSTitles.Easing"), easing_);

    unit_label_ = new QLabel(bgl_tr("OBSTitles.TextUnit"), this);
    unit_ = make_combo(this);
    unit_->addItem(bgl_tr("OBSTitles.Characters"), (int)LayerTransitionUnit::Character);
    unit_->addItem(bgl_tr("OBSTitles.Words"), (int)LayerTransitionUnit::Word);
    unit_->addItem(bgl_tr("OBSTitles.Sentences"), (int)LayerTransitionUnit::Sentence);
    unit_->setCurrentIndex(std::max(0, unit_->findData((int)transition.unit)));
    form->addRow(unit_label_, unit_);

    direction_label_ = new QLabel(bgl_tr("OBSTitles.Direction"), this);
    direction_ = make_combo(this);
    direction_->addItem(bgl_tr("OBSTitles.Left"), (int)LayerTransitionDirection::Left);
    direction_->addItem(bgl_tr("OBSTitles.Right"), (int)LayerTransitionDirection::Right);
    direction_->addItem(bgl_tr("OBSTitles.Up"), (int)LayerTransitionDirection::Up);
    direction_->addItem(bgl_tr("OBSTitles.Down"), (int)LayerTransitionDirection::Down);
    direction_->setCurrentIndex(std::max(0, direction_->findData((int)transition.direction)));
    form->addRow(direction_label_, direction_);

    stagger_ = new QDoubleSpinBox(this);
    stagger_->setRange(0.0, 95.0);
    stagger_->setSuffix(QStringLiteral(" %"));
    stagger_->setValue(transition.stagger * 100.0);
    stagger_label_ = make_numeric_drag_label(bgl_tr("OBSTitles.Stagger"), stagger_, this);
    form->addRow(stagger_label_, stagger_);

    blur_ = new QDoubleSpinBox(this);
    blur_->setRange(0.0, 256.0);
    blur_->setSuffix(QStringLiteral(" px"));
    blur_->setValue(transition.blur_amount);
    blur_label_ = make_numeric_drag_label(bgl_tr("OBSTitles.BlurAmount"), blur_, this);
    form->addRow(blur_label_, blur_);

    scale_ = new QDoubleSpinBox(this);
    scale_->setRange(-1000.0, 1000.0);
    scale_->setSuffix(QStringLiteral(" %"));
    scale_->setValue(transition.scale_from * 100.0);
    scale_label_ = make_numeric_drag_label(bgl_tr("OBSTitles.StartScale"), scale_, this);
    form->addRow(scale_label_, scale_);

    offset_ = new QDoubleSpinBox(this);
    offset_->setRange(0.0, 10000.0);
    offset_->setSuffix(QStringLiteral(" px"));
    offset_->setValue(transition.offset);
    offset_label_ = make_numeric_drag_label(bgl_tr("OBSTitles.Offset"), offset_, this);
    form->addRow(offset_label_, offset_);

    softness_ = new QDoubleSpinBox(this);
    softness_->setRange(0.0, 100.0);
    softness_->setSuffix(QStringLiteral(" %"));
    softness_->setValue(transition.softness * 100.0);
    softness_label_ = make_numeric_drag_label(bgl_tr("OBSTitles.Softness"), softness_, this);
    form->addRow(softness_label_, softness_);

    reverse_order_ = new QCheckBox(bgl_tr("OBSTitles.ReverseOrder"), this);
    reverse_order_->setChecked(transition.reverse_order);
    form->addRow(QString(), reverse_order_);
    layout->addLayout(form);

    auto update = [this]() {
        update_model_from_controls();
        update_control_visibility();
        static_cast<TransitionPreviewWidget *>(preview_)->set_transition(transition_);
    };
    connect(enabled_, &QCheckBox::toggled, this, update);
    connect(duration_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [update](double) { update(); });
    connect(easing_, qOverload<int>(&QComboBox::currentIndexChanged), this, [update](int) { update(); });
    connect(unit_, qOverload<int>(&QComboBox::currentIndexChanged), this, [update](int) { update(); });
    connect(direction_, qOverload<int>(&QComboBox::currentIndexChanged), this, [update](int) { update(); });
    connect(stagger_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [update](double) { update(); });
    connect(blur_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [update](double) { update(); });
    connect(scale_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [update](double) { update(); });
    connect(offset_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [update](double) { update(); });
    connect(softness_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [update](double) { update(); });
    connect(reverse_order_, &QCheckBox::toggled, this, update);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    update_control_visibility();
}

LayerTransition TransitionEditorDialog::transition() const
{
    return transition_;
}

void TransitionEditorDialog::update_model_from_controls()
{
    transition_.enabled = enabled_->isChecked();
    transition_.duration = duration_->value();
    transition_.easing = static_cast<EasingType>(easing_->currentData().toInt());
    transition_.unit = static_cast<LayerTransitionUnit>(unit_->currentData().toInt());
    transition_.direction = static_cast<LayerTransitionDirection>(direction_->currentData().toInt());
    transition_.stagger = stagger_->value() / 100.0;
    transition_.blur_amount = blur_->value();
    transition_.scale_from = scale_->value() / 100.0;
    transition_.offset = offset_->value();
    transition_.softness = softness_->value() / 100.0;
    transition_.reverse_order = reverse_order_->isChecked();
}

void TransitionEditorDialog::update_control_visibility()
{
    const bool text = transition_.kind == LayerTransitionKind::Text;
    const bool has_direction = transition_.type == LayerTransitionType::Wipe ||
                               transition_.type == LayerTransitionType::Slide ||
                               transition_.type == LayerTransitionType::TextSlide ||
                               transition_.type == LayerTransitionType::BlurSlide ||
                               transition_.type == LayerTransitionType::TextBlurSlide ||
                               transition_.type == LayerTransitionType::TextWipe;
    const bool has_blur = transition_.type == LayerTransitionType::OpacityBlur ||
                          transition_.type == LayerTransitionType::ZoomBlur ||
                          transition_.type == LayerTransitionType::BlurSlide ||
                          transition_.type == LayerTransitionType::TextBlur ||
                          transition_.type == LayerTransitionType::TextBlurSlide;
    const bool has_scale = transition_.type == LayerTransitionType::Scale ||
                           transition_.type == LayerTransitionType::ZoomBlur ||
                           transition_.type == LayerTransitionType::TextScale;
    const bool has_offset = transition_.type == LayerTransitionType::Slide ||
                            transition_.type == LayerTransitionType::BlurSlide ||
                            transition_.type == LayerTransitionType::TextSlide ||
                            transition_.type == LayerTransitionType::TextBlurSlide;
    const bool has_softness = transition_.type == LayerTransitionType::Wipe ||
                              transition_.type == LayerTransitionType::TextWipe;

    for (QWidget *widget : std::initializer_list<QWidget *>{unit_label_, unit_, stagger_label_, stagger_, reverse_order_})
        widget->setVisible(text);
    for (QWidget *widget : std::initializer_list<QWidget *>{direction_label_, direction_}) widget->setVisible(has_direction);
    for (QWidget *widget : std::initializer_list<QWidget *>{blur_label_, blur_}) widget->setVisible(has_blur);
    for (QWidget *widget : std::initializer_list<QWidget *>{scale_label_, scale_}) widget->setVisible(has_scale);
    for (QWidget *widget : std::initializer_list<QWidget *>{offset_label_, offset_}) widget->setVisible(has_offset);
    for (QWidget *widget : std::initializer_list<QWidget *>{softness_label_, softness_}) widget->setVisible(has_softness);
}
