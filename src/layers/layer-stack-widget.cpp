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
    columns->setFixedHeight(72);
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
    add_header("◉", 20);
    add_header("🔒", 20);
    add_header("", 12);
    add_header("#", 24);
    QLabel *name = new QLabel(obsgs_tr("OBSTitles.LayerNameHeader"), columns);
    name->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:bold;")
                            .arg(disabled_text.name(QColor::HexRgb)));
    ch->addWidget(name, 1);
    add_header(obsgs_tr("OBSTitles.ModeHeader"), 46, Qt::AlignLeft | Qt::AlignVCenter);
    add_header(obsgs_tr("OBSTitles.ParentHeader"), 58, Qt::AlignLeft | Qt::AlignVCenter);
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

    btn_add_ = make_layer_tool(obsgs_tr("OBSTitles.AddLayer"),
                               obs_icon("add.svg"),
                               obsgs_tr("OBSTitles.AddLayerTooltip"));
    auto *add_menu = new QMenu(btn_add_);
    add_menu->addAction(obs_icon("text.svg"),
                        obsgs_tr("OBSTitles.Text"), this, &LayerStack::on_add_text);
    add_menu->addAction(obs_icon("clock.svg"),
                        obsgs_tr("OBSTitles.Clock"), this, &LayerStack::on_add_clock);
    add_menu->addAction(obs_icon("text.svg"),
                        obsgs_tr("OBSTitles.Ticker"), this, &LayerStack::on_add_ticker);
    add_menu->addAction(obs_icon("shape.svg"),
                        obsgs_tr("OBSTitles.Shape"), this, &LayerStack::on_add_rect);
    add_menu->addAction(obs_icon("image.svg"),
                        obsgs_tr("OBSTitles.Image"), this, &LayerStack::on_add_image);
    btn_add_->setMenu(add_menu);
    btn_add_->setPopupMode(QToolButton::InstantPopup);
    btn_add_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}"));

    btn_move_up_ = make_layer_tool(obsgs_tr("OBSTitles.MoveLayerUp"),
                                   obs_icon("move-up.svg"),
                                   obsgs_tr("OBSTitles.MoveLayerUpTooltip"));
    btn_move_down_ = make_layer_tool(obsgs_tr("OBSTitles.MoveLayerDown"),
                                     obs_icon("move-down.svg"),
                                     obsgs_tr("OBSTitles.MoveLayerDownTooltip"));
    btn_del_ = make_layer_tool(obsgs_tr("OBSTitles.DeleteLayer"),
                               obs_icon("delete.svg"),
                               obsgs_tr("OBSTitles.DeleteLayerTooltip"));
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

        QToolButton *vis = make_toggle("layer-visible.svg", "layer-hidden.svg", l->visible, obsgs_tr("OBSTitles.LayerVisibilityTooltip"));
        QToolButton *lock = make_toggle("layer-lock.svg", "layer-unlock.svg", l->locked, obsgs_tr("OBSTitles.LockLayerTooltip"));
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
        expand->setToolTip(obsgs_tr("OBSTitles.ShowKeyframedPropertiesTooltip"));
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

        QLineEdit *name = new QLineEdit(QString::fromStdString(l->name), row_widget);
        name->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        name->setFrame(false);
        name->setReadOnly(l->locked);
        name->setToolTip(obsgs_tr("OBSTitles.RenameLayerTooltip"));
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

        QLabel *mode = new QLabel(obsgs_tr("OBSTitles.Normal"), row_widget);
        mode->setFixedWidth(54);
        mode->setStyleSheet(label_chip_style);
        hl->addWidget(mode);

        QComboBox *parent = new QComboBox(row_widget);
        parent->setFixedWidth(86);
        parent->setStyleSheet(combo_style);
        parent->addItem(obsgs_tr("OBSTitles.None"), "");
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
        mask->setFixedWidth(112);
        mask->setStyleSheet(combo_style);
        mask->addItem("No Mask", QVariant(QStringLiteral("|0")));
        for (const auto &candidate : title_->layers) {
            if (candidate->id == l->id) continue;
            mask->addItem(QString::fromStdString(candidate->name + " α"),
                          QString::fromStdString(candidate->id + "|" + std::to_string((int)MaskMode::Alpha)));
            mask->addItem(QString::fromStdString(candidate->name + " -α"),
                          QString::fromStdString(candidate->id + "|" + std::to_string((int)MaskMode::InvertedAlpha)));
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
        for (auto *prop : timeline_properties(*l)) {
            if (!prop->is_animated()) continue;
            QString label = property_label(prop->name);
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
            QLabel *diamond_indicator = new QLabel("◇", prop_widget);
            diamond_indicator->setFixedWidth(18);
            diamond_indicator->setAlignment(Qt::AlignCenter);
            diamond_indicator->setStyleSheet(QString("color:%1;").arg(layer_color(*l, row).name()));
            ph->addWidget(diamond_indicator);
            QLabel *prop_name = new QLabel(label, prop_widget);
            prop_name->setStyleSheet(QStringLiteral("color:%1;").arg(text.name(QColor::HexRgb)));
            ph->addWidget(prop_name, 1);
            QLabel *value = new QLabel(property_value_text(*prop, *l), prop_widget);
            value->setFixedWidth(95);
            value->setStyleSheet(QStringLiteral("color:%1;font-family:monospace;")
                                     .arg(highlight.name(QColor::HexRgb)));
            ph->addWidget(value);
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
    if (!has_layer)
        emit layer_selected(std::string());

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
    QAction *clone = menu.addAction(obsgs_tr("OBSTitles.CloneLayer"));
    QAction *copy = menu.addAction(obsgs_tr("OBSTitles.CopyLayer"));
    QAction *paste = menu.addAction(obsgs_tr("OBSTitles.PasteLayer"));
    paste->setEnabled(layer_clipboard_available_);
    menu.addSeparator();
    QAction *del = menu.addAction(obsgs_tr("OBSTitles.DeleteLayer"));

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

