#include "title-editor-internal.h"


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
    button->setStyleSheet(QStringLiteral("QPushButton{color:#fff;background:%1;border:1px solid #555;border-radius:3px;padding:3px 8px;}")
                          .arg(c.name()));
}

EffectsPanel::EffectsPanel(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *hint = new QLabel(QStringLiteral("Effect stack"), this);
    hint->setStyleSheet(QStringLiteral("color:#b8b8b8;font-weight:bold;"));
    layout->addWidget(hint);

    effect_list_ = new QListWidget(this);
    effect_list_->setObjectName(QStringLiteral("OBSGraphicsStudioProEffectsList"));
    effect_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    effect_list_->setAlternatingRowColors(true);
    effect_list_->setStyleSheet(QStringLiteral("QListWidget{background:#1a1a1a;border:1px solid #303030;color:#ddd;}QListWidget::item{padding:4px;}QListWidget::item:selected{background:#3b4f64;}"));
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
    button_layout->addStretch(1);
    layout->addWidget(button_bar);

    auto *settings_scroll = new QScrollArea(this);
    settings_scroll->setWidgetResizable(true);
    settings_scroll->setStyleSheet(QStringLiteral("QScrollArea{background:#151515;border:none;}"));
    settings_container_ = new QWidget(settings_scroll);
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
        emit_effect_changed();
    });

    connect(btn_add, &QToolButton::clicked, this, [this, btn_add]() {
        if (!layer_) return;
        QMenu menu(btn_add);
        menu.setStyleSheet(QStringLiteral("QMenu{color:#ddd;background:#252525;border:1px solid #3a3a3a;}QMenu::item{padding:5px 22px;}QMenu::item:selected{background:#3b4f64;}"));
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
        case LayerEffectType::DropShadow:
        case LayerEffectType::InnerShadow:
            effect.blend_mode = EffectBlendMode::Multiply;
            effect.effect_color = 0x99000000;
            break;
        case LayerEffectType::LongShadow:
            effect.blend_mode = EffectBlendMode::Multiply;
            effect.effect_color = 0x99000000;
            effect.effect_distance = 120.0f;
            effect.effect_angle = 135.0f;
            effect.effect_size = 0.0f;
            effect.effect_opacity = 0.45f;
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
        layer_->effects.push_back(effect);
        if (effect.type == LayerEffectType::BackgroundColor) { layer_->background_enabled = true; layer_->background_enabled_prop.static_value = 1.0; }
        if (effect.type == LayerEffectType::Outline) {
            layer_->outline_enabled = true;
            if (layer_->stroke_fill_type == 0) layer_->stroke_fill_type = 1;
            if (layer_->stroke_width <= 0.0f) layer_->stroke_width = 4.0f;
        }
        if (effect.type == LayerEffectType::DropShadow) { layer_->shadow_enabled = true; layer_->shadow_enabled_prop.static_value = 1.0; }
        if (effect.type == LayerEffectType::LongShadow) { layer_->long_shadow_enabled = true; if (layer_->long_shadow_length <= 0.0f) layer_->long_shadow_length = 120.0f; if (layer_->long_shadow_opacity <= 0.0f) layer_->long_shadow_opacity = 0.45f; }
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

    rebuild_stack();
}

void EffectsPanel::set_layer(std::shared_ptr<Layer> layer, double playhead)
{
    layer_ = layer;
    playhead_ = playhead;
    selected_index_ = layer_ && !layer_->effects.empty() ? std::clamp(selected_index_, 0, (int)layer_->effects.size() - 1) : -1;
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
    if (!layer_) return;
    layer_->background_enabled = layer_effect_enabled(*layer_, LayerEffectType::BackgroundColor, false);
    layer_->outline_enabled = layer_effect_enabled(*layer_, LayerEffectType::Outline, false);
    layer_->shadow_enabled = layer_effect_enabled(*layer_, LayerEffectType::DropShadow, false);
    layer_->long_shadow_enabled = layer_effect_enabled(*layer_, LayerEffectType::LongShadow, false);
    layer_->background_enabled_prop.static_value = layer_->background_enabled ? 1.0 : 0.0;
    layer_->shadow_enabled_prop.static_value = layer_->shadow_enabled ? 1.0 : 0.0;
}

void EffectsPanel::emit_effect_changed()
{
    sync_legacy_enabled_flags();
    emit property_changed(!numeric_label_dragging_);
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
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
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
        label->setStyleSheet(QStringLiteral("color:#999;"));
        settings_layout_->addWidget(label);
        settings_layout_->addStretch(1);
        return;
    }

    loading_values_ = true;
    const double lt = std::clamp(playhead_ - layer_->in_time, 0.0, std::max(0.0, layer_->out_time - layer_->in_time));
    auto *box = new QGroupBox(effect_type_name(selected_effect()->type), settings_container_);
    box->setStyleSheet(QStringLiteral("QGroupBox{color:#d0d0d0;background:#1b1b1b;border:1px solid #303030;margin-top:8px;padding-top:14px;}QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 3px;}QDoubleSpinBox,QComboBox{color:#ddd;background:#252525;border:1px solid #363636;border-radius:2px;padding:1px 3px;}QPushButton{color:#ddd;background:#2a2a2a;border:1px solid #3f3f3f;border-radius:3px;padding:3px 8px;}"));
    auto *form = new QFormLayout(box);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);
    auto spin = [box](double min, double max, double step) { auto *s = new QDoubleSpinBox(box); s->setRange(min, max); s->setSingleStep(step); s->setFixedHeight(22); return s; };
    auto combo = [box]() { auto *c = new QComboBox(box); c->setFixedHeight(22); return c; };
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
                                               emit property_changed(true);
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
        auto *fill = combo(); fill->addItem(obsgs_tr("OBSTitles.Solid"), 0); fill->addItem(obsgs_tr("OBSTitles.Gradient"), 1); fill->setCurrentIndex(fill->findData(layer_->background_fill_type));
        auto *color = color_button(eval_background_color(*layer_, lt), [this, lt](uint32_t argb){ layer_->background_color = argb; set_background_color_channels_at(*layer_, lt, argb); });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(eval_background_opacity(*layer_, lt));
        auto *pad_x = spin(0.0, 1000.0, 1.0); pad_x->setValue(eval_background_padding_x(*layer_, lt));
        auto *pad_y = spin(0.0, 1000.0, 1.0); pad_y->setValue(eval_background_padding_y(*layer_, lt));
        auto *corner = spin(0.0, 1000.0, 1.0); corner->setValue(eval_background_corner_radius(*layer_, lt));
        auto *grad_type = combo(); grad_type->addItem(obsgs_tr("OBSTitles.LinearGradient"), 0); grad_type->addItem(obsgs_tr("OBSTitles.RadialGradient"), 1); grad_type->setCurrentIndex(grad_type->findData(layer_->background_gradient_type));
        auto *grad_start = color_button(layer_->background_gradient_start_color, [this](uint32_t argb){ layer_->background_gradient_start_color = argb; });
        auto *grad_end = color_button(layer_->background_gradient_end_color, [this](uint32_t argb){ layer_->background_gradient_end_color = argb; });
        auto *grad_start_pos = spin(0.0, 1.0, 0.01); grad_start_pos->setDecimals(2); grad_start_pos->setValue(layer_->background_gradient_start_pos);
        auto *grad_end_pos = spin(0.0, 1.0, 0.01); grad_end_pos->setDecimals(2); grad_end_pos->setValue(layer_->background_gradient_end_pos);
        auto *grad_start_op = spin(0.0, 1.0, 0.01); grad_start_op->setDecimals(2); grad_start_op->setValue(layer_->background_gradient_start_opacity);
        auto *grad_end_op = spin(0.0, 1.0, 0.01); grad_end_op->setDecimals(2); grad_end_op->setValue(layer_->background_gradient_end_opacity);
        auto *grad_op = spin(0.0, 1.0, 0.01); grad_op->setDecimals(2); grad_op->setValue(layer_->background_gradient_opacity);
        auto *grad_angle = spin(-360.0, 360.0, 1.0); grad_angle->setValue(layer_->background_gradient_angle);
        auto *grad_cx = spin(0.0, 1.0, 0.01); grad_cx->setDecimals(2); grad_cx->setValue(layer_->background_gradient_center_x);
        auto *grad_cy = spin(0.0, 1.0, 0.01); grad_cy->setDecimals(2); grad_cy->setValue(layer_->background_gradient_center_y);
        auto *grad_scale = spin(0.01, 10.0, 0.05); grad_scale->setDecimals(2); grad_scale->setValue(layer_->background_gradient_scale);
        auto *grad_fx = spin(0.0, 1.0, 0.01); grad_fx->setDecimals(2); grad_fx->setValue(layer_->background_gradient_focal_x);
        auto *grad_fy = spin(0.0, 1.0, 0.01); grad_fy->setDecimals(2); grad_fy->setValue(layer_->background_gradient_focal_y);
        add_effect_row(obsgs_tr("OBSTitles.BackgroundFillTypeLabel"), fill);
        add_effect_row(obsgs_tr("OBSTitles.BackgroundColorLabel"), color);
        add_effect_row(obsgs_tr("OBSTitles.BackgroundOpacityLabel"), opacity);
        add_effect_row(obsgs_tr("OBSTitles.BackgroundHorizontalPaddingLabel"), pad_x);
        add_effect_row(obsgs_tr("OBSTitles.BackgroundVerticalPaddingLabel"), pad_y);
        add_effect_row(obsgs_tr("OBSTitles.BackgroundCornerLabel"), corner);
        add_effect_row(obsgs_tr("OBSTitles.GradientTypeLabel"), grad_type);
        add_effect_row(obsgs_tr("OBSTitles.StartColorLabel"), grad_start);
        add_effect_row(obsgs_tr("OBSTitles.EndColorLabel"), grad_end);
        add_effect_row(obsgs_tr("OBSTitles.StartStopLabel"), grad_start_pos);
        add_effect_row(obsgs_tr("OBSTitles.EndStopLabel"), grad_end_pos);
        add_effect_row(obsgs_tr("OBSTitles.StartOpacityLabel"), grad_start_op);
        add_effect_row(obsgs_tr("OBSTitles.EndOpacityLabel"), grad_end_op);
        add_effect_row(obsgs_tr("OBSTitles.OpacityLabel"), grad_op);
        add_effect_row(obsgs_tr("OBSTitles.AngleLabel"), grad_angle);
        add_effect_row(obsgs_tr("OBSTitles.CenterXLabel"), grad_cx);
        add_effect_row(obsgs_tr("OBSTitles.CenterYLabel"), grad_cy);
        add_effect_row(obsgs_tr("OBSTitles.ScaleLabel"), grad_scale);
        add_effect_row(obsgs_tr("OBSTitles.FocalXLabel"), grad_fx);
        add_effect_row(obsgs_tr("OBSTitles.FocalYLabel"), grad_fy);
        connect(fill, QOverload<int>::of(&QComboBox::activated), this, [this, fill](int){ layer_->background_fill_type = fill->currentData().toInt(); emit_effect_changed(); });
        connect(grad_type, QOverload<int>::of(&QComboBox::activated), this, [this, grad_type](int){ layer_->background_gradient_type = grad_type->currentData().toInt(); emit_effect_changed(); });
        connect(grad_start_pos, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->background_gradient_start_pos = v; emit_effect_changed(); }});
        connect(grad_end_pos, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->background_gradient_end_pos = v; emit_effect_changed(); }});
        connect(grad_start_op, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->background_gradient_start_opacity = v; emit_effect_changed(); }});
        connect(grad_end_op, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->background_gradient_end_opacity = v; emit_effect_changed(); }});
        connect(grad_op, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->background_gradient_opacity = v; emit_effect_changed(); }});
        connect(grad_angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->background_gradient_angle = v; emit_effect_changed(); }});
        connect(grad_cx, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->background_gradient_center_x = v; emit_effect_changed(); }});
        connect(grad_cy, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->background_gradient_center_y = v; emit_effect_changed(); }});
        connect(grad_scale, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->background_gradient_scale = v; emit_effect_changed(); }});
        connect(grad_fx, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->background_gradient_focal_x = v; emit_effect_changed(); }});
        connect(grad_fy, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->background_gradient_focal_y = v; emit_effect_changed(); }});
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_) { layer_->background_opacity = v; set_animated_value(layer_->background_opacity_prop, lt, v); emit_effect_changed(); }});
        connect(pad_x, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_) { layer_->background_padding_x = v; set_animated_value(layer_->background_padding_x_prop, lt, v); emit_effect_changed(); }});
        connect(pad_y, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_) { layer_->background_padding_y = v; set_animated_value(layer_->background_padding_y_prop, lt, v); emit_effect_changed(); }});
        connect(corner, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_) { layer_->background_corner_radius = v; set_animated_value(layer_->background_corner_radius_prop, lt, v); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::Outline) {
        auto *color = color_button(layer_->stroke_color, [this](uint32_t argb){ layer_->stroke_color = argb; });
        auto *width = spin(0.0, 200.0, 1.0); width->setValue(layer_->stroke_width);
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(layer_->outline_opacity);
        auto *join = combo(); join->addItem(obsgs_tr("OBSTitles.Miter"), 0); join->addItem(obsgs_tr("OBSTitles.Round"), 1); join->addItem(obsgs_tr("OBSTitles.Bevel"), 2); join->setCurrentIndex(join->findData(layer_->outline_join_style));
        auto *position = combo(); position->addItem(obsgs_tr("OBSTitles.Back"), 0); position->addItem(obsgs_tr("OBSTitles.Front"), 1); position->setCurrentIndex(layer_->outline_on_front ? 1 : 0);
        auto *aa = new QCheckBox(obsgs_tr("OBSTitles.AntialiasOutline"), box); aa->setChecked(layer_->outline_antialias);
        add_effect_row(obsgs_tr("OBSTitles.ColorLabel"), color);
        add_effect_row(obsgs_tr("OBSTitles.ThicknessLabel"), width);
        add_effect_row(obsgs_tr("OBSTitles.OpacityLabel"), opacity);
        add_effect_row(obsgs_tr("OBSTitles.JoinLabel"), join);
        add_effect_row(obsgs_tr("OBSTitles.PositionLabelIndented"), position);
        add_effect_row(QString(), aa);
        connect(width, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->stroke_width = v; emit_effect_changed(); }});
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_) { layer_->outline_opacity = v; emit_effect_changed(); }});
        connect(join, QOverload<int>::of(&QComboBox::activated), this, [this, join](int){ layer_->outline_join_style = join->currentData().toInt(); emit_effect_changed(); });
        connect(position, QOverload<int>::of(&QComboBox::activated), this, [this, position](int){ layer_->outline_on_front = position->currentData().toInt() == 1; emit_effect_changed(); });
        connect(aa, &QCheckBox::toggled, this, [this](bool v){ if (!loading_values_) { layer_->outline_antialias = v; emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::DropShadow) {
        LayerEffect *effect = selected_effect();
        auto *preset = combo(); preset->addItems({obsgs_tr("OBSTitles.Custom"), obsgs_tr("OBSTitles.Soft"), obsgs_tr("OBSTitles.Medium"), obsgs_tr("OBSTitles.Strong"), obsgs_tr("OBSTitles.Broadcast")});
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *color = color_button(effect->effect_color, [this](uint32_t argb){ if (selected_effect()) selected_effect()->effect_color = argb; });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->effect_opacity);
        auto *dist = spin(0.0, 4096.0, 1.0); dist->setValue(effect->effect_distance);
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(effect->effect_angle);
        auto *blur = spin(0.0, 512.0, 1.0); blur->setValue(effect->effect_size);
        auto *spread = spin(0.0, 512.0, 1.0); spread->setValue(effect->effect_spread);
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
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; emit_effect_changed(); }});
        connect(dist, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_distance = (float)v; emit_effect_changed(); }});
        connect(angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_angle = (float)v; emit_effect_changed(); }});
        connect(blur, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; emit_effect_changed(); }});
        connect(spread, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_spread = (float)v; emit_effect_changed(); }});
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
        auto *color = color_button(effect->effect_color, [this](uint32_t argb){ if (selected_effect()) selected_effect()->effect_color = argb; });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->effect_opacity);
        auto *length = spin(0.0, 4096.0, 5.0); length->setValue(effect->effect_distance);
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(effect->effect_angle);
        auto *falloff = spin(0.0, 8.0, 0.1); falloff->setDecimals(2); falloff->setValue(effect->effect_falloff);
        auto *blur_type = combo(); blur_type->addItem(obsgs_tr("OBSTitles.NoBlur"), (int)LongShadowBlurType::None); blur_type->addItem(obsgs_tr("OBSTitles.BoxBlur"), (int)LongShadowBlurType::Box); blur_type->addItem(obsgs_tr("OBSTitles.GaussianBlur"), (int)LongShadowBlurType::Gaussian); blur_type->addItem(obsgs_tr("OBSTitles.StackFastBlur"), (int)LongShadowBlurType::StackFast); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *blur = spin(0.0, 512.0, 1.0); blur->setValue(effect->effect_size);
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        add_effect_row(obsgs_tr("OBSTitles.LongShadowColor"), color);
        add_effect_row(obsgs_tr("OBSTitles.LongShadowOpacity"), opacity);
        add_effect_row(obsgs_tr("OBSTitles.LongShadowLength"), length);
        add_effect_row(obsgs_tr("OBSTitles.LongShadowAngle"), angle);
        add_effect_row(obsgs_tr("OBSTitles.LongShadowFalloff"), falloff);
        add_effect_row(obsgs_tr("OBSTitles.LongShadowBlurType"), blur_type);
        add_effect_row(obsgs_tr("OBSTitles.LongShadowBlur"), blur);
        add_effect_row(obsgs_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; emit_effect_changed(); }});
        connect(length, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_distance = (float)v; emit_effect_changed(); }});
        connect(angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_angle = (float)v; emit_effect_changed(); }});
        connect(falloff, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_falloff = (float)v; emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blur, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    }
    settings_layout_->addWidget(box);
    settings_layout_->addStretch(1);
    loading_values_ = false;
}


