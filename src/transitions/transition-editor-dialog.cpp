#include "transition-editor-dialog.h"

#include "title-localization.h"
#include "transition-preset-catalog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QImage>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFontMetricsF>
#include <QLabel>
#include <QLineEdit>
#include <QHideEvent>
#include <QShowEvent>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QPushButton>
#include <QSpinBox>
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
    static bool is_procedural_transition(LayerTransitionType type)
    {
        return type == LayerTransitionType::Blocks ||
               type == LayerTransitionType::ImageWipe ||
               type == LayerTransitionType::Clock ||
               type == LayerTransitionType::Iris ||
               type == LayerTransitionType::GradientWipe;
    }

    static double preview_smoothstep(double edge0, double edge1, double value)
    {
        if (edge1 <= edge0)
            return value >= edge1 ? 1.0 : 0.0;
        const double t = std::clamp((value - edge0) / (edge1 - edge0), 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }

    static double preview_hash(int x, int y, int seed)
    {
        const double value = std::sin((x * 127.1 + y * 311.7 + seed * 74.7)) * 43758.5453123;
        return value - std::floor(value);
    }

    QImage procedural_preview_mask(const QSize &size,
                                   const LayerTransitionVisualState &state) const
    {
        QImage mask(size, QImage::Format_ARGB32_Premultiplied);
        mask.fill(Qt::transparent);
        if (size.width() <= 0 || size.height() <= 0)
            return mask;

        QImage matte;
        if (state.type == LayerTransitionType::ImageWipe && !state.image_path.empty()) {
            matte.load(QString::fromStdString(state.image_path));
            if (!matte.isNull())
                matte = matte.convertToFormat(QImage::Format_ARGB32)
                             .scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }

        const double reveal = std::clamp(state.wipe, 0.0, 1.0);
        const double feather = std::max(0.0001, state.wipe_softness);
        const double radians = state.rotation * 0.017453292519943295;
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        constexpr double kPi = 3.14159265358979323846;

        for (int y = 0; y < size.height(); ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(mask.scanLine(y));
            for (int x = 0; x < size.width(); ++x) {
                const double u = (x + 0.5) / std::max(1, size.width());
                const double v = (y + 0.5) / std::max(1, size.height());
                double threshold = u;

                switch (state.type) {
                case LayerTransitionType::Blocks: {
                    const int columns = std::max(1, state.blocks_columns);
                    const int rows = std::max(1, state.blocks_rows);
                    const int cell_x = std::clamp(static_cast<int>(u * columns), 0, columns - 1);
                    const int cell_y = std::clamp(static_cast<int>(v * rows), 0, rows - 1);
                    threshold = preview_hash(cell_x, cell_y, state.random_seed);
                    break;
                }
                case LayerTransitionType::ImageWipe: {
                    if (!matte.isNull()) {
                        const QRgb pixel = matte.pixel(std::clamp(x, 0, matte.width() - 1),
                                                       std::clamp(y, 0, matte.height() - 1));
                        switch (state.image_channel) {
                        case 1: threshold = qAlpha(pixel) / 255.0; break;
                        case 2: threshold = qRed(pixel) / 255.0; break;
                        case 3: threshold = qGreen(pixel) / 255.0; break;
                        case 4: threshold = qBlue(pixel) / 255.0; break;
                        case 0:
                        default:
                            threshold = (0.2126 * qRed(pixel) + 0.7152 * qGreen(pixel) +
                                         0.0722 * qBlue(pixel)) / 255.0;
                            break;
                        }
                    }
                    break;
                }
                case LayerTransitionType::Clock: {
                    const double px = u - state.center_x;
                    const double py = v - state.center_y;
                    threshold = std::fmod(std::atan2(py, px) / (2.0 * kPi) + 0.5 +
                                           state.rotation / 360.0 + 1.0, 1.0);
                    if (!state.clockwise)
                        threshold = 1.0 - threshold;
                    break;
                }
                case LayerTransitionType::Iris: {
                    double px = (u - state.center_x) * 2.0;
                    const double py = (v - state.center_y) * 2.0;
                    px *= std::max(0.01, state.aspect);
                    if (state.profile == 1)
                        threshold = std::max(std::abs(px), std::abs(py));
                    else if (state.profile == 2)
                        threshold = (std::abs(px) + std::abs(py)) * 0.5;
                    else
                        threshold = std::sqrt(px * px + py * py) / 1.41421356237;
                    threshold = std::clamp(threshold, 0.0, 1.0);
                    break;
                }
                case LayerTransitionType::GradientWipe: {
                    const double px = u - state.center_x;
                    const double py = v - state.center_y;
                    const double qx = (px * cosine - py * sine) * std::max(0.01, state.aspect);
                    const double qy = px * sine + py * cosine;
                    if (state.profile == 1)
                        threshold = std::sqrt(qx * qx + qy * qy) * 1.41421356237;
                    else if (state.profile == 2)
                        threshold = std::abs(qx) + std::abs(qy);
                    else
                        threshold = qx + 0.5;
                    threshold = std::clamp(threshold, 0.0, 1.0);
                    break;
                }
                default:
                    break;
                }

                if (state.invert)
                    threshold = 1.0 - threshold;
                const double alpha = preview_smoothstep(threshold - feather,
                                                        threshold + feather,
                                                        reveal);
                row[x] = qRgba(255, 255, 255,
                               static_cast<int>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255.0)));
            }
        }
        return mask;
    }

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

        const bool procedural = is_procedural_transition(state.type);
        if (!procedural && state.wipe < 0.999) {
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

        auto paint_b = [&](QPainter &target, const QPointF &offset, double opacity) {
            target.save();
            target.translate(offset);
            target.setOpacity(std::clamp(state.opacity * opacity, 0.0, 1.0));
            target.fillRect(frame, QColor(134, 65, 93));
            target.setPen(Qt::white);
            target.setFont(font);
            target.drawText(frame, Qt::AlignCenter, QStringLiteral("B"));
            target.restore();
        };

        if (procedural) {
            QImage b_layer(size(), QImage::Format_ARGB32_Premultiplied);
            b_layer.fill(Qt::transparent);
            QPainter b_painter(&b_layer);
            b_painter.setRenderHint(QPainter::Antialiasing, true);
            if (state.blur > 0.01) {
                const double radius = std::clamp(state.blur * 0.18, 1.0, 8.0);
                for (const QPointF &offset : {QPointF(-radius, 0.0), QPointF(radius, 0.0),
                                              QPointF(0.0, -radius), QPointF(0.0, radius),
                                              QPointF(-radius * 0.7, -radius * 0.7),
                                              QPointF(radius * 0.7, radius * 0.7)})
                    paint_b(b_painter, offset, 0.11);
            }
            paint_b(b_painter, QPointF(), 1.0);
            b_painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            const QSize mask_size(std::max(1, static_cast<int>(std::ceil(frame.width()))),
                                  std::max(1, static_cast<int>(std::ceil(frame.height()))));
            const QImage mask = procedural_preview_mask(mask_size, state);
            b_painter.drawImage(frame.topLeft(), mask);
            b_painter.end();
            painter.drawImage(QPointF(0.0, 0.0), b_layer);
        } else {
            if (state.blur > 0.01) {
                const double radius = std::clamp(state.blur * 0.18, 1.0, 8.0);
                for (const QPointF &offset : {QPointF(-radius, 0.0), QPointF(radius, 0.0),
                                              QPointF(0.0, -radius), QPointF(0.0, radius),
                                              QPointF(-radius * 0.7, -radius * 0.7),
                                              QPointF(radius * 0.7, radius * 0.7)})
                    paint_b(painter, offset, 0.11);
            }
            paint_b(painter, QPointF(), 1.0);
        }
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

    blocks_columns_ = new QSpinBox(this);
    blocks_columns_->setRange(1, 128);
    blocks_columns_->setValue(std::clamp(transition.blocks_columns, 1, 128));
    blocks_columns_label_ = new QLabel(bgl_tr("OBSTitles.Columns"), this);
    form->addRow(blocks_columns_label_, blocks_columns_);

    blocks_rows_ = new QSpinBox(this);
    blocks_rows_->setRange(1, 128);
    blocks_rows_->setValue(std::clamp(transition.blocks_rows, 1, 128));
    blocks_rows_label_ = new QLabel(bgl_tr("OBSTitles.Rows"), this);
    form->addRow(blocks_rows_label_, blocks_rows_);

    random_seed_ = new QSpinBox(this);
    random_seed_->setRange(0, 999999);
    random_seed_->setValue(std::max(0, transition.random_seed));
    random_seed_label_ = new QLabel(bgl_tr("OBSTitles.Seed"), this);
    form->addRow(random_seed_label_, random_seed_);

    auto *image_row = new QWidget(this);
    auto *image_layout = new QHBoxLayout(image_row);
    image_layout->setContentsMargins(0, 0, 0, 0);
    image_path_ = new QLineEdit(QString::fromStdString(transition.image_path), image_row);
    image_browse_ = new QPushButton(bgl_tr("OBSTitles.Browse"), image_row);
    image_layout->addWidget(image_path_, 1);
    image_layout->addWidget(image_browse_);
    image_path_label_ = new QLabel(bgl_tr("OBSTitles.Image"), this);
    form->addRow(image_path_label_, image_row);

    image_channel_ = make_combo(this);
    image_channel_->addItem(QStringLiteral("Luma"), 0);
    image_channel_->addItem(QStringLiteral("Alpha"), 1);
    image_channel_->addItem(QStringLiteral("Red"), 2);
    image_channel_->addItem(QStringLiteral("Green"), 3);
    image_channel_->addItem(QStringLiteral("Blue"), 4);
    image_channel_->setCurrentIndex(std::max(0, image_channel_->findData(transition.image_channel)));
    image_channel_label_ = new QLabel(bgl_tr("OBSTitles.Channel"), this);
    form->addRow(image_channel_label_, image_channel_);

    invert_ = new QCheckBox(QStringLiteral("Invert"), this);
    invert_->setChecked(transition.invert);
    form->addRow(QString(), invert_);

    clockwise_ = new QCheckBox(bgl_tr("OBSTitles.Clockwise"), this);
    clockwise_->setChecked(transition.clockwise);
    form->addRow(QString(), clockwise_);

    center_x_ = new QDoubleSpinBox(this);
    center_x_->setRange(0.0, 100.0);
    center_x_->setSuffix(QStringLiteral(" %"));
    center_x_->setValue(std::clamp(transition.center_x, 0.0, 1.0) * 100.0);
    center_x_label_ = make_numeric_drag_label(bgl_tr("OBSTitles.CenterXLabel"), center_x_, this);
    form->addRow(center_x_label_, center_x_);

    center_y_ = new QDoubleSpinBox(this);
    center_y_->setRange(0.0, 100.0);
    center_y_->setSuffix(QStringLiteral(" %"));
    center_y_->setValue(std::clamp(transition.center_y, 0.0, 1.0) * 100.0);
    center_y_label_ = make_numeric_drag_label(bgl_tr("OBSTitles.CenterYLabel"), center_y_, this);
    form->addRow(center_y_label_, center_y_);

    rotation_ = new QDoubleSpinBox(this);
    rotation_->setRange(-3600.0, 3600.0);
    rotation_->setSuffix(QStringLiteral("°"));
    rotation_->setValue(transition.rotation);
    rotation_label_ = make_numeric_drag_label(bgl_tr("OBSTitles.Rotation"), rotation_, this);
    form->addRow(rotation_label_, rotation_);

    aspect_ = new QDoubleSpinBox(this);
    aspect_->setRange(0.01, 100.0);
    aspect_->setDecimals(3);
    aspect_->setSingleStep(0.05);
    aspect_->setValue(std::max(0.01, transition.aspect));
    aspect_label_ = make_numeric_drag_label(bgl_tr("OBSTitles.Aspect"), aspect_, this);
    form->addRow(aspect_label_, aspect_);

    profile_ = make_combo(this);
    if (transition.type == LayerTransitionType::Iris) {
        profile_->addItem(QStringLiteral("Circle"), 0);
        profile_->addItem(QStringLiteral("Square"), 1);
        profile_->addItem(QStringLiteral("Diamond"), 2);
    } else {
        profile_->addItem(QStringLiteral("Linear"), 0);
        profile_->addItem(QStringLiteral("Radial"), 1);
        profile_->addItem(QStringLiteral("Diamond"), 2);
    }
    profile_->setCurrentIndex(std::max(0, profile_->findData(transition.profile)));
    profile_label_ = new QLabel(bgl_tr("OBSTitles.EffectProfile"), this);
    form->addRow(profile_label_, profile_);
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
    connect(blocks_columns_, qOverload<int>(&QSpinBox::valueChanged), this, [update](int) { update(); });
    connect(blocks_rows_, qOverload<int>(&QSpinBox::valueChanged), this, [update](int) { update(); });
    connect(random_seed_, qOverload<int>(&QSpinBox::valueChanged), this, [update](int) { update(); });
    connect(image_path_, &QLineEdit::textChanged, this, [update](const QString &) { update(); });
    connect(image_channel_, qOverload<int>(&QComboBox::currentIndexChanged), this, [update](int) { update(); });
    connect(invert_, &QCheckBox::toggled, this, update);
    connect(clockwise_, &QCheckBox::toggled, this, update);
    connect(center_x_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [update](double) { update(); });
    connect(center_y_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [update](double) { update(); });
    connect(rotation_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [update](double) { update(); });
    connect(aspect_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [update](double) { update(); });
    connect(profile_, qOverload<int>(&QComboBox::currentIndexChanged), this, [update](int) { update(); });
    connect(image_browse_, &QPushButton::clicked, this, [this, update]() {
        const QString path = QFileDialog::getOpenFileName(
            this, bgl_tr("OBSTitles.ImageWipe"), image_path_->text(),
            bgl_tr("OBSTitles.ImageFileFilter"));
        if (!path.isEmpty()) {
            image_path_->setText(path);
            update();
        }
    });

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
    transition_.blocks_columns = blocks_columns_->value();
    transition_.blocks_rows = blocks_rows_->value();
    transition_.random_seed = random_seed_->value();
    transition_.image_path = image_path_->text().toStdString();
    transition_.image_channel = image_channel_->currentData().toInt();
    transition_.invert = invert_->isChecked();
    transition_.clockwise = clockwise_->isChecked();
    transition_.center_x = center_x_->value() / 100.0;
    transition_.center_y = center_y_->value() / 100.0;
    transition_.rotation = rotation_->value();
    transition_.aspect = aspect_->value();
    transition_.profile = profile_->currentData().toInt();
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
                              transition_.type == LayerTransitionType::TextWipe ||
                              transition_.type == LayerTransitionType::Blocks ||
                              transition_.type == LayerTransitionType::ImageWipe ||
                              transition_.type == LayerTransitionType::Clock ||
                              transition_.type == LayerTransitionType::Iris ||
                              transition_.type == LayerTransitionType::GradientWipe;
    const bool is_blocks = transition_.type == LayerTransitionType::Blocks;
    const bool is_image_wipe = transition_.type == LayerTransitionType::ImageWipe;
    const bool is_clock = transition_.type == LayerTransitionType::Clock;
    const bool is_iris = transition_.type == LayerTransitionType::Iris;
    const bool is_gradient = transition_.type == LayerTransitionType::GradientWipe;
    const bool has_center = is_clock || is_iris || is_gradient;
    const bool has_rotation = is_clock || is_gradient;
    const bool has_aspect = is_iris || is_gradient;
    const bool has_profile = is_iris || is_gradient;
    const bool procedural = is_blocks || is_image_wipe || is_clock || is_iris || is_gradient;

    for (QWidget *widget : std::initializer_list<QWidget *>{unit_label_, unit_, stagger_label_, stagger_, reverse_order_})
        widget->setVisible(text);
    for (QWidget *widget : std::initializer_list<QWidget *>{direction_label_, direction_}) widget->setVisible(has_direction);
    for (QWidget *widget : std::initializer_list<QWidget *>{blur_label_, blur_}) widget->setVisible(has_blur);
    for (QWidget *widget : std::initializer_list<QWidget *>{scale_label_, scale_}) widget->setVisible(has_scale);
    for (QWidget *widget : std::initializer_list<QWidget *>{offset_label_, offset_}) widget->setVisible(has_offset);
    for (QWidget *widget : std::initializer_list<QWidget *>{softness_label_, softness_}) widget->setVisible(has_softness);
    for (QWidget *widget : std::initializer_list<QWidget *>{blocks_columns_label_, blocks_columns_, blocks_rows_label_, blocks_rows_, random_seed_label_, random_seed_})
        widget->setVisible(is_blocks);
    for (QWidget *widget : std::initializer_list<QWidget *>{image_path_label_, image_path_->parentWidget(), image_channel_label_, image_channel_})
        widget->setVisible(is_image_wipe);
    invert_->setVisible(procedural);
    clockwise_->setVisible(is_clock);
    for (QWidget *widget : std::initializer_list<QWidget *>{center_x_label_, center_x_, center_y_label_, center_y_})
        widget->setVisible(has_center);
    for (QWidget *widget : std::initializer_list<QWidget *>{rotation_label_, rotation_})
        widget->setVisible(has_rotation);
    for (QWidget *widget : std::initializer_list<QWidget *>{aspect_label_, aspect_})
        widget->setVisible(has_aspect);
    for (QWidget *widget : std::initializer_list<QWidget *>{profile_label_, profile_})
        widget->setVisible(has_profile);
}
