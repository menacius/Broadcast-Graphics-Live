#include "title-editor-internal.h"

#include <QAbstractItemModel>

static QString obsgs_effects_panel_style()
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
        "QWidget#OBSGraphicsStudioProEffectsPanel{background:@window@;color:@windowText@;}"
        "QLabel{color:@windowText@;background:transparent;}"
        "QListWidget{background:@base@;border:1px solid @mid@;color:@text@;alternate-background-color:@alternate@;}"
        "QListWidget::item{padding:4px;}"
        "QListWidget::item:selected{background:@highlight@;color:@highlightedText@;}"
        "QToolButton{color:@buttonText@;background:@button@;border:1px solid @mid@;border-radius:2px;padding:2px;}"
        "QToolButton:hover{background:@hover@;border-color:@mid@;}"
        "QToolButton:pressed{background:@highlight@;color:@highlightedText@;border-color:@highlight@;}"
        "QToolButton:checked{background:@highlight@;color:@highlightedText@;border-color:@highlight@;}"
        "QScrollArea{background:@window@;border:none;}"
        "QWidget#OBSGraphicsStudioProEffectsSettingsContainer{background:@window@;}"
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

static QString obsgs_theme_control_style()
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

EffectsPanel::EffectsPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("OBSGraphicsStudioProEffectsPanel"));
    setStyleSheet(obsgs_effects_panel_style());

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *hint = new QLabel(QStringLiteral("Effect stack"), this);
    QFont hint_font = hint->font();
    hint_font.setBold(true);
    hint->setFont(hint_font);
    layout->addWidget(hint);

    effect_list_ = new QListWidget(this);
    effect_list_->setObjectName(QStringLiteral("OBSGraphicsStudioProEffectsList"));
    effect_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    effect_list_->setAlternatingRowColors(true);
    effect_list_->setDragEnabled(true);
    effect_list_->setAcceptDrops(true);
    effect_list_->setDragDropMode(QAbstractItemView::InternalMove);
    effect_list_->setDefaultDropAction(Qt::MoveAction);
    effect_list_->setDropIndicatorShown(true);
    
    layout->addWidget(effect_list_, 1);

    auto *button_bar = new QWidget(this);
    button_bar->setObjectName(QStringLiteral("OBSGraphicsStudioProEffectsButtonBar"));
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

    auto *btn_add = add_button("add.svg", QStringLiteral("Add Effect"));
    btn_remove_ = add_button("delete.svg", QStringLiteral("Remove Effect"));
    btn_duplicate_ = add_button("duplicate.svg", QStringLiteral("Duplicate Effect"));
    btn_move_up_ = add_button("move-up.svg", QStringLiteral("Move Effect Up"));
    btn_move_down_ = add_button("move-down.svg", QStringLiteral("Move Effect Down"));
    btn_respect_masks_ = add_button("timeline-mask.svg", QStringLiteral("Apply Effect Stack After Mask"));
    btn_respect_masks_->setCheckable(true);
    btn_respect_masks_->setToolTip(QStringLiteral("Toggle whether this layer's effect stack respects track mattes/masks. When enabled, effects are applied after the mask."));
    button_layout->addStretch(1);
    layout->addWidget(button_bar);

    auto *settings_scroll = new QScrollArea(this);
    settings_scroll->setWidgetResizable(true);
    
    settings_container_ = new QWidget(settings_scroll);
    settings_container_->setObjectName(QStringLiteral("OBSGraphicsStudioProEffectsSettingsContainer"));
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
        add_action(obsgs_tr("OBSTitles.BackgroundColor"), LayerEffectType::BackgroundColor);
        add_action(obsgs_tr("OBSTitles.Outline"), LayerEffectType::Outline);
        add_action(obsgs_tr("OBSTitles.DropShadow"), LayerEffectType::DropShadow);
        add_action(obsgs_tr("OBSTitles.LongShadow"), LayerEffectType::LongShadow);
        add_action(obsgs_tr("OBSTitles.BrightnessContrast"), LayerEffectType::BrightnessContrast);
        add_action(obsgs_tr("OBSTitles.Saturation"), LayerEffectType::Saturation);
        add_action(obsgs_tr("OBSTitles.ColorOverlay"), LayerEffectType::ColorOverlay);
        add_action(obsgs_tr("OBSTitles.Glow"), LayerEffectType::Glow);
        add_action(obsgs_tr("OBSTitles.InnerGlow"), LayerEffectType::InnerGlow);
        add_action(obsgs_tr("OBSTitles.InnerShadow"), LayerEffectType::InnerShadow);
        add_action(obsgs_tr("OBSTitles.Blur"), LayerEffectType::Blur);
        add_action(obsgs_tr("OBSTitles.MotionBlur"), LayerEffectType::MotionBlur);
        QAction *chosen = menu.exec(btn_add->mapToGlobal(QPoint(0, btn_add->height())));
        if (!chosen) return;
        LayerEffect effect;
        effect.type = (LayerEffectType)chosen->data().toInt();
        effect.enabled = true;
        switch (effect.type) {
        case LayerEffectType::BackgroundColor:
            effect.effect_color = 0xFF000000;
            effect.effect_opacity = 0.35f;
            effect.opacity_prop.static_value = effect.effect_opacity;
            set_effect_color_channels_at(effect, 0.0, effect.effect_color);
            break;
        case LayerEffectType::Outline:
            effect.effect_fill_type = 1;
            effect.effect_color = 0xFF000000;
            effect.effect_size = 4.0f;
            effect.effect_opacity = 1.0f;
            effect.size_prop.static_value = effect.effect_size;
            effect.opacity_prop.static_value = effect.effect_opacity;
            set_effect_color_channels_at(effect, 0.0, effect.effect_color);
            break;
        case LayerEffectType::DropShadow:
        case LayerEffectType::InnerShadow:
            effect.blend_mode = EffectBlendMode::Multiply;
            effect.effect_color = 0x99000000;
            effect.effect_opacity = 0.6f;
            effect.effect_size = 4.0f;
            effect.opacity_prop.static_value = effect.effect_opacity;
            effect.size_prop.static_value = effect.effect_size;
            set_effect_color_channels_at(effect, 0.0, effect.effect_color);
            break;
        case LayerEffectType::LongShadow:
            effect.blend_mode = EffectBlendMode::Multiply;
            effect.effect_color = 0x99000000;
            effect.effect_distance = 120.0f;
            effect.effect_angle = 135.0f;
            effect.effect_size = 0.0f;
            effect.effect_opacity = 0.45f;
            effect.opacity_prop.static_value = effect.effect_opacity;
            effect.distance_prop.static_value = effect.effect_distance;
            effect.angle_prop.static_value = effect.effect_angle;
            effect.size_prop.static_value = effect.effect_size;
            set_effect_color_channels_at(effect, 0.0, effect.effect_color);
            break;
        case LayerEffectType::ColorOverlay:
            effect.blend_mode = EffectBlendMode::Color;
            effect.effect_color = effect.tint_color;
            effect.effect_opacity = effect.tint_amount;
            break;
        case LayerEffectType::Glow:
        case LayerEffectType::InnerGlow:
            effect.blend_mode = EffectBlendMode::Additive;
            effect.effect_color = 0xFFFFFFFF;
            effect.effect_opacity = 0.75f;
            break;
        case LayerEffectType::Blur:
            effect.effect_size = 12.0f;
            effect.effect_opacity = 1.0f;
            effect.effect_blur_type = (int)ShadowBlurType::Gaussian;
            break;
        case LayerEffectType::MotionBlur:
            effect.effect_size = 180.0f;
            effect.effect_angle = 0.0f;
            effect.effect_opacity = 1.0f;
            effect.effect_samples = 8;
            effect.effect_centered = true;
            break;
        default:
            break;
        }
        effect.enabled_prop.static_value = 1.0;
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
        std::swap(layer_->effects[selected_index_], layer_->effects[selected_index_ - 1]);
        --selected_index_;
        rebuild_stack();
        emit_effect_changed();
    });

    connect(btn_move_down_, &QToolButton::clicked, this, [this]() {
        if (!layer_ || selected_index_ < 0 || selected_index_ + 1 >= (int)layer_->effects.size()) return;
        std::swap(layer_->effects[selected_index_], layer_->effects[selected_index_ + 1]);
        ++selected_index_;
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
        effect_list_->addItem(QStringLiteral("Select a layer to edit effects"));
        if (auto *item = effect_list_->item(0)) item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        selected_index_ = -1;
    } else if (layer_->effects.empty()) {
        effect_list_->addItem(QStringLiteral("No effects added"));
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
    while (QLayoutItem *item = settings_layout_->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
}

void EffectsPanel::load_settings()
{
    build_settings();
    if (!layer_ || !selected_effect()) {
        auto *label = new QLabel(layer_ ? QStringLiteral("Add an effect to edit its settings.") : QStringLiteral("Select a layer to edit effects."), settings_container_);
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
    auto spin = [box](double min, double max, double step) { auto *s = new QDoubleSpinBox(box); s->setRange(min, max); s->setSingleStep(step); s->setFixedHeight(22); s->setStyleSheet(obsgs_theme_control_style()); return s; };
    auto combo = [box]() { auto *c = new QComboBox(box); c->setFixedHeight(22); c->setStyleSheet(obsgs_theme_control_style()); return c; };
    auto color_button = [this, box](uint32_t argb, auto setter) {
        auto *button = new QPushButton(box);
        set_color_button_argb(button, argb);
        connect(button, &QPushButton::clicked, this, [this, button, setter]() {
            QColor picked = QColorDialog::getColor(color_from_argb(color_button_argb(button)), this, QStringLiteral("Choose Color"), QColorDialog::ShowAlphaChannel);
            if (!picked.isValid()) return;
            uint32_t argb = argb_from_color(picked);
            set_color_button_argb(button, argb);
            setter(argb);
            emit_effect_changed();
        });
        return button;
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
        fill->addItem(obsgs_tr("OBSTitles.Solid"), 0);
        fill->addItem(obsgs_tr("OBSTitles.Gradient"), 1);
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
        corner_grid->addWidget(new QLabel(QStringLiteral("TL"), corner_row), 0, 0);
        corner_grid->addWidget(corner_tl, 0, 1);
        corner_grid->addWidget(new QLabel(QStringLiteral("TR"), corner_row), 0, 2);
        corner_grid->addWidget(corner_tr, 0, 3);
        corner_grid->addWidget(new QLabel(QStringLiteral("BL"), corner_row), 1, 0);
        corner_grid->addWidget(corner_bl, 1, 1);
        corner_grid->addWidget(new QLabel(QStringLiteral("BR"), corner_row), 1, 2);
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
            button->setStyleSheet(obsgs_theme_control_style());
            corner_group->addButton(button, (int)type);
            corner_type_layout->addWidget(button);
        };
        make_corner_button(CornerType::Round, "R", QStringLiteral("Round"));
        make_corner_button(CornerType::Straight, "B", QStringLiteral("Bevel / Straight"));
        make_corner_button(CornerType::Concave, "I", QStringLiteral("Inverse / Concave"));
        make_corner_button(CornerType::Cutout, "C", QStringLiteral("Inset / Cutout"));
        corner_type_layout->addStretch(1);
        if (auto *button = corner_group->button(effect->effect_corner_type))
            button->setChecked(true);

        auto *grad_type = combo();
        grad_type->addItem(obsgs_tr("OBSTitles.LinearGradient"), 0);
        grad_type->addItem(obsgs_tr("OBSTitles.RadialGradient"), 1);
        grad_type->addItem(QStringLiteral("Angle"), 2);
        grad_type->addItem(QStringLiteral("Reflected"), 3);
        grad_type->addItem(QStringLiteral("Diamond"), 4);
        grad_type->setCurrentIndex(grad_type->findData(effect->effect_gradient_type));
        auto *grad_start = color_button(effect->effect_gradient_start_color, [this](uint32_t argb){ if (selected_effect()) selected_effect()->effect_gradient_start_color = argb; });
        auto *grad_end = color_button(effect->effect_gradient_end_color, [this](uint32_t argb){ if (selected_effect()) selected_effect()->effect_gradient_end_color = argb; });
        auto *grad_angle = spin(-360.0, 360.0, 1.0); grad_angle->setValue(effect->effect_gradient_angle);

        form->addRow(section_label(QStringLiteral("Appearance")));
        add_effect_row(QStringLiteral("Fill"), fill);
        add_effect_row(QStringLiteral("Fill Color"), fill_color);
        add_effect_row(QStringLiteral("Stroke Color"), stroke_color);
        add_effect_row(QStringLiteral("Stroke Width"), stroke_width);
        add_effect_row(QStringLiteral("Stroke Opacity"), stroke_opacity);
        add_effect_row(obsgs_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(obsgs_tr("OBSTitles.GradientTypeLabel"), grad_type);
        add_effect_row(obsgs_tr("OBSTitles.StartColorLabel"), grad_start);
        add_effect_row(obsgs_tr("OBSTitles.EndColorLabel"), grad_end);
        add_effect_row(obsgs_tr("OBSTitles.AngleLabel"), grad_angle);
        form->addRow(section_label(QStringLiteral("Padding")));
        add_effect_row(QStringLiteral("Left Padding"), pad_left);
        add_effect_row(QStringLiteral("Right Padding"), pad_right);
        add_effect_row(QStringLiteral("Top Padding"), pad_top);
        add_effect_row(QStringLiteral("Bottom Padding"), pad_bottom);
        form->addRow(section_label(QStringLiteral("Corners")));
        add_effect_row(QStringLiteral("TL/TR/BL/BR"), corner_row);
        add_effect_row(QStringLiteral("Corner Type"), corner_type_row);

        connect(fill, QOverload<int>::of(&QComboBox::activated), this, [this, fill](int){ if (selected_effect()) { selected_effect()->effect_fill_type = fill->currentData().toInt(); emit_effect_changed(); }});
        connect(grad_type, QOverload<int>::of(&QComboBox::activated), this, [this, grad_type](int){ if (selected_effect()) { selected_effect()->effect_gradient_type = grad_type->currentData().toInt(); emit_effect_changed(); }});
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
        auto *join = combo(); join->addItem(obsgs_tr("OBSTitles.Miter"), 0); join->addItem(obsgs_tr("OBSTitles.Round"), 1); join->addItem(obsgs_tr("OBSTitles.Bevel"), 2); join->setCurrentIndex(join->findData(effect->effect_join_style));
        auto *position = combo(); position->addItem(obsgs_tr("OBSTitles.Back"), 0); position->addItem(obsgs_tr("OBSTitles.Front"), 1); position->setCurrentIndex(effect->effect_on_front ? 1 : 0);
        auto *aa = new QCheckBox(obsgs_tr("OBSTitles.AntialiasOutline"), box); aa->setChecked(effect->effect_antialias);
        add_effect_row(obsgs_tr("OBSTitles.ColorLabel"), color);
        add_effect_row(obsgs_tr("OBSTitles.ThicknessLabel"), width);
        add_effect_row(obsgs_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(obsgs_tr("OBSTitles.JoinLabel"), join);
        add_effect_row(obsgs_tr("OBSTitles.PositionLabelIndented"), position);
        add_effect_row(QString(), aa);
        connect(width, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = v; set_animated_value(selected_effect()->size_prop, lt, v); emit_effect_changed(); }});
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = v; set_animated_value(selected_effect()->opacity_prop, lt, v); emit_effect_changed(); }});
        connect(join, QOverload<int>::of(&QComboBox::activated), this, [this, join](int){ if (selected_effect()) { selected_effect()->effect_join_style = join->currentData().toInt(); emit_effect_changed(); }});
        connect(position, QOverload<int>::of(&QComboBox::activated), this, [this, position](int){ if (selected_effect()) { selected_effect()->effect_on_front = position->currentData().toInt() == 1; emit_effect_changed(); }});
        connect(aa, &QCheckBox::toggled, this, [this](bool v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_antialias = v; emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::DropShadow) {
        LayerEffect *effect = selected_effect();
        auto *preset = combo(); preset->addItems({obsgs_tr("OBSTitles.Custom"), obsgs_tr("OBSTitles.Soft"), obsgs_tr("OBSTitles.Medium"), obsgs_tr("OBSTitles.Strong"), obsgs_tr("OBSTitles.Broadcast")});
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color = argb; set_effect_color_channels_at(*selected_effect(), lt, argb); } });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);
        auto *dist = spin(0.0, 4096.0, 1.0); dist->setValue(effect->distance_prop.is_animated() ? effect->distance_prop.evaluate(lt) : effect->effect_distance);
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(effect->angle_prop.is_animated() ? effect->angle_prop.evaluate(lt) : effect->effect_angle);
        auto *blur = spin(0.0, 512.0, 1.0); blur->setValue(effect->size_prop.is_animated() ? effect->size_prop.evaluate(lt) : effect->effect_size);
        auto *spread = spin(0.0, 512.0, 1.0); spread->setValue(effect->spread_prop.is_animated() ? effect->spread_prop.evaluate(lt) : effect->effect_spread);
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        add_effect_row(obsgs_tr("OBSTitles.PresetLabel"), preset);
        add_effect_row(obsgs_tr("OBSTitles.ColorLabel"), color);
        add_effect_row(obsgs_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(obsgs_tr("OBSTitles.DistanceLabel"), dist);
        add_effect_row(obsgs_tr("OBSTitles.AngleLabel"), angle);
        add_effect_row(obsgs_tr("OBSTitles.BlurTypeLabel"), blur_type);
        add_effect_row(obsgs_tr("OBSTitles.BlurLabel"), blur);
        add_effect_row(obsgs_tr("OBSTitles.SpreadLabel"), spread);
        add_effect_row(obsgs_tr("OBSTitles.BlendingModeLabel"), blend);
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
        add_effect_row(obsgs_tr("OBSTitles.BrightnessLabel"), brightness);
        add_effect_row(obsgs_tr("OBSTitles.ContrastLabel"), contrast);
        connect(brightness, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->brightness = (float)v; emit_effect_changed(); }});
        connect(contrast, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->contrast = (float)v; emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::Saturation) {
        LayerEffect *effect = selected_effect();
        auto *saturation = spin(0.0, 4.0, 0.05); saturation->setDecimals(2); saturation->setValue(effect->saturation);
        add_effect_row(obsgs_tr("OBSTitles.SaturationLabel"), saturation);
        connect(saturation, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->saturation = (float)v; emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::ColorOverlay) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(effect->effect_color, [this](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color = argb; selected_effect()->tint_color = argb; } });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->effect_opacity);
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        add_effect_row(obsgs_tr("OBSTitles.ColorOverlayColorLabel"), color);
        add_effect_row(obsgs_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(obsgs_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; selected_effect()->tint_amount = (float)v; emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::Glow || selected_effect()->type == LayerEffectType::InnerGlow) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(effect->effect_color, [this](uint32_t argb){ if (selected_effect()) selected_effect()->effect_color = argb; });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->effect_opacity);
        auto *size = spin(0.0, 512.0, 1.0); size->setValue(effect->effect_size);
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        add_effect_row(obsgs_tr("OBSTitles.ColorLabel"), color);
        add_effect_row(obsgs_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(obsgs_tr("OBSTitles.SizeRadiusLabel"), size);
        add_effect_row(obsgs_tr("OBSTitles.BlurTypeLabel"), blur_type);
        add_effect_row(obsgs_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; emit_effect_changed(); }});
        connect(size, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::Blur) {
        LayerEffect *effect = selected_effect();
        auto *amount = spin(0.0, 1.0, 0.05); amount->setDecimals(2); amount->setValue(effect->effect_opacity);
        auto *size = spin(0.0, 512.0, 1.0); size->setValue(effect->effect_size);
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        add_effect_row(obsgs_tr("OBSTitles.AmountLabel"), amount);
        add_effect_row(obsgs_tr("OBSTitles.SizeRadiusLabel"), size);
        add_effect_row(obsgs_tr("OBSTitles.BlurTypeLabel"), blur_type);
        connect(amount, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; emit_effect_changed(); }});
        connect(size, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::MotionBlur) {
        LayerEffect *effect = selected_effect();
        auto *amount = spin(0.0, 1.0, 0.05); amount->setDecimals(2); amount->setValue(effect->effect_opacity);
        auto *shutter = spin(0.0, 720.0, 5.0); shutter->setValue(effect->effect_size);
        auto *samples = new QSpinBox(box); samples->setRange(2, 64); samples->setValue(effect->effect_samples); samples->setFixedHeight(22);
        auto *centered = new QCheckBox(obsgs_tr("OBSTitles.MotionBlurCentered"), box); centered->setChecked(effect->effect_centered);
        add_effect_row(obsgs_tr("OBSTitles.AmountLabel"), amount);
        add_effect_row(obsgs_tr("OBSTitles.ShutterAngleLabel"), shutter);
        add_effect_row(obsgs_tr("OBSTitles.SamplesLabel"), samples);
        add_effect_row(QString(), centered);
        connect(amount, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; emit_effect_changed(); }});
        connect(shutter, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; emit_effect_changed(); }});
        connect(samples, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_samples = v; emit_effect_changed(); }});
        connect(centered, &QCheckBox::toggled, this, [this](bool v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_centered = v; emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::InnerShadow) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(effect->effect_color, [this](uint32_t argb){ if (selected_effect()) selected_effect()->effect_color = argb; });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->effect_opacity);
        auto *dist = spin(0.0, 4096.0, 1.0); dist->setValue(effect->effect_distance);
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(effect->effect_angle);
        auto *size = spin(0.0, 512.0, 1.0); size->setValue(effect->effect_size);
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        add_effect_row(obsgs_tr("OBSTitles.ColorLabel"), color);
        add_effect_row(obsgs_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(obsgs_tr("OBSTitles.DistanceLabel"), dist);
        add_effect_row(obsgs_tr("OBSTitles.AngleLabel"), angle);
        add_effect_row(obsgs_tr("OBSTitles.SizeRadiusLabel"), size);
        add_effect_row(obsgs_tr("OBSTitles.BlurTypeLabel"), blur_type);
        add_effect_row(obsgs_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; emit_effect_changed(); }});
        connect(dist, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_distance = (float)v; emit_effect_changed(); }});
        connect(angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_angle = (float)v; emit_effect_changed(); }});
        connect(size, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::LongShadow) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color = argb; set_effect_color_channels_at(*selected_effect(), lt, argb); } });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);
        auto *length = spin(0.0, 4096.0, 5.0); length->setValue(effect->distance_prop.is_animated() ? effect->distance_prop.evaluate(lt) : effect->effect_distance);
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(effect->angle_prop.is_animated() ? effect->angle_prop.evaluate(lt) : effect->effect_angle);
        auto *falloff = spin(0.0, 8.0, 0.1); falloff->setDecimals(2); falloff->setValue(effect->falloff_prop.is_animated() ? effect->falloff_prop.evaluate(lt) : effect->effect_falloff);
        auto *blur_type = combo(); blur_type->addItem(obsgs_tr("OBSTitles.NoBlur"), (int)LongShadowBlurType::None); blur_type->addItem(obsgs_tr("OBSTitles.BoxBlur"), (int)LongShadowBlurType::Box); blur_type->addItem(obsgs_tr("OBSTitles.GaussianBlur"), (int)LongShadowBlurType::Gaussian); blur_type->addItem(obsgs_tr("OBSTitles.StackFastBlur"), (int)LongShadowBlurType::StackFast); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *blur = spin(0.0, 512.0, 1.0); blur->setValue(effect->size_prop.is_animated() ? effect->size_prop.evaluate(lt) : effect->effect_size);
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        add_effect_row(obsgs_tr("OBSTitles.LongShadowColor"), color);
        add_effect_row(obsgs_tr("OBSTitles.LongShadowOpacity"), opacity);
        add_effect_row(obsgs_tr("OBSTitles.LongShadowLength"), length);
        add_effect_row(obsgs_tr("OBSTitles.LongShadowAngle"), angle);
        add_effect_row(obsgs_tr("OBSTitles.LongShadowFalloff"), falloff);
        add_effect_row(obsgs_tr("OBSTitles.LongShadowBlurType"), blur_type);
        add_effect_row(obsgs_tr("OBSTitles.LongShadowBlur"), blur);
        add_effect_row(obsgs_tr("OBSTitles.BlendingModeLabel"), blend);
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
