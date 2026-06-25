#include "title-editor-internal.h"

LayerStack::LayerStack(QWidget *parent) : QWidget(parent)
{
    const QPalette pal = qApp->palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor disabled_text = pal.color(QPalette::Disabled, QPalette::WindowText);
    const QColor border = pal.color(QPalette::Mid);
    const QColor button = pal.color(QPalette::Button);
    const QColor button_text = pal.color(QPalette::ButtonText);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor hover = button.lightness() < 128 ? button.lighter(125) : button.darker(108);
    setStyleSheet(QStringLiteral("background:%1;color:%2;")
                      .arg(window.name(QColor::HexRgb),
                           text.name(QColor::HexRgb)));
    auto *vl = new QVBoxLayout(this);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);


    QWidget *columns = new QWidget(this);
    columns->setFixedHeight(44);
    columns->setStyleSheet(QStringLiteral("background:%1;border-top:1px solid %2;border-bottom:1px solid %2;")
                                .arg(window.lightness() < 128 ? window.darker(112).name(QColor::HexRgb)
                                                              : window.darker(104).name(QColor::HexRgb),
                                     border.name(QColor::HexRgb)));
    auto *ch = new QHBoxLayout(columns);
    ch->setContentsMargins(4, 0, 4, 0);
    ch->setSpacing(4);
    auto add_header = [&](const QString &txt, int w, Qt::Alignment align = Qt::AlignCenter) {
        QLabel *label = new QLabel(txt, columns);
        label->setFixedWidth(w);
        label->setAlignment(align);
        label->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:bold;")
                                 .arg(disabled_text.name(QColor::HexRgb)));
        ch->addWidget(label);
    };
    auto add_header_icon = [&](const char *icon_name, int w, const QString &tip = QString()) {
        QToolButton *icon = new QToolButton(columns);
        icon->setFixedSize(w, 24);
        icon->setIcon(obs_icon(icon_name));
        icon->setIconSize(QSize(14, 14));
        icon->setAutoRaise(true);
        icon->setEnabled(false);
        icon->setToolTip(tip);
        icon->setStyleSheet(QStringLiteral("QToolButton{background:transparent;border:none;color:%1;}")
                                .arg(disabled_text.name(QColor::HexRgb)));
        ch->addWidget(icon);
    };
    add_header("◉", 20);
    add_header_icon("layer-lock.svg", 20, bgl_tr("OBSTitles.LockLayerTooltip"));
    add_header("", 12);
    add_header("#", 24);
    add_header("T", 18);
    add_header("", 24); // FX indicator column
    add_header("Matte", 44, Qt::AlignLeft | Qt::AlignVCenter);
    QLabel *name = new QLabel(bgl_tr("OBSTitles.LayerNameHeader"), columns);
    name->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:bold;")
                            .arg(disabled_text.name(QColor::HexRgb)));
    ch->addWidget(name, 1);
    add_header(bgl_tr("OBSTitles.ModeHeader"), 110, Qt::AlignLeft | Qt::AlignVCenter);
    add_header(bgl_tr("OBSTitles.ParentHeader"), 174, Qt::AlignLeft | Qt::AlignVCenter);
    add_header(bgl_tr("OBSTitles.MaskHeader"), 178, Qt::AlignLeft | Qt::AlignVCenter);
    vl->addWidget(columns);

    list_ = new QListWidget(this);
    list_->setDragDropMode(QAbstractItemView::InternalMove);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setAlternatingRowColors(false);
    list_->setUniformItemSizes(false);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list_->setStyleSheet(QStringLiteral(
        "QListWidget{background:%1;border:none;color:%2;}"
        "QListWidget::item{border-bottom:1px solid %3;}"
        "QListWidget::item:selected{background:%4;color:%5;}"
        "QListWidget::item:hover{background:%6;}")
        .arg(window.name(QColor::HexRgb),
             text.name(QColor::HexRgb),
             border.name(QColor::HexRgb),
             highlight.name(QColor::HexRgb),
             pal.color(QPalette::HighlightedText).name(QColor::HexRgb),
             hover.name(QColor::HexRgb)));
    vl->addWidget(list_, 1);

    auto *toolbar = new QToolBar(this);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setOrientation(Qt::Horizontal);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(16, 16));
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    toolbar->setStyleSheet(QStringLiteral(
        "QToolBar{background:%1;border-top:1px solid %2;spacing:2px;}"
        "QToolButton{color:%3;background:transparent;border:none;padding:3px;}"
        "QToolButton:hover{background:%4;border-radius:2px;}"
        "QToolButton:disabled{color:%5;}")
        .arg(window.name(QColor::HexRgb),
             border.name(QColor::HexRgb),
             button_text.name(QColor::HexRgb),
             hover.name(QColor::HexRgb),
             disabled_text.name(QColor::HexRgb)));

    auto make_layer_tool = [&](const QString &text, const QIcon &icon, const QString &tip) {
        auto *button = new QToolButton(toolbar);
        button->setText(text);
        button->setAccessibleName(text);
        button->setToolTip(tip);
        button->setIcon(icon);
        button->setIconSize(QSize(16, 16));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::StrongFocus);
        return button;
    };

    btn_add_ = make_layer_tool(bgl_tr("OBSTitles.AddLayer"),
                               obs_icon("add.svg"),
                               bgl_tr("OBSTitles.AddLayerTooltip"));
    auto *add_menu = new QMenu(btn_add_);
    add_menu->addAction(obs_icon("text.svg"),
                        bgl_tr("OBSTitles.Text"), this, &LayerStack::on_add_text);
    add_menu->addAction(obs_icon("clock.svg"),
                        bgl_tr("OBSTitles.Clock"), this, &LayerStack::on_add_clock);
    add_menu->addAction(obs_icon("text.svg"),
                        bgl_tr("OBSTitles.Ticker"), this, &LayerStack::on_add_ticker);
    add_menu->addAction(obs_icon("shape.svg"),
                        bgl_tr("OBSTitles.Shape"), this, &LayerStack::on_add_rect);
    add_menu->addAction(obs_icon("image.svg"),
                        bgl_tr("OBSTitles.Image"), this, &LayerStack::on_add_image);
    btn_add_->setMenu(add_menu);
    btn_add_->setPopupMode(QToolButton::InstantPopup);
    btn_add_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}"));

    btn_move_up_ = make_layer_tool(bgl_tr("OBSTitles.MoveLayerUp"),
                                   obs_icon("move-up.svg"),
                                   bgl_tr("OBSTitles.MoveLayerUpTooltip"));
    btn_move_down_ = make_layer_tool(bgl_tr("OBSTitles.MoveLayerDown"),
                                     obs_icon("move-down.svg"),
                                     bgl_tr("OBSTitles.MoveLayerDownTooltip"));
    btn_del_ = make_layer_tool(bgl_tr("OBSTitles.DeleteLayer"),
                               obs_icon("delete.svg"),
                               bgl_tr("OBSTitles.DeleteLayerTooltip"));
    btn_move_up_->setEnabled(false);
    btn_move_down_->setEnabled(false);
    btn_del_->setEnabled(false);

    toolbar->addWidget(btn_add_);
    toolbar->addWidget(btn_move_up_);
    toolbar->addWidget(btn_move_down_);
    toolbar->addSeparator();
    toolbar->addWidget(btn_del_);
    vl->addWidget(toolbar);

    connect(btn_move_up_, &QToolButton::clicked, this, &LayerStack::on_move_up);
    connect(btn_move_down_, &QToolButton::clicked, this, &LayerStack::on_move_down);
    connect(btn_del_, &QToolButton::clicked, this, &LayerStack::on_delete);
    connect(list_, &QListWidget::itemSelectionChanged,
            this, &LayerStack::on_selection_changed);
    connect(list_->model(), &QAbstractItemModel::rowsMoved,
            this, [this]() { sync_order_from_list(); });
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListWidget::customContextMenuRequested,
            this, &LayerStack::show_layer_context_menu);
    list_->viewport()->installEventFilter(this);
}

bool LayerStack::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == list_->viewport() && event->type() == QEvent::MouseButtonPress) {
        auto *mouse_event = static_cast<QMouseEvent *>(event);
        if (mouse_event->button() == Qt::LeftButton && !list_->itemAt(mouse_event->pos())) {
            QSignalBlocker blocker(list_);
            list_->clearSelection();
            list_->setCurrentItem(nullptr);
            emit layer_selected(std::string());
            emit layers_selected({});
            event->accept();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void LayerStack::set_title(std::shared_ptr<Title> t)
{
    title_ = t; populate();
}

void LayerStack::refresh() { populate(); }

void LayerStack::set_layer_clipboard_available(bool available)
{
    layer_clipboard_available_ = available;
}

QScrollBar *LayerStack::vertical_scroll_bar() const
{
    return list_ ? list_->verticalScrollBar() : nullptr;
}

void LayerStack::sync_order_from_list()
{
    if (!title_) return;

    std::vector<std::shared_ptr<Layer>> reordered;
    reordered.reserve(title_->layers.size());
    for (int i = list_->count() - 1; i >= 0; --i) {
        auto *item = list_->item(i);
        if (item->data(Qt::UserRole + 1).toString() == "property")
            continue;
        std::string id = item->data(Qt::UserRole).toString().toStdString();
        if (auto layer = title_->find_layer(id))
            reordered.push_back(layer);
    }
    if (reordered.size() == title_->layers.size()) {
        title_->layers = std::move(reordered);
        emit layer_order_changed();
    }
}

void LayerStack::populate()
{
    QString prev_id = list_->currentItem()
        ? list_->currentItem()->data(Qt::UserRole).toString()
        : QString();

    list_->blockSignals(true);
    list_->clear();
    if (!title_) { list_->blockSignals(false); return; }

    const QPalette pal = palette();
    const QColor text = pal.color(QPalette::WindowText);
    const QColor field_text = pal.color(QPalette::Text);
    const QColor button = pal.color(QPalette::Button);
    const QColor button_text = pal.color(QPalette::ButtonText);
    const QColor disabled_text = pal.color(QPalette::Disabled, QPalette::WindowText);
    const QColor base = pal.color(QPalette::Base);
    const QColor border = pal.color(QPalette::Mid);
    const QColor dark = pal.color(QPalette::Dark);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor hover = button.lightness() < 128 ? button.lighter(125) : button.darker(108);
    const QString button_style = QStringLiteral(
        "QToolButton{color:%1;background:transparent;border:none;}"
        "QToolButton:hover{background:%2;border-radius:2px;}"
        "QToolButton:checked{color:%3;}")
        .arg(button_text.name(QColor::HexRgb),
             hover.name(QColor::HexRgb),
             text.name(QColor::HexRgb));
    const QString combo_style = QStringLiteral(
        "QComboBox{color:%1;background:%2;border:none;border-radius:3px;padding-left:4px;}"
        "QComboBox::drop-down{border:none;}"
        "QComboBox QAbstractItemView{background:%2;color:%1;selection-background-color:%3;selection-color:%4;}")
        .arg(field_text.name(QColor::HexRgb),
             base.name(QColor::HexRgb),
             highlight.name(QColor::HexRgb),
             pal.color(QPalette::HighlightedText).name(QColor::HexRgb));
    const QString label_chip_style = QStringLiteral("color:%1;background:%2;border-radius:3px;padding-left:4px;")
                                         .arg(field_text.name(QColor::HexRgb),
                                              base.name(QColor::HexRgb));

    std::set<std::string> track_matte_source_ids;
    for (const auto &candidate : title_->layers) {
        if (candidate && candidate->mask_mode != MaskMode::None && !candidate->mask_source_id.empty())
            track_matte_source_ids.insert(candidate->mask_source_id);
    }

    int row = 0;
    for (auto it = title_->layers.rbegin(); it != title_->layers.rend(); ++it, ++row) {
        auto &l = *it;
        auto *item = new QListWidgetItem();
        item->setData(Qt::UserRole, QString::fromStdString(l->id));
        item->setData(Qt::UserRole + 1, "layer");
        item->setFlags((item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled |
                        Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled) & ~Qt::ItemIsUserCheckable);
        item->setSizeHint(QSize(0, 28));
        list_->addItem(item);

        QWidget *row_widget = new QWidget(list_);
        row_widget->setStyleSheet(QStringLiteral("background:transparent;color:%1;")
                                      .arg(text.name(QColor::HexRgb)));
        auto *hl = new QHBoxLayout(row_widget);
        hl->setContentsMargins(4, 0, 4, 0);
        hl->setSpacing(4);
        const bool is_mask_object = track_matte_source_ids.find(l->id) != track_matte_source_ids.end();

        auto make_toggle = [&](const char *on_icon, const char *off_icon, bool checked,
                               const QString &tip) {
            auto *btn = new QToolButton(row_widget);
            btn->setCheckable(true);
            btn->setChecked(checked);
            btn->setIcon(obs_icon(checked ? on_icon : off_icon));
            btn->setToolTip(tip);
            btn->setFixedSize(20, 20);
            btn->setIconSize(QSize(14, 14));
            btn->setAutoRaise(true);
            btn->setStyleSheet(button_style);
            connect(btn, &QToolButton::toggled, btn, [btn, on_icon, off_icon](bool state) {
                btn->setIcon(obs_icon(state ? on_icon : off_icon));
            });
            hl->addWidget(btn);
            return btn;
        };

        QToolButton *vis = make_toggle(is_mask_object ? "layer-mask-object.svg" : "layer-visible.svg",
                                       is_mask_object ? "layer-mask-object.svg" : "layer-hidden.svg",
                                       l->visible,
                                       is_mask_object
                                           ? bgl_tr("OBSTitles.LayerMaskObjectTooltip")
                                           : bgl_tr("OBSTitles.LayerVisibilityTooltip"));
        QToolButton *lock = make_toggle("layer-lock.svg", "layer-unlock.svg", l->locked, bgl_tr("OBSTitles.LockLayerTooltip"));
        connect(vis, &QToolButton::toggled, this, [this, id = l->id, item](bool checked) {
            list_->setCurrentItem(item);
            emit layer_visibility_changed(id, checked);
        });
        connect(lock, &QToolButton::toggled, this, [this, id = l->id, item](bool checked) {
            list_->setCurrentItem(item);
            emit layer_lock_changed(id, checked);
        });

        QToolButton *expand = new QToolButton(row_widget);
        expand->setCheckable(true);
        expand->setChecked(l->properties_expanded);
        expand->setIcon(obs_icon(l->properties_expanded ? "keyframes-expand.svg" : "keyframes-collapse.svg"));
        expand->setToolTip(bgl_tr("OBSTitles.ShowKeyframedPropertiesTooltip"));
        expand->setFixedSize(16, 20);
        expand->setIconSize(QSize(12, 12));
        expand->setAutoRaise(true);
        expand->setStyleSheet(button_style);
        connect(expand, &QToolButton::toggled, this, [this, expand, id = l->id](bool checked) {
            expand->setIcon(obs_icon(checked ? "keyframes-expand.svg" : "keyframes-collapse.svg"));
            emit layer_expand_changed(id, checked);
        });
        hl->addWidget(expand);

        QLabel *idx = new QLabel(QString::number(row + 1), row_widget);
        idx->setFixedWidth(24);
        idx->setAlignment(Qt::AlignCenter);
        idx->setStyleSheet(QStringLiteral("color:%1;font-weight:bold;")
                               .arg(disabled_text.name(QColor::HexRgb)));
        hl->addWidget(idx);

        QLabel *type = new QLabel(layer_type_short(l->type), row_widget);
        type->setFixedWidth(18);
        type->setAlignment(Qt::AlignCenter);
        type->setStyleSheet(QStringLiteral("background:%1;border:1px solid %2;color:%3;font-weight:bold;")
                                .arg(layer_color(*l, row).name(QColor::HexRgb),
                                     dark.name(QColor::HexRgb),
                                     pal.color(QPalette::HighlightedText).name(QColor::HexRgb)));
        hl->addWidget(type);

        const bool has_enabled_effect_stack = std::any_of(l->effects.begin(), l->effects.end(),
            [](const LayerEffect &effect) { return effect.enabled; });
        QLabel *fx_indicator = new QLabel(has_enabled_effect_stack ? bgl_tr("OBSTitles.FX") : QString(), row_widget);
        fx_indicator->setFixedSize(24, 18);
        fx_indicator->setAlignment(Qt::AlignCenter);
        fx_indicator->setToolTip(has_enabled_effect_stack
            ? bgl_tr("OBSTitles.LayerEffectStackTooltip")
            : QString());
        fx_indicator->setStyleSheet(has_enabled_effect_stack
            ? QStringLiteral("QLabel{background:%1;border:1px solid %2;border-radius:2px;color:%3;font-size:9px;font-weight:bold;}")
                  .arg(highlight.name(QColor::HexRgb),
                       border.name(QColor::HexRgb),
                       pal.color(QPalette::HighlightedText).name(QColor::HexRgb))
            : QStringLiteral("QLabel{background:transparent;border:none;}"));
        hl->addWidget(fx_indicator);

        auto add_matte_indicator = [&](const char *icon, const QString &tip, bool active) {
            QToolButton *indicator = new QToolButton(row_widget);
            indicator->setFixedSize(20, 20);
            indicator->setIconSize(QSize(14, 14));
            indicator->setAutoRaise(true);
            indicator->setEnabled(false);
            indicator->setStyleSheet(button_style);
            if (active) {
                indicator->setIcon(obs_icon(icon));
                indicator->setToolTip(tip);
            }
            hl->addWidget(indicator);
        };
        const bool used_as_track_matte = is_mask_object;
        const bool uses_track_matte = l->mask_mode != MaskMode::None && !l->mask_source_id.empty();
        add_matte_indicator("timeline-mask.svg",
                            bgl_tr("OBSTitles.TrackMatteTooltip"),
                            used_as_track_matte);
        add_matte_indicator((l->mask_mode == MaskMode::InvertedAlpha || l->mask_mode == MaskMode::InvertedLuma) ? "timeline-mask-inverted.svg" : "timeline-mask.svg",
                            bgl_tr("OBSTitles.MaskedLayerTooltip"),
                            uses_track_matte);

        QLineEdit *name = new QLineEdit(QString::fromStdString(l->name), row_widget);
        name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        name->setFrame(false);
        name->setReadOnly(l->locked);
        name->setToolTip(bgl_tr("OBSTitles.RenameLayerTooltip"));
        name->setStyleSheet(l->locked
            ? QStringLiteral("QLineEdit{color:%1;background:transparent;border:none;}")
                  .arg(disabled_text.name(QColor::HexRgb))
            : QStringLiteral("QLineEdit{color:%1;background:transparent;border:none;padding:1px;} "
                             "QLineEdit:focus{background:%2;border:1px solid %3;border-radius:2px;}")
                  .arg(text.name(QColor::HexRgb),
                       base.name(QColor::HexRgb),
                       highlight.name(QColor::HexRgb)));
        connect(name, &QLineEdit::editingFinished, this, [this, id = l->id, name]() {
            emit layer_name_changed(id, name->text().trimmed().toStdString());
        });
        hl->addWidget(name, 1);

        QComboBox *mode = new QComboBox(row_widget);
        mode->setFixedWidth(110);
        mode->setStyleSheet(combo_style);
        mode->setToolTip(bgl_tr("OBSTitles.LayerModesTooltip"));
        mode->addItem(obs_icon("timeline-modes.svg"), bgl_tr("OBSTitles.BlendModeNormal"), (int)EffectBlendMode::Normal);
        mode->addItem(obs_icon("timeline-modes.svg"), bgl_tr("OBSTitles.BlendModeMultiply"), (int)EffectBlendMode::Multiply);
        mode->addItem(obs_icon("timeline-modes.svg"), bgl_tr("OBSTitles.BlendModeAdditive"), (int)EffectBlendMode::Additive);
        mode->addItem(obs_icon("timeline-modes.svg"), bgl_tr("OBSTitles.BlendModeScreen"), (int)EffectBlendMode::Screen);
        mode->addItem(obs_icon("timeline-modes.svg"), bgl_tr("OBSTitles.BlendModeOverlay"), (int)EffectBlendMode::Overlay);
        mode->addItem(obs_icon("timeline-modes.svg"), bgl_tr("OBSTitles.BlendModeColor"), (int)EffectBlendMode::Color);
        int mode_idx = mode->findData((int)l->blend_mode);
        mode->setCurrentIndex(mode_idx >= 0 ? mode_idx : 0);
        connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, id = l->id, mode](int index) {
                    emit layer_blend_mode_changed(id, (EffectBlendMode)mode->itemData(index).toInt());
                });
        hl->addWidget(mode);

        QToolButton *pick_whip = new QToolButton(row_widget);
        pick_whip->setIcon(obs_icon("timeline-pick-whip.svg"));
        pick_whip->setToolTip(bgl_tr("OBSTitles.ParentPickWhipTooltip"));
        pick_whip->setFixedSize(20, 20);
        pick_whip->setIconSize(QSize(14, 14));
        pick_whip->setAutoRaise(true);
        pick_whip->setStyleSheet(button_style);
        hl->addWidget(pick_whip);

        QComboBox *parent = new QComboBox(row_widget);
        parent->setFixedWidth(150);
        parent->setStyleSheet(combo_style);
        parent->setToolTip(bgl_tr("OBSTitles.ParentLayerTooltip"));
        parent->addItem(bgl_tr("OBSTitles.None"), "");
        for (const auto &candidate : title_->layers) {
            if (candidate->id == l->id) continue;
            parent->addItem(QString::fromStdString(candidate->name), QString::fromStdString(candidate->id));
        }
        int parent_idx = parent->findData(QString::fromStdString(l->parent_id));
        parent->setCurrentIndex(parent_idx >= 0 ? parent_idx : 0);
        connect(parent, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, id = l->id, parent](int index) {
                    emit layer_parent_changed(id, parent->itemData(index).toString().toStdString());
                });
        hl->addWidget(parent);

        QComboBox *mask = new QComboBox(row_widget);
        mask->setFixedWidth(178);
        mask->setStyleSheet(combo_style);
        mask->setToolTip(bgl_tr("OBSTitles.TrackMatteTooltip"));
        mask->addItem(obs_icon("timeline-mask.svg"), bgl_tr("OBSTitles.NoMask"), QVariant(QStringLiteral("|0")));
        for (const auto &candidate : title_->layers) {
            if (candidate->id == l->id) continue;
            mask->addItem(obs_icon("timeline-mask.svg"), QString::fromStdString(candidate->name + " α"),
                          QString::fromStdString(candidate->id + "|" + std::to_string((int)MaskMode::Alpha)));
            mask->addItem(obs_icon("timeline-mask-inverted.svg"), QString::fromStdString(candidate->name + " -α"),
                          QString::fromStdString(candidate->id + "|" + std::to_string((int)MaskMode::InvertedAlpha)));
            mask->addItem(obs_icon("timeline-mask.svg"), QString::fromStdString(candidate->name + " Luma"),
                          QString::fromStdString(candidate->id + "|" + std::to_string((int)MaskMode::Luma)));
            mask->addItem(obs_icon("timeline-mask-inverted.svg"), QString::fromStdString(candidate->name + " -Luma"),
                          QString::fromStdString(candidate->id + "|" + std::to_string((int)MaskMode::InvertedLuma)));
        }
        QString mask_value = QString::fromStdString(l->mask_source_id + "|" + std::to_string((int)l->mask_mode));
        int mask_idx = mask->findData(mask_value);
        mask->setCurrentIndex(mask_idx >= 0 ? mask_idx : 0);
        connect(mask, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, id = l->id, mask](int index) {
                    const QStringList parts = mask->itemData(index).toString().split('|');
                    const std::string source_id = parts.value(0).toStdString();
                    const MaskMode mode = (MaskMode)parts.value(1).toInt();
                    emit layer_mask_changed(id, source_id, mode);
                });
        hl->addWidget(mask);

        list_->setItemWidget(item, row_widget);
        if ((prev_id.isEmpty() && list_->currentItem() == nullptr) ||
            prev_id == item->data(Qt::UserRole).toString())
            list_->setCurrentItem(item);

        if (!l->properties_expanded) continue;

        std::set<std::string> seen;
        for (auto prop : timeline_properties(*l)) {
            if (!prop.is_animated()) continue;
            QString label = property_label(prop.name());
            std::string key = label.toStdString();
            if (!seen.insert(key).second) continue;

            auto *prop_item = new QListWidgetItem();
            prop_item->setData(Qt::UserRole, QString::fromStdString(l->id));
            prop_item->setData(Qt::UserRole + 1, "property");
            prop_item->setData(Qt::UserRole + 2, label);
            prop_item->setFlags((prop_item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled) &
                                ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsUserCheckable));
            prop_item->setSizeHint(QSize(0, 28));
            list_->addItem(prop_item);

            QWidget *prop_widget = new QWidget(list_);
            auto *ph = new QHBoxLayout(prop_widget);
            ph->setContentsMargins(64, 0, 4, 0);
            ph->setSpacing(4);
            QToolButton *diamond_indicator = new QToolButton(prop_widget);
            diamond_indicator->setText(prop.keyframe_count() ? QStringLiteral("◆") : QStringLiteral("◇"));
            diamond_indicator->setFixedSize(18, 20);
            diamond_indicator->setAutoRaise(true);
            diamond_indicator->setCursor(Qt::PointingHandCursor);
            diamond_indicator->setToolTip(bgl_tr("OBSTitles.ToggleKeyframe"));
            diamond_indicator->setStyleSheet(QString("QToolButton{border:none;background:transparent;color:%1;}")
                                                  .arg(layer_color(*l, row).name()));
            connect(diamond_indicator, &QToolButton::clicked, this,
                    [this, id = l->id, name = prop.name()]() { emit property_keyframe_toggled(id, name); });
            ph->addWidget(diamond_indicator);
            QLabel *prop_name = new QLabel(label, prop_widget);
            prop_name->setStyleSheet(QStringLiteral("color:%1;").arg(text.name(QColor::HexRgb)));
            ph->addWidget(prop_name, 1);

            auto configure_spin = [&](QDoubleSpinBox *spin) {
                spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
                spin->setKeyboardTracking(false);
                spin->setDecimals(2);
                spin->setRange(-100000.0, 100000.0);
                spin->setFixedWidth(72);
                spin->setStyleSheet(QStringLiteral("QDoubleSpinBox{color:%1;background:%2;border:none;font-family:monospace;padding:1px 3px;}")
                                        .arg(highlight.name(QColor::HexRgb), base.name(QColor::HexRgb)));
            };
            const bool vector_prop = prop.vector != nullptr;
            QDoubleSpinBox *value_x = new QDoubleSpinBox(prop_widget);
            configure_spin(value_x);
            QDoubleSpinBox *value_y = nullptr;
            double x = 0.0, y = 0.0;
            if (vector_prop) {
                x = prop.vector->static_value.x;
                y = prop.vector->static_value.y;
                if (prop.name() == "scale") { x *= 100.0; y *= 100.0; }
                value_y = new QDoubleSpinBox(prop_widget);
                configure_spin(value_y);
                value_x->setFixedWidth(58);
                value_y->setFixedWidth(58);
                value_x->setValue(x);
                value_y->setValue(y);
                ph->addWidget(value_x);
                ph->addWidget(value_y);
            } else {
                x = prop.scalar ? prop.scalar->static_value : 0.0;
                if (prop.name() == "opacity" || prop.name() == "char_scale_x" || prop.name() == "char_scale_y") x *= 100.0;
                value_x->setValue(x);
                ph->addWidget(value_x);
            }
            auto emit_value = [this, id = l->id, name = prop.name(), value_x, value_y]() {
                emit property_value_changed(id, name, value_x->value(), value_y ? value_y->value() : 0.0);
            };
            connect(value_x, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [emit_value](double) { emit_value(); });
            if (value_y)
                connect(value_y, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [emit_value](double) { emit_value(); });
            list_->setItemWidget(prop_item, prop_widget);
        }
    }
    list_->blockSignals(false);
    on_selection_changed();
}

void LayerStack::set_selected_layer(const std::string &layer_id)
{
    set_selected_layers(layer_id.empty() ? std::vector<std::string>()
                                         : std::vector<std::string>{layer_id});
}

void LayerStack::set_selected_layers(const std::vector<std::string> &layer_ids)
{
    QSignalBlocker blocker(list_);
    list_->clearSelection();
    if (layer_ids.empty()) {
        list_->setCurrentItem(nullptr);
        return;
    }

    std::set<QString> ids;
    for (const auto &id : layer_ids)
        ids.insert(QString::fromStdString(id));

    QListWidgetItem *current = nullptr;
    QString primary = QString::fromStdString(layer_ids.back());
    for (int i = 0; i < list_->count(); ++i) {
        auto *item = list_->item(i);
        if (item->data(Qt::UserRole + 1).toString() != "layer") continue;
        QString id = item->data(Qt::UserRole).toString();
        if (ids.find(id) != ids.end()) {
            item->setSelected(true);
            if (id == primary) current = item;
        }
    }
    if (current) list_->setCurrentItem(current, QItemSelectionModel::NoUpdate);
}

std::string LayerStack::selected_id() const
{
    auto *item = list_->currentItem();
    if (item && item->isSelected())
        return item->data(Qt::UserRole).toString().toStdString();
    auto selected = list_->selectedItems();
    return selected.isEmpty() ? std::string() : selected.back()->data(Qt::UserRole).toString().toStdString();
}

std::vector<std::string> LayerStack::selected_ids() const
{
    std::vector<std::string> ids;
    for (auto *item : list_->selectedItems()) {
        if (item->data(Qt::UserRole + 1).toString() != "layer") continue;
        ids.push_back(item->data(Qt::UserRole).toString().toStdString());
    }
    return ids;
}

void LayerStack::on_selection_changed()
{
    std::string id = selected_id();
    const bool has_layer = !id.empty() && title_ && title_->find_layer(id);
    if (btn_del_) btn_del_->setEnabled(has_layer);

    bool can_move_up = false;
    bool can_move_down = false;
    if (has_layer) {
        auto selected = selected_ids();
        if (selected.size() > 1)
            emit layers_selected(selected);
        auto it = std::find_if(title_->layers.begin(), title_->layers.end(),
                               [&](const auto &layer) { return layer && layer->id == id; });
        if (it != title_->layers.end()) {
            int idx = (int)std::distance(title_->layers.begin(), it);
            can_move_down = idx > 0;
            can_move_up = idx < (int)title_->layers.size() - 1;
        }
        if (selected.size() <= 1)
            emit layer_selected(id);
    }
    if (!has_layer) {
        emit layer_selected(std::string());
        emit layers_selected({});
    }

    if (btn_move_up_) btn_move_up_->setEnabled(can_move_up);
    if (btn_move_down_) btn_move_down_->setEnabled(can_move_down);
}

void LayerStack::on_add_text() { emit add_layer_requested(LayerType::Text); }
void LayerStack::on_add_clock() { emit add_layer_requested(LayerType::Clock); }
void LayerStack::on_add_ticker() { emit add_layer_requested(LayerType::Ticker); }
void LayerStack::on_add_rect() { emit add_layer_requested(LayerType::Shape); }
void LayerStack::on_add_image() { emit add_layer_requested(LayerType::Image); }

void LayerStack::on_move_up()
{
    std::string id = selected_id();
    if (!title_ || id.empty()) return;
    auto layer = title_->find_layer(id);
    if (!layer) return;
    title_->move_layer(id, +1);
    emit layer_order_changed();
    set_selected_layer(id);
}

void LayerStack::on_move_down()
{
    std::string id = selected_id();
    if (!title_ || id.empty()) return;
    auto layer = title_->find_layer(id);
    if (!layer) return;
    title_->move_layer(id, -1);
    emit layer_order_changed();
    set_selected_layer(id);
}

void LayerStack::on_delete()
{
    std::string id = selected_id();
    if (!id.empty()) emit delete_layer_requested(id);
}

void LayerStack::show_layer_context_menu(const QPoint &pos)
{
    if (!title_) return;

    QListWidgetItem *item = list_->itemAt(pos);
    auto style_menu = [this](QMenu *menu) {
        if (!menu) return;
        const QPalette pal = palette();
        menu->setStyleSheet(QStringLiteral(
            "QMenu{color:%1;background:%2;border:1px solid %3;}"
            "QMenu::item{padding:5px 22px;}"
            "QMenu::item:selected{background:%4;color:%5;}"
            "QMenu::item:disabled{color:%6;}")
            .arg(pal.color(QPalette::Text).name(QColor::HexRgb),
                 pal.color(QPalette::Base).name(QColor::HexRgb),
                 pal.color(QPalette::Mid).name(QColor::HexRgb),
                 pal.color(QPalette::Highlight).name(QColor::HexRgb),
                 pal.color(QPalette::HighlightedText).name(QColor::HexRgb),
                 pal.color(QPalette::Disabled, QPalette::Text).name(QColor::HexRgb)));
    };
    if (!item) {
        if (btn_add_ && btn_add_->menu()) {
            style_menu(btn_add_->menu());
            btn_add_->menu()->exec(list_->viewport()->mapToGlobal(pos));
        }
        return;
    }

    std::string id = item ? item->data(Qt::UserRole).toString().toStdString() : selected_id();
    if (id.empty()) return;

    if (item && item->data(Qt::UserRole + 1).toString() == "layer" && !item->isSelected())
        list_->setCurrentItem(item);

    QMenu menu(this);
    style_menu(&menu);
    QAction *clone = menu.addAction(bgl_tr("OBSTitles.CloneLayer"));
    QAction *copy = menu.addAction(bgl_tr("OBSTitles.CopyLayer"));
    QAction *paste = menu.addAction(bgl_tr("OBSTitles.PasteLayer"));
    paste->setEnabled(layer_clipboard_available_);
    menu.addSeparator();
    QAction *del = menu.addAction(bgl_tr("OBSTitles.DeleteLayer"));

    QAction *chosen = menu.exec(list_->viewport()->mapToGlobal(pos));
    if (chosen == clone) emit clone_layer_requested(id);
    else if (chosen == copy) emit copy_layer_requested(id);
    else if (chosen == paste) emit paste_layer_requested(id);
    else if (chosen == del) emit delete_layer_requested(id);
}

void LayerStack::on_item_changed(QListWidgetItem *item)
{
    std::string id = item->data(Qt::UserRole).toString().toStdString();
    bool v = (item->checkState() == Qt::Checked);
    emit layer_visibility_changed(id, v);
}

/* ══════════════════════════════════════════════════════════════════
 *  TimelineWidget
 * ══════════════════════════════════════════════════════════════════ */
