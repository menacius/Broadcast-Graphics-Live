#include "title-editor-internal.h"


namespace {

const Layer *editor_layer_by_id_for_parenting(const std::shared_ptr<Title> &title, const std::string &id)
{
    if (!title || id.empty()) return nullptr;
    for (const auto &candidate : title->layers) {
        if (candidate && candidate->id == id) return candidate.get();
    }
    return nullptr;
}

QTransform editor_layer_world_transform_for_parenting(const std::shared_ptr<Title> &title,
                                                      const Layer &layer,
                                                      double playhead,
                                                      int depth = 0)
{
    QTransform xf;
    if (depth > 64) return xf;
    if (!layer.parent_id.empty()) {
        if (const Layer *parent = editor_layer_by_id_for_parenting(title, layer.parent_id))
            xf = editor_layer_world_transform_for_parenting(title, *parent, playhead, depth + 1);
    }
    const double lt = std::max(0.0, playhead - layer.in_time);
    xf.translate(layer.pos_x.evaluate(lt), layer.pos_y.evaluate(lt));
    xf.rotate(layer.rotation.evaluate(lt));
    xf.scale(layer.scale_x.evaluate(lt), layer.scale_y.evaluate(lt));
    return xf;
}

bool editor_parenting_would_cycle(const std::shared_ptr<Title> &title,
                                  const std::string &layer_id,
                                  const std::string &parent_id)
{
    std::string cursor = parent_id;
    int guard = 0;
    while (!cursor.empty() && guard++ < 64) {
        if (cursor == layer_id) return true;
        const Layer *parent = editor_layer_by_id_for_parenting(title, cursor);
        if (!parent) break;
        cursor = parent->parent_id;
    }
    return false;
}

// Editor-session-only defaults. These intentionally live only in process memory:
// closing/reopening the editor window within the same OBS run keeps the user's
// last foreground/background and new-layer style, but restarting OBS resets them
// to the compiled/project defaults instead of reading/writing QSettings.
bool g_editor_session_sidebar_colors_initialized = false;
QColor g_editor_session_foreground_color;
QColor g_editor_session_background_color;
bool g_editor_session_new_layer_defaults_initialized = false;
Layer g_editor_session_new_layer_style;

} // namespace

TitleEditor::TitleEditor(QWidget *parent)
    : QMainWindow(parent, Qt::Window)
{
    setWindowTitle(obsgs_tr("OBSTitles.EditorWindowTitle"));
    resize(1280, 760);
    setMinimumSize(900, 600);

    /* Dark background */
    QPalette pal = palette();
    pal.setColor(QPalette::Window,     C_BG_DARK);
    pal.setColor(QPalette::WindowText, C_TEXT);
    pal.setColor(QPalette::Base,       C_BG_MID);
    pal.setColor(QPalette::AlternateBase, C_BG_LIGHT);
    pal.setColor(QPalette::Text,       C_TEXT);
    pal.setColor(QPalette::Button,     C_BG_LIGHT);
    pal.setColor(QPalette::ButtonText, C_TEXT);
    pal.setColor(QPalette::Highlight,  C_ACCENT);
    setPalette(pal);
    setAutoFillBackground(true);
    setDockNestingEnabled(true);
    setDockOptions(QMainWindow::AllowNestedDocks |
                   QMainWindow::AllowTabbedDocks |
                   QMainWindow::AnimatedDocks |
                   QMainWindow::GroupedDragging);

    build_ui();
    load_sidebar_default_colors();
    load_new_layer_defaults();
    update_sidebar_color_swatches(nullptr);

    play_timer_ = new QTimer(this);
    play_timer_->setInterval(std::max(1, (int)std::round(obs_frame_duration() * 1000.0)));
    connect(play_timer_, &QTimer::timeout, this, &TitleEditor::tick);

    clock_timer_ = new QTimer(this);
    clock_timer_->setInterval(33);
    connect(clock_timer_, &QTimer::timeout, this, [this]() {
        update_title_bar();
        if (canvas_) canvas_->update();
    });
    clock_timer_->start();

    qApp->installEventFilter(this);
}

TitleEditor::~TitleEditor()
{
    save_sidebar_default_colors();
    save_new_layer_defaults();
    save_editor_layout();
    if (qApp)
        qApp->removeEventFilter(this);
}




QWidget *TitleEditor::create_effects_panel()
{
    effects_panel_ = new EffectsPanel(this);
    connect(effects_panel_, &EffectsPanel::property_changed, this, &TitleEditor::on_title_modified);
    return effects_panel_;
}

QWidget *TitleEditor::create_styles_panel()
{
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("OBSGraphicsStudioProStylesTabs"));
    tabs->setDocumentMode(true);

    auto make_tab = [](const QString &title, const QString &description) {
        auto *tab = new QWidget;
        auto *layout = new QVBoxLayout(tab);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(6);
        auto *heading = new QLabel(title, tab);
        QFont heading_font = heading->font();
        heading_font.setBold(true);
        heading->setFont(heading_font);
        heading->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(qApp->palette().color(QPalette::WindowText).name(QColor::HexRgb)));
        auto *body = new QLabel(description, tab);
        body->setWordWrap(true);
        body->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg((qApp->palette().color(QPalette::WindowText).lightness() < 128 ? qApp->palette().color(QPalette::WindowText).lighter(135) : qApp->palette().color(QPalette::WindowText).darker(135)).name(QColor::HexRgb)));
        layout->addWidget(heading);
        layout->addWidget(body);
        layout->addStretch(1);
        return tab;
    };

    tabs->addTab(make_tab(QStringLiteral("Text styles"),
                          QStringLiteral("Reusable typography presets, text treatments, and layer text settings will be managed here.")),
                 QStringLiteral("Text"));
    tabs->addTab(make_tab(QStringLiteral("Gradient styles"),
                          QStringLiteral("Reusable foreground and background gradient presets will be managed here.")),
                 QStringLiteral("Gradient"));
    tabs->addTab(make_tab(QStringLiteral("Pattern styles"),
                          QStringLiteral("Reusable pattern, texture, and fill presets will be managed here.")),
                 QStringLiteral("Pattern"));
    tabs->addTab(make_tab(QStringLiteral("Style presets"),
                          QStringLiteral("Saved style preset libraries and shared style settings will be organized here.")),
                 QStringLiteral("Presets"));

    return tabs;
}

QWidget *TitleEditor::create_color_swatches_panel()
{
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *hint = new QLabel(QStringLiteral("Reusable color palettes"), panel);
    hint->setWordWrap(true);
    QFont hint_font = hint->font();
    hint_font.setBold(true);
    hint->setFont(hint_font);
    hint->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg(qApp->palette().color(QPalette::WindowText).name(QColor::HexRgb)));
    layout->addWidget(hint);

    auto *grid_widget = new QWidget(panel);
    auto *grid = new QGridLayout(grid_widget);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(6);

    const std::array<QColor, 24> colors = {
        QColor("#ffffff"), QColor("#d9d9d9"), QColor("#a6a6a6"), QColor("#6f6f6f"),
        QColor("#262626"), QColor("#000000"), QColor("#ff4b4b"), QColor("#ff9f1c"),
        QColor("#ffd166"), QColor("#2ec4b6"), QColor("#00a8e8"), QColor("#7b61ff"),
        QColor("#f72585"), QColor("#b5179e"), QColor("#7209b7"), QColor("#3a0ca3"),
        QColor("#4361ee"), QColor("#4cc9f0"), QColor("#52b788"), QColor("#95d5b2"),
        QColor("#f4a261"), QColor("#e76f51"), QColor("#8d6e63"), QColor("#3d405b")
    };

    for (int i = 0; i < (int)colors.size(); ++i) {
        auto *swatch = new QToolButton(grid_widget);
        swatch->setObjectName(QStringLiteral("OBSGraphicsStudioProColorSwatch"));
        swatch->setFixedSize(24, 24);
        swatch->setAutoRaise(false);
        swatch->setToolTip(colors[i].name(QColor::HexRgb).toUpper());
        swatch->setStyleSheet(QStringLiteral("QToolButton{background:%1;border:1px solid %2;border-radius:3px;}"
                                             "QToolButton:hover{border:2px solid %3;}" )
                              .arg(colors[i].name(QColor::HexRgb),
                                   qApp->palette().color(QPalette::Mid).name(QColor::HexRgb),
                                   qApp->palette().color(QPalette::Highlight).name(QColor::HexRgb)));
        grid->addWidget(swatch, i / 6, i % 6);
    }

    layout->addWidget(grid_widget, 0, Qt::AlignTop | Qt::AlignLeft);
    auto *footer = new QLabel(QStringLiteral("Palette saving, palette import/export, and quick color application workflows will build on these swatches."), panel);
    footer->setWordWrap(true);
    footer->setStyleSheet(QStringLiteral("color:%1;background:transparent;").arg((qApp->palette().color(QPalette::WindowText).lightness() < 128 ? qApp->palette().color(QPalette::WindowText).lighter(145) : qApp->palette().color(QPalette::WindowText).darker(145)).name(QColor::HexRgb)));
    layout->addWidget(footer);
    layout->addStretch(1);

    return panel;
}

void TitleEditor::create_docked_panel_menu(QMenuBar *menu_bar)
{
    if (!menu_bar) return;

    auto *windows_menu = menu_bar->addMenu(QStringLiteral("Windows"));

    act_lock_panels_ = windows_menu->addAction(QStringLiteral("Lock Panels"));
    act_lock_panels_->setCheckable(true);
    connect(act_lock_panels_, &QAction::toggled, this, &TitleEditor::set_panels_locked);

    QAction *reset_layout_action = windows_menu->addAction(QStringLiteral("Reset to Default Layout"));
    connect(reset_layout_action, &QAction::triggered, this, &TitleEditor::reset_default_layout);

    windows_menu->addSeparator();
    act_tools_visible_ = windows_menu->addAction(QStringLiteral("Tools"));
    act_tools_visible_->setCheckable(true);
    act_tools_visible_->setChecked(true);
    connect(act_tools_visible_, &QAction::triggered, this, [this](bool visible) {
        if (tools_dock_) tools_dock_->setVisible(visible);
    });

    act_graphic_props_visible_ = windows_menu->addAction(QStringLiteral("Graphic Properties"));
    act_graphic_props_visible_->setCheckable(true);
    act_graphic_props_visible_->setChecked(true);
    connect(act_graphic_props_visible_, &QAction::triggered, this, [this](bool visible) {
        if (graphic_props_dock_) graphic_props_dock_->setVisible(visible);
    });

    act_layer_props_visible_ = windows_menu->addAction(QStringLiteral("Layer Properties"));
    act_layer_props_visible_->setCheckable(true);
    act_layer_props_visible_->setChecked(true);
    connect(act_layer_props_visible_, &QAction::triggered, this, [this](bool visible) {
        if (layer_props_dock_) layer_props_dock_->setVisible(visible);
    });

    act_effects_visible_ = windows_menu->addAction(QStringLiteral("Effects"));
    act_effects_visible_->setCheckable(true);
    act_effects_visible_->setChecked(true);
    connect(act_effects_visible_, &QAction::triggered, this, [this](bool visible) {
        if (effects_dock_) effects_dock_->setVisible(visible);
    });

    act_styles_visible_ = windows_menu->addAction(QStringLiteral("Styles"));
    act_styles_visible_->setCheckable(true);
    act_styles_visible_->setChecked(true);
    connect(act_styles_visible_, &QAction::triggered, this, [this](bool visible) {
        if (styles_dock_) styles_dock_->setVisible(visible);
    });

    act_color_swatches_visible_ = windows_menu->addAction(QStringLiteral("Color Swatches"));
    act_color_swatches_visible_->setCheckable(true);
    act_color_swatches_visible_->setChecked(true);
    connect(act_color_swatches_visible_, &QAction::triggered, this, [this](bool visible) {
        if (color_swatches_dock_) color_swatches_dock_->setVisible(visible);
    });

    act_timeline_visible_ = windows_menu->addAction(QStringLiteral("Timeline"));
    act_timeline_visible_->setCheckable(true);
    act_timeline_visible_->setChecked(true);
    connect(act_timeline_visible_, &QAction::triggered, this, [this](bool visible) {
        if (timeline_dock_) timeline_dock_->setVisible(visible);
    });
}

QDockWidget *TitleEditor::create_editor_dock(const QString &object_name, const QString &title, QWidget *panel)
{
    auto *dock = new QDockWidget(title, this);
    dock->setObjectName(object_name);
    const bool timeline_dock = object_name == QString::fromUtf8(kTimelineDockObjectName);
    dock->setAllowedAreas(timeline_dock
                              ? (Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea)
                              : (Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea));
    dock->setWidget(panel);
    dock->setFeatures(QDockWidget::DockWidgetClosable |
                      QDockWidget::DockWidgetMovable |
                      QDockWidget::DockWidgetFloatable);
    dock->setMinimumWidth(panel ? panel->minimumWidth() : 220);
    if (timeline_dock)
        dock->setMinimumHeight(panel ? panel->minimumHeight() : 180);

    QAction *visibility_action = nullptr;
    if (object_name == QString::fromUtf8(kGraphicPropertiesDockObjectName))
        visibility_action = act_graphic_props_visible_;
    else if (object_name == QString::fromUtf8(kLayerPropertiesDockObjectName))
        visibility_action = act_layer_props_visible_;
    else if (object_name == QString::fromUtf8(kEffectsDockObjectName))
        visibility_action = act_effects_visible_;
    else if (object_name == QString::fromUtf8(kStylesDockObjectName))
        visibility_action = act_styles_visible_;
    else if (object_name == QString::fromUtf8(kColorSwatchesDockObjectName))
        visibility_action = act_color_swatches_visible_;
    else if (object_name == QString::fromUtf8(kTimelineDockObjectName))
        visibility_action = act_timeline_visible_;
    else if (object_name == QStringLiteral("OBSGraphicsStudioProToolsDock"))
        visibility_action = act_tools_visible_;

    if (visibility_action) {
        connect(dock, &QDockWidget::visibilityChanged, this, [visibility_action](bool visible) {
            QSignalBlocker blocker(visibility_action);
            visibility_action->setChecked(visible);
        });
    }

    connect(dock, &QDockWidget::topLevelChanged, this, [this]() { save_editor_layout(); });
    connect(dock, &QDockWidget::dockLocationChanged, this, [this]() { save_editor_layout(); });
    connect(dock, &QDockWidget::visibilityChanged, this, [this]() { save_editor_layout(); });
    return dock;
}

void TitleEditor::load_editor_layout()
{
    restoring_editor_layout_ = true;

    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));

    const QByteArray geometry = settings.value(QString::fromUtf8(kEditorGeometryKey)).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);

    panels_locked_ = settings.value(QString::fromUtf8(kEditorPanelsLockedKey), panels_locked_).toBool();
    if (act_lock_panels_) {
        QSignalBlocker blocker(act_lock_panels_);
        act_lock_panels_->setChecked(panels_locked_);
    }

    const QByteArray window_state = settings.value(QString::fromUtf8(kEditorWindowStateKey)).toByteArray();
    if (!window_state.isEmpty())
        restoreState(window_state);

    settings.endGroup();

    if (act_tools_visible_ && tools_dock_) {
        QSignalBlocker blocker(act_tools_visible_);
        act_tools_visible_->setChecked(!tools_dock_->isHidden());
    }
    if (act_graphic_props_visible_ && graphic_props_dock_) {
        QSignalBlocker blocker(act_graphic_props_visible_);
        act_graphic_props_visible_->setChecked(!graphic_props_dock_->isHidden());
    }
    if (act_layer_props_visible_ && layer_props_dock_) {
        QSignalBlocker blocker(act_layer_props_visible_);
        act_layer_props_visible_->setChecked(!layer_props_dock_->isHidden());
    }
    if (act_effects_visible_ && effects_dock_) {
        QSignalBlocker blocker(act_effects_visible_);
        act_effects_visible_->setChecked(!effects_dock_->isHidden());
    }
    if (act_styles_visible_ && styles_dock_) {
        QSignalBlocker blocker(act_styles_visible_);
        act_styles_visible_->setChecked(!styles_dock_->isHidden());
    }
    if (act_color_swatches_visible_ && color_swatches_dock_) {
        QSignalBlocker blocker(act_color_swatches_visible_);
        act_color_swatches_visible_->setChecked(!color_swatches_dock_->isHidden());
    }
    if (act_timeline_visible_ && timeline_dock_) {
        QSignalBlocker blocker(act_timeline_visible_);
        act_timeline_visible_->setChecked(!timeline_dock_->isHidden());
    }

    restoring_editor_layout_ = false;
    update_panel_lock_state();
}

void TitleEditor::save_editor_layout() const
{
    if (restoring_editor_layout_ || editor_layout_save_suppressed_)
        return;

    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));
    settings.setValue(QString::fromUtf8(kEditorGeometryKey), saveGeometry());
    settings.setValue(QString::fromUtf8(kEditorWindowStateKey), saveState());
    settings.setValue(QString::fromUtf8(kEditorPanelsLockedKey), panels_locked_);
    settings.endGroup();
    settings.sync();
}

void TitleEditor::reset_default_layout()
{
    restoring_editor_layout_ = true;

    const QDockWidget::DockWidgetFeatures reset_features =
        QDockWidget::DockWidgetClosable |
        QDockWidget::DockWidgetMovable |
        QDockWidget::DockWidgetFloatable;
    for (auto *dock : {tools_dock_, graphic_props_dock_, layer_props_dock_, effects_dock_, styles_dock_, color_swatches_dock_, timeline_dock_}) {
        if (!dock) continue;
        dock->setMaximumWidth(dock == tools_dock_ ? 64 : QWIDGETSIZE_MAX);
        dock->setMinimumWidth(dock == tools_dock_ ? 46 : (dock->widget() ? dock->widget()->minimumWidth() : 220));
        dock->setFeatures(reset_features);
        dock->setAllowedAreas(dock == timeline_dock_
                                  ? (Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea)
                                  : (Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea));
    }

    if (tools_dock_) {
        tools_dock_->setFloating(false);
        tools_dock_->show();
        addDockWidget(Qt::RightDockWidgetArea, tools_dock_);
    }
    if (graphic_props_dock_) {
        graphic_props_dock_->setFloating(false);
        graphic_props_dock_->show();
        addDockWidget(Qt::LeftDockWidgetArea, graphic_props_dock_);
    }
    if (layer_props_dock_) {
        layer_props_dock_->setFloating(false);
        layer_props_dock_->show();
        addDockWidget(Qt::RightDockWidgetArea, layer_props_dock_);
        if (tools_dock_) splitDockWidget(tools_dock_, layer_props_dock_, Qt::Horizontal);
    }
    if (styles_dock_) {
        styles_dock_->setFloating(false);
        styles_dock_->show();
        addDockWidget(Qt::LeftDockWidgetArea, styles_dock_);
        if (graphic_props_dock_) splitDockWidget(graphic_props_dock_, styles_dock_, Qt::Horizontal);
    }
    if (color_swatches_dock_) {
        color_swatches_dock_->setFloating(false);
        color_swatches_dock_->show();
        addDockWidget(Qt::LeftDockWidgetArea, color_swatches_dock_);
        if (styles_dock_) tabifyDockWidget(styles_dock_, color_swatches_dock_);
        else if (graphic_props_dock_) splitDockWidget(graphic_props_dock_, color_swatches_dock_, Qt::Horizontal);
    }
    if (effects_dock_) {
        effects_dock_->setFloating(false);
        effects_dock_->show();
        addDockWidget(Qt::RightDockWidgetArea, effects_dock_);
        if (layer_props_dock_) splitDockWidget(layer_props_dock_, effects_dock_, Qt::Horizontal);
    }
    if (timeline_dock_) {
        timeline_dock_->setFloating(false);
        timeline_dock_->show();
        addDockWidget(Qt::BottomDockWidgetArea, timeline_dock_);
    }
    if (tools_dock_) tools_dock_->raise();
    if (graphic_props_dock_) graphic_props_dock_->raise();
    if (layer_props_dock_) layer_props_dock_->raise();
    if (styles_dock_) styles_dock_->raise();

    if (act_tools_visible_) {
        QSignalBlocker blocker(act_tools_visible_);
        act_tools_visible_->setChecked(true);
    }
    if (act_graphic_props_visible_) {
        QSignalBlocker blocker(act_graphic_props_visible_);
        act_graphic_props_visible_->setChecked(true);
    }
    if (act_layer_props_visible_) {
        QSignalBlocker blocker(act_layer_props_visible_);
        act_layer_props_visible_->setChecked(true);
    }
    if (act_effects_visible_) {
        QSignalBlocker blocker(act_effects_visible_);
        act_effects_visible_->setChecked(true);
    }
    if (act_styles_visible_) {
        QSignalBlocker blocker(act_styles_visible_);
        act_styles_visible_->setChecked(true);
    }
    if (act_color_swatches_visible_) {
        QSignalBlocker blocker(act_color_swatches_visible_);
        act_color_swatches_visible_->setChecked(true);
    }
    if (act_timeline_visible_) {
        QSignalBlocker blocker(act_timeline_visible_);
        act_timeline_visible_->setChecked(true);
    }

    resize(1280, 760);
    update_panel_lock_state();
    restoring_editor_layout_ = false;
    save_editor_layout();
}

void TitleEditor::set_panels_locked(bool locked)
{
    panels_locked_ = locked;
    update_panel_lock_state();
    save_editor_layout();
}

void TitleEditor::update_panel_lock_state()
{
    const QDockWidget::DockWidgetFeatures unlocked_features =
        QDockWidget::DockWidgetClosable |
        QDockWidget::DockWidgetMovable |
        QDockWidget::DockWidgetFloatable;
    const QDockWidget::DockWidgetFeatures locked_features = QDockWidget::DockWidgetClosable;

    for (auto *dock : {tools_dock_, graphic_props_dock_, layer_props_dock_, effects_dock_, styles_dock_, color_swatches_dock_, timeline_dock_}) {
        if (!dock) continue;
        dock->setFeatures(panels_locked_ ? locked_features : unlocked_features);
        dock->setAllowedAreas(panels_locked_ ? Qt::NoDockWidgetArea
                                             : (dock == timeline_dock_
                                                    ? (Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea)
                                                    : (Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea)));
        if (panels_locked_) {
            const int locked_width = std::max(dock->minimumWidth(), dock->width());
            if (dock == timeline_dock_) {
                const int locked_height = std::max(dock->minimumHeight(), dock->height());
                dock->setMinimumHeight(locked_height);
                dock->setMaximumHeight(locked_height);
            } else {
                dock->setMinimumWidth(locked_width);
                dock->setMaximumWidth(locked_width);
            }
        } else {
            dock->setMinimumWidth(dock == tools_dock_ ? 46 : (dock->widget() ? dock->widget()->minimumWidth() : 220));
            dock->setMaximumWidth(dock == tools_dock_ ? 64 : QWIDGETSIZE_MAX);
            if (dock == timeline_dock_) {
                dock->setMinimumHeight(dock->widget() ? dock->widget()->minimumHeight() : 180);
                dock->setMaximumHeight(QWIDGETSIZE_MAX);
            }
        }
    }
}


static bool text_has_rtl_direction(const QString &text, bool fallback_rtl = false)
{
    for (const QChar ch : text) {
        switch (ch.direction()) {
        case QChar::DirL:
        case QChar::DirLRE:
        case QChar::DirLRO:
            return false;
        case QChar::DirR:
        case QChar::DirAL:
        case QChar::DirRLE:
        case QChar::DirRLO:
            return true;
        default:
            break;
        }
    }
    return fallback_rtl;
}

static void set_text_layer_direction_defaults(Layer &layer, bool paragraph_box, bool rtl)
{
    layer.align_h = rtl ? 2 : 0;
    layer.align_v = 0;
    layer.origin_x = rtl ? 1.0f : 0.0f;
    layer.origin_y = 0.0f;
    layer.origin_x_prop.static_value = layer.origin_x;
    layer.origin_y_prop.static_value = layer.origin_y;
    layer.text_overflow_mode = paragraph_box ? 0 : 1;
    layer.text_box_width_to_text = !paragraph_box;
    layer.text_box_height_to_text = !paragraph_box;
    layer.max_text_box_width = 9999.0f;
    layer.max_text_box_height = 9999.0f;
    layer.rich_text.default_paragraph_format.align_h = layer.align_h;
    layer.rich_text.default_paragraph_format.align_v = layer.align_v;
    for (auto &block : layer.rich_text.blocks) {
        block.format.align_h = layer.align_h;
        block.format.align_v = layer.align_v;
    }
}

static void set_new_text_layer_contents_empty(Layer &layer)
{
    layer.text_content.clear();
    layer.rich_text_html.clear();
    layer.rich_text = rich_text_document_from_layer_defaults(layer);
    layer.rich_text.plain_text.clear();
    layer.rich_text.ranges.clear();
    layer.rich_text.blocks.clear();
    layer.rich_text.selection = {0, 0};
}

std::shared_ptr<Layer> TitleEditor::create_basic_layer(LayerType type, const QString &name_override)
{
    if (!title_) return nullptr;

    auto l = std::make_shared<Layer>();
    l->id = TitleDataStore::make_uuid();
    if (!name_override.isEmpty()) {
        l->name = name_override.toStdString();
    } else {
        l->name = (type == LayerType::Text) ? editor_text_std("OBSTitles.Text") :
                  (type == LayerType::Clock) ? editor_text_std("OBSTitles.Clock") :
                  (type == LayerType::Ticker) ? editor_text_std("OBSTitles.Ticker") :
                  (type == LayerType::Image) ? editor_text_std("OBSTitles.Image") : editor_text_std("OBSTitles.Shape");
    }
    l->type = type;
    l->text_content = (type == LayerType::Text) ? editor_text_std("OBSTitles.NewText") :
                      (type == LayerType::Ticker) ? editor_text_std("OBSTitles.NewTickerText") : "";
    l->rich_text = rich_text_document_from_layer_defaults(*l);
    if (type == LayerType::Text) {
        set_new_text_layer_contents_empty(*l);
        set_text_layer_direction_defaults(*l, false, false);
    }
    l->clock_format = (type == LayerType::Clock) ? "H:i:s" : l->clock_format;
    l->pos_x.static_value = title_->width / 2.0;
    l->pos_y.static_value = title_->height / 2.0;
    l->rect_width = title_->width * 0.5f;
    l->rect_height = (type == LayerType::Image) ? title_->height * 0.4f : 160.0f;
    l->box_width.static_value = l->rect_width;
    l->box_height.static_value = l->rect_height;
    l->origin_x_prop.static_value = l->origin_x;
    l->origin_y_prop.static_value = l->origin_y;
    apply_new_layer_defaults(*l);
    set_channel_statics(*l, true, l->text_color);
    set_channel_statics(*l, false, l->fill_color);
    l->out_time = title_->duration;
    return l;
}

void TitleEditor::create_shape_layer_from_canvas(ShapeType shape_type, const QPointF &canvas_pt)
{
    if (!title_) return;

    auto layer = create_basic_layer(LayerType::Shape, shape_display_name(shape_type));
    if (!layer) return;
    layer->shape_type = shape_type;
    layer->pos_x.static_value = canvas_pt.x();
    layer->pos_y.static_value = canvas_pt.y();
    if (shape_type == ShapeType::Ellipse || shape_type == ShapeType::Triangle ||
        shape_type == ShapeType::Star || shape_type == ShapeType::Polygon ||
        shape_type == ShapeType::Diamond) {
        const float size = std::min(layer->rect_width, layer->rect_height);
        layer->rect_width = size;
        layer->rect_height = size;
        layer->box_width.static_value = layer->rect_width;
        layer->box_height.static_value = layer->rect_height;
    }
    if (shape_type == ShapeType::RoundedRectangle) {
        set_layer_all_corner_radii(*layer, 18.0f);
        layer->corner_radius_locked = true;
    }

    canvas_created_shape_layer_id_ = layer->id;
    title_->add_layer(layer);
    layers_->refresh();
    on_layer_selected(layer->id);
}


void TitleEditor::create_text_layer_from_canvas(LayerType type, const QPointF &canvas_pt)
{
    if (!title_) return;
    if (type != LayerType::Text && type != LayerType::Clock && type != LayerType::Ticker)
        type = LayerType::Text;

    auto layer = create_basic_layer(type, text_tool_display_name(type));
    if (!layer) return;
    layer->pos_x.static_value = canvas_pt.x();
    layer->pos_y.static_value = canvas_pt.y();
    if (type == LayerType::Text) {
        set_new_text_layer_contents_empty(*layer);
        set_text_layer_direction_defaults(*layer, false, false);
    } else {
        layer->rich_text_html.clear();
    }

    canvas_created_shape_layer_id_ = layer->id;
    title_->add_layer(layer);
    layers_->refresh();
    on_layer_selected(layer->id);
}

void TitleEditor::update_canvas_created_shape(const QRectF &canvas_rect)
{
    if (!title_ || canvas_created_shape_layer_id_.empty()) return;
    auto layer = title_->find_layer(canvas_created_shape_layer_id_);
    if (!layer) return;

    QRectF rect = canvas_rect.normalized();
    const double width = std::max(1.0, rect.width());
    const double height = std::max(1.0, rect.height());
    if (rect.width() < 1.0) rect.setWidth(width);
    if (rect.height() < 1.0) rect.setHeight(height);

    if (is_canvas_text_layer(*layer)) {
        const bool rtl = text_has_rtl_direction(QString::fromStdString(layer->text_content), layer->origin_x > 0.5f);
        set_text_layer_direction_defaults(*layer, true, rtl);
        layer->pos_x.static_value = rtl ? rect.right() : rect.left();
        layer->pos_y.static_value = rect.top();
    } else {
        layer->pos_x.static_value = rect.center().x();
        layer->pos_y.static_value = rect.center().y();
    }
    layer->rect_width = (float)width;
    layer->rect_height = (float)height;
    layer->box_width.static_value = layer->rect_width;
    layer->box_height.static_value = layer->rect_height;
    if (!is_canvas_text_layer(*layer) && layer->shape_type == ShapeType::RoundedRectangle) {
        set_layer_all_corner_radii(*layer, (float)std::min(width, height) * 0.12f);
        layer->corner_radius_locked = true;
    }

    update_layer_panels(layer, playhead_);
}

void TitleEditor::finish_canvas_created_shape(bool keep_layer)
{
    if (!title_ || canvas_created_shape_layer_id_.empty()) return;
    const std::string layer_id = canvas_created_shape_layer_id_;
    canvas_created_shape_layer_id_.clear();

    auto layer = title_->find_layer(layer_id);
    if (!layer) return;
    const bool is_text_layer = is_canvas_text_layer(*layer);
    const bool too_small = is_text_layer
        ? false
        : (layer->shape_type == ShapeType::Line
               ? layer->rect_width < 2.0f
               : (layer->rect_width < 2.0f || layer->rect_height < 2.0f));
    if (!keep_layer || too_small) {
        title_->remove_layer(layer_id);
        layers_->refresh();
        timeline_->set_title(title_);
        sel_layer_id_.clear();
        if (canvas_) canvas_->set_selected_layers({});
        update_layer_panels(nullptr, playhead_);
        return;
    }

    layers_->refresh();
    timeline_->set_title(title_);
    on_layer_selected(layer_id);
    on_title_modified();
    if (is_text_layer && canvas_)
        canvas_->begin_text_edit_for_layer(layer_id);
}

void TitleEditor::build_ui()
{
    restoring_editor_layout_ = true;

    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *menu_bar = new QMenuBar(this);
    auto *file_menu = menu_bar->addMenu(obsgs_tr("OBSTitles.FileMenu"));
    QAction *new_action = file_menu->addAction(obsgs_tr("OBSTitles.New"));
    new_action->setShortcut(QKeySequence::New);
    connect(new_action, &QAction::triggered, this, &TitleEditor::new_title_contents);
    QAction *save_action = file_menu->addAction(obs_icon("save.svg"), obsgs_tr("OBSTitles.Save"));
    save_action->setShortcut(QKeySequence::Save);
    connect(save_action, &QAction::triggered, this, &TitleEditor::save_title);
    QAction *save_as_new_action = file_menu->addAction(obsgs_tr("OBSTitles.SaveAsNew"));
    connect(save_as_new_action, &QAction::triggered, this, &TitleEditor::save_title_as_new);
    QAction *save_library_action = file_menu->addAction(obsgs_tr("OBSTitles.SaveInLibrary"));
    connect(save_library_action, &QAction::triggered, this, [this]() { export_title_template(true); });
    QAction *export_action = file_menu->addAction(obs_icon("export.svg"), obsgs_tr("OBSTitles.Export"));
    connect(export_action, &QAction::triggered, this, [this]() { export_title_template(false); });
    file_menu->addSeparator();
    QAction *exit_action = file_menu->addAction(obs_icon("file-exit.svg"), obsgs_tr("OBSTitles.Exit"));
    connect(exit_action, &QAction::triggered, this, &TitleEditor::close);

    auto *edit_menu = menu_bar->addMenu(obsgs_tr("OBSTitles.EditMenu"));
    edit_menu->addAction(act_undo_ = new QAction(obs_icon("undo.svg"), obsgs_tr("OBSTitles.Undo"), this));
    act_undo_->setShortcut(QKeySequence::Undo);
    connect(act_undo_, &QAction::triggered, this, [this]() {
        if (undo_index_ > 0) restore_undo_snapshot(undo_index_ - 1);
    });
    edit_menu->addAction(act_redo_ = new QAction(obs_icon("redo.svg"), obsgs_tr("OBSTitles.Redo"), this));
    act_redo_->setShortcut(QKeySequence::Redo);
    connect(act_redo_, &QAction::triggered, this, [this]() {
        if (undo_index_ + 1 < (int)undo_stack_.size()) restore_undo_snapshot(undo_index_ + 1);
    });
    edit_menu->addSeparator();
    QAction *copy_action = edit_menu->addAction(obsgs_tr("OBSTitles.Copy"));
    copy_action->setShortcut(QKeySequence::Copy);
    connect(copy_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_selected_keyframes()) {
            timeline_->copy_keyframe_selection();
            return;
        }
        copy_selected_layer();
    });
    QAction *cut_action = edit_menu->addAction(obsgs_tr("OBSTitles.Cut"));
    cut_action->setShortcut(QKeySequence::Cut);
    connect(cut_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_selected_keyframes()) {
            timeline_->cut_keyframe_selection();
            return;
        }
        cut_selected_layer();
    });
    QAction *paste_action = edit_menu->addAction(obsgs_tr("OBSTitles.Paste"));
    paste_action->setShortcut(QKeySequence::Paste);
    connect(paste_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_keyframe_clipboard()) {
            timeline_->paste_keyframes_at_playhead();
            return;
        }
        paste_layer_from_clipboard();
    });
    QAction *delete_action = edit_menu->addAction(obsgs_tr("OBSTitles.Delete"));
    delete_action->setShortcut(QKeySequence::Delete);
    connect(delete_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_selected_keyframes()) {
            timeline_->delete_keyframe_selection();
            return;
        }
        delete_selected_layer();
    });
    edit_menu->addSeparator();
    QAction *preferences_action = edit_menu->addAction(obsgs_tr("OBSTitles.Preferences"));
    connect(preferences_action, &QAction::triggered, this, &TitleEditor::show_preferences);

    auto *view_menu = menu_bar->addMenu(QStringLiteral("View"));
    act_rulers_visible_ = view_menu->addAction(QStringLiteral("Rulers"));
    act_rulers_visible_->setCheckable(true);
    act_rulers_visible_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    act_rulers_visible_->setToolTip(QStringLiteral("Show or hide Photoshop-style canvas rulers."));
    connect(act_rulers_visible_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_rulers_visible(visible);
    });

    act_guides_visible_ = view_menu->addAction(QStringLiteral("Guides"));
    act_guides_visible_->setCheckable(true);
    act_guides_visible_->setChecked(true);
    act_guides_visible_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft));
    act_guides_visible_->setToolTip(QStringLiteral("Show or hide user-created guides."));
    connect(act_guides_visible_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_guides_visible(visible);
    });

    act_guides_locked_ = view_menu->addAction(QStringLiteral("Lock Guides"));
    act_guides_locked_->setCheckable(true);
    act_guides_locked_->setToolTip(QStringLiteral("Prevent guides from being moved, removed, or created from rulers."));
    connect(act_guides_locked_, &QAction::toggled, this, [this](bool locked) {
        if (canvas_) canvas_->set_guides_locked(locked);
    });

    act_guide_coordinates_ = view_menu->addAction(QStringLiteral("Show Guide Coordinates"));
    act_guide_coordinates_->setCheckable(true);
    act_guide_coordinates_->setChecked(true);
    connect(act_guide_coordinates_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_show_guide_coordinates(visible);
    });

    act_clear_guides_ = view_menu->addAction(QStringLiteral("Clear Guides"));
    act_clear_guides_->setToolTip(QStringLiteral("Remove all user-created horizontal and vertical guides."));
    connect(act_clear_guides_, &QAction::triggered, this, [this]() {
        if (canvas_) canvas_->clear_user_guides();
    });
    view_menu->addSeparator();

    QAction *snap_action = view_menu->addAction(QStringLiteral("Snap"));
    snap_action->setCheckable(true);
    snap_action->setChecked(true);
    snap_action->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Semicolon));
    snap_action->setToolTip(QStringLiteral("Globally enable or disable snapping without changing Snap To targets."));
    connect(snap_action, &QAction::toggled, this, [this](bool enabled) {
        if (canvas_) canvas_->set_snap_enabled(enabled);
    });

    auto *snap_to_menu = view_menu->addMenu(QStringLiteral("Snap To"));
    auto add_snap_to_action = [this, snap_to_menu](const QString &text, bool checked, auto setter) {
        QAction *action = snap_to_menu->addAction(text);
        action->setCheckable(true);
        action->setChecked(checked);
        connect(action, &QAction::toggled, this, [this, setter](bool enabled) {
            if (canvas_) (canvas_->*setter)(enabled);
        });
        return action;
    };
    add_snap_to_action(QStringLiteral("Guides"), true, &CanvasPreview::set_snap_to_guides);
    add_snap_to_action(QStringLiteral("Grid"), false, &CanvasPreview::set_snap_to_grid);
    add_snap_to_action(QStringLiteral("Object Edges"), true, &CanvasPreview::set_snap_to_object_edges);
    add_snap_to_action(QStringLiteral("Object Centers"), true, &CanvasPreview::set_snap_to_object_centers);
    add_snap_to_action(QStringLiteral("Canvas Bounds"), true, &CanvasPreview::set_snap_to_canvas_bounds);
    add_snap_to_action(QStringLiteral("Spacing / Alignment"), true, &CanvasPreview::set_snap_to_spacing);

    create_docked_panel_menu(menu_bar);

    auto *help_menu = menu_bar->addMenu(obsgs_tr("OBSTitles.HelpMenu"));
    QAction *about_action = help_menu->addAction(obs_icon("about.svg"), obsgs_tr("OBSTitles.About"));
    connect(about_action, &QAction::triggered, this, &TitleEditor::show_about);
    setMenuBar(menu_bar);

    /* ── Toolbar ── */
    build_toolbar();
    root->addWidget(toolbar_);

    /* ── Title name label bar ── */
    const QPalette editor_pal = qApp->palette();
    const QColor editor_window = editor_pal.color(QPalette::Window);
    const QColor editor_base = editor_pal.color(QPalette::Base);
    const QColor editor_text = editor_pal.color(QPalette::WindowText);
    const QColor editor_field_text = editor_pal.color(QPalette::Text);
    const QColor editor_button = editor_pal.color(QPalette::Button);
    const QColor editor_button_text = editor_pal.color(QPalette::ButtonText);
    const QColor editor_border = editor_pal.color(QPalette::Mid);
    const QColor editor_highlight = editor_pal.color(QPalette::Highlight);
    const QColor editor_hover = editor_button.lightness() < 128 ? editor_button.lighter(125) : editor_button.darker(108);

    auto *title_bar = new QWidget(this);
    title_bar->setStyleSheet(QStringLiteral("background:%1;color:%2;")
                                 .arg(editor_window.name(QColor::HexRgb),
                                      editor_text.name(QColor::HexRgb)));
    auto *title_bar_layout = new QHBoxLayout(title_bar);
    title_bar_layout->setContentsMargins(0, 3, 0, 3);
    title_bar_layout->setSpacing(6);
    title_bar_layout->addStretch(1);
    dirty_indicator_ = new QLabel(title_bar);
    dirty_indicator_->setFixedSize(10, 10);
    dirty_indicator_->setStyleSheet(QStringLiteral("background:%1;border-radius:5px;").arg(QColor(0xE3, 0x33, 0x33).name(QColor::HexRgb)));
    dirty_indicator_->setToolTip(obsgs_tr("OBSTitles.UnsavedChangesIndicator"));
    dirty_indicator_->hide();
    title_bar_layout->addWidget(dirty_indicator_, 0, Qt::AlignVCenter);
    title_lbl_ = new QLabel("—", title_bar);
    title_lbl_->setAlignment(Qt::AlignCenter);
    QFont tf = title_lbl_->font();
    tf.setPointSize(tf.pointSize() + 1);
    tf.setBold(true);
    title_lbl_->setFont(tf);
    title_lbl_->setStyleSheet(QStringLiteral("color:%1;").arg(editor_text.name(QColor::HexRgb)));
    title_lbl_->setToolTip(QStringLiteral("Double-click to rename"));
    title_lbl_->installEventFilter(this);
    title_bar_layout->addWidget(title_lbl_, 0, Qt::AlignVCenter);
    title_name_edit_ = new QLineEdit(title_bar);
    title_name_edit_->setAlignment(Qt::AlignCenter);
    title_name_edit_->setFont(tf);
    title_name_edit_->setMinimumWidth(180);
    title_name_edit_->setStyleSheet(QStringLiteral(
        "QLineEdit{color:%1;background:%2;border:1px solid %3;border-radius:3px;padding:1px 6px;}")
        .arg(editor_field_text.name(QColor::HexRgb),
             editor_base.name(QColor::HexRgb),
             editor_highlight.name(QColor::HexRgb)));
    title_name_edit_->hide();
    title_name_edit_->installEventFilter(this);
    connect(title_name_edit_, &QLineEdit::returnPressed, this, [this]() {
        commit_title_name_edit(true);
    });
    title_bar_layout->addWidget(title_name_edit_, 0, Qt::AlignVCenter);
    gpu_warning_lbl_ = new QLabel(title_bar);
    QFont gpu_warning_font = gpu_warning_lbl_->font();
    gpu_warning_font.setBold(true);
    gpu_warning_lbl_->setFont(gpu_warning_font);
    gpu_warning_lbl_->setStyleSheet(QStringLiteral("color:%1;").arg(QColor(0xFF, 0xCA, 0x4A).name(QColor::HexRgb)));
    gpu_warning_lbl_->hide();
    title_bar_layout->addWidget(gpu_warning_lbl_, 0, Qt::AlignVCenter);
    title_bar_layout->addStretch(1);
    root->addWidget(title_bar);

    /* ── Upper split: Canvas (dockable property panels live in QMainWindow dock areas) ── */
    auto *upper_split = new QSplitter(Qt::Horizontal, central);

    title_props_ = new TitlePropertiesPanel(this);
    title_props_->setMinimumWidth(240);

    auto *canvas_panel = new QWidget(upper_split);
    canvas_panel->setStyleSheet(QStringLiteral("background:%1;color:%2;")
                                    .arg(editor_window.name(QColor::HexRgb),
                                         editor_text.name(QColor::HexRgb)));
    auto *canvas_layout = new QVBoxLayout(canvas_panel);
    canvas_layout->setContentsMargins(0, 0, 0, 0);
    canvas_layout->setSpacing(0);
    canvas_ = new CanvasPreview(canvas_panel);
    canvas_->setMinimumSize(300, 200);
    if (act_rulers_visible_) act_rulers_visible_->setChecked(canvas_->rulers_visible());
    if (act_guides_visible_) act_guides_visible_->setChecked(canvas_->guides_visible());
    if (act_guides_locked_) act_guides_locked_->setChecked(canvas_->guides_locked());
    if (act_guide_coordinates_) act_guide_coordinates_->setChecked(canvas_->show_guide_coordinates());
    canvas_layout->addWidget(canvas_, 1);

    auto *canvas_zoom_bar = new QWidget(canvas_panel);
    canvas_zoom_bar->setFixedHeight(34);
    canvas_zoom_bar->setStyleSheet(QStringLiteral(
        "QWidget{background:%1;border-top:1px solid %2;color:%3;}"
        "QPushButton,QToolButton{color:%4;background:%5;border:1px solid %2;border-radius:3px;padding:3px 8px;}"
        "QPushButton:hover,QToolButton:hover{background:%6;}"
        "QToolButton::menu-indicator{image:none;}"
        "QSpinBox{color:%7;background:%8;border:1px solid %2;border-radius:3px;padding:2px 6px;}"
        "QSpinBox::up-button,QSpinBox::down-button{width:0;border:none;}"
        "QSlider::groove:horizontal{height:4px;background:%8;border-radius:2px;}"
        "QSlider::handle:horizontal{width:12px;margin:-5px 0;background:%4;border-radius:6px;}"
        "QSlider::sub-page:horizontal{background:%9;border-radius:2px;}")
        .arg(editor_window.name(QColor::HexRgb),
             editor_border.name(QColor::HexRgb),
             editor_text.name(QColor::HexRgb),
             editor_button_text.name(QColor::HexRgb),
             editor_button.name(QColor::HexRgb),
             editor_hover.name(QColor::HexRgb),
             editor_field_text.name(QColor::HexRgb),
             editor_base.name(QColor::HexRgb),
             editor_highlight.name(QColor::HexRgb)));
    auto *canvas_zoom_layout = new QHBoxLayout(canvas_zoom_bar);
    canvas_zoom_layout->setContentsMargins(10, 0, 10, 0);
    canvas_zoom_layout->setSpacing(8);
    auto *canvas_zoom_out = new QPushButton(canvas_zoom_bar);
    canvas_zoom_out->setIcon(obs_icon("zoom-out.svg"));
    canvas_zoom_out->setFixedWidth(30);
    auto *canvas_zoom_slider = new QSlider(Qt::Horizontal, canvas_zoom_bar);
    canvas_zoom_slider->setRange(5, 1600);
    canvas_zoom_slider->setValue(canvas_->zoom_percent());
    canvas_zoom_slider->setMinimumWidth(220);
    canvas_zoom_slider->setMaximumWidth(360);
    auto *canvas_zoom_in = new QPushButton(canvas_zoom_bar);
    canvas_zoom_in->setIcon(obs_icon("zoom-in.svg"));
    canvas_zoom_in->setFixedWidth(30);
    auto *canvas_zoom_percent = new QSpinBox(canvas_zoom_bar);
    canvas_zoom_percent->setRange(5, 1600);
    canvas_zoom_percent->setSuffix("%");
    canvas_zoom_percent->setAlignment(Qt::AlignCenter);
    canvas_zoom_percent->setButtonSymbols(QAbstractSpinBox::NoButtons);
    canvas_zoom_percent->setFixedWidth(72);
    canvas_zoom_percent->setValue(canvas_->zoom_percent());
    auto *fit_canvas = new QToolButton(canvas_zoom_bar);
    fit_canvas->setText("Fit");
    fit_canvas->setPopupMode(QToolButton::InstantPopup);
    auto *fit_canvas_menu = new QMenu(fit_canvas);
    auto add_canvas_zoom_action = [fit_canvas_menu](const QString &text, int percent) {
        QAction *action = fit_canvas_menu->addAction(text);
        action->setData(percent);
        return action;
    };
    QAction *fit_action = fit_canvas_menu->addAction("Fit");
    fit_action->setData(-1);
    QAction *fit_100_action = fit_canvas_menu->addAction("Fit up to 100%");
    fit_100_action->setData(-2);
    add_canvas_zoom_action("50%", 50);
    add_canvas_zoom_action("100%", 100);
    add_canvas_zoom_action("200%", 200);
    add_canvas_zoom_action("400%", 400);
    add_canvas_zoom_action("800%", 800);
    add_canvas_zoom_action("1600%", 1600);
    fit_canvas->setMenu(fit_canvas_menu);
    QSettings canvas_settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    canvas_settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));
    const int saved_checkerboard_pattern = std::clamp(
        canvas_settings.value(QString::fromUtf8(kEditorCanvasTransparencyKey), 1).toInt(), 0, 5);
    const bool saved_safe_guides_visible =
        canvas_settings.value(QString::fromUtf8(kEditorSafeGuidesVisibleKey), false).toBool();
    canvas_settings.endGroup();

    auto *checkerboard = new QToolButton(canvas_zoom_bar);
    checkerboard->setText("Transparency: Medium");
    checkerboard->setPopupMode(QToolButton::InstantPopup);
    auto *checkerboard_menu = new QMenu(checkerboard);
    auto *checkerboard_group = new QActionGroup(checkerboard_menu);
    checkerboard_group->setExclusive(true);
    auto add_checkerboard_action = [checkerboard_menu, checkerboard_group, saved_checkerboard_pattern](const QString &text, int pattern) {
        QAction *action = checkerboard_menu->addAction(text);
        action->setData(pattern);
        action->setCheckable(true);
        action->setChecked(pattern == saved_checkerboard_pattern);
        checkerboard_group->addAction(action);
        return action;
    };
    add_checkerboard_action("Light", 0);
    add_checkerboard_action("Medium", 1);
    add_checkerboard_action("Dark", 2);
    add_checkerboard_action("Solid white", 3);
    add_checkerboard_action("Solid black", 4);
    add_checkerboard_action("Solid grey", 5);
    if (auto *checked = checkerboard_group->checkedAction())
        checkerboard->setText(QString("Transparency: %1").arg(checked->text()));
    checkerboard->setMenu(checkerboard_menu);
    canvas_->set_checkerboard_pattern(saved_checkerboard_pattern);
    if (act_safe_guides_) {
        act_safe_guides_->setChecked(saved_safe_guides_visible);
        canvas_->set_safe_guides_visible(saved_safe_guides_visible);
    }
    canvas_zoom_layout->addWidget(canvas_zoom_out);
    canvas_zoom_layout->addWidget(canvas_zoom_slider);
    canvas_zoom_layout->addWidget(canvas_zoom_in);
    canvas_zoom_layout->addWidget(canvas_zoom_percent);
    canvas_zoom_layout->addWidget(fit_canvas);
    canvas_zoom_layout->addWidget(checkerboard);
    auto *safe_guides = new QToolButton(canvas_zoom_bar);
    safe_guides->setDefaultAction(act_safe_guides_);
    safe_guides->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    canvas_zoom_layout->addWidget(safe_guides);
    canvas_zoom_layout->addStretch(1);
    canvas_layout->addWidget(canvas_zoom_bar);
    connect(canvas_zoom_slider, &QSlider::valueChanged, canvas_, &CanvasPreview::set_zoom_percent);
    connect(canvas_zoom_percent, qOverload<int>(&QSpinBox::valueChanged), canvas_, &CanvasPreview::set_zoom_percent);
    connect(canvas_, &CanvasPreview::zoom_percent_changed, this, [canvas_zoom_slider, canvas_zoom_percent](int percent) {
        QSignalBlocker slider_blocker(canvas_zoom_slider);
        QSignalBlocker spin_blocker(canvas_zoom_percent);
        canvas_zoom_slider->setValue(percent);
        canvas_zoom_percent->setValue(percent);
    });
    connect(canvas_zoom_out, &QPushButton::clicked, this, [this]() {
        canvas_->set_zoom_percent((int)std::round(canvas_->zoom_percent() / 1.18));
    });
    connect(canvas_zoom_in, &QPushButton::clicked, this, [this]() {
        canvas_->set_zoom_percent((int)std::round(canvas_->zoom_percent() * 1.18));
    });
    connect(fit_canvas_menu, &QMenu::triggered, this, [this, fit_canvas](QAction *action) {
        int value = action->data().toInt();
        fit_canvas->setText(action->text());
        if (value == -1) canvas_->fit_canvas(false);
        else if (value == -2) canvas_->fit_canvas(true);
        else canvas_->set_zoom_percent(value);
    });
    connect(checkerboard_menu, &QMenu::triggered, this, [this, checkerboard](QAction *action) {
        checkerboard->setText(QString("Transparency: %1").arg(action->text()));
        canvas_->set_checkerboard_pattern(action->data().toInt());
    });
    upper_split->addWidget(canvas_panel);

    props_ = new PropertiesPanel(this);
    props_->setMinimumWidth(260);
    upper_split->setStretchFactor(0, 1);

    graphic_props_dock_ = create_editor_dock(QString::fromUtf8(kGraphicPropertiesDockObjectName),
                                             QStringLiteral("Graphic Properties"),
                                             title_props_);
    layer_props_dock_ = create_editor_dock(QString::fromUtf8(kLayerPropertiesDockObjectName),
                                           QStringLiteral("Layer Properties"),
                                           props_);
    effects_dock_ = create_editor_dock(QString::fromUtf8(kEffectsDockObjectName),
                                       QStringLiteral("Effects"),
                                       create_effects_panel());
    styles_dock_ = create_editor_dock(QString::fromUtf8(kStylesDockObjectName),
                                      QStringLiteral("Styles"),
                                      create_styles_panel());
    color_swatches_dock_ = create_editor_dock(QString::fromUtf8(kColorSwatchesDockObjectName),
                                              QStringLiteral("Color Swatches"),
                                              create_color_swatches_panel());
    tools_sidebar_ = new ToolsSidebar(this);
    tools_dock_ = create_editor_dock(QStringLiteral("OBSGraphicsStudioProToolsDock"),
                                     QStringLiteral("Tools"),
                                     tools_sidebar_);
    tools_dock_->setMinimumWidth(46);
    tools_dock_->setMaximumWidth(64);
    addDockWidget(Qt::LeftDockWidgetArea, graphic_props_dock_);
    addDockWidget(Qt::RightDockWidgetArea, tools_dock_);
    addDockWidget(Qt::RightDockWidgetArea, layer_props_dock_);
    splitDockWidget(tools_dock_, layer_props_dock_, Qt::Horizontal);
    addDockWidget(Qt::LeftDockWidgetArea, styles_dock_);
    splitDockWidget(graphic_props_dock_, styles_dock_, Qt::Horizontal);
    addDockWidget(Qt::LeftDockWidgetArea, color_swatches_dock_);
    tabifyDockWidget(styles_dock_, color_swatches_dock_);
    addDockWidget(Qt::RightDockWidgetArea, effects_dock_);
    splitDockWidget(layer_props_dock_, effects_dock_, Qt::Horizontal);
    tools_dock_->raise();
    graphic_props_dock_->raise();
    layer_props_dock_->raise();
    styles_dock_->raise();

    /* ── Timeline editor: full-width transport | LayerStack + Timeline | full-width zoom ── */
    auto *timeline_editor = new QWidget(this);
    auto *timeline_editor_layout = new QVBoxLayout(timeline_editor);
    timeline_editor_layout->setContentsMargins(0, 0, 0, 0);
    timeline_editor_layout->setSpacing(0);

    auto *timeline_transport = new QWidget(timeline_editor);
    timeline_transport->setFixedHeight(34);
    const QPalette timeline_pal = qApp->palette();
    const QColor timeline_window = timeline_pal.color(QPalette::Window);
    const QColor timeline_base = timeline_pal.color(QPalette::Base);
    const QColor timeline_text = timeline_pal.color(QPalette::WindowText);
    const QColor timeline_button = timeline_pal.color(QPalette::Button);
    const QColor timeline_button_text = timeline_pal.color(QPalette::ButtonText);
    const QColor timeline_border = timeline_pal.color(QPalette::Mid);
    const QColor timeline_highlight = timeline_pal.color(QPalette::Highlight);
    const QColor timeline_hover = timeline_button.lightness() < 128 ? timeline_button.lighter(125) : timeline_button.darker(108);
    timeline_transport->setStyleSheet(QStringLiteral(
        "QWidget{background:%1;border-bottom:1px solid %2;color:%3;}"
        "QToolButton{color:%4;background:transparent;padding:3px 7px;border:none;}"
        "QToolButton:hover{background:%5;border-radius:2px;}"
        "QToolButton:checked{background:%6;color:%7;border-radius:2px;}"
        "QLabel{color:%6;font-family:monospace;}")
        .arg(timeline_window.name(QColor::HexRgb),
             timeline_border.name(QColor::HexRgb),
             timeline_text.name(QColor::HexRgb),
             timeline_button_text.name(QColor::HexRgb),
             timeline_hover.name(QColor::HexRgb),
             timeline_highlight.name(QColor::HexRgb),
             timeline_pal.color(QPalette::HighlightedText).name(QColor::HexRgb)));
    auto *transport_layout = new QHBoxLayout(timeline_transport);
    transport_layout->setContentsMargins(8, 0, 8, 0);
    transport_layout->setSpacing(2);
    auto make_transport_button = [timeline_transport](QAction *action) {
        auto *button = new QToolButton(timeline_transport);
        button->setDefaultAction(action);
        button->setIconSize(QSize(14, 14));
        button->setAutoRaise(true);
        return button;
    };
    transport_layout->addStretch(1);
    time_lbl_ = new QLabel("0.000 s", timeline_transport);
    time_lbl_->setMinimumWidth(116);
    time_lbl_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    time_lbl_->setStyleSheet(QStringLiteral("color:%1;font-family:monospace;")
                                 .arg(timeline_highlight.name(QColor::HexRgb)));
    transport_layout->addWidget(time_lbl_, 0, Qt::AlignVCenter);
    transport_layout->addSpacing(8);
    transport_layout->addWidget(make_transport_button(act_rew_));
    transport_layout->addWidget(make_transport_button(act_prev_kf_));
    transport_layout->addWidget(make_transport_button(act_play_));
    transport_layout->addWidget(make_transport_button(act_full_loop_));
    QAction *step_forward_action = new QAction(obs_icon("step-forward.svg"), obsgs_tr("OBSTitles.StepForward"), timeline_transport);
    connect(step_forward_action, &QAction::triggered, this, &TitleEditor::step_forward);
    transport_layout->addWidget(make_transport_button(step_forward_action));
    transport_layout->addWidget(make_transport_button(act_next_kf_));
    transport_layout->addStretch(1);
    timeline_editor_layout->addWidget(timeline_transport);

    auto *lower_split = new QSplitter(Qt::Horizontal, timeline_editor);

    auto *layers_panel = new QWidget(lower_split);
    auto *layers_layout = new QVBoxLayout(layers_panel);
    layers_layout->setContentsMargins(0, 0, 0, 0);
    layers_layout->setSpacing(0);

    layers_ = new LayerStack(layers_panel);
    layers_->setMinimumHeight(140);
    layers_layout->addWidget(layers_, 1);
    layers_panel->setMinimumWidth(280);
    layers_panel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    lower_split->addWidget(layers_panel);

    auto *timeline_panel = new QWidget(lower_split);
    auto *timeline_panel_layout = new QVBoxLayout(timeline_panel);
    timeline_panel_layout->setContentsMargins(0, 0, 0, 0);
    timeline_panel_layout->setSpacing(0);

    timeline_ = new TimelineWidget(timeline_panel);
    timeline_->setMinimumHeight(140);
    timeline_panel_layout->addWidget(timeline_, 1);

    auto *timeline_zoom_bar = new QWidget(timeline_panel);
    timeline_zoom_bar->setFixedHeight(34);
    timeline_zoom_bar->setStyleSheet(QStringLiteral(
        "QWidget{background:%1;border-top:1px solid %2;color:%3;}"
        "QPushButton{color:%4;background:%5;border:1px solid %2;border-radius:3px;padding:3px 8px;}"
        "QPushButton:hover{background:%6;}"
        "QSlider::groove:horizontal{height:4px;background:%7;border-radius:2px;}"
        "QSlider::handle:horizontal{width:12px;margin:-5px 0;background:%4;border-radius:6px;}"
        "QSlider::sub-page:horizontal{background:%8;border-radius:2px;}")
        .arg(timeline_window.name(QColor::HexRgb),
             timeline_border.name(QColor::HexRgb),
             timeline_text.name(QColor::HexRgb),
             timeline_button_text.name(QColor::HexRgb),
             timeline_button.name(QColor::HexRgb),
             timeline_hover.name(QColor::HexRgb),
             timeline_base.name(QColor::HexRgb),
             timeline_highlight.name(QColor::HexRgb)));
    auto *zoom_layout = new QHBoxLayout(timeline_zoom_bar);
    zoom_layout->setContentsMargins(10, 0, 10, 0);
    zoom_layout->setSpacing(8);
    auto *zoom_out = new QPushButton(timeline_zoom_bar);
    zoom_out->setIcon(obs_icon("zoom-out.svg"));
    zoom_out->setFixedWidth(30);
    auto *zoom_slider = new QSlider(Qt::Horizontal, timeline_zoom_bar);
    zoom_slider->setRange(5, 1200);
    zoom_slider->setValue(timeline_->zoom_percent());
    zoom_slider->setMinimumWidth(220);
    zoom_slider->setMaximumWidth(360);
    auto *zoom_in = new QPushButton(timeline_zoom_bar);
    zoom_in->setIcon(obs_icon("zoom-in.svg"));
    zoom_in->setFixedWidth(30);
    auto *fit_timeline = new QPushButton(obsgs_tr("OBSTitles.FitTimeline"), timeline_zoom_bar);
    zoom_layout->addWidget(zoom_out);
    zoom_layout->addWidget(zoom_slider);
    zoom_layout->addWidget(zoom_in);
    zoom_layout->addWidget(fit_timeline);
    zoom_layout->addStretch(1);
    connect(zoom_slider, &QSlider::valueChanged, timeline_, &TimelineWidget::set_zoom_percent);
    connect(timeline_, &TimelineWidget::zoom_percent_changed, this, [zoom_slider](int percent) {
        QSignalBlocker blocker(zoom_slider);
        zoom_slider->setValue(percent);
    });
    connect(zoom_out, &QPushButton::clicked, this, [this]() {
        timeline_->set_zoom_percent((int)std::round(timeline_->zoom_percent() / 1.18));
    });
    connect(zoom_in, &QPushButton::clicked, this, [this]() {
        timeline_->set_zoom_percent((int)std::round(timeline_->zoom_percent() * 1.18));
    });
    connect(fit_timeline, &QPushButton::clicked, timeline_, &TimelineWidget::fit_timeline);
    timeline_panel_layout->addWidget(timeline_zoom_bar);
    lower_split->addWidget(timeline_panel);

    if (auto *scroll_bar = layers_->vertical_scroll_bar()) {
        connect(scroll_bar, &QScrollBar::valueChanged, timeline_, &TimelineWidget::set_vertical_scroll);
        connect(timeline_, &TimelineWidget::vertical_scroll_delta_requested, this,
                [scroll_bar](int delta) { scroll_bar->setValue(scroll_bar->value() + delta); });
    }
    lower_split->setStretchFactor(0, 1);
    lower_split->setStretchFactor(1, 3);
    lower_split->setCollapsible(0, false);
    lower_split->setCollapsible(1, false);
    timeline_editor_layout->addWidget(lower_split, 1);

    root->addWidget(upper_split, 1);

    timeline_editor->setMinimumHeight(210);
    timeline_dock_ = create_editor_dock(QString::fromUtf8(kTimelineDockObjectName),
                                        QStringLiteral("Timeline"),
                                        timeline_editor);
    timeline_dock_->setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
    timeline_dock_->setMinimumHeight(220);
    addDockWidget(Qt::BottomDockWidgetArea, timeline_dock_);

    load_editor_layout();

    /* ── Connect sub-widget signals ── */
    connect(layers_, &LayerStack::layer_selected,
            this, &TitleEditor::on_layer_selected);
    connect(layers_, &LayerStack::layers_selected,
            this, [this](const std::vector<std::string> &ids) {
                sel_layer_id_ = ids.size() == 1 ? ids.back() : std::string();
                canvas_->set_selected_layers(ids);
                timeline_->set_selected_layer(sel_layer_id_);
                if (!title_ || ids.size() != 1) {
                    update_layer_panels(nullptr, playhead_);
                    return;
                }
                auto layer = title_->find_layer(sel_layer_id_);
                update_layer_panels(layer, playhead_);
            });

    connect(layers_, &LayerStack::add_layer_requested,
            this, [this](LayerType type) {
                if (!title_) return;
                auto l = create_basic_layer(type);
                if (!l) return;
                if (type == LayerType::Image) {
                    l->lock_aspect_ratio = true;
                    QString path = QFileDialog::getOpenFileName(
                        this, obsgs_tr("OBSTitles.ChooseImage"), QString(),
                        obsgs_tr("OBSTitles.ImageFileFilter"));
                    if (path.isEmpty()) return;
                    l->image_path = path.toStdString();
                    QSize image_size = editor_image_intrinsic_size(path);
                    if (image_size.isValid() && !image_size.isEmpty()) {
                        l->rect_width = (float)image_size.width();
                        l->rect_height = (float)image_size.height();
                        l->box_width.static_value = l->rect_width;
                        l->box_height.static_value = l->rect_height;
                    }
                }
                title_->add_layer(l);
                layers_->refresh();
                on_layer_selected(l->id);
                on_title_modified();
                if (type == LayerType::Text && canvas_)
                    canvas_->begin_text_edit_for_layer(l->id);
            });

    connect(layers_, &LayerStack::clone_layer_requested,
            this, [this](const std::string &lid) {
                if (!title_) return;
                auto selected = layers_ ? layers_->selected_ids() : std::vector<std::string>{};
                if (std::find(selected.begin(), selected.end(), lid) == selected.end())
                    on_layer_selected(lid);
                duplicate_selected_layers();
            });

    connect(layers_, &LayerStack::copy_layer_requested,
            this, [this](const std::string &lid) {
                auto selected = layers_ ? layers_->selected_ids() : std::vector<std::string>{};
                if (std::find(selected.begin(), selected.end(), lid) == selected.end())
                    on_layer_selected(lid);
                copy_selected_layer();
            });

    connect(layers_, &LayerStack::paste_layer_requested,
            this, [this](const std::string &anchor_id) {
                if (!anchor_id.empty()) on_layer_selected(anchor_id);
                paste_layer_from_clipboard();
            });

    connect(layers_, &LayerStack::delete_layer_requested,
            this, [this](const std::string &lid) {
                auto selected = layers_ ? layers_->selected_ids() : std::vector<std::string>{};
                if (std::find(selected.begin(), selected.end(), lid) == selected.end())
                    on_layer_selected(lid);
                delete_selected_layer();
            });

    connect(layers_, &LayerStack::layer_visibility_changed,
            this, [this](const std::string &lid, bool visible) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->visible = visible;
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_lock_changed,
            this, [this](const std::string &lid, bool locked) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->locked = locked;
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_expand_changed,
            this, [this](const std::string &lid, bool expanded) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->properties_expanded = expanded;
                    layers_->refresh();
                    timeline_->set_title(title_);
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_parent_changed,
            this, [this](const std::string &lid, const std::string &parent_id) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                                 std::max(0.0, layer->out_time - layer->in_time));
                    const QPointF world_pos = editor_layer_world_transform_for_parenting(title_, *layer, playhead_)
                        .map(QPointF(0.0, 0.0));

                    std::string next_parent_id = parent_id;
                    if (next_parent_id == layer->id || editor_parenting_would_cycle(title_, layer->id, next_parent_id))
                        next_parent_id.clear();

                    layer->parent_id = next_parent_id;
                    QPointF local_pos = world_pos;
                    if (!next_parent_id.empty()) {
                        if (const Layer *parent = editor_layer_by_id_for_parenting(title_, next_parent_id)) {
                            bool invertible = false;
                            QTransform parent_xf = editor_layer_world_transform_for_parenting(title_, *parent, playhead_);
                            QTransform parent_inv = parent_xf.inverted(&invertible);
                            if (invertible)
                                local_pos = parent_inv.map(world_pos);
                        }
                    }
                    set_animated_value(layer->pos_x, lt, local_pos.x());
                    set_animated_value(layer->pos_y, lt, local_pos.y());
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_mask_changed,
            this, [this](const std::string &lid, const std::string &mask_source_id, MaskMode mask_mode) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->mask_source_id = mask_source_id;
                    layer->mask_mode = mask_source_id.empty() ? MaskMode::None : mask_mode;
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_blend_mode_changed,
            this, [this](const std::string &lid, EffectBlendMode blend_mode) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    layer->blend_mode = blend_mode;
                    on_title_modified();
                }
            });

    connect(layers_, &LayerStack::layer_name_changed,
            this, [this](const std::string &lid, const std::string &name) {
                if (!title_) return;
                if (auto layer = title_->find_layer(lid)) {
                    if (layer->name == name) return;
                    layer->name = name.empty() ? editor_text_std("OBSTitles.Layer") : name;
                    timeline_->set_title(title_);
                    on_title_modified();
                    QTimer::singleShot(0, layers_, [this]() {
                        if (layers_) layers_->refresh();
                    });
                }
            });

    connect(timeline_, &TimelineWidget::playhead_changed,
            this, &TitleEditor::on_playhead_changed);
    connect(timeline_, &TimelineWidget::layer_selected,
            this, &TitleEditor::on_layer_selected);
    connect(timeline_, &TimelineWidget::keyframe_easing_changed,
            this, [this]() { on_title_modified(); });

    connect(props_, &PropertiesPanel::property_changed,
            this, &TitleEditor::on_title_modified);
    connect(props_, &PropertiesPanel::text_char_format_changed,
            this, [this](const std::string &layer_id, const RichTextCharFormat &format, uint32_t mask) {
                if (canvas_) canvas_->apply_active_text_char_format(layer_id, format, mask);
            });
    connect(props_, &PropertiesPanel::gradient_editor_active_changed,
            this, [this](bool active) {
                if (canvas_) canvas_->set_gradient_editor_active(active);
            });
    connect(title_props_, &TitlePropertiesPanel::title_changed,
            this, [this](bool push_undo_snapshot) {
                if (!title_) return;
                playhead_ = std::clamp(playhead_, 0.0, title_->duration);
                on_title_modified(push_undo_snapshot);
                timeline_->set_title(title_);
                on_playhead_changed(playhead_);
            });
    connect(layers_, &LayerStack::layer_order_changed,
            this, [this]() {
                layers_->refresh();
                canvas_->refresh_preview();
                timeline_->set_title(title_);
                on_title_modified();
            });

    connect(canvas_, &CanvasPreview::layer_clicked,
            this, &TitleEditor::on_layer_selected);
    connect(canvas_, &CanvasPreview::layers_selected,
            this, [this](const std::vector<std::string> &ids) {
                sel_layer_id_ = ids.size() == 1 ? ids.back() : std::string();
                layers_->set_selected_layers(ids);
                canvas_->set_selected_layers(ids);
                timeline_->set_selected_layer(sel_layer_id_);
                if (!title_ || ids.size() != 1) {
                    update_layer_panels(nullptr, playhead_);
                    return;
                }
                auto layer = title_->find_layer(sel_layer_id_);
                update_layer_panels(layer, playhead_);
            });
    connect(canvas_, &CanvasPreview::layer_geometry_changed,
            this, [this]() {
                on_title_modified();
                if (title_ && !sel_layer_id_.empty()) {
                    if (auto layer = title_->find_layer(sel_layer_id_))
                        update_layer_panels(layer, playhead_);
                }
            });
    connect(canvas_, &CanvasPreview::text_edit_changed,
            this, [this](const std::string &layer_id) {
                if (!title_) return;
                if (props_) props_->set_active_text_edit_layer(layer_id);
                on_title_modified(false);
                if (auto layer = title_->find_layer(layer_id))
                    update_layer_panels(layer, playhead_);
            });
    connect(canvas_, &CanvasPreview::text_edit_cursor_changed,
            this, [this](const std::string &layer_id) {
                if (!title_) return;
                if (props_) props_->set_active_text_edit_layer(layer_id);
                if (auto layer = title_->find_layer(layer_id))
                    update_layer_panels(layer, playhead_);
            });
    connect(canvas_, &CanvasPreview::text_edit_committed,
            this, [this](const std::string &layer_id) {
                if (!title_) return;
                if (props_) props_->set_active_text_edit_layer(std::string());
                on_title_modified();
                if (auto layer = title_->find_layer(layer_id))
                    update_layer_panels(layer, playhead_);
            });
    connect(canvas_, &CanvasPreview::color_picked,
            this, &TitleEditor::apply_picked_color_to_selection);
    connect(canvas_, &CanvasPreview::layer_structure_changed,
            this, [this]() {
                layers_->refresh();
                timeline_->set_title(title_);
            });
    connect(canvas_, &CanvasPreview::shape_drawing_started,
            this, &TitleEditor::create_shape_layer_from_canvas);
    connect(canvas_, &CanvasPreview::text_drawing_started,
            this, &TitleEditor::create_text_layer_from_canvas);
    connect(canvas_, &CanvasPreview::shape_drawing_changed,
            this, &TitleEditor::update_canvas_created_shape);
    connect(canvas_, &CanvasPreview::shape_drawing_finished,
            this, &TitleEditor::finish_canvas_created_shape);
    if (tools_sidebar_) {
        connect(tools_sidebar_, &ToolsSidebar::selection_tool_requested, this, [this]() {
            if (canvas_) canvas_->set_selection_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::shape_tool_requested, this, [this](ShapeType shape_type) {
            if (tools_sidebar_) tools_sidebar_->set_selected_shape(shape_type);
            if (canvas_) canvas_->set_shape_tool_active(shape_type);
        });
        connect(tools_sidebar_, &ToolsSidebar::text_tool_requested, this, [this](LayerType type) {
            if (tools_sidebar_) tools_sidebar_->set_selected_text_layer_type(type);
            if (canvas_) canvas_->set_text_tool_active(type);
        });
        connect(tools_sidebar_, &ToolsSidebar::color_picker_tool_requested, this, [this]() {
            if (canvas_) canvas_->set_color_picker_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::gradient_tool_requested, this, [this]() {
            if (canvas_) canvas_->set_gradient_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::foreground_color_requested, this, [this]() {
            if (title_ && !sel_layer_id_.empty()) {
                if (props_) props_->open_foreground_color_selector();
            } else {
                open_default_sidebar_color_popup(true);
            }
        });
        connect(tools_sidebar_, &ToolsSidebar::background_color_requested, this, [this]() {
            if (title_ && !sel_layer_id_.empty()) {
                if (props_) props_->open_background_color_selector();
            } else {
                open_default_sidebar_color_popup(false);
            }
        });
        connect(tools_sidebar_, &ToolsSidebar::foreground_background_swap_requested, this, [this]() {
            if (title_ && !sel_layer_id_.empty()) {
                if (props_) props_->swap_foreground_background_colors();
                auto layer = title_->find_layer(sel_layer_id_);
                if (layer) set_default_sidebar_colors_from_layer(*layer);
                update_layer_panels(layer, playhead_);
            } else {
                std::swap(default_foreground_color_, default_background_color_);
                save_sidebar_default_colors();
                update_sidebar_color_swatches(nullptr);
            }
        });
    }
}

void TitleEditor::align_selected_to_canvas(int x_mode, int y_mode)
{
    if (!title_ || sel_layer_id_.empty()) return;
    auto ids = layers_ ? layers_->selected_ids() : std::vector<std::string>{sel_layer_id_};
    std::shared_ptr<Layer> last_layer;
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer) continue;
        double lt = std::clamp(playhead_ - layer->in_time, 0.0, std::max(0.0, layer->out_time - layer->in_time));
        double w = eval_box_width(*layer, lt);
        double h = eval_box_height(*layer, lt);
        double x = layer->origin_x * w;
        if (x_mode == 1) x = title_->width / 2.0;
        if (x_mode == 2) x = title_->width - (1.0 - layer->origin_x) * w;
        double y = layer->origin_y * h;
        if (y_mode == 1) y = title_->height / 2.0;
        if (y_mode == 2) y = title_->height - (1.0 - layer->origin_y) * h;
        set_animated_value(layer->pos_x, lt, x);
        set_animated_value(layer->pos_y, lt, y);
        last_layer = layer;
    }
    on_title_modified();
    if (last_layer) update_layer_panels(last_layer, playhead_);
}


void TitleEditor::flip_selected_layers(bool horizontal)
{
    if (!title_ || sel_layer_id_.empty()) return;
    auto ids = layers_ ? layers_->selected_ids() : std::vector<std::string>{sel_layer_id_};
    if (ids.empty()) return;

    std::shared_ptr<Layer> last_layer;
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;
        const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                     std::max(0.0, layer->out_time - layer->in_time));
        AnimatedProperty &prop = horizontal ? layer->scale_x : layer->scale_y;
        const double current = prop.evaluate(lt);
        set_animated_value(prop, lt, -current);
        last_layer = layer;
    }

    if (!last_layer) return;
    on_title_modified();
    update_layer_panels(last_layer, playhead_);
}

void TitleEditor::rotate_selected_layers(double degrees)
{
    if (!title_ || sel_layer_id_.empty()) return;
    auto ids = layers_ ? layers_->selected_ids() : std::vector<std::string>{sel_layer_id_};
    if (ids.empty()) return;

    std::shared_ptr<Layer> last_layer;
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;
        const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                     std::max(0.0, layer->out_time - layer->in_time));
        set_animated_value(layer->rotation, lt, layer->rotation.evaluate(lt) + degrees);
        last_layer = layer;
    }

    if (!last_layer) return;
    on_title_modified();
    update_layer_panels(last_layer, playhead_);
}

void TitleEditor::align_selected_layers_horizontal()
{
    align_selected_layers(1, -1);
}

void TitleEditor::align_selected_layers_vertical()
{
    align_selected_layers(-1, 1);
}

void TitleEditor::align_selected_layers(int x_mode, int y_mode)
{
    if (!title_ || sel_layer_id_.empty()) return;
    auto ids = layers_ ? layers_->selected_ids() : std::vector<std::string>{sel_layer_id_};
    if (ids.empty()) return;

    struct Entry {
        std::shared_ptr<Layer> layer;
        double lt;
        double width;
        double height;
        double scale_x;
        double scale_y;
    };

    std::vector<Entry> entries;
    double min_left = std::numeric_limits<double>::infinity();
    double max_right = -std::numeric_limits<double>::infinity();
    double min_top = std::numeric_limits<double>::infinity();
    double max_bottom = -std::numeric_limits<double>::infinity();

    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;
        double lt = std::clamp(playhead_ - layer->in_time, 0.0, std::max(0.0, layer->out_time - layer->in_time));
        double width = eval_box_width(*layer, lt);
        double height = eval_box_height(*layer, lt);
        double sx = layer->scale_x.evaluate(lt);
        double sy = layer->scale_y.evaluate(lt);
        double x0 = layer->pos_x.evaluate(lt) - layer->origin_x * width * sx;
        double x1 = layer->pos_x.evaluate(lt) + (1.0 - layer->origin_x) * width * sx;
        double y0 = layer->pos_y.evaluate(lt) - layer->origin_y * height * sy;
        double y1 = layer->pos_y.evaluate(lt) + (1.0 - layer->origin_y) * height * sy;
        double left = std::min(x0, x1);
        double right = std::max(x0, x1);
        double top = std::min(y0, y1);
        double bottom = std::max(y0, y1);
        min_left = std::min(min_left, left);
        max_right = std::max(max_right, right);
        min_top = std::min(min_top, top);
        max_bottom = std::max(max_bottom, bottom);
        entries.push_back({layer, lt, width, height, sx, sy});
    }

    if (entries.empty()) return;
    if (alignment_target_ == 0 && entries.size() < 2) return;

    double target_left = min_left;
    double target_hcenter = (min_left + max_right) / 2.0;
    double target_right = max_right;
    double target_top = min_top;
    double target_vcenter = (min_top + max_bottom) / 2.0;
    double target_bottom = max_bottom;

    if (alignment_target_ == 1 || alignment_target_ == 2) {
        const double safe_inset = alignment_target_ == 1 ? OBS_GRAPHICS_SAFE_PERCENT : OBS_ACTION_SAFE_PERCENT;
        target_left = title_->width * safe_inset;
        target_hcenter = title_->width / 2.0;
        target_right = title_->width * (1.0 - safe_inset);
        target_top = title_->height * safe_inset;
        target_vcenter = title_->height / 2.0;
        target_bottom = title_->height * (1.0 - safe_inset);
    } else if (alignment_target_ == 3) {
        target_left = 0.0;
        target_hcenter = title_->width / 2.0;
        target_right = title_->width;
        target_top = 0.0;
        target_vcenter = title_->height / 2.0;
        target_bottom = title_->height;
    }

    std::shared_ptr<Layer> last_layer;
    for (const auto &entry : entries) {
        if (x_mode >= 0) {
            const double x0 = -entry.layer->origin_x * entry.width * entry.scale_x;
            const double x1 = (1.0 - entry.layer->origin_x) * entry.width * entry.scale_x;
            const double left_offset = std::min(x0, x1);
            const double right_offset = std::max(x0, x1);
            double next_x = entry.layer->pos_x.evaluate(entry.lt);
            if (x_mode == 0) next_x = target_left - left_offset;
            if (x_mode == 1) next_x = target_hcenter - (left_offset + right_offset) / 2.0;
            if (x_mode == 2) next_x = target_right - right_offset;
            set_animated_value(entry.layer->pos_x, entry.lt, next_x);
        }
        if (y_mode >= 0) {
            const double y0 = -entry.layer->origin_y * entry.height * entry.scale_y;
            const double y1 = (1.0 - entry.layer->origin_y) * entry.height * entry.scale_y;
            const double top_offset = std::min(y0, y1);
            const double bottom_offset = std::max(y0, y1);
            double next_y = entry.layer->pos_y.evaluate(entry.lt);
            if (y_mode == 0) next_y = target_top - top_offset;
            if (y_mode == 1) next_y = target_vcenter - (top_offset + bottom_offset) / 2.0;
            if (y_mode == 2) next_y = target_bottom - bottom_offset;
            set_animated_value(entry.layer->pos_y, entry.lt, next_y);
        }
        last_layer = entry.layer;
    }
    on_title_modified();
    if (last_layer) update_layer_panels(last_layer, playhead_);
}

void TitleEditor::build_toolbar()
{
    constexpr int kEditorToolbarIconSize = 18;
    constexpr int kEditorToolbarButtonExtent = 30;
    const QPalette pal = qApp->palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor button = pal.color(QPalette::Button);
    const QColor button_text = pal.color(QPalette::ButtonText);
    const QColor border = pal.color(QPalette::Mid);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlighted_text = pal.color(QPalette::HighlightedText);
    const QColor base = pal.color(QPalette::Base);
    const QColor hover = button.lightness() < 128 ? button.lighter(125) : button.darker(108);

    toolbar_ = new QToolBar(this);
    toolbar_->setMovable(false);
    toolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar_->setIconSize(QSize(kEditorToolbarIconSize, kEditorToolbarIconSize));
    toolbar_->setStyleSheet(QStringLiteral(
        "QToolBar{background:%1;border-bottom:1px solid %2;spacing:2px;}"
        "QToolButton{color:%3;background:transparent;min-width:%4px;min-height:%4px;max-width:%4px;max-height:%4px;padding:0;border:none;}"
        "QToolButton:hover{background:%5;border-radius:3px;}"
        "QToolButton:pressed{background:%6;color:%7;border-radius:3px;}"
        "QToolButton:checked{background:%6;color:%7;border-radius:3px;}"
        "QToolButton[toolButtonStyle=\"1\"]{min-width:auto;max-width:none;padding:2px 8px;}"
        "QDoubleSpinBox{color:%8;background:%9;border:1px solid %2;border-radius:3px;padding:2px 4px;}"
        "QDoubleSpinBox::up-button,QDoubleSpinBox::down-button{width:0;border:none;}")
        .arg(window.name(QColor::HexRgb),
             border.name(QColor::HexRgb),
             button_text.name(QColor::HexRgb),
             QString::number(kEditorToolbarButtonExtent),
             hover.name(QColor::HexRgb),
             highlight.name(QColor::HexRgb),
             highlighted_text.name(QColor::HexRgb),
             text.name(QColor::HexRgb),
             base.name(QColor::HexRgb)));

    act_rew_ = new QAction(obs_icon("rewind.svg"), obsgs_tr("OBSTitles.Rewind"), this);
    act_prev_kf_ = new QAction(obs_icon("previous-keyframe.svg"), obsgs_tr("OBSTitles.PreviousKeyframe"), this);
    act_play_ = new QAction(obs_icon("play.svg"), obsgs_tr("OBSTitles.Play"), this);
    act_full_loop_ = new QAction(obs_icon("loop.svg"), obsgs_tr("OBSTitles.LoopPreview"), this);
    act_full_loop_->setToolTip(obsgs_tr("OBSTitles.LoopPreviewTooltip"));
    act_next_kf_ = new QAction(obs_icon("next-keyframe.svg"), obsgs_tr("OBSTitles.NextKeyframe"), this);

    connect(act_rew_, &QAction::triggered, this, &TitleEditor::rewind);
    connect(act_prev_kf_, &QAction::triggered, this, &TitleEditor::previous_keyframe);
    connect(act_play_, &QAction::triggered, this, &TitleEditor::play_pause);
    connect(act_full_loop_, &QAction::triggered, this, &TitleEditor::play_full_loop);
    connect(act_next_kf_, &QAction::triggered, this, &TitleEditor::next_keyframe);

    toolbar_->addSeparator();
    auto *align_target = new QToolButton(toolbar_);
    align_target->setIcon(obs_icon("alignment-target.svg"));
    align_target->setIconSize(toolbar_->iconSize());
    align_target->setToolButtonStyle(Qt::ToolButtonIconOnly);
    align_target->setToolTip(obsgs_tr("OBSTitles.AlignmentTarget"));
    align_target->setAccessibleName(obsgs_tr("OBSTitles.AlignmentTarget"));
    align_target->setPopupMode(QToolButton::InstantPopup);
    align_target->setFixedSize(kEditorToolbarButtonExtent, kEditorToolbarButtonExtent);
    align_target->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;}"));
    auto *align_menu = new QMenu(align_target);
    QAction *target_selection = align_menu->addAction(obsgs_tr("OBSTitles.AlignToSelection"));
    QAction *target_title_safe = align_menu->addAction(obsgs_tr("OBSTitles.AlignToTitleSafeGuides"));
    QAction *target_action_safe = align_menu->addAction(obsgs_tr("OBSTitles.AlignToActionSafeGuides"));
    QAction *target_artboard = align_menu->addAction(obsgs_tr("OBSTitles.AlignToArtboard"));
    target_selection->setCheckable(true);
    target_title_safe->setCheckable(true);
    target_action_safe->setCheckable(true);
    target_artboard->setCheckable(true);
    target_artboard->setChecked(true);
    auto update_alignment_target = [this, align_target, target_selection, target_title_safe, target_action_safe, target_artboard](int target) {
        alignment_target_ = target;
        target_selection->setChecked(target == 0);
        target_title_safe->setChecked(target == 1);
        target_action_safe->setChecked(target == 2);
        target_artboard->setChecked(target == 3);
        QString tooltip = obsgs_tr("OBSTitles.AlignToArtboard");
        if (target == 0)
            tooltip = obsgs_tr("OBSTitles.AlignToSelection");
        else if (target == 1)
            tooltip = obsgs_tr("OBSTitles.AlignToTitleSafeGuides");
        else if (target == 2)
            tooltip = obsgs_tr("OBSTitles.AlignToActionSafeGuides");
        align_target->setToolTip(tooltip);
    };
    connect(target_selection, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(0); });
    connect(target_title_safe, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(1); });
    connect(target_action_safe, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(2); });
    connect(target_artboard, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(3); });
    align_target->setMenu(align_menu);
    toolbar_->addWidget(align_target);

    auto add_align_action = [this](const char *icon_name, const QString &tip, int x_mode, int y_mode) {
        QAction *action = toolbar_->addAction(obs_icon(icon_name), tip);
        action->setToolTip(tip);
        connect(action, &QAction::triggered, this, [this, x_mode, y_mode]() {
            align_selected_layers(x_mode, y_mode);
        });
        return action;
    };
    add_align_action("align-left.svg", obsgs_tr("OBSTitles.AlignLeft"), 0, -1);
    add_align_action("align-horizontal-center.svg", obsgs_tr("OBSTitles.AlignHorizontalCenter"), 1, -1);
    add_align_action("align-right.svg", obsgs_tr("OBSTitles.AlignRight"), 2, -1);
    add_align_action("align-top.svg", obsgs_tr("OBSTitles.AlignTop"), -1, 0);
    add_align_action("align-vertical-center.svg", obsgs_tr("OBSTitles.AlignVerticalCenter"), -1, 1);
    add_align_action("align-bottom.svg", obsgs_tr("OBSTitles.AlignBottom"), -1, 2);
    add_align_action("align-center-artboard.svg", obsgs_tr("OBSTitles.AlignCenterToArtboard"), 1, 1);

    toolbar_->addSeparator();
    auto add_flip_action = [this](const char *icon_name, const QString &text, bool horizontal) {
        QAction *action = toolbar_->addAction(obs_icon(icon_name), text);
        action->setToolTip(text);
        connect(action, &QAction::triggered, this, [this, horizontal]() {
            flip_selected_layers(horizontal);
        });
        return action;
    };
    add_flip_action("flip-horizontal.svg", obsgs_tr("OBSTitles.FlipHorizontal"), true);
    add_flip_action("flip-vertical.svg", obsgs_tr("OBSTitles.FlipVertical"), false);

    toolbar_->addSeparator();
    auto *rotation_degrees = new QDoubleSpinBox(toolbar_);
    rotation_degrees->setRange(-9999.0, 9999.0);
    rotation_degrees->setDecimals(1);
    rotation_degrees->setSingleStep(1.0);
    rotation_degrees->setValue(90.0);
    rotation_degrees->setSuffix(QStringLiteral("°"));
    rotation_degrees->setToolTip(obsgs_tr("OBSTitles.RotateDegreesTooltip"));
    rotation_degrees->setAccessibleName(obsgs_tr("OBSTitles.RotateDegrees"));
    rotation_degrees->setFixedWidth(78);
    toolbar_->addWidget(rotation_degrees);
    auto add_rotate_action = [this, rotation_degrees](const char *icon_name, const QString &text, double direction) {
        QAction *action = toolbar_->addAction(obs_icon(icon_name), text);
        action->setToolTip(text);
        connect(action, &QAction::triggered, this, [this, rotation_degrees, direction]() {
            rotate_selected_layers(rotation_degrees->value() * direction);
        });
        return action;
    };
    add_rotate_action("rotate-left.svg", obsgs_tr("OBSTitles.RotateLeft"), -1.0);
    add_rotate_action("rotate-right.svg", obsgs_tr("OBSTitles.RotateRight"), 1.0);

    act_safe_guides_ = new QAction(obs_icon("safe.svg"), obsgs_tr("OBSTitles.Safe"), this);
    act_safe_guides_->setCheckable(true);
    act_safe_guides_->setToolTip(obsgs_tr("OBSTitles.SafeTooltip"));
    connect(act_safe_guides_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_safe_guides_visible(visible);
    });

    toolbar_->addSeparator();
    toolbar_->addAction(act_undo_);
    toolbar_->addAction(act_redo_);
    update_undo_redo_actions();

    auto *toolbar_spacer = new QWidget(toolbar_);
    toolbar_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar_->addWidget(toolbar_spacer);

    act_live_editing_ = new QAction(obsgs_tr("OBSTitles.LiveEditing"), this);
    act_live_editing_->setCheckable(true);
    act_live_editing_->setToolTip(obsgs_tr("OBSTitles.LiveEditingTooltip"));
    connect(act_live_editing_, &QAction::toggled, this, &TitleEditor::set_live_editing_enabled);

    auto *live_editing_button = new QToolButton(toolbar_);
    live_editing_button->setDefaultAction(act_live_editing_);
    live_editing_button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    live_editing_button->setStyleSheet(QStringLiteral(
        "QToolButton{color:%1;background:transparent;min-width:0;max-width:none;min-height:%2px;max-height:%2px;padding:2px 8px;border:none;}"
        "QToolButton:hover{background:%3;border-radius:3px;}"
        "QToolButton:checked{background:%4;color:%5;border-radius:3px;}")
        .arg(button_text.name(QColor::HexRgb),
             QString::number(kEditorToolbarButtonExtent),
             hover.name(QColor::HexRgb),
             highlight.name(QColor::HexRgb),
             highlighted_text.name(QColor::HexRgb)));
    toolbar_->addWidget(live_editing_button);

}


static QString editor_template_library_root_path()
{
    char *path = obs_module_config_path("template-library");
    QString root = path ? QString::fromUtf8(path) : QDir::homePath();
    if (path) bfree(path);
    QDir().mkpath(root);
    return root;
}

static int editor_dialog_layout_spacing(QWidget *widget)
{
    const int spacing = widget->style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing, nullptr, widget);
    return spacing > 0 ? spacing : 6;
}

static QStringList editor_template_library_category_paths(const QString &root_path)
{
    QStringList categories;
    QDirIterator it(root_path, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString rel = QDir(root_path).relativeFilePath(it.filePath());
        if (!rel.isEmpty()) categories << rel;
    }
    if (categories.isEmpty()) categories << QStringLiteral("Custom");
    categories.sort(Qt::CaseInsensitive);
    return categories;
}

static QString editor_sanitized_template_category_path(const QString &category)
{
    QString cleaned = QDir::cleanPath(category.trimmed());
    cleaned.replace(QRegularExpression(QStringLiteral("[\\\\:*?\"<>|]")), QStringLiteral("_"));
    while (cleaned.startsWith(QStringLiteral("../")))
        cleaned.remove(0, 3);
    if (cleaned == QStringLiteral(".") || cleaned == QStringLiteral(".."))
        cleaned.clear();
    return cleaned;
}

static bool prompt_editor_template_library_category(QWidget *parent, QString &category)
{
    const QString root_path = editor_template_library_root_path();
    QDialog dialog(parent);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.TemplateLibraryCategoryTitle"));
    dialog.setModal(true);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(editor_dialog_layout_spacing(&dialog));

    auto *prompt = new QLabel(obsgs_tr("OBSTitles.TemplateLibraryCategoryPrompt"), &dialog);
    prompt->setWordWrap(true);
    layout->addWidget(prompt);

    auto *combo = new QComboBox(&dialog);
    combo->setEditable(true);
    combo->addItems(editor_template_library_category_paths(root_path));
    layout->addWidget(combo);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        const QString safe_category = editor_sanitized_template_category_path(combo->currentText());
        if (safe_category.isEmpty()) {
            QMessageBox::warning(&dialog, obsgs_tr("OBSTitles.TemplateLibraryCategoryTitle"),
                                 obsgs_tr("OBSTitles.TemplateLibraryCategoryRequired"));
            return;
        }
        category = safe_category;
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    return dialog.exec() == QDialog::Accepted;
}

static bool prompt_editor_template_metadata(QWidget *parent, const Title &title,
                                           TitleTemplateExportMetadata &metadata,
                                           const QString &window_title = obsgs_tr("OBSTitles.ExportTemplateDetails"))
{
    if (metadata.title.empty()) metadata.title = title.name;
    if (metadata.description.empty()) metadata.description = title.description;
    if (metadata.creator.empty()) metadata.creator = title.creator;
    if (metadata.creation_date.empty()) {
        metadata.creation_date = title.creation_date.empty()
            ? QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString()
            : title.creation_date;
    }
    if (metadata.screenshot_png_base64.empty())
        metadata.screenshot_png_base64 = title.preview_screenshot_png_base64;

    QDialog dialog(parent);
    dialog.setWindowTitle(window_title);
    dialog.setModal(true);
    dialog.resize(560, 500);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(editor_dialog_layout_spacing(&dialog));

    auto *preview_label = new QLabel(obsgs_tr("OBSTitles.TemplateScreenshotPreviewLabel"), &dialog);
    QFont label_font = preview_label->font();
    label_font.setBold(true);
    preview_label->setFont(label_font);
    layout->addWidget(preview_label);

    auto *preview = new QLabel(&dialog);
    preview->setAlignment(Qt::AlignCenter);
    preview->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    preview->setMinimumHeight(160);
    QPixmap pixmap;
    const QByteArray png = QByteArray::fromBase64(QByteArray::fromStdString(metadata.screenshot_png_base64));
    if (!png.isEmpty() && pixmap.loadFromData(png, "PNG"))
        preview->setPixmap(pixmap.scaled(QSize(480, 180), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    else
        preview->setText(obsgs_tr("OBSTitles.TemplateScreenshotFailed"));
    layout->addWidget(preview);

    auto *form = new QFormLayout();
    auto *title_edit = new QLineEdit(QString::fromStdString(metadata.title), &dialog);
    auto *description_edit = new QTextEdit(&dialog);
    description_edit->setAcceptRichText(false);
    description_edit->setPlainText(QString::fromStdString(metadata.description));
    description_edit->setMinimumHeight(96);
    auto *creator_edit = new QLineEdit(QString::fromStdString(metadata.creator), &dialog);
    auto *date_edit = new QLineEdit(QString::fromStdString(metadata.creation_date), &dialog);

    form->addRow(obsgs_tr("OBSTitles.TemplateExportTitleLabel"), title_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateExportDescriptionLabel"), description_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateExportCreatorLabel"), creator_edit);
    form->addRow(obsgs_tr("OBSTitles.TemplateCreationDateLabel"), date_edit);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        if (title_edit->text().trimmed().isEmpty()) {
            QMessageBox::warning(&dialog, obsgs_tr("OBSTitles.ExportTemplateDetails"),
                                 obsgs_tr("OBSTitles.TemplateExportTitleRequired"));
            return;
        }
        dialog.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
        return false;

    metadata.title = title_edit->text().trimmed().toStdString();
    metadata.description = description_edit->toPlainText().trimmed().toStdString();
    metadata.creator = creator_edit->text().trimmed().toStdString();
    metadata.creation_date = date_edit->text().trimmed().toStdString();
    return true;
}

void TitleEditor::copy_title_to_store(const std::shared_ptr<Title> &source,
                                      const std::shared_ptr<Title> &dest) const
{
    if (!source || !dest) return;
    const std::string dest_id = dest->id;
    *dest = *source;
    dest->id = dest_id;
    dest->layers.clear();
    dest->layers.reserve(source->layers.size());
    for (const auto &layer : source->layers) {
        if (layer) dest->layers.push_back(std::make_shared<Layer>(*layer));
    }
}

void TitleEditor::new_title_contents()
{
    if (!title_) return;
    if (QMessageBox::question(this, obsgs_tr("OBSTitles.New"),
                              obsgs_tr("OBSTitles.NewTitleConfirm")) != QMessageBox::Yes)
        return;

    title_->layers.clear();
    sel_layer_id_.clear();
    layers_->refresh();
    canvas_->set_selected_layers({});
    update_layer_panels(nullptr, playhead_);
    on_title_modified();
}

bool TitleEditor::persist_title_changes(bool update_preview_screenshot, bool show_saved_status)
{
    if (!title_) return false;
    auto stored = TitleDataStore::instance().get_title(editing_title_id_.empty() ? title_->id : editing_title_id_);
    if (!stored) {
        stored = TitleDataStore::instance().create_title(title_->name);
        editing_title_id_ = stored->id;
        title_->id = stored->id;
    }
    copy_title_to_store(title_, stored);
    if (update_preview_screenshot) {
        title_->preview_screenshot_png_base64 = title_manual_screenshot_png_base64(*title_);
        stored->preview_screenshot_png_base64 = title_->preview_screenshot_png_base64;
    }
    TitleDataStore::instance().notify_change();
    TitleDataStore::instance().save();
    emit title_saved(stored->id);
    set_dirty(false);
    if (show_saved_status)
        setWindowTitle(obsgs_tr("OBSTitles.EditorSavedTitle"));
    return true;
}

bool TitleEditor::save_title()
{
    return persist_title_changes(true, true);
}

void TitleEditor::set_live_editing_enabled(bool enabled)
{
    live_editing_ = enabled;
    if (act_live_editing_ && act_live_editing_->isChecked() != enabled) {
        QSignalBlocker blocker(act_live_editing_);
        act_live_editing_->setChecked(enabled);
    }
    if (live_editing_ && dirty_)
        save_live_edit();
}

void TitleEditor::set_gpu_pipeline_enabled(bool enabled)
{
    if (TitlePreferences::use_gpu() == enabled)
        return;

    TitlePreferences::set_use_gpu(enabled);
    TitlePreferences::notify_changed(this);
    update_title_bar();
    if (canvas_)
        canvas_->refresh_preview();
}

void TitleEditor::save_live_edit()
{
    if (!live_editing_ || !title_) return;
    persist_title_changes(false, false);
}

void TitleEditor::save_title_as_new()
{
    if (!title_) return;

    Title temp = *title_;
    temp.preview_screenshot_png_base64 = title_manual_screenshot_png_base64(temp);
    TitleTemplateExportMetadata metadata;
    metadata.screenshot_png_base64 = temp.preview_screenshot_png_base64;
    if (!prompt_editor_template_metadata(this, temp, metadata, obsgs_tr("OBSTitles.SaveAsNew")))
        return;

    auto created = TitleDataStore::instance().create_title(metadata.title);
    title_->name = metadata.title;
    title_->description = metadata.description;
    title_->creator = metadata.creator;
    title_->creation_date = metadata.creation_date;
    title_->preview_screenshot_png_base64 = metadata.screenshot_png_base64;
    copy_title_to_store(title_, created);
    created->name = metadata.title;
    created->description = metadata.description;
    created->creator = metadata.creator;
    created->creation_date = metadata.creation_date;
    created->preview_screenshot_png_base64 = metadata.screenshot_png_base64;
    editing_title_id_ = created->id;
    title_->id = created->id;
    update_title_bar();
    TitleDataStore::instance().notify_change();
    TitleDataStore::instance().save();
    emit title_saved(created->id);
    set_dirty(false);
    setWindowTitle(obsgs_tr("OBSTitles.EditorSavedTitle"));
}

void TitleEditor::export_title_template(bool save_in_library)
{
    if (!title_) return;

    Title temp = *title_;
    temp.preview_screenshot_png_base64 = title_manual_screenshot_png_base64(temp);
    TitleTemplateExportMetadata metadata;
    metadata.screenshot_png_base64 = temp.preview_screenshot_png_base64;
    if (!prompt_editor_template_metadata(this, temp, metadata))
        return;

    QString safe_name = QString::fromStdString(metadata.title).trimmed();
    if (safe_name.isEmpty()) safe_name = obsgs_tr("OBSTitles.TemplateFileDialogTitle");
    safe_name.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));

    QString path;
    if (save_in_library) {
        QString category;
        if (!prompt_editor_template_library_category(this, category))
            return;
        QDir root(editor_template_library_root_path());
        root.mkpath(category);
        path = root.filePath(QStringLiteral("%1/%2.ogspt").arg(category, safe_name));
    } else {
        path = QFileDialog::getSaveFileName(this, obsgs_tr("OBSTitles.ExportTitleTemplate"),
                                            QDir(editor_template_library_root_path()).filePath(safe_name + QStringLiteral(".ogspt")),
                                            obsgs_tr("OBSTitles.TemplateFileFilter"));
        if (path.isEmpty()) return;
        if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".ogspt");
    }

    auto stored = TitleDataStore::instance().create_title(metadata.title);
    copy_title_to_store(title_, stored);
    stored->name = metadata.title;
    stored->description = metadata.description;
    stored->creator = metadata.creator;
    stored->creation_date = metadata.creation_date;
    stored->preview_screenshot_png_base64 = metadata.screenshot_png_base64;

    std::string error;
    if (!TitleDataStore::instance().export_title(stored->id, path.toStdString(), metadata, &error)) {
        QMessageBox::warning(this, obsgs_tr("OBSTitles.ExportTitleTemplate"), QString::fromStdString(error));
    } else {
        QMessageBox::information(this, obsgs_tr("OBSTitles.ExportTitleTemplate"),
                                 obsgs_tr("OBSTitles.ExportedStatusFormat").arg(QFileInfo(path).fileName()));
    }
    TitleDataStore::instance().delete_title(stored->id);
}

/* ── open_title ──────────────────────────────────────────────────── */
void TitleEditor::open_title(const std::string &tid)
{
    play_timer_->stop();
    playing_ = false;
    act_play_->setText("▶");
    act_play_->setIcon(obs_icon("play.svg"));
    playhead_ = 0.0;
    playback_reverse_ = false;
    full_loop_playback_ = false;

    auto stored_title = TitleDataStore::instance().get_title(tid);
    if (!stored_title) return;
    editing_title_id_ = tid;
    title_ = clone_title(*stored_title);

    update_title_bar();
    canvas_->set_title(title_);
    layers_->set_title(title_);
    layers_->set_layer_clipboard_available(!layer_clipboard_.empty());
    timeline_->set_title(title_);
    props_->set_title(title_);
    title_props_->set_title(title_);

    if (!title_->layers.empty())
        on_layer_selected(title_->layers.back()->id);
    else
        update_layer_panels(nullptr, playhead_);

    QTimer::singleShot(0, timeline_, [this]() {
        if (timeline_) timeline_->fit_timeline();
    });

    undo_stack_.clear();
    undo_index_ = -1;
    push_undo_snapshot();
    update_undo_redo_actions();

    on_playhead_changed(0.0);
    set_dirty(false);
}

std::shared_ptr<Title> TitleEditor::clone_title(const Title &title) const
{
    auto clone = std::make_shared<Title>(title);
    clone->layers.clear();
    clone->layers.reserve(title.layers.size());
    for (const auto &layer : title.layers) {
        if (layer) clone->layers.push_back(std::make_shared<Layer>(*layer));
    }
    return clone;
}


std::shared_ptr<Layer> TitleEditor::clone_layer_for_insert(const Layer &layer, bool suffix_name) const
{
    auto clone = std::make_shared<Layer>(layer);
    clone->id = TitleDataStore::make_uuid();
    if (suffix_name)
        clone->name = clone->name.empty() ? editor_text_std("OBSTitles.LayerCopy") : clone->name + editor_text_std("OBSTitles.CopySuffix");
    if (!clone->parent_id.empty() && (!title_ || !title_->find_layer(clone->parent_id)))
        clone->parent_id.clear();
    if (!clone->mask_source_id.empty() && (!title_ || !title_->find_layer(clone->mask_source_id))) {
        clone->mask_source_id.clear();
        clone->mask_mode = MaskMode::None;
    }
    return clone;
}

void TitleEditor::insert_layer_above(const std::string &anchor_id, std::shared_ptr<Layer> layer)
{
    if (!title_ || !layer) return;

    auto it = std::find_if(title_->layers.begin(), title_->layers.end(),
                           [&](const auto &candidate) {
                               return candidate && candidate->id == anchor_id;
                           });
    if (it == title_->layers.end())
        title_->layers.push_back(layer);
    else
        title_->layers.insert(it + 1, layer);
}

void TitleEditor::select_after_layer_list_mutation(const std::string &layer_id)
{
    layers_->refresh();
    timeline_->set_title(title_);
    on_layer_selected(layer_id);
}


std::vector<std::string> TitleEditor::selected_layer_ids_for_operation() const
{
    std::vector<std::string> requested = layers_ ? layers_->selected_ids() : std::vector<std::string>{};
    if (requested.empty() && !sel_layer_id_.empty())
        requested.push_back(sel_layer_id_);

    std::set<std::string> requested_set(requested.begin(), requested.end());
    std::vector<std::string> ordered_ids;
    if (!title_ || requested_set.empty()) return ordered_ids;

    for (const auto &layer : title_->layers) {
        if (layer && requested_set.find(layer->id) != requested_set.end())
            ordered_ids.push_back(layer->id);
    }
    return ordered_ids;
}

std::vector<std::shared_ptr<Layer>> TitleEditor::clone_layers_for_insert(const std::vector<std::shared_ptr<Layer>> &layers,
                                                                         bool suffix_name) const
{
    std::map<std::string, std::string> cloned_ids_by_original;
    std::vector<std::shared_ptr<Layer>> clones;
    clones.reserve(layers.size());

    for (const auto &layer : layers) {
        if (!layer) continue;
        auto clone = std::make_shared<Layer>(*layer);
        clone->id = TitleDataStore::make_uuid();
        if (suffix_name)
            clone->name = clone->name.empty() ? editor_text_std("OBSTitles.LayerCopy")
                                              : clone->name + editor_text_std("OBSTitles.CopySuffix");
        cloned_ids_by_original[layer->id] = clone->id;
        clones.push_back(clone);
    }

    for (auto &clone : clones) {
        auto parent_clone = cloned_ids_by_original.find(clone->parent_id);
        if (parent_clone != cloned_ids_by_original.end()) {
            clone->parent_id = parent_clone->second;
        } else if (!clone->parent_id.empty() && (!title_ || !title_->find_layer(clone->parent_id))) {
            clone->parent_id.clear();
        }

        auto mask_clone = cloned_ids_by_original.find(clone->mask_source_id);
        if (mask_clone != cloned_ids_by_original.end()) {
            clone->mask_source_id = mask_clone->second;
        } else if (!clone->mask_source_id.empty() && (!title_ || !title_->find_layer(clone->mask_source_id))) {
            clone->mask_source_id.clear();
            clone->mask_mode = MaskMode::None;
        }
    }

    return clones;
}

void TitleEditor::apply_picked_color_to_selection(const QColor &color)
{
    if (!title_ || !color.isValid()) return;

    const auto ids = selected_layer_ids_for_operation();
    if (ids.empty()) return;

    const uint32_t argb = argb_from_color(color);
    std::shared_ptr<Layer> last_changed;
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;

        const double local_time = std::clamp(playhead_ - layer->in_time, 0.0,
                                             std::max(0.0, layer->out_time - layer->in_time));
        if (is_canvas_text_layer(*layer)) {
            RichTextCharFormat fmt = layer_char_format_for_editor(*layer);
            fmt.fill.type = 0;
            fmt.fill.color = argb;
            apply_rich_text_format_to_layer_range(*layer, fmt, RichTextCharFillColor, false);
            set_color_channels_at(*layer, true, local_time, argb);
            last_changed = layer;
        } else if (layer->type == LayerType::Shape || layer->type == LayerType::SolidRect) {
            layer->fill_type = 0;
            layer->fill_color = argb;
            set_color_channels_at(*layer, false, local_time, argb);
            last_changed = layer;
        }
    }

    if (!last_changed) return;
    on_title_modified();
    update_layer_panels(last_changed, playhead_);
}

void TitleEditor::duplicate_selected_layers()
{
    if (!title_) return;
    const auto ids = selected_layer_ids_for_operation();
    if (ids.empty()) return;

    std::set<std::string> selected_ids(ids.begin(), ids.end());
    std::vector<std::shared_ptr<Layer>> originals;
    originals.reserve(ids.size());
    for (const auto &layer : title_->layers) {
        if (layer && selected_ids.find(layer->id) != selected_ids.end())
            originals.push_back(layer);
    }

    auto clones = clone_layers_for_insert(originals, true);
    if (clones.empty()) return;

    std::map<std::string, std::shared_ptr<Layer>> clones_by_original;
    for (size_t i = 0; i < originals.size() && i < clones.size(); ++i)
        clones_by_original[originals[i]->id] = clones[i];

    std::vector<std::shared_ptr<Layer>> next_layers;
    next_layers.reserve(title_->layers.size() + clones.size());
    for (const auto &layer : title_->layers) {
        next_layers.push_back(layer);
        if (!layer) continue;
        auto clone = clones_by_original.find(layer->id);
        if (clone != clones_by_original.end())
            next_layers.push_back(clone->second);
    }
    title_->layers = std::move(next_layers);

    std::vector<std::string> clone_ids;
    clone_ids.reserve(clones.size());
    for (const auto &clone : clones)
        if (clone) clone_ids.push_back(clone->id);

    sel_layer_id_ = clone_ids.empty() ? std::string() : clone_ids.back();
    layers_->refresh();
    layers_->set_selected_layers(clone_ids);
    canvas_->set_selected_layers(clone_ids);
    timeline_->set_title(title_);
    timeline_->set_selected_layer(sel_layer_id_);
    if (!sel_layer_id_.empty()) {
        if (auto layer = title_->find_layer(sel_layer_id_)) update_layer_panels(layer, playhead_);
    }
    on_title_modified();
}

void TitleEditor::copy_selected_layer()
{
    if (!title_) return;
    const auto ids = selected_layer_ids_for_operation();
    if (ids.empty()) return;

    layer_clipboard_.clear();
    layer_clipboard_.reserve(ids.size());
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (layer) layer_clipboard_.push_back(std::make_shared<Layer>(*layer));
    }
    if (layers_) layers_->set_layer_clipboard_available(!layer_clipboard_.empty());
}

void TitleEditor::paste_layer_from_clipboard()
{
    if (!title_ || layer_clipboard_.empty()) return;

    std::vector<std::shared_ptr<Layer>> clipboard_layers;
    clipboard_layers.reserve(layer_clipboard_.size());
    for (const auto &layer : layer_clipboard_)
        if (layer) clipboard_layers.push_back(layer);

    auto pasted_layers = clone_layers_for_insert(clipboard_layers, true);
    if (pasted_layers.empty()) return;

    auto insert_pos = title_->layers.end();
    if (!sel_layer_id_.empty()) {
        auto anchor = std::find_if(title_->layers.begin(), title_->layers.end(),
                                   [this](const auto &layer) { return layer && layer->id == sel_layer_id_; });
        if (anchor != title_->layers.end()) insert_pos = anchor + 1;
    }
    title_->layers.insert(insert_pos, pasted_layers.begin(), pasted_layers.end());

    std::vector<std::string> pasted_ids;
    pasted_ids.reserve(pasted_layers.size());
    for (const auto &layer : pasted_layers)
        if (layer) pasted_ids.push_back(layer->id);

    sel_layer_id_ = pasted_ids.empty() ? std::string() : pasted_ids.back();
    layers_->refresh();
    layers_->set_selected_layers(pasted_ids);
    canvas_->set_selected_layers(pasted_ids);
    timeline_->set_title(title_);
    timeline_->set_selected_layer(sel_layer_id_);
    if (!sel_layer_id_.empty()) {
        if (auto layer = title_->find_layer(sel_layer_id_)) update_layer_panels(layer, playhead_);
    }
    on_title_modified();
}

void TitleEditor::delete_selected_layer()
{
    if (!title_) return;
    const auto ids = selected_layer_ids_for_operation();
    if (ids.empty()) return;

    std::set<std::string> removed_ids(ids.begin(), ids.end());
    int first_removed_index = (int)title_->layers.size();
    std::vector<std::shared_ptr<Layer>> remaining;
    remaining.reserve(title_->layers.size());
    for (int i = 0; i < (int)title_->layers.size(); ++i) {
        auto &layer = title_->layers[(size_t)i];
        if (!layer || removed_ids.find(layer->id) != removed_ids.end()) {
            first_removed_index = std::min(first_removed_index, i);
            continue;
        }
        remaining.push_back(layer);
    }

    if (remaining.size() == title_->layers.size()) return;

    for (auto &layer : remaining) {
        if (!layer) continue;
        if (removed_ids.find(layer->parent_id) != removed_ids.end()) layer->parent_id.clear();
        if (removed_ids.find(layer->mask_source_id) != removed_ids.end()) {
            layer->mask_source_id.clear();
            layer->mask_mode = MaskMode::None;
        }
    }

    title_->layers = std::move(remaining);
    sel_layer_id_.clear();
    layers_->refresh();

    if (!title_->layers.empty()) {
        const int select_index = std::clamp(first_removed_index, 0, (int)title_->layers.size() - 1);
        on_layer_selected(title_->layers[(size_t)select_index]->id);
    } else {
        layers_->set_selected_layers({});
        canvas_->set_selected_layers({});
        timeline_->set_title(title_);
        timeline_->set_selected_layer(std::string());
        update_layer_panels(nullptr, playhead_);
    }

    on_title_modified();
}

void TitleEditor::cut_selected_layer()
{
    copy_selected_layer();
    delete_selected_layer();
}

void TitleEditor::push_undo_snapshot()
{
    if (!title_ || restoring_undo_) return;
    if (undo_index_ + 1 < (int)undo_stack_.size())
        undo_stack_.erase(undo_stack_.begin() + undo_index_ + 1, undo_stack_.end());
    undo_stack_.push_back(clone_title(*title_));
    if (undo_stack_.size() > 30)
        undo_stack_.erase(undo_stack_.begin());
    undo_index_ = (int)undo_stack_.size() - 1;
    update_undo_redo_actions();
}

void TitleEditor::restore_undo_snapshot(int index)
{
    if (!title_ || index < 0 || index >= (int)undo_stack_.size()) return;
    restoring_undo_ = true;
    auto snapshot = undo_stack_[(size_t)index];
    title_->name = snapshot->name;
    title_->description = snapshot->description;
    title_->creator = snapshot->creator;
    title_->creation_date = snapshot->creation_date;
    title_->duration = snapshot->duration;
    title_->loop_start = snapshot->loop_start;
    title_->loop_end = snapshot->loop_end;
    title_->playback_mode = snapshot->playback_mode;
    title_->loop_type = snapshot->loop_type;
    title_->pause_time = snapshot->pause_time;
    title_->bg_color = snapshot->bg_color;
    title_->width = snapshot->width;
    title_->height = snapshot->height;
    title_->live_text_rows = snapshot->live_text_rows;
    title_->live_text_column_order = snapshot->live_text_column_order;
    title_->live_text_header_state = snapshot->live_text_header_state;
    title_->external_data_enabled = snapshot->external_data_enabled;
    title_->current_cue_row = snapshot->current_cue_row;
    title_->pending_cue_row = snapshot->pending_cue_row;
    title_->cue_revision = snapshot->cue_revision;
    title_->layers.clear();
    title_->layers.reserve(snapshot->layers.size());
    for (const auto &layer : snapshot->layers) {
        if (layer) title_->layers.push_back(std::make_shared<Layer>(*layer));
    }
    undo_index_ = index;
    if (!sel_layer_id_.empty() && !title_->find_layer(sel_layer_id_))
        sel_layer_id_.clear();
    if (sel_layer_id_.empty() && !title_->layers.empty())
        sel_layer_id_ = title_->layers.back()->id;
    update_title_bar();
    canvas_->set_title(title_, true);
    layers_->set_title(title_);
    timeline_->set_title(title_);
    props_->set_title(title_);
    title_props_->set_title(title_);
    if (!sel_layer_id_.empty()) on_layer_selected(sel_layer_id_);
    else update_layer_panels(nullptr, playhead_);
    on_playhead_changed(std::clamp(playhead_, 0.0, title_->duration));
    restoring_undo_ = false;
    update_undo_redo_actions();
    set_dirty(true);
    save_live_edit();
}

void TitleEditor::update_undo_redo_actions()
{
    if (act_undo_) act_undo_->setEnabled(undo_index_ > 0);
    if (act_redo_) act_redo_->setEnabled(undo_index_ >= 0 && undo_index_ + 1 < (int)undo_stack_.size());
}

void TitleEditor::update_title_bar()
{
    if (title_ && title_lbl_)
        title_lbl_->setText(QString::fromStdString(title_->name));
    if (graphic_props_dock_) {
        graphic_props_dock_->setWindowTitle(title_
            ? QStringLiteral("Properties: %1").arg(QString::fromStdString(title_->name))
            : QStringLiteral("Properties"));
    }
    if (dirty_indicator_)
        dirty_indicator_->setVisible(dirty_);
    if (gpu_warning_lbl_) {
        const bool show_gpu_warning = TitlePreferences::use_gpu() && !TitlePreferences::gpu_available();
        gpu_warning_lbl_->setVisible(show_gpu_warning);
        gpu_warning_lbl_->setText(show_gpu_warning ? obsgs_tr("OBSTitles.GPUFallbackWarning") : QString());
        gpu_warning_lbl_->setToolTip(show_gpu_warning
            ? QString::fromUtf8(TitlePreferences::gpu_unavailable_reason())
            : QString());
    }
}

void TitleEditor::begin_title_name_edit()
{
    if (!title_ || !title_lbl_ || !title_name_edit_)
        return;

    title_name_edit_->setText(QString::fromStdString(title_->name));
    title_lbl_->hide();
    title_name_edit_->show();
    title_name_edit_->setFocus(Qt::MouseFocusReason);
    title_name_edit_->selectAll();
}

void TitleEditor::commit_title_name_edit(bool accept)
{
    if (!title_name_edit_ || !title_name_edit_->isVisible())
        return;

    const QString next_name = title_name_edit_->text().trimmed();
    title_name_edit_->hide();
    if (title_lbl_)
        title_lbl_->show();

    if (accept && title_ && !next_name.isEmpty() && next_name.toStdString() != title_->name) {
        title_->name = next_name.toStdString();
        update_title_bar();
        if (title_props_)
            title_props_->set_title(title_);
        set_dirty(true);
        push_undo_snapshot();
        save_live_edit();
    } else {
        update_title_bar();
    }
}

void TitleEditor::set_dirty(bool dirty)
{
    dirty_ = dirty;
    if (dirty_indicator_)
        dirty_indicator_->setVisible(dirty_);
    update_title_bar();
    setWindowTitle(obsgs_tr(dirty_ ? "OBSTitles.EditorModifiedTitle" : "OBSTitles.EditorWindowTitle"));
}

bool TitleEditor::confirm_save_before_close()
{
    if (!dirty_)
        return true;

    QMessageBox dialog(this);
    dialog.setIcon(QMessageBox::Warning);
    dialog.setWindowTitle(obsgs_tr("OBSTitles.UnsavedChangesTitle"));
    dialog.setText(obsgs_tr("OBSTitles.UnsavedChangesPrompt"));
    dialog.setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
    dialog.setDefaultButton(QMessageBox::Yes);
    dialog.setEscapeButton(QMessageBox::Cancel);

    const auto result = static_cast<QMessageBox::StandardButton>(dialog.exec());
    if (result == QMessageBox::Yes)
        return save_title();
    if (result == QMessageBox::No)
        return true;
    return false;
}

/* ── Transport ───────────────────────────────────────────────────── */
void TitleEditor::play_pause()
{
    if (!title_) return;
    if (!playing_)
        full_loop_playback_ = false;
    playing_ = !playing_;
    if (playing_) {
        if (title_->playback_mode != 2 && playhead_ >= title_->duration)
            on_playhead_changed(0.0);
        if (title_->playback_mode == 2 && playhead_ >= std::clamp(title_->pause_time, 0.0, title_->duration))
            on_playhead_changed(0.0);
        act_play_->setText("⏸");
        act_play_->setIcon(obs_icon("pause.svg"));
        playback_clock_.restart();
        play_timer_->start();
    } else {
        act_play_->setText("▶");
        act_play_->setIcon(obs_icon("play.svg"));
        play_timer_->stop();
    }
}

void TitleEditor::play_full_loop()
{
    if (!title_) return;
    full_loop_playback_ = true;
    playback_reverse_ = false;
    if (!playing_ || playhead_ >= title_->duration)
        on_playhead_changed(0.0);
    playing_ = true;
    act_play_->setText("⏸");
    act_play_->setIcon(obs_icon("pause.svg"));
    playback_clock_.restart();
    play_timer_->start();
}

void TitleEditor::rewind()
{
    full_loop_playback_ = false;
    playback_reverse_ = false;
    on_playhead_changed(0.0);
}

void TitleEditor::step_forward()
{
    if (!title_) return;
    on_playhead_changed(std::min(snap_to_obs_frame(playhead_ + obs_frame_duration()), title_->duration));
}


static void collect_timeline_keyframes(const std::shared_ptr<Layer> &layer,
                                       std::vector<double> &times)
{
    if (!layer) return;
    for (auto *prop : timeline_properties(*layer)) {
        for (const auto &kf : prop->keyframes)
            times.push_back(layer->in_time + kf.time);
    }
}

void TitleEditor::previous_keyframe()
{
    if (!title_) return;
    std::vector<double> times;
    if (!sel_layer_id_.empty())
        collect_timeline_keyframes(title_->find_layer(sel_layer_id_), times);
    if (times.empty())
        for (const auto &layer : title_->layers) collect_timeline_keyframes(layer, times);

    constexpr double kEpsilon = 1.0 / 240.0;
    double target = -1.0;
    for (double t : times) {
        if (t < playhead_ - kEpsilon)
            target = std::max(target, t);
    }
    if (target >= 0.0) on_playhead_changed(target);
}

void TitleEditor::next_keyframe()
{
    if (!title_) return;
    std::vector<double> times;
    if (!sel_layer_id_.empty())
        collect_timeline_keyframes(title_->find_layer(sel_layer_id_), times);
    if (times.empty())
        for (const auto &layer : title_->layers) collect_timeline_keyframes(layer, times);

    constexpr double kEpsilon = 1.0 / 240.0;
    double target = title_->duration + 1.0;
    for (double t : times) {
        if (t > playhead_ + kEpsilon)
            target = std::min(target, t);
    }
    if (target <= title_->duration) on_playhead_changed(target);
}

void TitleEditor::tick()
{
    if (!title_ || !playing_) return;
    double dt = playback_clock_.isValid() ? playback_clock_.restart() / 1000.0 : 0.0;
    if (dt <= 0.0 || dt > 0.25) dt = play_timer_->interval() / 1000.0;

    double duration = std::max(0.001, title_->duration);
    double loop_start = std::clamp(title_->loop_start, 0.0, title_->duration);
    double loop_end = std::clamp(title_->loop_end, loop_start, title_->duration);
    double loop_len = std::max(0.001, loop_end - loop_start);
    double t = playhead_;

    if (full_loop_playback_) {
        t = std::fmod(playhead_ + dt, duration);
    } else {
        switch (title_->playback_mode) {
        case 1: /* Loop in/out between Loop Start and Loop End */
            if (loop_end <= loop_start + 0.0001) {
                t = std::fmod(playhead_ + dt, duration);
            } else if (title_->loop_type == 1) {
                t += (playback_reverse_ ? -dt : dt);
                if (!playback_reverse_ && t >= loop_end) {
                    t = loop_end - std::fmod(t - loop_end, loop_len);
                    playback_reverse_ = true;
                } else if (playback_reverse_ && t <= loop_start) {
                    t = loop_start + std::fmod(loop_start - t, loop_len);
                    playback_reverse_ = false;
                }
            } else {
                t = playhead_ + dt;
                if (t >= loop_end)
                    t = loop_start + std::fmod(t - loop_end, loop_len);
            }
            break;
        case 2: { /* Pause at timeline position */
            double pause_time = std::clamp(title_->pause_time, 0.0, title_->duration);
            t = playhead_ + dt;
            if (t >= pause_time) {
                t = pause_time;
                playing_ = false;
                play_timer_->stop();
                act_play_->setText("▶");
                act_play_->setIcon(obs_icon("play.svg"));
            }
            break;
        }
        default: /* Play once */
            t = playhead_ + dt;
            if (t >= title_->duration) {
                t = title_->duration;
                playing_ = false;
                play_timer_->stop();
                act_play_->setText("▶");
                act_play_->setIcon(obs_icon("play.svg"));
            }
            break;
        }
    }
    on_playhead_changed(snap_to_obs_frame(t));
}

static bool editor_focus_accepts_text(QWidget *widget)
{
    return qobject_cast<QLineEdit *>(widget) ||
           qobject_cast<QTextEdit *>(widget) ||
           qobject_cast<QAbstractSpinBox *>(widget) ||
           qobject_cast<QComboBox *>(widget);
}

void TitleEditor::show_about()
{
    QMessageBox::about(
        this,
        obsgs_tr("OBSTitles.AboutTitle"),
        obsgs_tr("OBSTitles.AboutTextFormat").arg(QStringLiteral(PLUGIN_VERSION)));
}

void TitleEditor::show_preferences()
{
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(obsgs_tr("OBSTitles.Preferences"));
    dialog->setModal(false);
    dialog->resize(620, 420);

    const QPalette pal = dialog->palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor button = pal.color(QPalette::Button);
    const QColor button_text = pal.color(QPalette::ButtonText);
    const QColor border = pal.color(QPalette::Mid);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlighted_text = pal.color(QPalette::HighlightedText);
    const QColor disabled_text = pal.color(QPalette::Disabled, QPalette::WindowText);
    const QColor hover = button.lightness() < 128 ? button.lighter(125) : button.darker(108);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto *content = new QWidget(dialog);
    auto *content_layout = new QHBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(12);

    auto *tabs = new QListWidget(content);
    tabs->setFixedWidth(138);
    tabs->setFrameShape(QFrame::NoFrame);
    tabs->setSelectionMode(QAbstractItemView::SingleSelection);
    tabs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tabs->addItem(QStringLiteral("Appearance"));
    tabs->addItem(QStringLiteral("Advanced"));
    tabs->setStyleSheet(QStringLiteral(
        "QListWidget{background:%1;border:1px solid %2;color:%3;outline:none;}"
        "QListWidget::item{padding:8px 10px;border-bottom:1px solid %2;}"
        "QListWidget::item:hover{background:%4;}"
        "QListWidget::item:selected{background:%5;color:%6;}")
        .arg(base.name(QColor::HexRgb),
             border.name(QColor::HexRgb),
             text.name(QColor::HexRgb),
             hover.name(QColor::HexRgb),
             highlight.name(QColor::HexRgb),
             highlighted_text.name(QColor::HexRgb)));

    auto *pages = new QStackedWidget(content);
    pages->setStyleSheet(QStringLiteral("QWidget{background:%1;color:%2;}")
                             .arg(window.name(QColor::HexRgb),
                                  text.name(QColor::HexRgb)));

    auto *colors_page = new QWidget(pages);
    auto *colors_layout = new QVBoxLayout(colors_page);
    colors_layout->setContentsMargins(0, 0, 0, 0);
    colors_layout->setSpacing(10);

    auto *colors_title = new QLabel(QStringLiteral("Timeline Colors"), colors_page);
    QFont title_font = colors_title->font();
    title_font.setPointSize(title_font.pointSize() + 2);
    title_font.setBold(true);
    colors_title->setFont(title_font);
    colors_layout->addWidget(colors_title);

    auto *grid = new QGridLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);
    colors_layout->addLayout(grid);

    auto update_appearance = [this]() {
        if (layers_)
            layers_->refresh();
        if (timeline_)
            timeline_->update();
        if (canvas_)
            canvas_->update();
    };

    auto color_to_text = [](const QColor &color) {
        return color.alpha() < 255 ? color.name(QColor::HexArgb).toUpper()
                                   : color.name(QColor::HexRgb).toUpper();
    };
    auto apply_color_button = [border, highlight, color_to_text](QPushButton *button, const QColor &color) {
        const QColor label_color = color.lightness() < 128 ? Qt::white : Qt::black;
        button->setText(color_to_text(color));
        button->setStyleSheet(QStringLiteral(
            "QPushButton{color:%1;background:%2;border:1px solid %3;border-radius:3px;padding:4px 8px;text-align:left;}"
            "QPushButton:hover{border-color:%4;}")
            .arg(label_color.name(QColor::HexRgb),
                 color.name(QColor::HexArgb),
                 border.name(QColor::HexRgb),
                 highlight.name(QColor::HexRgb)));
    };
    auto add_color_button_row = [&](int row, const QString &label, std::function<QColor()> current_color,
                                    std::function<void(const QColor &)> apply_color) {
        auto *name = new QLabel(label, colors_page);
        name->setStyleSheet(QStringLiteral("color:%1;").arg(text.name(QColor::HexRgb)));
        auto *button = new QPushButton(colors_page);
        button->setMinimumWidth(116);
        button->setCursor(Qt::PointingHandCursor);
        apply_color_button(button, current_color());
        grid->addWidget(name, row, 0);
        grid->addWidget(button, row, 1);
        connect(button, &QPushButton::clicked, dialog, [dialog, button, label, apply_color_button, apply_color, current_color]() mutable {
            auto *picker = new QColorDialog(current_color(), dialog);
            picker->setAttribute(Qt::WA_DeleteOnClose);
            picker->setWindowTitle(label);
            picker->setOption(QColorDialog::ShowAlphaChannel, true);
            connect(picker, &QColorDialog::currentColorChanged, dialog, [button, apply_color_button, apply_color](const QColor &color) mutable {
                if (!color.isValid())
                    return;
                apply_color(color);
                apply_color_button(button, color);
            });
            picker->open();
        });
    };
    auto add_timeline_color_row = [&](int row, const QString &label, TitlePreferences::TimelineColorRole role) {
        add_color_button_row(row, label, [role]() { return TitlePreferences::timeline_color(role); }, [role, update_appearance](const QColor &color) {
            TitlePreferences::set_timeline_color(role, color);
            update_appearance();
        });
    };

    int color_row = 0;
    add_timeline_color_row(color_row++, QStringLiteral("Text layers"), TitlePreferences::TimelineColorRole::TextLayer);
    add_timeline_color_row(color_row++, QStringLiteral("Clock layers"), TitlePreferences::TimelineColorRole::ClockLayer);
    add_timeline_color_row(color_row++, QStringLiteral("Ticker layers"), TitlePreferences::TimelineColorRole::TickerLayer);
    add_timeline_color_row(color_row++, QStringLiteral("Object layers"), TitlePreferences::TimelineColorRole::ObjectLayer);
    add_timeline_color_row(color_row++, QStringLiteral("Image layers"), TitlePreferences::TimelineColorRole::ImageLayer);
    add_timeline_color_row(color_row++, QStringLiteral("Current time"), TitlePreferences::TimelineColorRole::Current);
    add_timeline_color_row(color_row++, QStringLiteral("Pause marker"), TitlePreferences::TimelineColorRole::Pause);
    add_timeline_color_row(color_row++, QStringLiteral("Loop start/end"), TitlePreferences::TimelineColorRole::Loop);
    add_color_button_row(color_row++, QStringLiteral("Scene mask objects"), []() { return TitlePreferences::scene_mask_color(); }, [update_appearance](const QColor &color) {
        TitlePreferences::set_scene_mask_color(color);
        update_appearance();
    });
    grid->setColumnStretch(2, 1);
    colors_layout->addStretch(1);

    auto *advanced_page = new QWidget(pages);
    auto *advanced_layout = new QVBoxLayout(advanced_page);
    advanced_layout->setContentsMargins(0, 0, 0, 0);
    advanced_layout->setSpacing(10);

    auto *advanced_title = new QLabel(QStringLiteral("Advanced"), advanced_page);
    advanced_title->setFont(title_font);
    advanced_layout->addWidget(advanced_title);

    auto *use_gpu = new QCheckBox(obsgs_tr("OBSTitles.UseGPU"), advanced_page);
    use_gpu->setChecked(TitlePreferences::use_gpu());
    use_gpu->setToolTip(obsgs_tr("OBSTitles.UseGPUTooltip"));
    use_gpu->setStyleSheet(QStringLiteral("QCheckBox{color:%1;}QCheckBox:disabled{color:%2;}")
                               .arg(text.name(QColor::HexRgb),
                                    disabled_text.name(QColor::HexRgb)));
    advanced_layout->addWidget(use_gpu);
    advanced_layout->addStretch(1);

    pages->addWidget(colors_page);
    pages->addWidget(advanced_page);
    content_layout->addWidget(tabs);
    content_layout->addWidget(pages, 1);
    layout->addWidget(content, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    layout->addWidget(buttons);

    tabs->setCurrentRow(0);
    connect(tabs, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(use_gpu, &QCheckBox::toggled, this, &TitleEditor::set_gpu_pipeline_enabled);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}


bool TitleEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == title_lbl_ && event->type() == QEvent::MouseButtonDblClick) {
        auto *mouse_event = static_cast<QMouseEvent *>(event);
        if (mouse_event->button() == Qt::LeftButton) {
            begin_title_name_edit();
            event->accept();
            return true;
        }
    }

    if (watched == title_name_edit_) {
        if (event->type() == QEvent::KeyPress) {
            auto *key_event = static_cast<QKeyEvent *>(event);
            if (key_event->key() == Qt::Key_Escape) {
                commit_title_name_edit(false);
                key_event->accept();
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            commit_title_name_edit(false);
            return false;
        }
    }

    if (event->type() == QEvent::KeyPress && isActiveWindow()) {
        auto *key_event = static_cast<QKeyEvent *>(event);
        auto *widget = qobject_cast<QWidget *>(watched);
        const bool in_editor = widget && (widget == this || isAncestorOf(widget));
        if (in_editor && key_event->key() == Qt::Key_Space && !key_event->isAutoRepeat()) {
            if (!editor_focus_accepts_text(focusWidget())) {
                play_pause();
                key_event->accept();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void TitleEditor::keyPressEvent(QKeyEvent *ev)
{
    if (ev->matches(QKeySequence::Undo)) {
        if (undo_index_ > 0) restore_undo_snapshot(undo_index_ - 1);
        ev->accept();
        return;
    }
    if (ev->matches(QKeySequence::Redo)) {
        if (undo_index_ + 1 < (int)undo_stack_.size()) restore_undo_snapshot(undo_index_ + 1);
        ev->accept();
        return;
    }
    QWidget *fw = focusWidget();
    bool editing_value = editor_focus_accepts_text(fw);
    if (!editing_value && ev->key() == Qt::Key_Escape) {
        close();
        ev->accept();
        return;
    }
    if (!editing_value && timeline_ && ev->matches(QKeySequence::Copy) &&
        timeline_->has_selected_keyframes()) {
        timeline_->copy_keyframe_selection();
        ev->accept();
        return;
    }
    if (!editing_value && timeline_ && ev->matches(QKeySequence::Cut) &&
        timeline_->has_selected_keyframes()) {
        timeline_->cut_keyframe_selection();
        ev->accept();
        return;
    }
    if (!editing_value && timeline_ && ev->matches(QKeySequence::Paste) &&
        timeline_->has_keyframe_clipboard()) {
        timeline_->paste_keyframes_at_playhead();
        ev->accept();
        return;
    }
    if (!editing_value && timeline_ &&
        (ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) &&
        timeline_->has_selected_keyframes()) {
        timeline_->delete_keyframe_selection();
        ev->accept();
        return;
    }
    if (!editing_value && ev->matches(QKeySequence::Copy) && !sel_layer_id_.empty()) {
        copy_selected_layer();
        ev->accept();
        return;
    }
    if (!editing_value && ev->matches(QKeySequence::Cut) && !sel_layer_id_.empty()) {
        cut_selected_layer();
        ev->accept();
        return;
    }
    if (!editing_value && ev->matches(QKeySequence::Paste) && !layer_clipboard_.empty()) {
        paste_layer_from_clipboard();
        ev->accept();
        return;
    }
    if (!editing_value && ev->key() == Qt::Key_Delete && !sel_layer_id_.empty()) {
        delete_selected_layer();
        ev->accept();
        return;
    }
    if (ev->key() == Qt::Key_Space && !ev->isAutoRepeat()) {
        if (!editor_focus_accepts_text(focusWidget())) {
            play_pause();
            ev->accept();
            return;
        }
    }
    QMainWindow::keyPressEvent(ev);
}


void TitleEditor::closeEvent(QCloseEvent *ev)
{
    if (confirm_save_before_close()) {
        save_editor_layout();
        editor_layout_save_suppressed_ = true;
        ev->accept();
    } else {
        ev->ignore();
    }
}

void TitleEditor::reject()
{
    close();
}

/* ── Signal handlers ─────────────────────────────────────────────── */



static void copy_editor_layer_style_fields(Layer &dst, const Layer &src)
{
    dst.font_family = src.font_family;
    dst.font_style = src.font_style;
    dst.font_size = src.font_size;
    dst.font_size_prop.static_value = src.font_size_prop.static_value;
    dst.font_bold = src.font_bold;
    dst.font_italic = src.font_italic;
    dst.font_kerning = src.font_kerning;
    dst.kerning_mode = src.kerning_mode;
    dst.manual_kerning = src.manual_kerning;
    dst.text_leading = src.text_leading;
    dst.char_tracking = src.char_tracking;
    dst.char_tracking_prop.static_value = src.char_tracking_prop.static_value;
    dst.char_scale_x = src.char_scale_x;
    dst.char_scale_y = src.char_scale_y;
    dst.char_scale_x_prop.static_value = src.char_scale_x_prop.static_value;
    dst.char_scale_y_prop.static_value = src.char_scale_y_prop.static_value;
    dst.baseline_shift = src.baseline_shift;
    dst.baseline_shift_prop.static_value = src.baseline_shift_prop.static_value;
    dst.text_style = src.text_style;
    dst.text_underline = src.text_underline;
    dst.text_strikethrough = src.text_strikethrough;
    dst.text_ligatures = src.text_ligatures;
    dst.text_stylistic_alternates = src.text_stylistic_alternates;
    dst.text_fractions = src.text_fractions;
    dst.text_opentype_features = src.text_opentype_features;
    dst.text_overflow_mode = src.text_overflow_mode;
    dst.text_fit_min_scale = src.text_fit_min_scale;
    dst.text_box_width_to_text = src.text_box_width_to_text;
    dst.text_box_height_to_text = src.text_box_height_to_text;
    dst.max_text_box_width = src.max_text_box_width;
    dst.max_text_box_height = src.max_text_box_height;

    dst.text_color = src.text_color;
    dst.fill_color = src.fill_color;
    dst.fill_type = src.fill_type;
    dst.gradient_type = src.gradient_type;
    dst.gradient_start_color = src.gradient_start_color;
    dst.gradient_end_color = src.gradient_end_color;
    dst.gradient_start_pos = src.gradient_start_pos;
    dst.gradient_end_pos = src.gradient_end_pos;
    dst.gradient_start_opacity = src.gradient_start_opacity;
    dst.gradient_end_opacity = src.gradient_end_opacity;
    dst.gradient_opacity = src.gradient_opacity;
    dst.gradient_angle = src.gradient_angle;
    dst.gradient_center_x = src.gradient_center_x;
    dst.gradient_center_y = src.gradient_center_y;
    dst.gradient_scale = src.gradient_scale;
    dst.gradient_focal_x = src.gradient_focal_x;
    dst.gradient_focal_y = src.gradient_focal_y;
    dst.gradient_stops = src.gradient_stops;

    dst.outline_enabled = src.outline_enabled;
    dst.stroke_fill_type = src.stroke_fill_type;
    dst.stroke_color = src.stroke_color;
    dst.stroke_width = src.stroke_width;
    dst.outline_opacity = src.outline_opacity;
    dst.outline_join_style = src.outline_join_style;
    dst.outline_on_front = src.outline_on_front;
    dst.outline_antialias = src.outline_antialias;
    dst.stroke_gradient_type = src.stroke_gradient_type;
    dst.stroke_gradient_start_color = src.stroke_gradient_start_color;
    dst.stroke_gradient_end_color = src.stroke_gradient_end_color;
    dst.stroke_gradient_start_pos = src.stroke_gradient_start_pos;
    dst.stroke_gradient_end_pos = src.stroke_gradient_end_pos;
    dst.stroke_gradient_start_opacity = src.stroke_gradient_start_opacity;
    dst.stroke_gradient_end_opacity = src.stroke_gradient_end_opacity;
    dst.stroke_gradient_opacity = src.stroke_gradient_opacity;
    dst.stroke_gradient_angle = src.stroke_gradient_angle;
    dst.stroke_gradient_center_x = src.stroke_gradient_center_x;
    dst.stroke_gradient_center_y = src.stroke_gradient_center_y;
    dst.stroke_gradient_scale = src.stroke_gradient_scale;
    dst.stroke_gradient_focal_x = src.stroke_gradient_focal_x;
    dst.stroke_gradient_focal_y = src.stroke_gradient_focal_y;
    dst.stroke_gradient_stops = src.stroke_gradient_stops;

    dst.background_enabled = src.background_enabled;
    dst.background_color = src.background_color;
    dst.background_opacity = src.background_opacity;
    dst.background_padding_x = src.background_padding_x;
    dst.background_padding_y = src.background_padding_y;
    dst.background_corner_radius = src.background_corner_radius;
    dst.background_fill_type = src.background_fill_type;
    dst.background_gradient_type = src.background_gradient_type;
    dst.background_gradient_start_color = src.background_gradient_start_color;
    dst.background_gradient_end_color = src.background_gradient_end_color;
    dst.background_gradient_start_opacity = src.background_gradient_start_opacity;
    dst.background_gradient_end_opacity = src.background_gradient_end_opacity;
    dst.background_gradient_opacity = src.background_gradient_opacity;

    dst.align_h = src.align_h;
    dst.align_v = src.align_v;
    dst.paragraph_indent_left = src.paragraph_indent_left;
    dst.paragraph_indent_right = src.paragraph_indent_right;
    dst.paragraph_indent_first_line = src.paragraph_indent_first_line;
    dst.paragraph_space_before = src.paragraph_space_before;
    dst.paragraph_space_after = src.paragraph_space_after;
    dst.paragraph_hyphenate = src.paragraph_hyphenate;
    dst.paragraph_indent_left_prop.static_value = src.paragraph_indent_left_prop.static_value;
    dst.paragraph_indent_right_prop.static_value = src.paragraph_indent_right_prop.static_value;
    dst.paragraph_indent_first_line_prop.static_value = src.paragraph_indent_first_line_prop.static_value;
    dst.paragraph_space_before_prop.static_value = src.paragraph_space_before_prop.static_value;
    dst.paragraph_space_after_prop.static_value = src.paragraph_space_after_prop.static_value;
}

void TitleEditor::load_sidebar_default_colors()
{
    if (g_editor_session_sidebar_colors_initialized) {
        default_foreground_color_ = g_editor_session_foreground_color;
        default_background_color_ = g_editor_session_background_color;
        return;
    }

    g_editor_session_foreground_color = default_foreground_color_;
    g_editor_session_background_color = default_background_color_;
    g_editor_session_sidebar_colors_initialized = true;
}

void TitleEditor::save_sidebar_default_colors() const
{
    g_editor_session_foreground_color = default_foreground_color_;
    g_editor_session_background_color = default_background_color_;
    g_editor_session_sidebar_colors_initialized = true;
}



void TitleEditor::copy_layer_style_to_new_layer_defaults(const Layer &layer)
{
    copy_editor_layer_style_fields(default_new_layer_style_, layer);
    save_new_layer_defaults();
}

void TitleEditor::apply_new_layer_defaults(Layer &layer) const
{
    copy_editor_layer_style_fields(layer, default_new_layer_style_);
    const uint32_t fg_argb = argb_from_color(default_foreground_color_);
    const uint32_t bg_argb = argb_from_color(default_background_color_);
    const bool text_like = layer.type == LayerType::Text || layer.type == LayerType::Clock || layer.type == LayerType::Ticker;
    if (text_like) {
        // Text, Clock and Ticker layers use text_color for solid fill rendering,
        // but they also share the generic fill_type / gradient fields with shape
        // layers. Keep those generic fill defaults intact so the sidebar
        // foreground swatch behaves like Photoshop/Illustrator for every new
        // drawable object, not only shapes.
        layer.fill_type = default_new_layer_style_.fill_type;

        const uint32_t default_solid_fill =
            (default_new_layer_style_.fill_color != 0) ? default_new_layer_style_.fill_color :
            ((default_new_layer_style_.text_color != 0) ? default_new_layer_style_.text_color : fg_argb);

        if (layer.fill_type == 1) {
            // Gradient text still needs a valid solid fallback/selection color.
            layer.text_color = (default_new_layer_style_.text_color != 0)
                ? default_new_layer_style_.text_color
                : default_new_layer_style_.gradient_start_color;
            if (layer.text_color == 0)
                layer.text_color = fg_argb;
        } else {
            layer.fill_type = 0;
            layer.text_color = default_solid_fill;
            layer.fill_color = default_solid_fill;
        }

        layer.stroke_color = (default_new_layer_style_.stroke_color != 0)
            ? default_new_layer_style_.stroke_color
            : bg_argb;

        RichTextCharFormat fmt = layer_char_format_for_editor(layer);
        layer.rich_text.default_format = fmt;
        layer.rich_text.default_paragraph_format.align_h = layer.align_h;
        layer.rich_text.default_paragraph_format.align_v = layer.align_v;
        apply_rich_text_format_to_layer_range(layer, fmt, (RichTextCharFontFamily | RichTextCharFontStyle | RichTextCharFontSize | RichTextCharBold | RichTextCharItalic | RichTextCharUnderline | RichTextCharStrikethrough | RichTextCharTracking | RichTextCharScaleX | RichTextCharScaleY | RichTextCharBaselineShift | RichTextCharFillColor | RichTextCharKerning | RichTextCharTextStyle | RichTextCharLigatures | RichTextCharStylisticAlternates | RichTextCharFractions | RichTextCharOpenTypeFeatures | RichTextCharLanguage), false);
    } else if (layer.type == LayerType::Shape || layer.type == LayerType::SolidRect) {
        layer.fill_color = (default_new_layer_style_.fill_color != 0) ? default_new_layer_style_.fill_color : fg_argb;
        layer.stroke_color = (default_new_layer_style_.stroke_color != 0) ? default_new_layer_style_.stroke_color : bg_argb;
    }
}

void TitleEditor::load_new_layer_defaults()
{
    if (g_editor_session_new_layer_defaults_initialized) {
        default_new_layer_style_ = g_editor_session_new_layer_style;
        return;
    }

    g_editor_session_new_layer_style = default_new_layer_style_;
    g_editor_session_new_layer_defaults_initialized = true;
}


void TitleEditor::save_new_layer_defaults() const
{
    g_editor_session_new_layer_style = default_new_layer_style_;
    g_editor_session_new_layer_defaults_initialized = true;
}


void TitleEditor::set_default_sidebar_colors_from_layer(const Layer &layer)
{
    const double lt = std::clamp(playhead_ - layer.in_time, 0.0,
                                 std::max(0.0, layer.out_time - layer.in_time));
    const bool is_text_like = layer.type == LayerType::Text || layer.type == LayerType::Clock ||
                              layer.type == LayerType::Ticker;
    default_foreground_color_ = color_from_argb(is_text_like ? eval_text_color(layer, lt) : eval_fill_color(layer, lt));
    default_background_color_ = color_from_argb(eval_outline_color(layer, lt));
    copy_layer_style_to_new_layer_defaults(layer);

    // For text-like layers the visible fill color lives in text_color, while
    // shape-like layers use fill_color. Mirror the solid text fill into the
    // generic fill default so the next Shape/Text/Clock/Ticker starts with the
    // same foreground fill regardless of which layer type changed it.
    if (is_text_like) {
        default_new_layer_style_.fill_type = layer.fill_type;
        if (layer.fill_type == 0)
            default_new_layer_style_.fill_color = is_text_like ? eval_text_color(layer, lt) : eval_fill_color(layer, lt);
        default_new_layer_style_.text_color = eval_text_color(layer, lt);
    } else {
        default_new_layer_style_.text_color = eval_fill_color(layer, lt);
    }

    save_sidebar_default_colors();
    save_new_layer_defaults();
}

void TitleEditor::update_sidebar_color_swatches(std::shared_ptr<Layer> layer)
{
    if (!tools_sidebar_) return;
    if (layer) {
        const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                     std::max(0.0, layer->out_time - layer->in_time));
        const bool is_text_like = layer->type == LayerType::Text || layer->type == LayerType::Clock ||
                                  layer->type == LayerType::Ticker;
        const uint32_t fg = is_text_like ? eval_text_color(*layer, lt) : eval_fill_color(*layer, lt);
        const uint32_t bg = eval_outline_color(*layer, lt);
        // Show the actual active fill in the sidebar swatch for every layer type that
        // supports gradients, including Text / Clock / Ticker. Previously text-like
        // layers were forced to a solid color fallback, so their gradient state was
        // stored on the layer but never visible in the sidebar swatch.
        if (layer->fill_type == 1) {
            tools_sidebar_->set_foreground_gradient(color_from_argb(layer->gradient_start_color),
                                                    color_from_argb(layer->gradient_end_color),
                                                    layer->gradient_type);
        } else {
            tools_sidebar_->set_foreground_color(color_from_argb(fg));
        }
        if (layer->stroke_fill_type == 2) {
            tools_sidebar_->set_background_gradient(color_from_argb(layer->stroke_gradient_start_color),
                                                    color_from_argb(layer->stroke_gradient_end_color),
                                                    layer->stroke_gradient_type);
        } else {
            tools_sidebar_->set_background_color(color_from_argb(bg));
        }
    } else {
        if (default_new_layer_style_.fill_type == 1) {
            tools_sidebar_->set_foreground_gradient(color_from_argb(default_new_layer_style_.gradient_start_color),
                                                    color_from_argb(default_new_layer_style_.gradient_end_color),
                                                    default_new_layer_style_.gradient_type);
        } else {
            tools_sidebar_->set_foreground_color(default_foreground_color_);
        }
        if (default_new_layer_style_.stroke_fill_type == 2) {
            tools_sidebar_->set_background_gradient(color_from_argb(default_new_layer_style_.stroke_gradient_start_color),
                                                    color_from_argb(default_new_layer_style_.stroke_gradient_end_color),
                                                    default_new_layer_style_.stroke_gradient_type);
        } else {
            tools_sidebar_->set_background_color(default_background_color_);
        }
    }
}


void TitleEditor::open_default_sidebar_color_popup(bool foreground)
{
    QColor &target = foreground ? default_foreground_color_ : default_background_color_;

    QDialog popup(this, Qt::Popup | Qt::FramelessWindowHint);
    popup.setModal(true);
    popup.setMinimumWidth(320);
    popup.setStyleSheet(QStringLiteral(
        "QDialog{background:%1;border:1px solid %2;}"
        "QTabWidget::pane{border:1px solid %2;background:%1;}"
        "QTabBar::tab{color:%3;background:%4;border:1px solid %2;padding:5px 10px;}"
        "QTabBar::tab:selected{background:%5;color:%6;}"
        "QLabel{color:%3;background:transparent;}"
        "QLineEdit{color:%3;background:%7;border:1px solid %2;border-radius:2px;padding:3px 5px;}"
        "QPushButton{color:%3;background:%4;border:1px solid %2;border-radius:2px;padding:4px 8px;}"
        "QPushButton:hover{background:%8;}"
    ).arg(qApp->palette().color(QPalette::Window).name(QColor::HexRgb),
          qApp->palette().color(QPalette::Mid).name(QColor::HexRgb),
          qApp->palette().color(QPalette::WindowText).name(QColor::HexRgb),
          qApp->palette().color(QPalette::Button).name(QColor::HexRgb),
          qApp->palette().color(QPalette::Highlight).name(QColor::HexRgb),
          qApp->palette().color(QPalette::HighlightedText).name(QColor::HexRgb),
          qApp->palette().color(QPalette::Base).name(QColor::HexRgb),
          (qApp->palette().color(QPalette::Button).lightness() < 128
              ? qApp->palette().color(QPalette::Button).lighter(125)
              : qApp->palette().color(QPalette::Button).darker(108)).name(QColor::HexRgb)));

    auto *root = new QVBoxLayout(&popup);
    root->setContentsMargins(8, 8, 8, 8);
    auto *tabs = new QTabWidget(&popup);
    root->addWidget(tabs);

    auto *color_tab = new QWidget(tabs);
    auto *color_layout = new QVBoxLayout(color_tab);
    color_layout->setContentsMargins(8, 8, 8, 8);
    color_layout->setSpacing(8);
    auto *label = new QLabel(foreground ? QStringLiteral("Default foreground color")
                                         : QStringLiteral("Default background color"), color_tab);
    auto *swatch = new QPushButton(color_tab);
    swatch->setFixedSize(90, 52);
    auto *hex = new QLineEdit(color_tab);
    auto update_controls = [&]() {
        swatch->setStyleSheet(QStringLiteral("QPushButton{background:%1;border:1px solid %2;border-radius:2px;}")
            .arg(target.name(QColor::HexArgb), qApp->palette().color(QPalette::Mid).name(QColor::HexRgb)));
        hex->setText(target.alpha() < 255 ? target.name(QColor::HexArgb).toUpper()
                                          : target.name(QColor::HexRgb).toUpper());
        update_sidebar_color_swatches(nullptr);
    };
    auto apply_color = [&](const QColor &color) {
        if (!color.isValid()) return;
        target = color;
        const uint32_t argb = argb_from_color(color);
        if (foreground) {
            default_new_layer_style_.fill_type = 0;
            default_new_layer_style_.fill_color = argb;
            default_new_layer_style_.text_color = argb;
        } else {
            default_new_layer_style_.stroke_fill_type = 1;
            default_new_layer_style_.stroke_color = argb;
        }
        save_sidebar_default_colors();
        save_new_layer_defaults();
        update_controls();
    };
    connect(swatch, &QPushButton::clicked, &popup, [&]() {
        QColor picked = QColorDialog::getColor(target, &popup,
            foreground ? QStringLiteral("Foreground Color") : QStringLiteral("Background Color"),
            QColorDialog::ShowAlphaChannel);
        apply_color(picked);
    });
    connect(hex, &QLineEdit::editingFinished, &popup, [&]() {
        QColor parsed(hex->text().trimmed());
        if (parsed.isValid()) apply_color(parsed);
        else update_controls();
    });
    color_layout->addWidget(label);
    color_layout->addWidget(swatch, 0, Qt::AlignLeft);
    color_layout->addWidget(hex);
    tabs->addTab(color_tab, QStringLiteral("Color"));

    auto *gradient_tab = new QWidget(tabs);
    auto *gradient_layout = new QVBoxLayout(gradient_tab);
    auto *gradient_label = new QLabel(QStringLiteral("Gradient defaults will use the main layer selector when a layer is selected."), gradient_tab);
    gradient_label->setWordWrap(true);
    gradient_layout->addWidget(gradient_label);
    gradient_layout->addStretch(1);
    tabs->addTab(gradient_tab, QStringLiteral("Gradient"));

    auto *swatches_tab = new QWidget(tabs);
    auto *swatches_layout = new QVBoxLayout(swatches_tab);
    auto *swatches_label = new QLabel(QStringLiteral("Saved foreground/background defaults are used for new objects."), swatches_tab);
    swatches_label->setWordWrap(true);
    swatches_layout->addWidget(swatches_label);
    swatches_layout->addStretch(1);
    tabs->addTab(swatches_tab, QStringLiteral("Swatches"));

    update_controls();
    popup.adjustSize();
    const QPoint cursor_pos = QCursor::pos();
    const QPoint desired_pos(cursor_pos.x() + 14, cursor_pos.y() - popup.height() / 2);
    popup.move(clamp_popup_position_to_screen(desired_pos, popup.size(), this));
    popup.exec();
}

void TitleEditor::update_layer_panels(std::shared_ptr<Layer> layer, double playhead)
{
    if (layer_props_dock_) {
        layer_props_dock_->setWindowTitle(layer
            ? QStringLiteral("Properties: %1").arg(QString::fromStdString(layer->name))
            : QStringLiteral("Properties: No selection"));
    }
    if (props_) props_->set_layer(layer, playhead);
    update_sidebar_color_swatches(layer);
    if (effects_panel_) effects_panel_->set_layer(layer, playhead);
}

void TitleEditor::on_layer_selected(const std::string &lid)
{
    sel_layer_id_ = lid;
    layers_->set_selected_layer(lid);
    canvas_->set_selected_layer(lid);
    timeline_->set_selected_layer(lid);

    if (!title_ || lid.empty()) {
        update_layer_panels(nullptr, playhead_);
        return;
    }
    auto layer = title_->find_layer(lid);
    if (layer) update_layer_panels(layer, playhead_);
}

void TitleEditor::on_playhead_changed(double t)
{
    t = title_ ? std::clamp(snap_to_obs_frame(t), 0.0, title_->duration) : snap_to_obs_frame(t);
    playhead_ = t;
    canvas_->set_playhead(t);
    timeline_->set_playhead(t);

    if (!sel_layer_id_.empty() && title_) {
        auto l = title_->find_layer(sel_layer_id_);
        if (l) update_layer_panels(l, t);
    }

    if (time_lbl_)
        time_lbl_->setText(obsgs_tr("OBSTitles.TimeFpsFormat").arg(format_timecode(t)).arg(obs_frame_rate(), 0, 'f', 2));
}

void TitleEditor::on_title_modified(bool push_undo)
{
    if (title_) set_dirty(true);
    canvas_->refresh_preview();
    if (title_props_) title_props_->set_title(title_);
    if (timeline_) timeline_->set_title(title_);
    if (title_ && !sel_layer_id_.empty()) {
        if (auto layer = title_->find_layer(sel_layer_id_)) {
            set_default_sidebar_colors_from_layer(*layer);
            update_sidebar_color_swatches(layer);
        }
    } else {
        update_sidebar_color_swatches(nullptr);
    }
    if (push_undo)
        push_undo_snapshot();
    save_live_edit();
}

/* ══════════════════════════════════════════════════════════════════
 *  CanvasPreview
 * ══════════════════════════════════════════════════════════════════ */

