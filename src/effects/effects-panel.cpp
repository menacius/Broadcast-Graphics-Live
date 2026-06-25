#include "title-editor-internal.h"
#include "effect-preset-catalog.h"

#include <QAbstractItemModel>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QScopedValueRollback>
#include <cmath>
#include <functional>
#include <utility>

namespace {

class EffectsStackListWidget final : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<bool(const QString &)> external_drop_handler;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event && bgs::effects::mime_has_effect_preset(event->mimeData())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        QListWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event && bgs::effects::mime_has_effect_preset(event->mimeData())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        QListWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (event && bgs::effects::mime_has_effect_preset(event->mimeData())) {
            const QString path = bgs::effects::effect_preset_path_from_mime(event->mimeData());
            if (external_drop_handler && external_drop_handler(path)) {
                event->setDropAction(Qt::CopyAction);
                event->accept();
            } else {
                // This is an external preset payload, not an internal list
                // reorder. Never pass a failed preset drop to QListWidget,
                // which may try to interpret it as model data.
                event->ignore();
            }
            return;
        }
        QListWidget::dropEvent(event);
    }
};

} // namespace

static QString bgl_effects_panel_style()
{
    const QPalette pal = qApp->palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor window_text = pal.color(QPalette::WindowText);
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    const QColor button = pal.color(QPalette::Button);
    const QColor button_text = pal.color(QPalette::ButtonText);
    const QColor mid = pal.color(QPalette::Mid);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlighted_text = pal.color(QPalette::HighlightedText);
    const QColor alternate = pal.color(QPalette::AlternateBase);
    const QColor hover = button.lightness() < 128 ? button.lighter(122) : button.darker(108);
    const QColor disabled = window_text.lightness() < 128 ? window_text.lighter(160) : window_text.darker(160);

    QString css = QStringLiteral(
        "QWidget#BroadcastGraphicsLiveEffectsPanel{background:@window@;color:@windowText@;}"
        "QLabel{color:@windowText@;background:transparent;}"
        "QListWidget{background:@base@;border:1px solid @mid@;color:@text@;alternate-background-color:@alternate@;}"
        "QListWidget::item{padding:4px;}"
        "QListWidget::item:selected{background:@highlight@;color:@highlightedText@;}"
        "QToolButton{color:@buttonText@;background:@button@;border:1px solid @mid@;border-radius:2px;padding:2px;}"
        "QToolButton:hover{background:@hover@;border-color:@mid@;}"
        "QToolButton:pressed{background:@highlight@;color:@highlightedText@;border-color:@highlight@;}"
        "QToolButton:checked{background:@highlight@;color:@highlightedText@;border-color:@highlight@;}"
        "QScrollArea{background:@window@;border:none;}"
        "QWidget#BroadcastGraphicsLiveEffectsSettingsContainer{background:@window@;}"
        "QMenu{color:@windowText@;background:@window@;border:1px solid @mid@;}"
        "QMenu::item{padding:5px 22px;}"
        "QMenu::item:selected{background:@highlight@;color:@highlightedText@;}"
        "QMenu::item:disabled{color:@disabled@;}");
    css.replace(QStringLiteral("@window@"), window.name(QColor::HexRgb));
    css.replace(QStringLiteral("@windowText@"), window_text.name(QColor::HexRgb));
    css.replace(QStringLiteral("@base@"), base.name(QColor::HexRgb));
    css.replace(QStringLiteral("@text@"), text.name(QColor::HexRgb));
    css.replace(QStringLiteral("@button@"), button.name(QColor::HexRgb));
    css.replace(QStringLiteral("@buttonText@"), button_text.name(QColor::HexRgb));
    css.replace(QStringLiteral("@mid@"), mid.name(QColor::HexRgb));
    css.replace(QStringLiteral("@highlight@"), highlight.name(QColor::HexRgb));
    css.replace(QStringLiteral("@highlightedText@"), highlighted_text.name(QColor::HexRgb));
    css.replace(QStringLiteral("@alternate@"), alternate.name(QColor::HexRgb));
    css.replace(QStringLiteral("@hover@"), hover.name(QColor::HexRgb));
    css.replace(QStringLiteral("@disabled@"), disabled.name(QColor::HexRgb));
    return css;
}

static QString bgl_theme_control_style()
{
    const QPalette pal = qApp->palette();
    return QStringLiteral(
        "QDoubleSpinBox,QSpinBox,QComboBox,QLineEdit,QTextEdit{color:%1;background:%2;"
        "border:1px solid %3;border-radius:2px;padding:1px 3px;selection-background-color:%4;}"
        "QDoubleSpinBox:focus,QSpinBox:focus,QComboBox:focus,QLineEdit:focus,QTextEdit:focus{border-color:%4;}")
        .arg(pal.color(QPalette::Text).name(QColor::HexRgb), pal.color(QPalette::Base).name(QColor::HexRgb),
             pal.color(QPalette::Mid).name(QColor::HexRgb), pal.color(QPalette::Highlight).name(QColor::HexRgb));
}


static uint32_t color_button_argb(QPushButton *button)
{
    return button ? button->property("argb").toUInt() : 0xFFFFFFFF;
}

static void set_color_button_argb(QPushButton *button, uint32_t argb)
{
    if (!button) return;
    button->setProperty("argb", argb);
    QColor c = color_from_argb(argb);
    button->setText(c.name(QColor::HexArgb).toUpper());
    const QColor border = qApp->palette().color(QPalette::Mid);
    const QColor text = c.lightness() < 128 ? QColor(Qt::white) : QColor(Qt::black);
    button->setStyleSheet(QStringLiteral("QPushButton{color:%1;background:%2;border:1px solid %3;border-radius:3px;padding:3px 8px;}")
                          .arg(text.name(QColor::HexRgb), c.name(QColor::HexRgb), border.name(QColor::HexRgb)));
}

static uint32_t panel_eval_effect_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.color_a, (effect.effect_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.color_r, (effect.effect_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.color_g, (effect.effect_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.color_b, effect.effect_color & 0xFF, t);
}

static uint32_t panel_eval_effect_stroke_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.stroke_color_a, (effect.effect_stroke_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.stroke_color_r, (effect.effect_stroke_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.stroke_color_g, (effect.effect_stroke_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.stroke_color_b, effect.effect_stroke_color & 0xFF, t);
}

static double panel_eval_effect_property(const AnimatedProperty &prop, double fallback, double t)
{
    return prop.is_animated() ? prop.evaluate(t) : fallback;
}

static void set_effect_color_channels_at(LayerEffect &effect, double time, uint32_t argb)
{
    set_animated_value(effect.color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(effect.color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(effect.color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(effect.color_b, time, argb & 0xFF);
}

static void set_effect_stroke_color_channels_at(LayerEffect &effect, double time, uint32_t argb)
{
    set_animated_value(effect.stroke_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(effect.stroke_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(effect.stroke_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(effect.stroke_color_b, time, argb & 0xFF);
}

static uint32_t panel_eval_effect_secondary_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.secondary_color_a, (effect.effect_secondary_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.secondary_color_r, (effect.effect_secondary_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.secondary_color_g, (effect.effect_secondary_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.secondary_color_b, effect.effect_secondary_color & 0xFF, t);
}

static void set_effect_secondary_color_channels_at(LayerEffect &effect, double time, uint32_t argb)
{
    set_animated_value(effect.secondary_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(effect.secondary_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(effect.secondary_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(effect.secondary_color_b, time, argb & 0xFF);
}

EffectsPanel::EffectsPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("BroadcastGraphicsLiveEffectsPanel"));
    setAcceptDrops(true);
    setStyleSheet(bgl_effects_panel_style());

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *hint = new QLabel(bgl_tr("OBSTitles.EffectStack"), this);
    QFont hint_font = hint->font();
    hint_font.setBold(true);
    hint->setFont(hint_font);
    layout->addWidget(hint);

    auto *effect_stack_list = new EffectsStackListWidget(this);
    effect_stack_list->external_drop_handler = [this](const QString &file_path) {
        return add_effect_from_preset_file(file_path);
    };
    effect_list_ = effect_stack_list;
    effect_list_->setObjectName(QStringLiteral("BroadcastGraphicsLiveEffectsList"));
    effect_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    effect_list_->setAlternatingRowColors(true);
    effect_list_->setDragEnabled(true);
    effect_list_->setAcceptDrops(true);
    effect_list_->setDragDropMode(QAbstractItemView::InternalMove);
    effect_list_->setDefaultDropAction(Qt::MoveAction);
    effect_list_->setDropIndicatorShown(true);
    
    layout->addWidget(effect_list_, 1);

    auto *button_bar = new QWidget(this);
    button_bar->setObjectName(QStringLiteral("BroadcastGraphicsLiveEffectsButtonBar"));
    auto *button_layout = new QHBoxLayout(button_bar);
    button_layout->setContentsMargins(0, 0, 0, 0);
    button_layout->setSpacing(4);

    auto add_button = [button_bar, button_layout](const char *icon, const QString &tip) {
        auto *button = new QToolButton(button_bar);
        button->setIcon(obs_icon(icon));
        button->setIconSize(QSize(16, 16));
        button->setToolTip(tip);
        button->setAutoRaise(true);
        button_layout->addWidget(button);
        return button;
    };

    auto *btn_add = add_button("add.svg", bgl_tr("OBSTitles.AddEffect"));
    btn_remove_ = add_button("delete.svg", bgl_tr("OBSTitles.RemoveEffect"));
    btn_duplicate_ = add_button("duplicate.svg", bgl_tr("OBSTitles.DuplicateEffect"));
    btn_move_up_ = add_button("move-up.svg", bgl_tr("OBSTitles.MoveEffectUp"));
    btn_move_down_ = add_button("move-down.svg", bgl_tr("OBSTitles.MoveEffectDown"));
    btn_respect_masks_ = add_button("timeline-mask.svg", bgl_tr("OBSTitles.ApplyEffectStackAfterMask"));
    btn_respect_masks_->setCheckable(true);
    btn_respect_masks_->setToolTip(bgl_tr("OBSTitles.ApplyEffectStackAfterMaskTooltip"));
    button_layout->addStretch(1);
    layout->addWidget(button_bar);

    auto *settings_scroll = new QScrollArea(this);
    settings_scroll->setWidgetResizable(true);
    
    settings_container_ = new QWidget(settings_scroll);
    settings_container_->setObjectName(QStringLiteral("BroadcastGraphicsLiveEffectsSettingsContainer"));
    settings_layout_ = new QVBoxLayout(settings_container_);
    settings_layout_->setContentsMargins(0, 6, 0, 0);
    settings_layout_->setSpacing(6);
    settings_scroll->setWidget(settings_container_);
    layout->addWidget(settings_scroll, 2);

    connect(effect_list_, &QListWidget::currentRowChanged, this, [this](int row) {
        selected_index_ = row;
        load_settings();
    });

    connect(effect_list_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (loading_values_ || !layer_) return;
        int row = effect_list_->row(item);
        if (row < 0 || row >= (int)layer_->effects.size()) return;
        layer_->effects[row].enabled = item->checkState() == Qt::Checked;
        set_animated_value(layer_->effects[row].enabled_prop, std::clamp(playhead_ - layer_->in_time, 0.0,
                           std::max(0.0, layer_->out_time - layer_->in_time)),
                           layer_->effects[row].enabled ? 1.0 : 0.0);
        emit_effect_changed();
    });

    connect(effect_list_->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex &, int, int, const QModelIndex &, int) {
                apply_effect_list_order_from_items();
            });

    connect(btn_add, &QToolButton::clicked, this, [this, btn_add]() {
        if (!layer_) return;
        QMenu menu(btn_add);
        
        const auto add_action = [&menu](const QString &name, LayerEffectType type) {
            auto *action = menu.addAction(name);
            action->setData((int)type);
        };
        add_action(bgl_tr("OBSTitles.BackgroundColor"), LayerEffectType::BackgroundColor);
        add_action(bgl_tr("OBSTitles.Outline"), LayerEffectType::Outline);
        add_action(bgl_tr("OBSTitles.DropShadow"), LayerEffectType::DropShadow);
        add_action(bgl_tr("OBSTitles.LongShadow"), LayerEffectType::LongShadow);
        add_action(bgl_tr("OBSTitles.BrightnessContrast"), LayerEffectType::BrightnessContrast);
        add_action(bgl_tr("OBSTitles.Saturation"), LayerEffectType::Saturation);
        add_action(bgl_tr("OBSTitles.ColorOverlay"), LayerEffectType::ColorOverlay);
        add_action(bgl_tr("OBSTitles.Glow"), LayerEffectType::Glow);
        add_action(bgl_tr("OBSTitles.InnerGlow"), LayerEffectType::InnerGlow);
        add_action(bgl_tr("OBSTitles.InnerShadow"), LayerEffectType::InnerShadow);
        add_action(bgl_tr("OBSTitles.Blur"), LayerEffectType::Blur);
        add_action(bgl_tr("OBSTitles.MotionBlur"), LayerEffectType::MotionBlur);
        add_action(bgl_tr("OBSTitles.Bloom"), LayerEffectType::Bloom);
        add_action(bgl_tr("OBSTitles.Emboss"), LayerEffectType::Emboss);
        add_action(bgl_tr("OBSTitles.LensFlare"), LayerEffectType::LensFlare);
        add_action(bgl_tr("OBSTitles.Vignette"), LayerEffectType::Vignette);
        add_action(bgl_tr("OBSTitles.Noise"), LayerEffectType::Noise);
        add_action(bgl_tr("OBSTitles.RoughenEdges"), LayerEffectType::RoughenEdges);
        QAction *chosen = menu.exec(btn_add->mapToGlobal(QPoint(0, btn_add->height())));
        if (!chosen) return;
        LayerEffect effect = bgs::effects::make_default_layer_effect(
            static_cast<LayerEffectType>(chosen->data().toInt()));
        layer_->effects.push_back(effect);
        selected_index_ = (int)layer_->effects.size() - 1;
        rebuild_stack();
        emit_effect_changed();
    });

    connect(btn_remove_, &QToolButton::clicked, this, [this]() {
        if (!layer_ || selected_index_ < 0 || selected_index_ >= (int)layer_->effects.size()) return;
        layer_->effects.erase(layer_->effects.begin() + selected_index_);
        if (selected_index_ >= (int)layer_->effects.size()) selected_index_ = (int)layer_->effects.size() - 1;
        sync_legacy_enabled_flags();
        rebuild_stack();
        emit_effect_changed();
    });

    connect(btn_duplicate_, &QToolButton::clicked, this, [this]() {
        if (!layer_ || selected_index_ < 0 || selected_index_ >= (int)layer_->effects.size()) return;
        layer_->effects.insert(layer_->effects.begin() + selected_index_ + 1, layer_->effects[selected_index_]);
        ++selected_index_;
        sync_legacy_enabled_flags();
        rebuild_stack();
        emit_effect_changed();
    });

    connect(btn_move_up_, &QToolButton::clicked, this, [this]() {
        if (!layer_ || selected_index_ <= 0 || selected_index_ >= (int)layer_->effects.size()) return;
        const int target = selected_index_ - 1;
        std::swap(layer_->effects[(size_t)selected_index_], layer_->effects[(size_t)target]);
        selected_index_ = target;
        rebuild_stack();
        emit_effect_changed();
    });

    connect(btn_move_down_, &QToolButton::clicked, this, [this]() {
        if (!layer_ || selected_index_ < 0 || selected_index_ + 1 >= (int)layer_->effects.size()) return;
        const int target = selected_index_ + 1;
        std::swap(layer_->effects[(size_t)selected_index_], layer_->effects[(size_t)target]);
        selected_index_ = target;
        rebuild_stack();
        emit_effect_changed();
    });

    connect(btn_respect_masks_, &QToolButton::toggled, this, [this](bool checked) {
        if (loading_values_ || !layer_) return;
        layer_->effect_stack_respects_masks = checked;
        emit_effect_changed();
    });

    rebuild_stack();
}


void EffectsPanel::dragEnterEvent(QDragEnterEvent *event)
{
    if (event && bgs::effects::mime_has_effect_preset(event->mimeData())) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    QWidget::dragEnterEvent(event);
}

void EffectsPanel::dragMoveEvent(QDragMoveEvent *event)
{
    if (event && bgs::effects::mime_has_effect_preset(event->mimeData())) {
        if (layer_ && !layer_->locked) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        } else {
            event->ignore();
        }
        return;
    }
    QWidget::dragMoveEvent(event);
}

void EffectsPanel::dropEvent(QDropEvent *event)
{
    if (event && bgs::effects::mime_has_effect_preset(event->mimeData())) {
        const QString path = bgs::effects::effect_preset_path_from_mime(event->mimeData());
        if (add_effect_from_preset_file(path)) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        } else {
            event->ignore();
        }
        return;
    }
    QWidget::dropEvent(event);
}

bool EffectsPanel::add_effect_from_preset_file(const QString &file_path)
{
    if (!layer_ || layer_->locked)
        return false;

    bgs::effects::EffectPresetDescriptor descriptor;
    if (!bgs::effects::load_effect_preset_file(file_path, &descriptor))
        return false;

    layer_->effects.push_back(descriptor.effect);
    selected_index_ = static_cast<int>(layer_->effects.size()) - 1;
    rebuild_stack();
    emit_effect_changed();
    return true;
}

void EffectsPanel::set_layer(std::shared_ptr<Layer> layer, double playhead)
{
    const bool same_layer = layer_ && layer && layer_.get() == layer.get();
    const int previous_effect_count = layer_ ? (int)layer_->effects.size() : -1;
    layer_ = layer;
    playhead_ = playhead;
    selected_index_ = layer_ && !layer_->effects.empty() ? std::clamp(selected_index_, 0, (int)layer_->effects.size() - 1) : -1;
    if (same_layer && previous_effect_count == (layer_ ? (int)layer_->effects.size() : -1) &&
        (settings_editor_has_focus() || numeric_label_dragging_)) {
        return;
    }
    rebuild_stack();
}

LayerEffect *EffectsPanel::selected_effect()
{
    if (!layer_ || selected_index_ < 0 || selected_index_ >= (int)layer_->effects.size()) return nullptr;
    return &layer_->effects[selected_index_];
}

const LayerEffect *EffectsPanel::selected_effect() const
{
    if (!layer_ || selected_index_ < 0 || selected_index_ >= (int)layer_->effects.size()) return nullptr;
    return &layer_->effects[selected_index_];
}

void EffectsPanel::sync_legacy_enabled_flags()
{
    /* Effect stack is the only source of truth. Legacy layer flags are kept
     * only for old project import/export fallback and are not synchronized.
     */
}

void EffectsPanel::emit_effect_changed()
{
    sync_legacy_enabled_flags();
    emit property_changed(!numeric_label_dragging_);
}

bool EffectsPanel::settings_editor_has_focus() const
{
    QWidget *focus = qApp ? qApp->focusWidget() : nullptr;
    return focus && settings_container_ &&
           (focus == settings_container_ || settings_container_->isAncestorOf(focus));
}

void EffectsPanel::update_playhead(double playhead)
{
    playhead_ = playhead;
    update_bound_controls();
}

void EffectsPanel::update_bound_controls()
{
    if (!layer_ || loading_values_ || numeric_label_dragging_)
        return;

    const double lt = std::clamp(playhead_ - layer_->in_time, 0.0,
                                 std::max(0.0, layer_->out_time - layer_->in_time));

    if (effect_list_) {
        QSignalBlocker blocker(effect_list_);
        const int count = std::min(effect_list_->count(), (int)layer_->effects.size());
        for (int row = 0; row < count; ++row) {
            if (QListWidgetItem *item = effect_list_->item(row)) {
                const bool enabled = eval_effect_enabled(layer_->effects[(size_t)row], lt);
                const Qt::CheckState state = enabled ? Qt::Checked : Qt::Unchecked;
                if (item->checkState() != state)
                    item->setCheckState(state);
            }
        }
    }

    const LayerEffect *effect = selected_effect();
    if (!effect)
        return;

    QScopedValueRollback<bool> loading_guard(loading_values_, true);
    for (const auto &binding : numeric_bindings_) {
        if (!binding.spin || !binding.value)
            continue;
        const double value = binding.value(*effect, lt);
        if (!std::isfinite(value) || std::abs(binding.spin->value() - value) < 0.000001)
            continue;
        QSignalBlocker blocker(binding.spin);
        binding.spin->setValue(value);
    }

    for (const auto &binding : color_bindings_) {
        if (!binding.button || !binding.value)
            continue;
        const uint32_t argb = binding.value(*effect, lt);
        if (binding.button->property("argb").toUInt() == argb)
            continue;
        set_color_button_argb(binding.button, argb);
    }
}

void EffectsPanel::apply_effect_list_order_from_items()
{
    if (loading_values_ || !layer_ || !effect_list_)
        return;
    const int count = (int)layer_->effects.size();
    if (effect_list_->count() != count || count <= 1)
        return;

    std::vector<LayerEffect> reordered;
    reordered.reserve((size_t)count);
    std::vector<bool> used((size_t)count, false);
    for (int row = 0; row < count; ++row) {
        QListWidgetItem *item = effect_list_->item(row);
        const int source_index = item ? item->data(Qt::UserRole).toInt() : -1;
        if (source_index < 0 || source_index >= count || used[(size_t)source_index])
            return;
        reordered.push_back(layer_->effects[(size_t)source_index]);
        used[(size_t)source_index] = true;
    }

    layer_->effects = std::move(reordered);
    selected_index_ = std::clamp(effect_list_->currentRow(), 0, count - 1);

    QSignalBlocker blocker(effect_list_);
    for (int row = 0; row < count; ++row) {
        if (QListWidgetItem *item = effect_list_->item(row))
            item->setData(Qt::UserRole, row);
    }

    sync_legacy_enabled_flags();
    emit_effect_changed();
    load_settings();
}

void EffectsPanel::rebuild_stack()
{
    loading_values_ = true;
    effect_list_->clear();
    if (!layer_) {
        effect_list_->addItem(bgl_tr("OBSTitles.SelectLayerEditEffects"));
        if (auto *item = effect_list_->item(0)) item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        selected_index_ = -1;
    } else if (layer_->effects.empty()) {
        effect_list_->addItem(bgl_tr("OBSTitles.NoEffectsAdded"));
        if (auto *item = effect_list_->item(0)) item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        selected_index_ = -1;
    } else {
        for (int i = 0; i < (int)layer_->effects.size(); ++i) {
            const auto &effect = layer_->effects[i];
            auto *item = new QListWidgetItem(effect_type_name(effect.type));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable |
                           Qt::ItemIsEnabled | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            item->setData(Qt::UserRole, i);
            item->setCheckState(effect.enabled ? Qt::Checked : Qt::Unchecked);
            effect_list_->addItem(item);
        }
        selected_index_ = std::clamp(selected_index_, 0, (int)layer_->effects.size() - 1);
        effect_list_->setCurrentRow(selected_index_);
    }
    loading_values_ = false;

    const bool has_selection = selected_effect() != nullptr;
    if (btn_remove_) btn_remove_->setEnabled(has_selection);
    if (btn_duplicate_) btn_duplicate_->setEnabled(has_selection);
    if (btn_move_up_) btn_move_up_->setEnabled(has_selection && selected_index_ > 0);
    if (btn_move_down_) btn_move_down_->setEnabled(has_selection && layer_ && selected_index_ + 1 < (int)layer_->effects.size());
    if (btn_respect_masks_) {
        QSignalBlocker blocker(btn_respect_masks_);
        btn_respect_masks_->setEnabled(layer_ != nullptr);
        btn_respect_masks_->setChecked(layer_ && layer_->effect_stack_respects_masks);
    }

    load_settings();
}

void EffectsPanel::build_settings()
{
    numeric_bindings_.clear();
    color_bindings_.clear();
    while (QLayoutItem *item = settings_layout_->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
}

void EffectsPanel::load_settings()
{
    build_settings();
    if (!layer_ || !selected_effect()) {
        auto *label = new QLabel(layer_ ? bgl_tr("OBSTitles.AddEffectSettingsHint") : bgl_tr("OBSTitles.SelectLayerEditEffectsHint"), settings_container_);
        label->setWordWrap(true);
        
        settings_layout_->addWidget(label);
        settings_layout_->addStretch(1);
        return;
    }

    loading_values_ = true;
    const double lt = std::clamp(playhead_ - layer_->in_time, 0.0, std::max(0.0, layer_->out_time - layer_->in_time));
    auto *box = new QGroupBox(effect_type_name(selected_effect()->type), settings_container_);
    {
        const QPalette pal = qApp->palette();
        const QColor section_bg = pal.color(QPalette::Window).lightness() < 128
                                    ? pal.color(QPalette::Window).lighter(112)
                                    : pal.color(QPalette::Window).darker(104);
        const QColor hover = pal.color(QPalette::Button).lightness() < 128
                             ? pal.color(QPalette::Button).lighter(122)
                             : pal.color(QPalette::Button).darker(108);
        QString group_css = QStringLiteral(
            "QGroupBox{color:@windowText@;background:@section@;border:1px solid @mid@;margin-top:8px;padding-top:14px;}"
            "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 3px;}"
            "QDoubleSpinBox,QComboBox{color:@text@;background:@base@;border:1px solid @mid@;border-radius:2px;padding:1px 3px;}"
            "QDoubleSpinBox:focus,QComboBox:focus{border-color:@highlight@;}"
            "QPushButton{color:@buttonText@;background:@button@;border:1px solid @mid@;border-radius:3px;padding:3px 8px;}"
            "QPushButton:hover{background:@hover@;border-color:@mid@;}"
            "QPushButton:pressed{background:@highlight@;color:@highlightedText@;border-color:@highlight@;}");
        group_css.replace(QStringLiteral("@windowText@"), pal.color(QPalette::WindowText).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@section@"), section_bg.name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@mid@"), pal.color(QPalette::Mid).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@text@"), pal.color(QPalette::Text).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@base@"), pal.color(QPalette::Base).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@highlight@"), pal.color(QPalette::Highlight).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@buttonText@"), pal.color(QPalette::ButtonText).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@button@"), pal.color(QPalette::Button).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@hover@"), hover.name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@highlightedText@"), pal.color(QPalette::HighlightedText).name(QColor::HexRgb));
        box->setStyleSheet(group_css);
    }
    auto *form = new QFormLayout(box);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);
    auto spin = [box](double min, double max, double step) { auto *s = new QDoubleSpinBox(box); s->setRange(min, max); s->setSingleStep(step); s->setFixedHeight(22); s->setStyleSheet(bgl_theme_control_style()); return s; };
    auto combo = [box]() { auto *c = new QComboBox(box); c->setFixedHeight(22); c->setStyleSheet(bgl_theme_control_style()); return c; };
    auto color_button = [this, box](uint32_t argb, auto setter) {
        auto *button = new QPushButton(box);
        set_color_button_argb(button, argb);
        connect(button, &QPushButton::clicked, this, [this, button, setter]() {
            QColor picked = bgl_pick_color(color_from_argb(color_button_argb(button)), this, bgl_tr("OBSTitles.ChooseColor"));
            if (!picked.isValid()) return;
            uint32_t argb = argb_from_color(picked);
            set_color_button_argb(button, argb);
            setter(argb);
            emit_effect_changed();
        });
        return button;
    };
    auto bind_numeric = [this](QDoubleSpinBox *spin,
                               std::function<double(const LayerEffect &, double)> value) {
        if (spin && value)
            numeric_bindings_.push_back({spin, std::move(value)});
    };
    auto bind_color = [this](QPushButton *button,
                             std::function<uint32_t(const LayerEffect &, double)> value) {
        if (button && value)
            color_bindings_.push_back({button, std::move(value)});
    };

    auto add_effect_row = [this, box, form](const QString &label_text, QWidget *field) {
        if (label_text.isEmpty()) {
            form->addRow(label_text, field);
            return;
        }
        auto *label = new NumericDragLabel(label_text, field, box,
                                           [this]() {
                                                if (loading_values_) return;
                                                numeric_label_dragging_ = true;
                                            },
                                           [this]() {
                                                if (loading_values_) return;
                                                numeric_label_dragging_ = false;
                                                emit property_changed(true);
                                           });
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->addRow(label, field);
    };

    if (selected_effect()->type == LayerEffectType::BackgroundColor) {
        LayerEffect *effect = selected_effect();
        auto section_label = [box](const QString &text) {
            auto *label = new QLabel(text, box);
            QFont f = label->font();
            f.setBold(true);
            label->setFont(f);
            return label;
        };
        auto *fill = combo();
        fill->addItem(bgl_tr("OBSTitles.Solid"), 0);
        fill->addItem(bgl_tr("OBSTitles.Gradient"), 1);
        fill->setCurrentIndex(fill->findData(effect->effect_fill_type));
        auto *fill_color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){
            if (!selected_effect()) return;
            selected_effect()->effect_color = argb;
            set_effect_color_channels_at(*selected_effect(), lt, argb);
        });
        auto *opacity = spin(0.0, 1.0, 0.05);
        opacity->setDecimals(2);
        opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);

        auto *stroke_color = color_button(panel_eval_effect_stroke_color(*effect, lt), [this, lt](uint32_t argb){
            if (!selected_effect()) return;
            selected_effect()->effect_stroke_color = argb;
            set_effect_stroke_color_channels_at(*selected_effect(), lt, argb);
        });
        auto *stroke_width = spin(0.0, 1000.0, 1.0);
        stroke_width->setValue(effect->stroke_width_prop.is_animated() ? effect->stroke_width_prop.evaluate(lt) : effect->effect_stroke_width);
        auto *stroke_opacity = spin(0.0, 1.0, 0.05);
        stroke_opacity->setDecimals(2);
        stroke_opacity->setValue(effect->stroke_opacity_prop.is_animated() ? effect->stroke_opacity_prop.evaluate(lt) : effect->effect_stroke_opacity);

        auto *pad_left = spin(-1000.0, 1000.0, 1.0); pad_left->setValue(effect->padding_left_prop.is_animated() ? effect->padding_left_prop.evaluate(lt) : effect->effect_padding_left);
        auto *pad_right = spin(-1000.0, 1000.0, 1.0); pad_right->setValue(effect->padding_right_prop.is_animated() ? effect->padding_right_prop.evaluate(lt) : effect->effect_padding_right);
        auto *pad_top = spin(-1000.0, 1000.0, 1.0); pad_top->setValue(effect->padding_top_prop.is_animated() ? effect->padding_top_prop.evaluate(lt) : effect->effect_padding_top);
        auto *pad_bottom = spin(-1000.0, 1000.0, 1.0); pad_bottom->setValue(effect->padding_bottom_prop.is_animated() ? effect->padding_bottom_prop.evaluate(lt) : effect->effect_padding_bottom);

        auto *corner_tl = spin(0.0, 1000.0, 1.0); corner_tl->setValue(effect->corner_radius_tl_prop.is_animated() ? effect->corner_radius_tl_prop.evaluate(lt) : effect->effect_corner_radius_tl);
        auto *corner_tr = spin(0.0, 1000.0, 1.0); corner_tr->setValue(effect->corner_radius_tr_prop.is_animated() ? effect->corner_radius_tr_prop.evaluate(lt) : effect->effect_corner_radius_tr);
        auto *corner_br = spin(0.0, 1000.0, 1.0); corner_br->setValue(effect->corner_radius_br_prop.is_animated() ? effect->corner_radius_br_prop.evaluate(lt) : effect->effect_corner_radius_br);
        auto *corner_bl = spin(0.0, 1000.0, 1.0); corner_bl->setValue(effect->corner_radius_bl_prop.is_animated() ? effect->corner_radius_bl_prop.evaluate(lt) : effect->effect_corner_radius_bl);
        auto *corner_row = new QWidget(box);
        auto *corner_grid = new QGridLayout(corner_row);
        corner_grid->setContentsMargins(0, 0, 0, 0);
        corner_grid->setHorizontalSpacing(6);
        corner_grid->setVerticalSpacing(4);
        corner_grid->addWidget(new QLabel(bgl_tr("OBSTitles.TL"), corner_row), 0, 0);
        corner_grid->addWidget(corner_tl, 0, 1);
        corner_grid->addWidget(new QLabel(bgl_tr("OBSTitles.TR"), corner_row), 0, 2);
        corner_grid->addWidget(corner_tr, 0, 3);
        corner_grid->addWidget(new QLabel(bgl_tr("OBSTitles.BL"), corner_row), 1, 0);
        corner_grid->addWidget(corner_bl, 1, 1);
        corner_grid->addWidget(new QLabel(bgl_tr("OBSTitles.BR"), corner_row), 1, 2);
        corner_grid->addWidget(corner_br, 1, 3);

        auto *corner_type_row = new QWidget(box);
        auto *corner_type_layout = new QHBoxLayout(corner_type_row);
        corner_type_layout->setContentsMargins(0, 0, 0, 0);
        corner_type_layout->setSpacing(4);
        auto *corner_group = new QButtonGroup(corner_type_row);
        corner_group->setExclusive(true);
        auto make_corner_button = [corner_type_row, corner_group, corner_type_layout](CornerType type, const char *text, const QString &tip) {
            auto *button = new QToolButton(corner_type_row);
            button->setText(QString::fromLatin1(text));
            button->setToolTip(tip);
            button->setCheckable(true);
            button->setFixedSize(26, 22);
            button->setStyleSheet(bgl_theme_control_style());
            corner_group->addButton(button, (int)type);
            corner_type_layout->addWidget(button);
        };
        make_corner_button(CornerType::Round, "R", bgl_tr("OBSTitles.Round"));
        make_corner_button(CornerType::Straight, "B", bgl_tr("OBSTitles.BevelStraight"));
        make_corner_button(CornerType::Concave, "I", bgl_tr("OBSTitles.InverseConcave"));
        make_corner_button(CornerType::Cutout, "C", bgl_tr("OBSTitles.InsetCutout"));
        corner_type_layout->addStretch(1);
        if (auto *button = corner_group->button(effect->effect_corner_type))
            button->setChecked(true);

        auto *grad_type = combo();
        grad_type->addItem(bgl_tr("OBSTitles.LinearGradient"), 0);
        grad_type->addItem(bgl_tr("OBSTitles.RadialGradient"), 1);
        grad_type->addItem(bgl_tr("OBSTitles.ConicalGradient"), 2);
        grad_type->setCurrentIndex(std::max(0, grad_type->findData(effect->effect_gradient_type)));
        auto *grad_spread = combo();
        grad_spread->addItem(bgl_tr("OBSTitles.No"), 0);
        grad_spread->addItem(bgl_tr("OBSTitles.Repeat"), 2);
        grad_spread->addItem(bgl_tr("OBSTitles.Reflect"), 1);
        grad_spread->setCurrentIndex(std::max(0, grad_spread->findData(effect->effect_gradient_spread)));
        auto *grad_start = color_button(effect->effect_gradient_start_color, [this](uint32_t argb){ if (selected_effect()) selected_effect()->effect_gradient_start_color = argb; });
        auto *grad_end = color_button(effect->effect_gradient_end_color, [this](uint32_t argb){ if (selected_effect()) selected_effect()->effect_gradient_end_color = argb; });
        auto *grad_angle = spin(-360.0, 360.0, 1.0); grad_angle->setValue(effect->effect_gradient_angle);

        bind_color(fill_color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_color(effect, t);
        });
        bind_color(stroke_color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_stroke_color(effect, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(stroke_width, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.stroke_width_prop, effect.effect_stroke_width, t);
        });
        bind_numeric(stroke_opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.stroke_opacity_prop, effect.effect_stroke_opacity, t);
        });
        bind_numeric(pad_left, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.padding_left_prop, effect.effect_padding_left, t);
        });
        bind_numeric(pad_right, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.padding_right_prop, effect.effect_padding_right, t);
        });
        bind_numeric(pad_top, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.padding_top_prop, effect.effect_padding_top, t);
        });
        bind_numeric(pad_bottom, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.padding_bottom_prop, effect.effect_padding_bottom, t);
        });
        bind_numeric(corner_tl, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.corner_radius_tl_prop, effect.effect_corner_radius_tl, t);
        });
        bind_numeric(corner_tr, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.corner_radius_tr_prop, effect.effect_corner_radius_tr, t);
        });
        bind_numeric(corner_br, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.corner_radius_br_prop, effect.effect_corner_radius_br, t);
        });
        bind_numeric(corner_bl, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.corner_radius_bl_prop, effect.effect_corner_radius_bl, t);
        });

        form->addRow(section_label(bgl_tr("OBSTitles.Appearance")));
        add_effect_row(bgl_tr("OBSTitles.Fill"), fill);
        add_effect_row(bgl_tr("OBSTitles.FillColor"), fill_color);
        add_effect_row(bgl_tr("OBSTitles.StrokeColor"), stroke_color);
        add_effect_row(bgl_tr("OBSTitles.StrokeWidth"), stroke_width);
        add_effect_row(bgl_tr("OBSTitles.StrokeOpacity"), stroke_opacity);
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(bgl_tr("OBSTitles.GradientTypeLabel"), grad_type);
        add_effect_row(bgl_tr("OBSTitles.SpreadLabel"), grad_spread);
        add_effect_row(bgl_tr("OBSTitles.StartColorLabel"), grad_start);
        add_effect_row(bgl_tr("OBSTitles.EndColorLabel"), grad_end);
        add_effect_row(bgl_tr("OBSTitles.AngleLabel"), grad_angle);
        form->addRow(section_label(bgl_tr("OBSTitles.Padding")));
        add_effect_row(bgl_tr("OBSTitles.LeftPadding"), pad_left);
        add_effect_row(bgl_tr("OBSTitles.RightPadding"), pad_right);
        add_effect_row(bgl_tr("OBSTitles.TopPadding"), pad_top);
        add_effect_row(bgl_tr("OBSTitles.BottomPadding"), pad_bottom);
        form->addRow(section_label(bgl_tr("OBSTitles.Corners")));
        add_effect_row(bgl_tr("OBSTitles.CornerInitials"), corner_row);
        add_effect_row(bgl_tr("OBSTitles.CornerType"), corner_type_row);

        connect(fill, QOverload<int>::of(&QComboBox::activated), this, [this, fill](int){ if (selected_effect()) { selected_effect()->effect_fill_type = fill->currentData().toInt(); emit_effect_changed(); }});
        connect(grad_type, QOverload<int>::of(&QComboBox::activated), this, [this, grad_type](int){ if (selected_effect()) { selected_effect()->effect_gradient_type = grad_type->currentData().toInt(); emit_effect_changed(); }});
        connect(grad_spread, QOverload<int>::of(&QComboBox::activated), this, [this, grad_spread](int){ if (selected_effect()) { selected_effect()->effect_gradient_spread = grad_spread->currentData().toInt(); emit_effect_changed(); }});
        connect(grad_angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_gradient_angle = v; emit_effect_changed(); }});
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = v; set_animated_value(selected_effect()->opacity_prop, lt, v); emit_effect_changed(); }});
        connect(stroke_width, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_stroke_width = v; set_animated_value(selected_effect()->stroke_width_prop, lt, v); emit_effect_changed(); }});
        connect(stroke_opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_stroke_opacity = v; set_animated_value(selected_effect()->stroke_opacity_prop, lt, v); emit_effect_changed(); }});
        connect(pad_left, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_padding_left = v; set_animated_value(selected_effect()->padding_left_prop, lt, v); emit_effect_changed(); }});
        connect(pad_right, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_padding_right = v; set_animated_value(selected_effect()->padding_right_prop, lt, v); emit_effect_changed(); }});
        connect(pad_top, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_padding_top = v; set_animated_value(selected_effect()->padding_top_prop, lt, v); emit_effect_changed(); }});
        connect(pad_bottom, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_padding_bottom = v; set_animated_value(selected_effect()->padding_bottom_prop, lt, v); emit_effect_changed(); }});
        connect(corner_tl, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_corner_radius_tl = v; set_animated_value(selected_effect()->corner_radius_tl_prop, lt, v); emit_effect_changed(); }});
        connect(corner_tr, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_corner_radius_tr = v; set_animated_value(selected_effect()->corner_radius_tr_prop, lt, v); emit_effect_changed(); }});
        connect(corner_br, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_corner_radius_br = v; set_animated_value(selected_effect()->corner_radius_br_prop, lt, v); emit_effect_changed(); }});
        connect(corner_bl, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_corner_radius_bl = v; set_animated_value(selected_effect()->corner_radius_bl_prop, lt, v); emit_effect_changed(); }});
        connect(corner_group, &QButtonGroup::idClicked, this, [this](int id){
            if (!loading_values_ && selected_effect()) {
                selected_effect()->effect_corner_type = std::clamp(id, 0, 3);
                emit_effect_changed();
            }
        });
    } else if (selected_effect()->type == LayerEffectType::Outline) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){
            if (!selected_effect()) return;
            selected_effect()->effect_color = argb;
            set_effect_color_channels_at(*selected_effect(), lt, argb);
        });
        auto *width = spin(0.0, 200.0, 1.0); width->setValue(effect->size_prop.is_animated() ? effect->size_prop.evaluate(lt) : effect->effect_size);
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);
        auto *join = combo(); join->addItem(bgl_tr("OBSTitles.Miter"), 0); join->addItem(bgl_tr("OBSTitles.Round"), 1); join->addItem(bgl_tr("OBSTitles.Bevel"), 2); join->setCurrentIndex(join->findData(effect->effect_join_style));
        auto *position = combo(); position->addItem(bgl_tr("OBSTitles.Back"), 0); position->addItem(bgl_tr("OBSTitles.Front"), 1); position->setCurrentIndex(effect->effect_on_front ? 1 : 0);
        auto *aa = new QCheckBox(bgl_tr("OBSTitles.AntialiasOutline"), box); aa->setChecked(effect->effect_antialias);
        bind_color(color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_color(effect, t);
        });
        bind_numeric(width, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), color);
        add_effect_row(bgl_tr("OBSTitles.ThicknessLabel"), width);
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(bgl_tr("OBSTitles.JoinLabel"), join);
        add_effect_row(bgl_tr("OBSTitles.PositionLabelIndented"), position);
        add_effect_row(QString(), aa);
        connect(width, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = v; set_animated_value(selected_effect()->size_prop, lt, v); emit_effect_changed(); }});
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = v; set_animated_value(selected_effect()->opacity_prop, lt, v); emit_effect_changed(); }});
        connect(join, QOverload<int>::of(&QComboBox::activated), this, [this, join](int){ if (selected_effect()) { selected_effect()->effect_join_style = join->currentData().toInt(); emit_effect_changed(); }});
        connect(position, QOverload<int>::of(&QComboBox::activated), this, [this, position](int){ if (selected_effect()) { selected_effect()->effect_on_front = position->currentData().toInt() == 1; emit_effect_changed(); }});
        connect(aa, &QCheckBox::toggled, this, [this](bool v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_antialias = v; emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::DropShadow) {
        LayerEffect *effect = selected_effect();
        auto *preset = combo(); preset->addItems({bgl_tr("OBSTitles.Custom"), bgl_tr("OBSTitles.Soft"), bgl_tr("OBSTitles.Medium"), bgl_tr("OBSTitles.Strong"), bgl_tr("OBSTitles.Broadcast")});
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color = argb; set_effect_color_channels_at(*selected_effect(), lt, argb); } });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);
        auto *dist = spin(0.0, 4096.0, 1.0); dist->setValue(effect->distance_prop.is_animated() ? effect->distance_prop.evaluate(lt) : effect->effect_distance);
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(effect->angle_prop.is_animated() ? effect->angle_prop.evaluate(lt) : effect->effect_angle);
        auto *blur = spin(0.0, 512.0, 1.0); blur->setValue(effect->size_prop.is_animated() ? effect->size_prop.evaluate(lt) : effect->effect_size);
        auto *spread = spin(0.0, 512.0, 1.0); spread->setValue(effect->spread_prop.is_animated() ? effect->spread_prop.evaluate(lt) : effect->effect_spread);
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_color(color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_color(effect, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(dist, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.distance_prop, effect.effect_distance, t);
        });
        bind_numeric(angle, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.angle_prop, effect.effect_angle, t);
        });
        bind_numeric(blur, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        bind_numeric(spread, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.spread_prop, effect.effect_spread, t);
        });
        add_effect_row(bgl_tr("OBSTitles.PresetLabel"), preset);
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), color);
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(bgl_tr("OBSTitles.DistanceLabel"), dist);
        add_effect_row(bgl_tr("OBSTitles.AngleLabel"), angle);
        add_effect_row(bgl_tr("OBSTitles.BlurTypeLabel"), blur_type);
        add_effect_row(bgl_tr("OBSTitles.BlurLabel"), blur);
        add_effect_row(bgl_tr("OBSTitles.SpreadLabel"), spread);
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; set_animated_value(selected_effect()->opacity_prop, lt, v); emit_effect_changed(); }});
        connect(dist, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_distance = (float)v; set_animated_value(selected_effect()->distance_prop, lt, v); emit_effect_changed(); }});
        connect(angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_angle = (float)v; set_animated_value(selected_effect()->angle_prop, lt, v); emit_effect_changed(); }});
        connect(blur, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; set_animated_value(selected_effect()->size_prop, lt, v); emit_effect_changed(); }});
        connect(spread, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_spread = (float)v; set_animated_value(selected_effect()->spread_prop, lt, v); emit_effect_changed(); }});
        connect(preset, QOverload<int>::of(&QComboBox::activated), this, [this, preset]() {
            auto *effect = selected_effect();
            if (!effect) return;
            switch (preset->currentIndex()) {
            case 1: effect->effect_opacity = 0.35f; effect->effect_distance = 4.0f; effect->effect_size = 10.0f; effect->effect_spread = 0.0f; break;
            case 2: effect->effect_opacity = 0.55f; effect->effect_distance = 8.0f; effect->effect_size = 8.0f; effect->effect_spread = 0.0f; break;
            case 3: effect->effect_opacity = 0.75f; effect->effect_distance = 12.0f; effect->effect_size = 6.0f; effect->effect_spread = 0.0f; break;
            case 4: effect->effect_opacity = 0.6f; effect->effect_distance = 6.0f; effect->effect_size = 3.0f; effect->effect_spread = 2.0f; break;
            default: return;
            }
            const double t = layer_ ? std::clamp(playhead_ - layer_->in_time, 0.0, std::max(0.0, layer_->out_time - layer_->in_time)) : 0.0;
            set_animated_value(effect->opacity_prop, t, effect->effect_opacity);
            set_animated_value(effect->distance_prop, t, effect->effect_distance);
            set_animated_value(effect->size_prop, t, effect->effect_size);
            set_animated_value(effect->spread_prop, t, effect->effect_spread);
            emit_effect_changed();
            load_settings();
        });
    } else if (selected_effect()->type == LayerEffectType::BrightnessContrast) {
        LayerEffect *effect = selected_effect();
        auto *brightness = spin(-1.0, 1.0, 0.01); brightness->setDecimals(2); brightness->setValue(effect->brightness);
        auto *contrast = spin(0.0, 4.0, 0.05); contrast->setDecimals(2); contrast->setValue(effect->contrast);
        add_effect_row(bgl_tr("OBSTitles.BrightnessLabel"), brightness);
        add_effect_row(bgl_tr("OBSTitles.ContrastLabel"), contrast);
        connect(brightness, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->brightness = (float)v; emit_effect_changed(); }});
        connect(contrast, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->contrast = (float)v; emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::Saturation) {
        LayerEffect *effect = selected_effect();
        auto *saturation = spin(0.0, 4.0, 0.05); saturation->setDecimals(2); saturation->setValue(effect->saturation);
        add_effect_row(bgl_tr("OBSTitles.SaturationLabel"), saturation);
        connect(saturation, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->saturation = (float)v; emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::ColorOverlay) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(effect->effect_color, [this](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color = argb; selected_effect()->tint_color = argb; } });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(panel_eval_effect_property(effect->opacity_prop, effect->effect_opacity, lt));
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        add_effect_row(bgl_tr("OBSTitles.ColorOverlayColorLabel"), color);
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; selected_effect()->tint_amount = (float)v; set_animated_value(selected_effect()->opacity_prop, lt, v); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::Glow || selected_effect()->type == LayerEffectType::InnerGlow) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(effect->effect_color, [this](uint32_t argb){ if (selected_effect()) selected_effect()->effect_color = argb; });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(panel_eval_effect_property(effect->opacity_prop, effect->effect_opacity, lt));
        auto *size = spin(0.0, 512.0, 1.0); size->setValue(panel_eval_effect_property(effect->size_prop, effect->effect_size, lt));
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(size, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), color);
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(bgl_tr("OBSTitles.SizeRadiusLabel"), size);
        add_effect_row(bgl_tr("OBSTitles.BlurTypeLabel"), blur_type);
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; set_animated_value(selected_effect()->opacity_prop, lt, v); emit_effect_changed(); }});
        connect(size, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; set_animated_value(selected_effect()->size_prop, lt, v); emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::Blur) {
        LayerEffect *effect = selected_effect();
        auto *amount = spin(0.0, 1.0, 0.05); amount->setDecimals(2); amount->setValue(panel_eval_effect_property(effect->opacity_prop, effect->effect_opacity, lt));
        auto *size = spin(0.0, 512.0, 1.0); size->setValue(panel_eval_effect_property(effect->size_prop, effect->effect_size, lt));
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        bind_numeric(amount, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(size, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        add_effect_row(bgl_tr("OBSTitles.AmountLabel"), amount);
        add_effect_row(bgl_tr("OBSTitles.SizeRadiusLabel"), size);
        add_effect_row(bgl_tr("OBSTitles.BlurTypeLabel"), blur_type);
        connect(amount, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; set_animated_value(selected_effect()->opacity_prop, lt, v); emit_effect_changed(); }});
        connect(size, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; set_animated_value(selected_effect()->size_prop, lt, v); emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::MotionBlur) {
        LayerEffect *effect = selected_effect();
        auto *amount = spin(0.0, 1.0, 0.05); amount->setDecimals(2); amount->setValue(panel_eval_effect_property(effect->opacity_prop, effect->effect_opacity, lt));
        auto *shutter = spin(0.0, 720.0, 5.0); shutter->setValue(panel_eval_effect_property(effect->size_prop, effect->effect_size, lt));
        auto *samples = new QSpinBox(box); samples->setRange(2, 64); samples->setValue(effect->effect_samples); samples->setFixedHeight(22);
        auto *centered = new QCheckBox(bgl_tr("OBSTitles.MotionBlurCentered"), box); centered->setChecked(effect->effect_centered);
        bind_numeric(amount, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(shutter, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        add_effect_row(bgl_tr("OBSTitles.AmountLabel"), amount);
        add_effect_row(bgl_tr("OBSTitles.ShutterAngleLabel"), shutter);
        add_effect_row(bgl_tr("OBSTitles.SamplesLabel"), samples);
        add_effect_row(QString(), centered);
        connect(amount, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; set_animated_value(selected_effect()->opacity_prop, lt, v); emit_effect_changed(); }});
        connect(shutter, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; set_animated_value(selected_effect()->size_prop, lt, v); emit_effect_changed(); }});
        connect(samples, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_samples = v; emit_effect_changed(); }});
        connect(centered, &QCheckBox::toggled, this, [this](bool v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_centered = v; emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::Bloom) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color = argb; set_effect_color_channels_at(*selected_effect(), lt, argb); emit_effect_changed(); } });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);
        auto *threshold = spin(0.0, 1.0, 0.01); threshold->setDecimals(2); threshold->setValue(effect->spread_prop.is_animated() ? effect->spread_prop.evaluate(lt) : effect->effect_spread);
        auto *radius = spin(0.0, 512.0, 1.0); radius->setValue(effect->size_prop.is_animated() ? effect->size_prop.evaluate(lt) : effect->effect_size);
        auto *intensity = spin(0.0, 8.0, 0.1); intensity->setDecimals(2); intensity->setValue(effect->falloff_prop.is_animated() ? effect->falloff_prop.evaluate(lt) : effect->effect_falloff);
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_color(color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_color(effect, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(threshold, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.spread_prop, effect.effect_spread, t);
        });
        bind_numeric(radius, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        bind_numeric(intensity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.falloff_prop, effect.effect_falloff, t);
        });
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), color);
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(bgl_tr("OBSTitles.ThresholdLabel"), threshold);
        add_effect_row(bgl_tr("OBSTitles.SizeRadiusLabel"), radius);
        add_effect_row(bgl_tr("OBSTitles.IntensityLabel"), intensity);
        add_effect_row(bgl_tr("OBSTitles.BlurTypeLabel"), blur_type);
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity=(float)v; set_animated_value(selected_effect()->opacity_prop, lt, v); emit_effect_changed(); }});
        connect(threshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_spread=(float)v; set_animated_value(selected_effect()->spread_prop, lt, v); emit_effect_changed(); }});
        connect(radius, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size=(float)v; set_animated_value(selected_effect()->size_prop, lt, v); emit_effect_changed(); }});
        connect(intensity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_falloff=(float)v; set_animated_value(selected_effect()->falloff_prop, lt, v); emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type=blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode=(EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::LensFlare) {
        LayerEffect *effect = selected_effect();
        auto *profile = combo();
        profile->addItem(QStringLiteral("Classic 35mm"), 0);
        profile->addItem(QStringLiteral("Anamorphic Blue"), 1);
        profile->addItem(QStringLiteral("Cinematic Warm"), 2);
        profile->addItem(QStringLiteral("Modern Sci-Fi"), 3);
        profile->addItem(QStringLiteral("Subtle Natural"), 4);
        profile->setCurrentIndex(profile->findData(effect->effect_profile));
        auto *primary = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb) {
            if (!selected_effect()) return;
            selected_effect()->effect_color = argb;
            set_effect_color_channels_at(*selected_effect(), lt, argb);
        });
        auto *secondary = color_button(panel_eval_effect_secondary_color(*effect, lt), [this, lt](uint32_t argb) {
            if (!selected_effect()) return;
            selected_effect()->effect_secondary_color = argb;
            set_effect_secondary_color_channels_at(*selected_effect(), lt, argb);
        });
        auto *amount = spin(0.0, 10.0, 0.05); amount->setValue(panel_eval_effect_property(effect->amount_prop, effect->effect_amount, lt));
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setValue(panel_eval_effect_property(effect->opacity_prop, effect->effect_opacity, lt));
        auto *scale = spin(0.01, 20.0, 0.05); scale->setValue(panel_eval_effect_property(effect->scale_prop, effect->effect_scale, lt));
        auto *radius = spin(0.001, 4.0, 0.01); radius->setValue(panel_eval_effect_property(effect->size_prop, effect->effect_size, lt));
        auto *spread = spin(0.0, 4.0, 0.05); spread->setValue(panel_eval_effect_property(effect->spread_prop, effect->effect_spread, lt));
        auto *falloff = spin(0.01, 16.0, 0.1); falloff->setValue(panel_eval_effect_property(effect->falloff_prop, effect->effect_falloff, lt));
        auto *angle = spin(-360.0, 360.0, 1.0); angle->setValue(panel_eval_effect_property(effect->angle_prop, effect->effect_angle, lt));
        auto *center_x = spin(-4.0, 4.0, 0.01); center_x->setValue(panel_eval_effect_property(effect->center_x_prop, effect->effect_center_x, lt));
        auto *center_y = spin(-4.0, 4.0, 0.01); center_y->setValue(panel_eval_effect_property(effect->center_y_prop, effect->effect_center_y, lt));
        auto *ghosts = spin(2.0, 12.0, 1.0);
        ghosts->setDecimals(0);
        ghosts->setValue(panel_eval_effect_property(
            effect->complexity_prop, effect->effect_complexity, lt));
        bind_color(primary, [](const LayerEffect &e, double t) { return panel_eval_effect_color(e, t); });
        bind_color(secondary, [](const LayerEffect &e, double t) { return panel_eval_effect_secondary_color(e, t); });
        bind_numeric(amount, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.amount_prop, e.effect_amount, t); });
        bind_numeric(opacity, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.opacity_prop, e.effect_opacity, t); });
        bind_numeric(scale, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.scale_prop, e.effect_scale, t); });
        bind_numeric(radius, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.size_prop, e.effect_size, t); });
        bind_numeric(spread, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.spread_prop, e.effect_spread, t); });
        bind_numeric(falloff, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.falloff_prop, e.effect_falloff, t); });
        bind_numeric(angle, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.angle_prop, e.effect_angle, t); });
        bind_numeric(center_x, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.center_x_prop, e.effect_center_x, t); });
        bind_numeric(center_y, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.center_y_prop, e.effect_center_y, t); });
        bind_numeric(ghosts, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.complexity_prop, e.effect_complexity, t); });
        add_effect_row(bgl_tr("OBSTitles.EffectProfile"), profile);
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), primary);
        add_effect_row(bgl_tr("OBSTitles.SecondaryColor"), secondary);
        add_effect_row(bgl_tr("OBSTitles.Amount"), amount);
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(bgl_tr("OBSTitles.Scale"), scale);
        add_effect_row(bgl_tr("OBSTitles.SizeRadiusLabel"), radius);
        add_effect_row(bgl_tr("OBSTitles.SpreadLabel"), spread);
        add_effect_row(bgl_tr("OBSTitles.FalloffLabel"), falloff);
        add_effect_row(bgl_tr("OBSTitles.AngleLabel"), angle);
        add_effect_row(bgl_tr("OBSTitles.CenterX"), center_x);
        add_effect_row(bgl_tr("OBSTitles.CenterY"), center_y);
        add_effect_row(bgl_tr("OBSTitles.Ghosts"), ghosts);
        connect(profile, QOverload<int>::of(&QComboBox::activated), this, [this, profile](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_profile=profile->currentData().toInt(); emit_effect_changed(); }});
        connect(ghosts, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, lt](double v) {
                    if (!loading_values_ && selected_effect()) {
                        selected_effect()->effect_complexity = (float)v;
                        selected_effect()->effect_samples = (int)std::round(v);
                        set_animated_value(selected_effect()->complexity_prop, lt, v);
                        emit_effect_changed();
                    }
                });
        const auto bind_value = [this, lt](QDoubleSpinBox *w, float LayerEffect::*field, AnimatedProperty LayerEffect::*prop) {
            connect(w, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt, field, prop](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->*field=(float)v; set_animated_value(selected_effect()->*prop, lt, v); emit_effect_changed(); }});
        };
        bind_value(amount, &LayerEffect::effect_amount, &LayerEffect::amount_prop);
        bind_value(opacity, &LayerEffect::effect_opacity, &LayerEffect::opacity_prop);
        bind_value(scale, &LayerEffect::effect_scale, &LayerEffect::scale_prop);
        bind_value(radius, &LayerEffect::effect_size, &LayerEffect::size_prop);
        bind_value(spread, &LayerEffect::effect_spread, &LayerEffect::spread_prop);
        bind_value(falloff, &LayerEffect::effect_falloff, &LayerEffect::falloff_prop);
        bind_value(angle, &LayerEffect::effect_angle, &LayerEffect::angle_prop);
        bind_value(center_x, &LayerEffect::effect_center_x, &LayerEffect::center_x_prop);
        bind_value(center_y, &LayerEffect::effect_center_y, &LayerEffect::center_y_prop);
    } else if (selected_effect()->type == LayerEffectType::Vignette) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color=argb; set_effect_color_channels_at(*selected_effect(), lt, argb); }});
        auto *amount = spin(0.0, 2.0, 0.02); amount->setValue(panel_eval_effect_property(effect->amount_prop, effect->effect_amount, lt));
        auto *scale = spin(0.01, 4.0, 0.02); scale->setValue(panel_eval_effect_property(effect->scale_prop, effect->effect_scale, lt));
        auto *soft = spin(0.0, 1.0, 0.01); soft->setValue(panel_eval_effect_property(effect->softness_prop, effect->effect_softness, lt));
        auto *round = spin(-1.0, 1.0, 0.02); round->setValue(panel_eval_effect_property(effect->roundness_prop, effect->effect_roundness, lt));
        auto *cx = spin(-4.0, 4.0, 0.01); cx->setValue(panel_eval_effect_property(effect->center_x_prop, effect->effect_center_x, lt));
        auto *cy = spin(-4.0, 4.0, 0.01); cy->setValue(panel_eval_effect_property(effect->center_y_prop, effect->effect_center_y, lt));
        auto *invert = new QCheckBox(bgl_tr("OBSTitles.Invert"), box); invert->setChecked(effect->effect_invert);
        bind_color(color, [](const LayerEffect &e,double t){ return panel_eval_effect_color(e,t); });
        const auto init_bind=[&](QDoubleSpinBox *w, const AnimatedProperty LayerEffect::*prop, const float LayerEffect::*field){ bind_numeric(w,[prop,field](const LayerEffect&e,double t){return panel_eval_effect_property(e.*prop,e.*field,t);});};
        init_bind(amount,&LayerEffect::amount_prop,&LayerEffect::effect_amount); init_bind(scale,&LayerEffect::scale_prop,&LayerEffect::effect_scale); init_bind(soft,&LayerEffect::softness_prop,&LayerEffect::effect_softness); init_bind(round,&LayerEffect::roundness_prop,&LayerEffect::effect_roundness); init_bind(cx,&LayerEffect::center_x_prop,&LayerEffect::effect_center_x); init_bind(cy,&LayerEffect::center_y_prop,&LayerEffect::effect_center_y);
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"),color); add_effect_row(bgl_tr("OBSTitles.Amount"),amount); add_effect_row(bgl_tr("OBSTitles.Scale"),scale); add_effect_row(bgl_tr("OBSTitles.SoftnessLabel"),soft); add_effect_row(bgl_tr("OBSTitles.Roundness"),round); add_effect_row(bgl_tr("OBSTitles.CenterX"),cx); add_effect_row(bgl_tr("OBSTitles.CenterY"),cy); add_effect_row(QString(),invert);
        const auto bind_value=[this,lt](QDoubleSpinBox*w,float LayerEffect::*f,AnimatedProperty LayerEffect::*p){connect(w,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this,lt,f,p](double v){if(!loading_values_&&selected_effect()){selected_effect()->*f=(float)v;set_animated_value(selected_effect()->*p,lt,v);emit_effect_changed();}});};
        bind_value(amount,&LayerEffect::effect_amount,&LayerEffect::amount_prop); bind_value(scale,&LayerEffect::effect_scale,&LayerEffect::scale_prop); bind_value(soft,&LayerEffect::effect_softness,&LayerEffect::softness_prop); bind_value(round,&LayerEffect::effect_roundness,&LayerEffect::roundness_prop); bind_value(cx,&LayerEffect::effect_center_x,&LayerEffect::center_x_prop); bind_value(cy,&LayerEffect::effect_center_y,&LayerEffect::center_y_prop);
        connect(invert,&QCheckBox::toggled,this,[this](bool v){if(!loading_values_&&selected_effect()){selected_effect()->effect_invert=v;emit_effect_changed();}});
    } else if (selected_effect()->type == LayerEffectType::Noise || selected_effect()->type == LayerEffectType::RoughenEdges) {
        LayerEffect *effect = selected_effect();
        const bool noise = effect->type == LayerEffectType::Noise;
        QComboBox *profile = nullptr;
        if (noise) { profile=combo(); profile->addItem(QStringLiteral("Uniform"),0); profile->addItem(QStringLiteral("Smooth"),1); profile->addItem(QStringLiteral("Gaussian"),2); profile->addItem(QStringLiteral("Film Grain"),3); profile->addItem(QStringLiteral("Digital Speckle"),4); profile->addItem(QStringLiteral("Organic Grain"),5); profile->setCurrentIndex(profile->findData(effect->effect_profile)); add_effect_row(bgl_tr("OBSTitles.EffectProfile"),profile); }
        auto *opacity=spin(0.0,1.0,0.01); opacity->setValue(panel_eval_effect_property(effect->opacity_prop,effect->effect_opacity,lt));
        auto *amount=spin(0.0,4.0,0.01); amount->setValue(panel_eval_effect_property(effect->amount_prop,effect->effect_amount,lt));
        auto *scale=spin(0.001,1000.0,0.1); scale->setValue(panel_eval_effect_property(effect->scale_prop,effect->effect_scale,lt));
        auto *soft=spin(0.0,1.0,0.01); soft->setValue(panel_eval_effect_property(effect->softness_prop,effect->effect_softness,lt));
        auto *complexity=spin(1.0,12.0,0.25); complexity->setValue(panel_eval_effect_property(effect->complexity_prop,effect->effect_complexity,lt));
        auto *evolution=spin(-100000.0,100000.0,1.0); evolution->setValue(panel_eval_effect_property(effect->evolution_prop,effect->effect_evolution,lt));
        auto *seed=new QSpinBox(box); seed->setRange(0,1000000); seed->setValue(effect->effect_seed);
        QDoubleSpinBox *speed=nullptr; QCheckBox *animated=nullptr; QCheckBox *mono=nullptr;
        if(noise){speed=spin(-100.0,100.0,0.1);speed->setValue(panel_eval_effect_property(effect->speed_prop,effect->effect_speed,lt));animated=new QCheckBox(bgl_tr("OBSTitles.Animated"),box);animated->setChecked(effect->effect_animated);mono=new QCheckBox(bgl_tr("OBSTitles.Monochrome"),box);mono->setChecked(effect->effect_monochrome);}
        auto *invert=new QCheckBox(bgl_tr("OBSTitles.Invert"),box);invert->setChecked(effect->effect_invert);
        const auto init_bind=[&](QDoubleSpinBox*w,const AnimatedProperty LayerEffect::*p,const float LayerEffect::*f){if(w)bind_numeric(w,[p,f](const LayerEffect&e,double t){return panel_eval_effect_property(e.*p,e.*f,t);});};
        init_bind(opacity,&LayerEffect::opacity_prop,&LayerEffect::effect_opacity);init_bind(amount,&LayerEffect::amount_prop,&LayerEffect::effect_amount);init_bind(scale,&LayerEffect::scale_prop,&LayerEffect::effect_scale);init_bind(soft,&LayerEffect::softness_prop,&LayerEffect::effect_softness);init_bind(complexity,&LayerEffect::complexity_prop,&LayerEffect::effect_complexity);init_bind(evolution,&LayerEffect::evolution_prop,&LayerEffect::effect_evolution);init_bind(speed,&LayerEffect::speed_prop,&LayerEffect::effect_speed);
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"),opacity);add_effect_row(bgl_tr("OBSTitles.Amount"),amount);add_effect_row(bgl_tr("OBSTitles.Scale"),scale);add_effect_row(bgl_tr("OBSTitles.SoftnessLabel"),soft);add_effect_row(bgl_tr("OBSTitles.Complexity"),complexity);add_effect_row(bgl_tr("OBSTitles.Evolution"),evolution);if(speed)add_effect_row(bgl_tr("OBSTitles.Speed"),speed);add_effect_row(bgl_tr("OBSTitles.Seed"),seed);if(animated)add_effect_row(QString(),animated);if(mono)add_effect_row(QString(),mono);add_effect_row(QString(),invert);
        const auto bind_value=[this,lt](QDoubleSpinBox*w,float LayerEffect::*f,AnimatedProperty LayerEffect::*p){if(w)connect(w,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this,lt,f,p](double v){if(!loading_values_&&selected_effect()){selected_effect()->*f=(float)v;set_animated_value(selected_effect()->*p,lt,v);emit_effect_changed();}});};
        bind_value(opacity,&LayerEffect::effect_opacity,&LayerEffect::opacity_prop);bind_value(amount,&LayerEffect::effect_amount,&LayerEffect::amount_prop);bind_value(scale,&LayerEffect::effect_scale,&LayerEffect::scale_prop);bind_value(soft,&LayerEffect::effect_softness,&LayerEffect::softness_prop);bind_value(complexity,&LayerEffect::effect_complexity,&LayerEffect::complexity_prop);bind_value(evolution,&LayerEffect::effect_evolution,&LayerEffect::evolution_prop);bind_value(speed,&LayerEffect::effect_speed,&LayerEffect::speed_prop);
        if(profile)connect(profile,QOverload<int>::of(&QComboBox::activated),this,[this,profile](int){if(!loading_values_&&selected_effect()){selected_effect()->effect_profile=profile->currentData().toInt();emit_effect_changed();}});
        connect(seed,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int v){if(!loading_values_&&selected_effect()){selected_effect()->effect_seed=v;emit_effect_changed();}});
        if(animated)connect(animated,&QCheckBox::toggled,this,[this](bool v){if(!loading_values_&&selected_effect()){selected_effect()->effect_animated=v;emit_effect_changed();}});
        if(mono)connect(mono,&QCheckBox::toggled,this,[this](bool v){if(!loading_values_&&selected_effect()){selected_effect()->effect_monochrome=v;emit_effect_changed();}});
        connect(invert,&QCheckBox::toggled,this,[this](bool v){if(!loading_values_&&selected_effect()){selected_effect()->effect_invert=v;emit_effect_changed();}});
    } else if (selected_effect()->type == LayerEffectType::Emboss) {
        LayerEffect *effect = selected_effect();
        auto *depth = spin(0.1, 32.0, 0.1); depth->setDecimals(2); depth->setValue(effect->size_prop.is_animated() ? effect->size_prop.evaluate(lt) : effect->effect_size);
        auto *height = spin(0.1, 32.0, 0.1); height->setDecimals(2); height->setValue(effect->distance_prop.is_animated() ? effect->distance_prop.evaluate(lt) : effect->effect_distance);
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(effect->angle_prop.is_animated() ? effect->angle_prop.evaluate(lt) : effect->effect_angle);
        auto *softness = spin(0.0, 16.0, 0.1); softness->setDecimals(2); softness->setValue(effect->spread_prop.is_animated() ? effect->spread_prop.evaluate(lt) : effect->effect_spread);
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_numeric(depth, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        bind_numeric(height, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.distance_prop, effect.effect_distance, t);
        });
        bind_numeric(angle, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.angle_prop, effect.effect_angle, t);
        });
        bind_numeric(softness, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.spread_prop, effect.effect_spread, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        add_effect_row(bgl_tr("OBSTitles.DepthLabel"), depth);
        add_effect_row(bgl_tr("OBSTitles.HeightLabel"), height);
        add_effect_row(bgl_tr("OBSTitles.AngleLabel"), angle);
        add_effect_row(bgl_tr("OBSTitles.SoftnessLabel"), softness);
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(depth, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size=(float)v; set_animated_value(selected_effect()->size_prop, lt, v); emit_effect_changed(); }});
        connect(height, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_distance=(float)v; set_animated_value(selected_effect()->distance_prop, lt, v); emit_effect_changed(); }});
        connect(angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_angle=(float)v; set_animated_value(selected_effect()->angle_prop, lt, v); emit_effect_changed(); }});
        connect(softness, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_spread=(float)v; set_animated_value(selected_effect()->spread_prop, lt, v); emit_effect_changed(); }});
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity=(float)v; set_animated_value(selected_effect()->opacity_prop, lt, v); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode=(EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::InnerShadow) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){
            if (selected_effect()) {
                selected_effect()->effect_color = argb;
                set_effect_color_channels_at(*selected_effect(), lt, argb);
            }
        });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(panel_eval_effect_property(effect->opacity_prop, effect->effect_opacity, lt));
        auto *dist = spin(0.0, 4096.0, 1.0); dist->setValue(panel_eval_effect_property(effect->distance_prop, effect->effect_distance, lt));
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(panel_eval_effect_property(effect->angle_prop, effect->effect_angle, lt));
        auto *size = spin(0.0, 512.0, 1.0); size->setValue(panel_eval_effect_property(effect->size_prop, effect->effect_size, lt));
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_color(color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_color(effect, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(dist, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.distance_prop, effect.effect_distance, t);
        });
        bind_numeric(angle, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.angle_prop, effect.effect_angle, t);
        });
        bind_numeric(size, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), color);
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(bgl_tr("OBSTitles.DistanceLabel"), dist);
        add_effect_row(bgl_tr("OBSTitles.AngleLabel"), angle);
        add_effect_row(bgl_tr("OBSTitles.SizeRadiusLabel"), size);
        add_effect_row(bgl_tr("OBSTitles.BlurTypeLabel"), blur_type);
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; set_animated_value(selected_effect()->opacity_prop, lt, v); emit_effect_changed(); }});
        connect(dist, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_distance = (float)v; set_animated_value(selected_effect()->distance_prop, lt, v); emit_effect_changed(); }});
        connect(angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_angle = (float)v; set_animated_value(selected_effect()->angle_prop, lt, v); emit_effect_changed(); }});
        connect(size, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; set_animated_value(selected_effect()->size_prop, lt, v); emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::LongShadow) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color = argb; set_effect_color_channels_at(*selected_effect(), lt, argb); } });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);
        auto *length = spin(0.0, 4096.0, 5.0); length->setValue(effect->distance_prop.is_animated() ? effect->distance_prop.evaluate(lt) : effect->effect_distance);
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(effect->angle_prop.is_animated() ? effect->angle_prop.evaluate(lt) : effect->effect_angle);
        auto *falloff = spin(0.0, 8.0, 0.1); falloff->setDecimals(2); falloff->setValue(effect->falloff_prop.is_animated() ? effect->falloff_prop.evaluate(lt) : effect->effect_falloff);
        auto *blur_type = combo(); blur_type->addItem(bgl_tr("OBSTitles.NoBlur"), (int)LongShadowBlurType::None); blur_type->addItem(bgl_tr("OBSTitles.BoxBlur"), (int)LongShadowBlurType::Box); blur_type->addItem(bgl_tr("OBSTitles.GaussianBlur"), (int)LongShadowBlurType::Gaussian); blur_type->addItem(bgl_tr("OBSTitles.StackFastBlur"), (int)LongShadowBlurType::StackFast); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *blur = spin(0.0, 512.0, 1.0); blur->setValue(effect->size_prop.is_animated() ? effect->size_prop.evaluate(lt) : effect->effect_size);
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_color(color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_color(effect, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(length, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.distance_prop, effect.effect_distance, t);
        });
        bind_numeric(angle, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.angle_prop, effect.effect_angle, t);
        });
        bind_numeric(falloff, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.falloff_prop, effect.effect_falloff, t);
        });
        bind_numeric(blur, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        add_effect_row(bgl_tr("OBSTitles.LongShadowColor"), color);
        add_effect_row(bgl_tr("OBSTitles.LongShadowOpacity"), opacity);
        add_effect_row(bgl_tr("OBSTitles.LongShadowLength"), length);
        add_effect_row(bgl_tr("OBSTitles.LongShadowAngle"), angle);
        add_effect_row(bgl_tr("OBSTitles.LongShadowFalloff"), falloff);
        add_effect_row(bgl_tr("OBSTitles.LongShadowBlurType"), blur_type);
        add_effect_row(bgl_tr("OBSTitles.LongShadowBlur"), blur);
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; set_animated_value(selected_effect()->opacity_prop, lt, v); emit_effect_changed(); }});
        connect(length, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_distance = (float)v; set_animated_value(selected_effect()->distance_prop, lt, v); emit_effect_changed(); }});
        connect(angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_angle = (float)v; set_animated_value(selected_effect()->angle_prop, lt, v); emit_effect_changed(); }});
        connect(falloff, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_falloff = (float)v; set_animated_value(selected_effect()->falloff_prop, lt, v); emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blur, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; set_animated_value(selected_effect()->size_prop, lt, v); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    }
    settings_layout_->addWidget(box);
    settings_layout_->addStretch(1);
    loading_values_ = false;
}
