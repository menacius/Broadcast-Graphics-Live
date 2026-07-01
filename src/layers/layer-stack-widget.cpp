#include "title-editor-internal.h"
#include "bgl-modern-controls.h"

#include <functional>
#include <map>
#include <unordered_map>


namespace {

using LayerPtr = std::shared_ptr<Layer>;

constexpr int kLayerListMargin = 4;
constexpr int kLayerListSpacing = 4;
constexpr int kLayerVisibilityWidth = 20;
constexpr int kLayerLockWidth = 20;
constexpr int kLayerExpandWidth = 18;
constexpr int kLayerIndexWidth = 24;
constexpr int kLayerTypeWidth = 18;
constexpr int kLayerFxWidth = 24;
constexpr int kLayerMatteIndicatorWidth = 20;
constexpr int kLayerNameMinimumWidth = 180;
constexpr int kLayerModeWidth = 110;
constexpr int kLayerParentWidth = 150;
constexpr int kLayerMaskWidth = 130;
constexpr int kLayerMatteControlWidth = 20;

/* Fixed columns + minimum usable layer-name area + layout gaps.  Include
 * extra room for the vertical scrollbar so the splitter can never compress
 * the layer list until controls paint over one another. */
constexpr int kLayerStackMinimumWidth = 836;

static TimelinePropertyRef layer_timeline_property(Layer &layer,
                                                   const std::string &property_name)
{
    for (TimelinePropertyRef prop : timeline_properties(layer))
        if (prop.name() == property_name) return prop;
    return {};
}

static int keyframe_index_at_time(const TimelinePropertyRef &prop, double local_time)
{
    if (!prop) return -1;
    const double tolerance = std::max(1e-6, obs_frame_duration() * 0.5 + 1e-6);
    for (int index = 0; index < static_cast<int>(prop.keyframe_count()); ++index)
        if (std::abs(prop.keyframe_time(static_cast<size_t>(index)) - local_time) <= tolerance)
            return index;
    return -1;
}

static bool valid_group_parent(const std::shared_ptr<Title> &title,
                               const std::string &parent_id)
{
    if (!title || parent_id.empty()) return false;
    const auto parent = title->find_layer(parent_id);
    return parent && layer_type_is_container(parent->type);
}

static std::string hierarchy_scope_id(const std::shared_ptr<Title> &title,
                                      const Layer &layer)
{
    return valid_group_parent(title, layer.parent_id) ? layer.parent_id
                                                       : std::string();
}

static bool has_group_ancestor(const std::shared_ptr<Title> &title,
                               const Layer &layer,
                               const std::string &candidate_group_id)
{
    if (!title || candidate_group_id.empty()) return false;
    std::set<std::string> visited;
    std::string parent_id = layer.parent_id;
    while (!parent_id.empty() && visited.insert(parent_id).second) {
        if (parent_id == candidate_group_id) return true;
        const auto parent = title->find_layer(parent_id);
        if (!parent || !layer_type_is_container(parent->type)) break;
        parent_id = parent->parent_id;
    }
    return false;
}

static void reorder_visible_siblings(std::vector<LayerPtr> &siblings,
                                     const std::vector<std::string> &visual_top_to_bottom)
{
    if (siblings.empty() || visual_top_to_bottom.empty()) return;
    std::map<std::string, LayerPtr> by_id;
    for (const auto &layer : siblings)
        if (layer) by_id[layer->id] = layer;

    std::vector<LayerPtr> desired;
    desired.reserve(visual_top_to_bottom.size());
    for (auto it = visual_top_to_bottom.rbegin();
         it != visual_top_to_bottom.rend(); ++it) {
        const auto found = by_id.find(*it);
        if (found != by_id.end()) desired.push_back(found->second);
    }
    if (desired.size() != siblings.size())
        return; // Collapsed/legacy scopes are intentionally left untouched.
    siblings = std::move(desired);
}

static std::vector<LayerPtr> canonical_group_model_order(
    const std::shared_ptr<Title> &title,
    const std::map<std::string, std::vector<std::string>> &visual_orders)
{
    std::vector<LayerPtr> result;
    if (!title) return result;

    std::map<std::string, std::vector<LayerPtr>> children;
    std::vector<LayerPtr> roots;
    for (const auto &layer : title->layers) {
        if (!layer) continue;
        const std::string scope = hierarchy_scope_id(title, *layer);
        if (scope.empty()) roots.push_back(layer);
        else children[scope].push_back(layer);
    }

    auto root_order = visual_orders.find(std::string());
    if (root_order != visual_orders.end())
        reorder_visible_siblings(roots, root_order->second);
    for (auto &[parent_id, siblings] : children) {
        const auto found = visual_orders.find(parent_id);
        if (found != visual_orders.end())
            reorder_visible_siblings(siblings, found->second);
    }

    std::set<std::string> emitted;
    std::function<void(const LayerPtr &)> append_subtree;
    append_subtree = [&](const LayerPtr &layer) {
        if (!layer || !emitted.insert(layer->id).second) return;
        const auto found = children.find(layer->id);
        if (found != children.end()) {
            for (const auto &child : found->second)
                append_subtree(child);
        }
        result.push_back(layer);
    };
    for (const auto &root : roots)
        append_subtree(root);
    for (const auto &layer : title->layers)
        append_subtree(layer);
    return result;
}

static bool move_layer_within_hierarchy(const std::shared_ptr<Title> &title,
                                        const std::string &layer_id,
                                        int visual_direction)
{
    if (!title || layer_id.empty() || visual_direction == 0) return false;
    const auto layer = title->find_layer(layer_id);
    if (!layer) return false;

    std::map<std::string, std::vector<std::string>> visual_orders;
    for (const auto &row : visible_layer_hierarchy_rows(title)) {
        if (!row.layer) continue;
        visual_orders[hierarchy_scope_id(title, *row.layer)].push_back(row.layer->id);
    }
    auto &siblings = visual_orders[hierarchy_scope_id(title, *layer)];
    const auto found = std::find(siblings.begin(), siblings.end(), layer_id);
    if (found == siblings.end()) return false;
    const int index = static_cast<int>(std::distance(siblings.begin(), found));
    const int destination = index + (visual_direction > 0 ? -1 : 1);
    if (destination < 0 || destination >= static_cast<int>(siblings.size()))
        return false;
    std::swap(siblings[static_cast<size_t>(index)],
              siblings[static_cast<size_t>(destination)]);

    auto reordered = canonical_group_model_order(title, visual_orders);
    if (reordered.size() != title->layers.size() || reordered == title->layers)
        return false;
    title->layers = std::move(reordered);
    return true;
}

} // namespace

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
    setMinimumWidth(kLayerStackMinimumWidth);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
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
    ch->setContentsMargins(kLayerListMargin, 0, kLayerListMargin, 0);
    ch->setSpacing(kLayerListSpacing);
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
    add_header_icon("visibility-normal.svg", kLayerVisibilityWidth,
                    bgl_tr("OBSTitles.LayerVisibilityTooltip"));
    add_header_icon("layer-lock.svg", kLayerLockWidth, bgl_tr("OBSTitles.LockLayerTooltip"));
    add_header("", kLayerExpandWidth);
    add_header("#", kLayerIndexWidth);
    add_header("T", kLayerTypeWidth);
    add_header("", kLayerFxWidth); // FX indicator column
    /* A single role column mirrors AE's track-matte switch area.  The
     * destination glyph is the column header; rows show source or destination
     * according to the role of that layer. */
    add_header_icon("matte-destination.svg", kLayerMatteIndicatorWidth,
                    bgl_tr("OBSTitles.MatteRoleColumnTooltip"));
    QLabel *name = new QLabel(bgl_tr("OBSTitles.LayerNameHeader"), columns);
    name->setMinimumWidth(kLayerNameMinimumWidth);
    name->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-weight:bold;")
                            .arg(disabled_text.name(QColor::HexRgb)));
    ch->addWidget(name, 1);
    add_header(bgl_tr("OBSTitles.ModeHeader"), kLayerModeWidth, Qt::AlignLeft | Qt::AlignVCenter);
    add_header(bgl_tr("OBSTitles.ParentHeader"), kLayerParentWidth, Qt::AlignLeft | Qt::AlignVCenter);
    add_header(bgl_tr("OBSTitles.MaskHeader"), kLayerMaskWidth, Qt::AlignLeft | Qt::AlignVCenter);
    add_header_icon("matte-alpha.svg", kLayerMatteControlWidth, bgl_tr("OBSTitles.MatteAlphaLumaHeaderTooltip"));
    add_header_icon("matte-normal.svg", kLayerMatteControlWidth, bgl_tr("OBSTitles.MatteNormalInvertedHeaderTooltip"));
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
    add_menu->addSeparator();
    add_menu->addAction(obs_icon("lightning.svg"),
                        bgl_tr("OBSTitles.AdjustmentLayer"), this, &LayerStack::on_add_adjustment);
    add_menu->addAction(obs_icon("shape.svg"),
                        bgl_tr("OBSTitles.ColorSolid"), this, &LayerStack::on_add_color_solid);
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

void LayerStack::set_playhead(double timeline_time)
{
    playhead_ = timeline_time;
    update_property_rows();
}

void LayerStack::update_property_rows()
{
    if (!title_ || !list_) return;
    for (int row = 0; row < list_->count(); ++row) {
        QListWidgetItem *item = list_->item(row);
        if (!item || item->data(Qt::UserRole + 1).toString() != QStringLiteral("property"))
            continue;
        const std::string layer_id = item->data(Qt::UserRole).toString().toStdString();
        const std::string property_name = item->data(Qt::UserRole + 3).toString().toStdString();
        auto layer = title_->find_layer(layer_id);
        if (!layer) continue;
        TimelinePropertyRef prop = layer_timeline_property(*layer, property_name);
        if (!prop) continue;
        const double local_time = std::clamp(playhead_ - layer->in_time, 0.0,
            std::max(0.0, layer->out_time - layer->in_time));
        QWidget *row_widget = list_->itemWidget(item);
        if (!row_widget) continue;
        if (auto *diamond = row_widget->findChild<QToolButton *>(QStringLiteral("keyframeDiamond"))) {
            diamond->setText(keyframe_index_at_time(prop, local_time) >= 0
                ? QStringLiteral("◆") : QStringLiteral("◇"));
            diamond->setEnabled(!layer->locked);
        }
        auto *value_x = row_widget->findChild<QDoubleSpinBox *>(QStringLiteral("keyframeValueX"));
        auto *value_y = row_widget->findChild<QDoubleSpinBox *>(QStringLiteral("keyframeValueY"));
        if (!value_x) continue;
        double x = 0.0, y = 0.0;
        if (prop.vector) {
            const Vec2Value evaluated = prop.vector->evaluate(local_time);
            x = evaluated.x;
            y = evaluated.y;
            if (property_name == "scale") { x *= 100.0; y *= 100.0; }
        } else {
            x = prop.graph_value(local_time);
            if (property_name == "opacity" || property_name == "char_scale_x" ||
                property_name == "char_scale_y") x *= 100.0;
        }
        value_x->setEnabled(!layer->locked);
        if (!value_x->hasFocus()) {
            QSignalBlocker blocker(value_x);
            value_x->setValue(x);
        }
        if (value_y) {
            value_y->setEnabled(!layer->locked);
            if (!value_y->hasFocus()) {
                QSignalBlocker blocker(value_y);
                value_y->setValue(y);
            }
        }
    }
}

void LayerStack::sync_order_from_list()
{
    if (!title_) return;

    std::map<std::string, std::vector<std::string>> visual_orders;
    for (int i = 0; i < list_->count(); ++i) {
        auto *item = list_->item(i);
        if (!item || item->data(Qt::UserRole + 1).toString() != "layer")
            continue;
        const std::string id = item->data(Qt::UserRole).toString().toStdString();
        const auto layer = title_->find_layer(id);
        if (!layer) continue;
        visual_orders[hierarchy_scope_id(title_, *layer)].push_back(id);
    }

    auto reordered = canonical_group_model_order(title_, visual_orders);
    if (reordered.size() == title_->layers.size() && reordered != title_->layers) {
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

    const auto display_layers = visible_layer_hierarchy_rows(title_);

    for (int row = 0; row < static_cast<int>(display_layers.size()); ++row) {
        auto l = display_layers[static_cast<size_t>(row)].layer;
        const int hierarchy_depth = display_layers[static_cast<size_t>(row)].depth;
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
        hl->setContentsMargins(kLayerListMargin, 0, kLayerListMargin, 0);
        hl->setSpacing(kLayerListSpacing);
        const bool is_mask_object = track_matte_source_ids.find(l->id) != track_matte_source_ids.end();

        auto make_toggle = [&](const char *on_icon, const char *off_icon, bool checked,
                               const QString &tip) {
            auto *btn = new QToolButton(row_widget);
            btn->setCheckable(true);
            btn->setChecked(checked);
            btn->setIcon(obs_icon(checked ? on_icon : off_icon));
            btn->setToolTip(tip);
            btn->setFixedSize(kLayerVisibilityWidth, 20);
            btn->setIconSize(QSize(14, 14));
            btn->setAutoRaise(true);
            btn->setStyleSheet(button_style);
            connect(btn, &QToolButton::toggled, btn, [btn, on_icon, off_icon](bool state) {
                btn->setIcon(obs_icon(state ? on_icon : off_icon));
            });
            hl->addWidget(btn);
            return btn;
        };

        QToolButton *vis = nullptr;
        if (is_mask_object) {
            vis = new QToolButton(row_widget);
            vis->setCheckable(false);
            vis->setFixedSize(kLayerVisibilityWidth, 20);
            vis->setIconSize(QSize(14, 14));
            vis->setAutoRaise(true);
            vis->setStyleSheet(button_style);
            auto update_matte_visibility_button = [vis](MatteVisibilityMode mode) {
                switch (mode) {
                case MatteVisibilityMode::HiddenInactive:
                    vis->setIcon(obs_icon("no-visibility.svg"));
                    vis->setToolTip(bgl_tr("OBSTitles.MatteHiddenInactiveTooltip"));
                    break;
                case MatteVisibilityMode::VisibleAndMatte:
                    vis->setIcon(obs_icon("visibility-normal.svg"));
                    vis->setToolTip(bgl_tr("OBSTitles.MatteVisibleActiveTooltip"));
                    break;
                case MatteVisibilityMode::MatteOnly:
                default:
                    vis->setIcon(obs_icon("visibility-matte.svg"));
                    vis->setToolTip(bgl_tr("OBSTitles.MatteOnlyTooltip"));
                    break;
                }
            };
            update_matte_visibility_button(l->matte_visibility_mode);
            connect(vis, &QToolButton::clicked, this,
                    [this, id = l->id, item, vis, mode = l->matte_visibility_mode,
                     update_matte_visibility_button]() mutable {
                list_->setCurrentItem(item);
                const int next = (static_cast<int>(mode) + 1) % 3;
                mode = static_cast<MatteVisibilityMode>(next);
                update_matte_visibility_button(mode);
                emit layer_matte_visibility_changed(id, mode);
            });
            hl->addWidget(vis);
        } else {
            vis = make_toggle("visibility-normal.svg", "no-visibility.svg", l->visible,
                              bgl_tr("OBSTitles.LayerVisibilityTooltip"));
            connect(vis, &QToolButton::toggled, this, [this, id = l->id, item](bool checked) {
                list_->setCurrentItem(item);
                emit layer_visibility_changed(id, checked);
            });
        }

        QToolButton *lock = make_toggle("layer-lock.svg", "layer-unlock.svg", l->locked, bgl_tr("OBSTitles.LockLayerTooltip"));
        connect(lock, &QToolButton::toggled, this, [this, id = l->id, item](bool checked) {
            list_->setCurrentItem(item);
            emit layer_lock_changed(id, checked);
        });

        const bool is_group = l->type == LayerType::Group;
        const bool is_asset = l->type == LayerType::Asset;
        const int group_state = !is_group ? -1
            : (!l->group_collapsed ? 2 : (l->properties_expanded ? 1 : 0));
        const bool expanded = !is_group && layer_keyframe_sections_expanded(*l);
        QToolButton *expand = nullptr;
        if (is_asset) {
            expand = new QToolButton(row_widget);
            expand->setIcon(obs_icon("duplicate.svg"));
            expand->setIconSize(QSize(12, 12));
            expand->setToolTip(bgl_tr("OBSTitles.AssetLayerTooltip"));
            expand->setEnabled(false);
            expand->setFixedSize(kLayerExpandWidth, 20);
            expand->setAutoRaise(true);
            expand->setStyleSheet(button_style);
        } else {
            auto *caret = new BglCaretButton(row_widget);
            caret->setCaretState(is_group ? group_state : (expanded ? 2 : 0));
            expand = caret;
            if (is_group) {
                caret->setToolTip(group_state == 0
                    ? bgl_tr("OBSTitles.GroupExpansionClosedTooltip")
                    : group_state == 1
                        ? bgl_tr("OBSTitles.GroupExpansionKeyframesTooltip")
                        : bgl_tr("OBSTitles.GroupExpansionChildrenTooltip"));
                connect(caret, &QToolButton::clicked, this,
                        [this, caret, id = l->id]() {
                    const int next_state = (caret->caretState() + 1) % 3;
                    caret->setCaretState(next_state);
                    emit group_expansion_state_changed(id, next_state);
                });
            } else {
                caret->setToolTip(bgl_tr("OBSTitles.ShowKeyframedPropertiesTooltip"));
                connect(caret, &QToolButton::clicked, this,
                        [this, caret, id = l->id]() {
                    const bool next = caret->caretState() == 0;
                    caret->setCaretState(next ? 2 : 0);
                    emit layer_expand_changed(id, next);
                });
            }
        }
        hl->addWidget(expand);

        QLabel *idx = new QLabel(QString::number(row + 1), row_widget);
        idx->setFixedWidth(kLayerIndexWidth);
        idx->setAlignment(Qt::AlignCenter);
        idx->setStyleSheet(QStringLiteral("color:%1;font-weight:bold;")
                               .arg(disabled_text.name(QColor::HexRgb)));
        hl->addWidget(idx);

        QLabel *type = new QLabel(layer_type_short(l->type), row_widget);
        type->setFixedWidth(kLayerTypeWidth);
        type->setAlignment(Qt::AlignCenter);
        type->setStyleSheet(QStringLiteral("background:%1;border:1px solid %2;color:%3;font-weight:bold;")
                                .arg(layer_color(*l, row).name(QColor::HexRgb),
                                     dark.name(QColor::HexRgb),
                                     pal.color(QPalette::HighlightedText).name(QColor::HexRgb)));
        hl->addWidget(type);

        const bool has_effect_stack = !l->effects.empty();
        const bool has_enabled_effect_stack = std::any_of(l->effects.begin(), l->effects.end(),
            [](const LayerEffect &effect) { return effect.enabled; });
        const bool has_external_binding = std::any_of(
            l->external_bindings.begin(), l->external_bindings.end(),
            [](const ExternalPropertyBinding &binding) { return binding.enabled; });
        QToolButton *fx_indicator = new QToolButton(row_widget);
        fx_indicator->setText(has_effect_stack
            ? (has_external_binding ? QStringLiteral("FX•") : bgl_tr("OBSTitles.FX"))
            : (has_external_binding ? QStringLiteral("D") : QString()));
        fx_indicator->setCheckable(has_effect_stack);
        fx_indicator->setChecked(has_enabled_effect_stack);
        fx_indicator->setFixedSize(kLayerFxWidth, 18);
        QString layer_indicator_tip;
        if (has_effect_stack)
            layer_indicator_tip = has_enabled_effect_stack
                ? bgl_tr("OBSTitles.DisableLayerEffectsTooltip")
                : bgl_tr("OBSTitles.EnableLayerEffectsTooltip");
        if (has_external_binding) {
            if (!layer_indicator_tip.isEmpty()) layer_indicator_tip += QStringLiteral("\n");
            layer_indicator_tip += QStringLiteral("Layer has external data binding");
        }
        fx_indicator->setToolTip(layer_indicator_tip);
        fx_indicator->setCursor(has_effect_stack ? Qt::PointingHandCursor : Qt::ArrowCursor);
        fx_indicator->setStyleSheet((has_effect_stack || has_external_binding)
            ? QStringLiteral(
                  "QToolButton{background:transparent;border:1px solid %1;border-radius:2px;color:%2;font-size:9px;font-weight:bold;}"
                  "QToolButton:checked{background:%3;color:%4;}"
                  "QToolButton:hover{border-color:%3;}")
                  .arg((has_external_binding ? QColor(QStringLiteral("#f0a000")) : border).name(QColor::HexRgb),
                       disabled_text.name(QColor::HexRgb),
                       highlight.name(QColor::HexRgb),
                       pal.color(QPalette::HighlightedText).name(QColor::HexRgb))
            : QStringLiteral("QToolButton{background:transparent;border:none;}"));
        if (has_effect_stack) {
            connect(fx_indicator, &QToolButton::toggled, this,
                    [this, id = l->id, item, fx_indicator](bool enabled) {
                        list_->setCurrentItem(item);
                        fx_indicator->setToolTip(enabled
                            ? bgl_tr("OBSTitles.DisableLayerEffectsTooltip")
                            : bgl_tr("OBSTitles.EnableLayerEffectsTooltip"));
                        emit layer_effects_enabled_changed(id, enabled);
                    });
        }
        hl->addWidget(fx_indicator);

        auto add_matte_indicator = [&](const char *icon, const QString &tip, bool active) {
            QToolButton *indicator = new QToolButton(row_widget);
            indicator->setFixedSize(kLayerMatteIndicatorWidth, 20);
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
        if (uses_track_matte) {
            add_matte_indicator("matte-destination.svg",
                                used_as_track_matte
                                    ? bgl_tr("OBSTitles.MatteSourceAndDestinationTooltip")
                                    : bgl_tr("OBSTitles.MaskedLayerTooltip"),
                                true);
        } else {
            add_matte_indicator("matte-source.svg",
                                bgl_tr("OBSTitles.TrackMatteTooltip"),
                                used_as_track_matte);
        }

        // Keep every layer-list column aligned with the header. Hierarchy indentation
        // belongs only to the flexible Name column; applying it to the row margins
        // shifts every preceding/following control for child layers.
        QWidget *name_cell = new QWidget(row_widget);
        name_cell->setMinimumWidth(kLayerNameMinimumWidth);
        name_cell->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto *name_layout = new QHBoxLayout(name_cell);
        name_layout->setContentsMargins(hierarchy_depth * 18, 0, 0, 0);
        name_layout->setSpacing(0);

        QLineEdit *name = new QLineEdit(QString::fromStdString(l->name), name_cell);
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
        name_layout->addWidget(name, 1);
        hl->addWidget(name_cell, 1);

        QComboBox *mode = new QComboBox(row_widget);
        mode->setFixedWidth(kLayerModeWidth);
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
        mode->setEnabled(true);
        connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, id = l->id, mode](int index) {
                    emit layer_blend_mode_changed(id, (EffectBlendMode)mode->itemData(index).toInt());
                });
        hl->addWidget(mode);

        QComboBox *parent = new QComboBox(row_widget);
        parent->setFixedWidth(kLayerParentWidth);
        parent->setStyleSheet(combo_style);
        parent->setToolTip(bgl_tr("OBSTitles.ParentLayerTooltip"));
        parent->addItem(bgl_tr("OBSTitles.None"), "");
        for (int candidate_row = 0; candidate_row < static_cast<int>(title_->layers.size()); ++candidate_row) {
            const auto &candidate = title_->layers[static_cast<size_t>(candidate_row)];
            if (!candidate || candidate->id == l->id) continue;
            const int layer_number = static_cast<int>(title_->layers.size()) - candidate_row;
            const QString label = QStringLiteral("%1. %2")
                                      .arg(layer_number)
                                      .arg(QString::fromStdString(candidate->name));
            parent->addItem(label, QString::fromStdString(candidate->id));
        }
        int parent_idx = parent->findData(QString::fromStdString(l->transform_parent_id));
        parent->setCurrentIndex(parent_idx >= 0 ? parent_idx : 0);
        connect(parent, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, id = l->id, parent](int index) {
                    emit layer_parent_changed(id, parent->itemData(index).toString().toStdString());
                });
        hl->addWidget(parent);

        QComboBox *matte = new QComboBox(row_widget);
        matte->setFixedWidth(kLayerMaskWidth);
        matte->setStyleSheet(combo_style);
        matte->setToolTip(bgl_tr("OBSTitles.TrackMatteTooltip"));
        matte->addItem(bgl_tr("OBSTitles.NoMask"), QString());
        for (int candidate_row = 0; candidate_row < static_cast<int>(title_->layers.size()); ++candidate_row) {
            const auto &candidate = title_->layers[static_cast<size_t>(candidate_row)];
            if (!candidate || candidate->id == l->id) continue;
            /* A child cannot use one of its container groups as a matte: the
             * group result already depends on that child, which would create
             * a recursive compositing graph. Groups remain valid matte sources
             * for every layer outside their own subtree. */
            if (layer_type_is_container(candidate->type) &&
                has_group_ancestor(title_, *l, candidate->id))
                continue;
            const int layer_number = static_cast<int>(title_->layers.size()) - candidate_row;
            const QString label = QStringLiteral("%1. %2")
                                      .arg(layer_number)
                                      .arg(QString::fromStdString(candidate->name));
            matte->addItem(label, QString::fromStdString(candidate->id));
        }

        const int matte_idx = matte->findData(QString::fromStdString(l->mask_source_id));
        matte->setCurrentIndex(matte_idx >= 0 ? matte_idx : 0);
        hl->addWidget(matte);

        const bool has_matte = !l->mask_source_id.empty() && l->mask_mode != MaskMode::None;
        const bool uses_luma = l->mask_mode == MaskMode::Luma || l->mask_mode == MaskMode::InvertedLuma;
        const bool uses_clipping = l->mask_mode == MaskMode::Clipping ||
                                   l->mask_mode == MaskMode::InvertedClipping;
        const bool is_inverted = l->mask_mode == MaskMode::InvertedAlpha ||
                                 l->mask_mode == MaskMode::InvertedLuma ||
                                 l->mask_mode == MaskMode::InvertedClipping;

        QToolButton *matte_type = new QToolButton(row_widget);
        matte_type->setCheckable(false);
        matte_type->setProperty("matteType", uses_clipping ? 2 : (uses_luma ? 1 : 0));
        auto update_matte_type_button = [matte_type](bool enabled) {
            if (!enabled) {
                matte_type->setIcon(QIcon());
                matte_type->setToolTip(QString());
                return;
            }
            const int type = matte_type->property("matteType").toInt();
            if (type == 2) {
                matte_type->setIcon(obs_icon("matte-clipping.svg"));
                matte_type->setToolTip(bgl_tr("OBSTitles.MatteClipping"));
            } else if (type == 1) {
                matte_type->setIcon(obs_icon("matte-luma.svg"));
                matte_type->setToolTip(bgl_tr("OBSTitles.MatteLuma"));
            } else {
                matte_type->setIcon(obs_icon("matte-alpha.svg"));
                matte_type->setToolTip(bgl_tr("OBSTitles.MatteAlpha"));
            }
        };
        update_matte_type_button(has_matte);
        matte_type->setFixedSize(kLayerMatteControlWidth, 20);
        matte_type->setIconSize(QSize(14, 14));
        matte_type->setAutoRaise(true);
        matte_type->setStyleSheet(button_style);
        matte_type->setEnabled(has_matte);
        hl->addWidget(matte_type);

        QToolButton *matte_invert = new QToolButton(row_widget);
        matte_invert->setCheckable(true);
        matte_invert->setChecked(is_inverted);
        if (has_matte) {
            matte_invert->setIcon(obs_icon(is_inverted ? "matte-inverted.svg" : "matte-normal.svg"));
            matte_invert->setToolTip(is_inverted ? bgl_tr("OBSTitles.MatteInverted") : bgl_tr("OBSTitles.MatteNormal"));
        } else {
            matte_invert->setIcon(QIcon());
            matte_invert->setToolTip(QString());
        }
        matte_invert->setFixedSize(kLayerMatteControlWidth, 20);
        matte_invert->setIconSize(QSize(14, 14));
        matte_invert->setAutoRaise(true);
        matte_invert->setStyleSheet(button_style);
        matte_invert->setEnabled(has_matte);
        hl->addWidget(matte_invert);

        auto selected_matte_mode = [matte_type, matte_invert]() {
            const bool inverted = matte_invert->isChecked();
            switch (matte_type->property("matteType").toInt()) {
            case 2:
                return inverted ? MaskMode::InvertedClipping : MaskMode::Clipping;
            case 1:
                return inverted ? MaskMode::InvertedLuma : MaskMode::Luma;
            case 0:
            default:
                return inverted ? MaskMode::InvertedAlpha : MaskMode::Alpha;
            }
        };

        connect(matte, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this, id = l->id, matte, matte_type, matte_invert,
                 selected_matte_mode, update_matte_type_button](int index) {
                    const std::string source_id = matte->itemData(index).toString().toStdString();
                    const bool enabled = !source_id.empty();
                    matte_type->setEnabled(enabled);
                    matte_invert->setEnabled(enabled);
                    update_matte_type_button(enabled);
                    if (enabled) {
                        const bool inverted = matte_invert->isChecked();
                        matte_invert->setIcon(obs_icon(inverted ? "matte-inverted.svg" : "matte-normal.svg"));
                        matte_invert->setToolTip(inverted ? bgl_tr("OBSTitles.MatteInverted") : bgl_tr("OBSTitles.MatteNormal"));
                    } else {
                        matte_invert->setIcon(QIcon());
                        matte_invert->setToolTip(QString());
                    }
                    emit layer_mask_changed(id, source_id,
                                            enabled ? selected_matte_mode() : MaskMode::None);
                });
        connect(matte_type, &QToolButton::clicked, this,
                [this, id = l->id, matte, matte_type,
                 selected_matte_mode, update_matte_type_button]() {
                    const int next_type = (matte_type->property("matteType").toInt() + 1) % 3;
                    matte_type->setProperty("matteType", next_type);
                    update_matte_type_button(true);
                    const std::string source_id = matte->currentData().toString().toStdString();
                    if (!source_id.empty())
                        emit layer_mask_changed(id, source_id, selected_matte_mode());
                });
        connect(matte_invert, &QToolButton::toggled, this,
                [this, id = l->id, matte, matte_invert, selected_matte_mode](bool inverted) {
                    matte_invert->setIcon(obs_icon(inverted ? "matte-inverted.svg" : "matte-normal.svg"));
                    matte_invert->setToolTip(inverted ? bgl_tr("OBSTitles.MatteInverted") : bgl_tr("OBSTitles.MatteNormal"));
                    const std::string source_id = matte->currentData().toString().toStdString();
                    if (!source_id.empty()) emit layer_mask_changed(id, source_id, selected_matte_mode());
                });

        list_->setItemWidget(item, row_widget);
        if ((prev_id.isEmpty() && list_->currentItem() == nullptr) ||
            prev_id == item->data(Qt::UserRole).toString())
            list_->setCurrentItem(item);

        /* Groups expose the same animated transform/opacity/effect rows as
         * every other layer. Their hierarchy caret controls child visibility;
         * it must not suppress the Group's own keyframe properties. */
        if (!layer_keyframe_sections_expanded(*l)) continue;

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
            prop_item->setData(Qt::UserRole + 3, QString::fromStdString(prop.name()));
            prop_item->setFlags((prop_item->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled) &
                                ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsUserCheckable));
            prop_item->setSizeHint(QSize(0, 28));
            list_->addItem(prop_item);

            QWidget *prop_widget = new QWidget(list_);
            prop_widget->setObjectName(QStringLiteral("layerKeyframePropertyRow"));
            prop_widget->setStyleSheet(QStringLiteral(
                "QWidget#layerKeyframePropertyRow{background:transparent;color:%1;}")
                .arg(text.name(QColor::HexRgb)));
            auto *ph = new QHBoxLayout(prop_widget);
            ph->setContentsMargins(64, 0, 4, 0);
            ph->setSpacing(4);
            QToolButton *diamond_indicator = new QToolButton(prop_widget);
            diamond_indicator->setObjectName(QStringLiteral("keyframeDiamond"));
            diamond_indicator->setProperty("layerId", QString::fromStdString(l->id));
            diamond_indicator->setProperty("propertyName", QString::fromStdString(prop.name()));
            const double property_local_time = std::clamp(
                playhead_ - l->in_time, 0.0, std::max(0.0, l->out_time - l->in_time));
            diamond_indicator->setText(keyframe_index_at_time(prop, property_local_time) >= 0
                ? QStringLiteral("◆") : QStringLiteral("◇"));
            diamond_indicator->setFixedSize(18, 20);
            diamond_indicator->setEnabled(!l->locked);
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

            /* Extension and grouped colour channels keep their authoring
             * surface in the owning effect/appearance panel.  The layer list
             * still exposes their keyframe diamond and temporal menu, but does
             * not show a misleading single numeric field for a multi-channel
             * value. */
            if (prop.is_extension() || prop.is_scalar_group()) {
                list_->setItemWidget(prop_item, prop_widget);
                continue;
            }

            auto configure_spin = [&](QDoubleSpinBox *spin, const char *object_name) {
                spin->setObjectName(QString::fromLatin1(object_name));
                spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
                spin->setKeyboardTracking(false);
                spin->setAlignment(Qt::AlignRight);
                spin->setDecimals(2);
                spin->setRange(-100000.0, 100000.0);
                spin->setFixedWidth(74);
                spin->setMinimumHeight(22);
                spin->setStyleSheet(QStringLiteral(
                    "QDoubleSpinBox{color:%1;background:%2;border:1px solid %3;"
                    "border-radius:3px;padding:2px 5px;selection-background-color:%4;selection-color:%5;}"
                    "QDoubleSpinBox:hover{border-color:%6;}"
                    "QDoubleSpinBox:focus{border-color:%4;}"
                    "QDoubleSpinBox:disabled{color:%7;background:%8;}")
                    .arg(field_text.name(QColor::HexRgb),
                         base.name(QColor::HexRgb),
                         border.name(QColor::HexRgb),
                         highlight.name(QColor::HexRgb),
                         pal.color(QPalette::HighlightedText).name(QColor::HexRgb),
                         text.name(QColor::HexRgb),
                         disabled_text.name(QColor::HexRgb),
                         button.name(QColor::HexRgb)));
            };
            const bool vector_prop = prop.vector != nullptr;
            QDoubleSpinBox *value_x = new QDoubleSpinBox(prop_widget);
            configure_spin(value_x, "keyframeValueX");
            QDoubleSpinBox *value_y = nullptr;
            double x = 0.0, y = 0.0;
            if (vector_prop) {
                const Vec2Value evaluated = prop.vector->evaluate(property_local_time);
                x = evaluated.x;
                y = evaluated.y;
                if (prop.name() == "scale") { x *= 100.0; y *= 100.0; }
                value_y = new QDoubleSpinBox(prop_widget);
                configure_spin(value_y, "keyframeValueY");
                value_x->setFixedWidth(58);
                value_y->setFixedWidth(58);
                value_x->setValue(x);
                value_y->setValue(y);
                ph->addWidget(value_x);
                ph->addWidget(value_y);
            } else {
                x = prop.graph_value(property_local_time);
                if (prop.name() == "opacity" || prop.name() == "char_scale_x" || prop.name() == "char_scale_y") x *= 100.0;
                value_x->setValue(x);
                ph->addWidget(value_x);
            }
            value_x->setProperty("layerId", QString::fromStdString(l->id));
            value_x->setProperty("propertyName", QString::fromStdString(prop.name()));
            if (value_y) {
                value_y->setProperty("layerId", QString::fromStdString(l->id));
                value_y->setProperty("propertyName", QString::fromStdString(prop.name()));
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
    update_property_rows();
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
        if (auto layer = title_->find_layer(id)) {
            std::vector<std::shared_ptr<Layer>> siblings;
            const std::string scope = hierarchy_scope_id(title_, *layer);
            for (const auto &candidate : title_->layers) {
                if (candidate && hierarchy_scope_id(title_, *candidate) == scope)
                    siblings.push_back(candidate);
            }
            const auto it = std::find_if(siblings.begin(), siblings.end(),
                [&](const auto &candidate) { return candidate && candidate->id == id; });
            if (it != siblings.end()) {
                const int idx = static_cast<int>(std::distance(siblings.begin(), it));
                can_move_down = idx > 0;
                can_move_up = idx < static_cast<int>(siblings.size()) - 1;
            }
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
void LayerStack::on_add_adjustment() { emit add_layer_requested(LayerType::Adjustment); }
void LayerStack::on_add_color_solid() { emit add_layer_requested(LayerType::ColorSolid); }

void LayerStack::on_move_up()
{
    std::string id = selected_id();
    if (!title_ || id.empty()) return;
    if (!move_layer_within_hierarchy(title_, id, +1)) return;
    emit layer_order_changed();
    set_selected_layer(id);
}

void LayerStack::on_move_down()
{
    std::string id = selected_id();
    if (!title_ || id.empty()) return;
    if (!move_layer_within_hierarchy(title_, id, -1)) return;
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

    if (item->data(Qt::UserRole + 1).toString() == QStringLiteral("property")) {
        const std::string layer_id = item->data(Qt::UserRole).toString().toStdString();
        const std::string property_name = item->data(Qt::UserRole + 3).toString().toStdString();
        auto layer = title_->find_layer(layer_id);
        if (!layer || layer->locked) return;
        TimelinePropertyRef prop = layer_timeline_property(*layer, property_name);
        if (!prop) return;
        const double local_time = std::clamp(playhead_ - layer->in_time, 0.0,
            std::max(0.0, layer->out_time - layer->in_time));
        const int key_index = keyframe_index_at_time(prop, local_time);

        QMenu menu(this);
        style_menu(&menu);
        QAction *toggle_key = menu.addAction(key_index >= 0
            ? bgl_tr("OBSTitles.DeleteKeyframe") : bgl_tr("OBSTitles.AddKeyframe"));
        QMenu *temporal = menu.addMenu(bgl_tr("OBSTitles.TemporalInterpolation"));
        style_menu(temporal);
        temporal->setEnabled(key_index >= 0);
        std::map<QAction *, TemporalInterpolationMode> modes;
        auto temporal_label = [](TemporalInterpolationMode mode) {
            switch (mode) {
            case TemporalInterpolationMode::Linear: return bgl_tr("OBSTitles.Linear");
            case TemporalInterpolationMode::Hold: return bgl_tr("OBSTitles.Hold");
            case TemporalInterpolationMode::AutoBezier: return bgl_tr("OBSTitles.TemporalAutoBezier");
            case TemporalInterpolationMode::ContinuousBezier: return bgl_tr("OBSTitles.TemporalContinuousBezier");
            case TemporalInterpolationMode::ManualBezier: return bgl_tr("OBSTitles.TemporalManualBezier");
            }
            return bgl_tr("OBSTitles.Linear");
        };
        auto *mode_group = new QActionGroup(temporal);
        mode_group->setExclusive(true);
        for (TemporalInterpolationMode mode : {TemporalInterpolationMode::Linear,
                 TemporalInterpolationMode::Hold, TemporalInterpolationMode::AutoBezier,
                 TemporalInterpolationMode::ContinuousBezier,
                 TemporalInterpolationMode::ManualBezier}) {
            QAction *action = temporal->addAction(temporal_label(mode));
            action->setCheckable(true);
            action->setActionGroup(mode_group);
            action->setChecked(key_index >= 0 &&
                prop.keyframe_temporal_mode(static_cast<size_t>(key_index)) == mode);
            modes[action] = mode;
        }
        temporal->addSeparator();
        QAction *easy = temporal->addAction(bgl_tr("OBSTitles.EasyEase"));
        QAction *easy_in = temporal->addAction(bgl_tr("OBSTitles.EasyEaseIn"));
        QAction *easy_out = temporal->addAction(bgl_tr("OBSTitles.EasyEaseOut"));
        temporal->addSeparator();
        QAction *velocity = temporal->addAction(bgl_tr("OBSTitles.KeyframeVelocity"));

        QAction *chosen = menu.exec(list_->viewport()->mapToGlobal(pos));
        if (!chosen) return;
        if (chosen == toggle_key) {
            emit property_keyframe_toggled(layer_id, property_name);
            return;
        }
        const auto mode = modes.find(chosen);
        if (mode != modes.end()) {
            emit property_temporal_mode_changed(layer_id, property_name,
                                                static_cast<int>(mode->second));
            return;
        }
        if (chosen == easy || chosen == easy_in || chosen == easy_out) {
            emit property_easy_ease_requested(layer_id, property_name,
                chosen == easy || chosen == easy_in,
                chosen == easy || chosen == easy_out);
            return;
        }
        if (chosen == velocity)
            emit property_velocity_requested(layer_id, property_name);
        return;
    }

    std::string id = item ? item->data(Qt::UserRole).toString().toStdString() : selected_id();
    if (id.empty()) return;

    if (item && item->data(Qt::UserRole + 1).toString() == "layer" && !item->isSelected())
        list_->setCurrentItem(item);

    const std::vector<std::string> selection = selected_ids();
    auto selected_layer_is_group = [this](const std::string &layer_id) {
        auto layer = title_ ? title_->find_layer(layer_id) : nullptr;
        return layer && layer->type == LayerType::Group;
    };
    auto parent_is_group = [this](const std::string &layer_id) {
        auto layer = title_ ? title_->find_layer(layer_id) : nullptr;
        auto parent = layer && !layer->parent_id.empty() ? title_->find_layer(layer->parent_id) : nullptr;
        return parent && layer_type_is_container(parent->type);
    };
    auto would_cycle = [this](const std::string &child_id, const std::string &group_id) {
        if (child_id.empty() || group_id.empty() || child_id == group_id)
            return true;
        std::string current = group_id;
        int guard = 0;
        while (!current.empty() && guard++ < 64) {
            if (current == child_id)
                return true;
            auto layer = title_->find_layer(current);
            if (!layer)
                break;
            current = layer->parent_id;
        }
        return false;
    };

    QMenu menu(this);
    style_menu(&menu);
    QAction *group_layers = menu.addAction(bgl_tr("OBSTitles.GroupLayers"));
    group_layers->setEnabled(selection.size() >= 2);
    QAction *ungroup_layers = menu.addAction(bgl_tr("OBSTitles.UngroupLayers"));
    ungroup_layers->setEnabled(std::any_of(selection.begin(), selection.end(), selected_layer_is_group));

    QMenu *add_to_group = menu.addMenu(bgl_tr("OBSTitles.AddToGroup"));
    style_menu(add_to_group);
    std::map<QAction *, std::string> group_targets;
    bool has_available_group = false;
    for (const auto &candidate : title_->layers) {
        if (!candidate || candidate->type != LayerType::Group || candidate->locked)
            continue;
        bool valid = false;
        for (const auto &selected_id : selection) {
            const auto selected_layer = title_->find_layer(selected_id);
            if (!selected_layer || selected_layer->locked ||
                selected_id == candidate->id ||
                selected_layer->parent_id == candidate->id ||
                would_cycle(selected_id, candidate->id))
                continue;
            valid = true;
            break;
        }
        if (!valid)
            continue;
        QAction *target = add_to_group->addAction(QString::fromStdString(candidate->name));
        group_targets[target] = candidate->id;
        has_available_group = true;
    }
    if (!has_available_group) {
        QAction *none = add_to_group->addAction(bgl_tr("OBSTitles.NoAvailableGroups"));
        none->setEnabled(false);
    }
    QAction *remove_from_group = menu.addAction(bgl_tr("OBSTitles.RemoveFromGroup"));
    remove_from_group->setEnabled(std::any_of(selection.begin(), selection.end(), parent_is_group));

    menu.addSeparator();
    QAction *clone = menu.addAction(bgl_tr("OBSTitles.CloneLayer"));
    QAction *copy = menu.addAction(bgl_tr("OBSTitles.CopyLayer"));
    QAction *paste = menu.addAction(bgl_tr("OBSTitles.PasteLayer"));
    paste->setEnabled(layer_clipboard_available_);
    menu.addSeparator();
    QAction *del = menu.addAction(bgl_tr("OBSTitles.DeleteLayer"));

    QAction *chosen = menu.exec(list_->viewport()->mapToGlobal(pos));
    if (chosen == group_layers) emit group_layers_requested();
    else if (chosen == ungroup_layers) emit ungroup_layers_requested();
    else if (chosen == remove_from_group) emit remove_from_group_requested();
    else if (chosen && group_targets.find(chosen) != group_targets.end())
        emit add_to_group_requested(group_targets.at(chosen));
    else if (chosen == clone) emit clone_layer_requested(id);
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
