    act_timeline_visible_ = windows_menu->addAction(bgl_tr("OBSTitles.Timeline"));
    act_timeline_visible_->setCheckable(true);
    act_timeline_visible_->setChecked(true);
    connect(act_timeline_visible_, &QAction::triggered, this, [this](bool visible) {
        if (timeline_dock_) timeline_dock_->setVisible(visible);
    });

    act_prerender_visible_ = windows_menu->addAction(bgl_tr("OBSTitles.PlaybackAndCache"));
    act_prerender_visible_->setCheckable(true);
    act_prerender_visible_->setChecked(true);
    connect(act_prerender_visible_, &QAction::triggered, this, [this](bool visible) {
        if (prerender_dock_) prerender_dock_->setVisible(visible);
    });
}

QDockWidget *TitleEditor::create_editor_dock(const QString &object_name, const QString &title, QWidget *panel)
{
    auto *dock = new QDockWidget(title, this);
    bgl_apply_brand_icon(dock);
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
    else if (object_name == QString::fromUtf8(kEffectsPresetsDockObjectName))
        visibility_action = act_effects_presets_visible_;
    else if (object_name == QString::fromUtf8(kStylesDockObjectName))
        visibility_action = act_styles_visible_;
    else if (object_name == QString::fromUtf8(kTimelineDockObjectName))
        visibility_action = act_timeline_visible_;
    else if (object_name == QString::fromUtf8(kPrerenderDockObjectName))
        visibility_action = act_prerender_visible_;
    else if (object_name == QStringLiteral("BroadcastGraphicsLiveToolsDock"))
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
    dock->installEventFilter(this);
    return dock;
}

void TitleEditor::load_editor_layout()
{
    restoring_editor_layout_ = true;

    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));

    const int layout_version = settings.value(QString::fromUtf8(kEditorLayoutVersionKey), 0).toInt();
    const QByteArray geometry = settings.value(QString::fromUtf8(kEditorGeometryKey)).toByteArray();
    const QByteArray window_state = settings.value(QString::fromUtf8(kEditorWindowStateKey)).toByteArray();

    bool restored = false;
    if (layout_version == kEditorLayoutVersion && !window_state.isEmpty()) {
        if (!geometry.isEmpty())
            restoreGeometry(geometry);
        panels_locked_ = settings.value(QString::fromUtf8(kEditorPanelsLockedKey), false).toBool();
        restored = restoreState(window_state);
    }

    settings.endGroup();

    /* Apply the requested Broadcast Graphics Live workspace on first launch,
     * after an incompatible saved layout, or after this default layout schema
     * changes. The new state is then persisted and user customizations are
     * preserved normally on subsequent launches. */
    if (!restored) {
        restoring_editor_layout_ = false;
        reset_default_layout();
        return;
    }

    if (act_lock_panels_) {
        QSignalBlocker blocker(act_lock_panels_);
        act_lock_panels_->setChecked(panels_locked_);
    }

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
    if (act_effects_presets_visible_ && effects_presets_dock_) {
        QSignalBlocker blocker(act_effects_presets_visible_);
        act_effects_presets_visible_->setChecked(!effects_presets_dock_->isHidden());
    }
    if (act_styles_visible_ && styles_dock_) {
        QSignalBlocker blocker(act_styles_visible_);
        act_styles_visible_->setChecked(!styles_dock_->isHidden());
    }
    if (act_timeline_visible_ && timeline_dock_) {
        QSignalBlocker blocker(act_timeline_visible_);
        act_timeline_visible_->setChecked(!timeline_dock_->isHidden());
    }
    if (act_prerender_visible_ && prerender_dock_) {
        QSignalBlocker blocker(act_prerender_visible_);
        act_prerender_visible_->setChecked(!prerender_dock_->isHidden());
    }

    restoring_editor_layout_ = false;
    update_panel_lock_state();
}

void TitleEditor::save_editor_layout() const
{
    if (restoring_editor_layout_ || editor_layout_save_suppressed_)
        return;

    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));
    settings.setValue(QString::fromUtf8(kEditorGeometryKey), saveGeometry());
    settings.setValue(QString::fromUtf8(kEditorWindowStateKey), saveState());
    settings.setValue(QString::fromUtf8(kEditorPanelsLockedKey), panels_locked_);
    settings.setValue(QString::fromUtf8(kEditorLayoutVersionKey), kEditorLayoutVersion);
    settings.endGroup();
    settings.sync();
}

void TitleEditor::reset_default_layout()
{
    restoring_editor_layout_ = true;
    panels_locked_ = false;

    const QDockWidget::DockWidgetFeatures reset_features =
        QDockWidget::DockWidgetClosable |
        QDockWidget::DockWidgetMovable |
        QDockWidget::DockWidgetFloatable;
    const auto all_docks = {tools_dock_, graphic_props_dock_, layer_props_dock_,
                            effects_dock_, effects_presets_dock_, styles_dock_,
                            timeline_dock_, prerender_dock_};
    for (auto *dock : all_docks) {
        if (!dock)
            continue;
        dock->setFloating(false);
        removeDockWidget(dock);
        dock->setMaximumWidth(dock == tools_dock_ ? 64 : QWIDGETSIZE_MAX);
        if (dock == tools_dock_)
            dock->setMinimumWidth(46);
        else if (dock == effects_dock_ || dock == effects_presets_dock_)
            dock->setMinimumWidth(130);
        else
            dock->setMinimumWidth(dock->widget() ? dock->widget()->minimumWidth() : 220);
        dock->setFeatures(reset_features);
        dock->setAllowedAreas(dock == timeline_dock_
                                  ? (Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea)
                                  : (Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea));
    }

    /* The timeline owns both bottom corners so it spans the complete editor,
     * matching the reference workspace. */
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);

    /* Left workspace: tools | title properties, color/styles and the paired
     * Effects & Presets / Effects row. */
    if (tools_dock_) {
        tools_dock_->show();
        addDockWidget(Qt::LeftDockWidgetArea, tools_dock_);
    }
    if (graphic_props_dock_) {
        graphic_props_dock_->show();
        addDockWidget(Qt::LeftDockWidgetArea, graphic_props_dock_);
        if (tools_dock_)
            splitDockWidget(tools_dock_, graphic_props_dock_, Qt::Horizontal);
    }
    if (styles_dock_) {
        styles_dock_->show();
        addDockWidget(Qt::LeftDockWidgetArea, styles_dock_);
        if (graphic_props_dock_)
            splitDockWidget(graphic_props_dock_, styles_dock_, Qt::Vertical);
    }
    if (effects_presets_dock_) {
        effects_presets_dock_->show();
        addDockWidget(Qt::LeftDockWidgetArea, effects_presets_dock_);
        if (styles_dock_)
            splitDockWidget(styles_dock_, effects_presets_dock_, Qt::Vertical);
    }
    if (effects_dock_) {
        effects_dock_->show();
        addDockWidget(Qt::LeftDockWidgetArea, effects_dock_);
        if (effects_presets_dock_)
            splitDockWidget(effects_presets_dock_, effects_dock_, Qt::Horizontal);
    }

    /* The full layer/title properties panel occupies the right side. */
    if (layer_props_dock_) {
        layer_props_dock_->show();
        addDockWidget(Qt::RightDockWidgetArea, layer_props_dock_);
    }

    if (timeline_dock_) {
        timeline_dock_->show();
        addDockWidget(Qt::BottomDockWidgetArea, timeline_dock_);
    }

    /* Playback & Cache remains available from Window, but is closed in the
     * default workspace because caching itself is disabled by default. */
    if (prerender_dock_) {
        addDockWidget(Qt::RightDockWidgetArea, prerender_dock_);
        if (layer_props_dock_)
            splitDockWidget(layer_props_dock_, prerender_dock_, Qt::Vertical);
        prerender_dock_->hide();
    }

    if (tools_dock_)
        tools_dock_->raise();
    if (graphic_props_dock_)
        graphic_props_dock_->raise();
    if (styles_dock_)
        styles_dock_->raise();
    if (layer_props_dock_)
        layer_props_dock_->raise();

    const auto set_visibility_action = [](QAction *action, bool visible) {
        if (!action)
            return;
        QSignalBlocker blocker(action);
        action->setChecked(visible);
    };
    set_visibility_action(act_tools_visible_, true);
    set_visibility_action(act_graphic_props_visible_, true);
    set_visibility_action(act_layer_props_visible_, true);
    set_visibility_action(act_effects_visible_, true);
    set_visibility_action(act_effects_presets_visible_, true);
    set_visibility_action(act_styles_visible_, true);
    set_visibility_action(act_timeline_visible_, true);
    set_visibility_action(act_prerender_visible_, false);
    if (act_lock_panels_) {
        QSignalBlocker blocker(act_lock_panels_);
        act_lock_panels_->setChecked(false);
    }

    QSize default_size(1680, 900);
    QScreen *target_screen = screen();
    if (!target_screen && windowHandle())
        target_screen = windowHandle()->screen();
    if (!target_screen)
        target_screen = QApplication::primaryScreen();
    if (target_screen) {
        const QSize available = target_screen->availableGeometry().size();
        const QSize safe_max(std::max(minimumWidth(), static_cast<int>(available.width() * 0.96)),
                             std::max(minimumHeight(), static_cast<int>(available.height() * 0.94)));
        default_size = default_size.boundedTo(safe_max);
    }
    default_size = default_size.expandedTo(minimumSize());
    resize(default_size);

    const auto apply_default_sizes = [this]() {
        if (tools_dock_ && graphic_props_dock_ && layer_props_dock_)
            resizeDocks({tools_dock_, graphic_props_dock_, layer_props_dock_},
                        {46, 280, 285}, Qt::Horizontal);
        if (graphic_props_dock_ && styles_dock_ && effects_presets_dock_)
            resizeDocks({graphic_props_dock_, styles_dock_, effects_presets_dock_},
                        {105, 320, 245}, Qt::Vertical);
        if (effects_presets_dock_ && effects_dock_)
            resizeDocks({effects_presets_dock_, effects_dock_}, {160, 130}, Qt::Horizontal);
        if (timeline_dock_)
            resizeDocks({timeline_dock_}, {230}, Qt::Vertical);
    };
    apply_default_sizes();

    update_panel_lock_state();
    restoring_editor_layout_ = false;
    save_editor_layout();

    /* QMainWindow finalizes nested dock geometry after the event loop resumes.
     * Re-apply the proportions once so the first visible frame matches the
     * intended workspace, then persist the settled state. */
    QTimer::singleShot(0, this, [this, apply_default_sizes]() {
        apply_default_sizes();
        save_editor_layout();
    });
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

    for (auto *dock : {tools_dock_, graphic_props_dock_, layer_props_dock_, effects_dock_, effects_presets_dock_, styles_dock_, timeline_dock_, prerender_dock_}) {
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
    layer.origin_prop.static_value.x = layer.origin_x;
    layer.origin_prop.static_value.y = layer.origin_y;
    layer.text_overflow_mode = paragraph_box ? 0 : 1;
    layer.text_box_width_to_text = !paragraph_box;
    layer.text_box_height_to_text = !paragraph_box;
    layer.max_text_box_width = 9999.0f;
    layer.max_text_box_height = 9999.0f;
    layer.rich_text.default_paragraph_format.align_h = layer.align_h;
    layer.rich_text.default_paragraph_format.align_v = layer.align_v;
}

static void set_new_text_layer_contents_empty(Layer &layer)
{
    layer.text_content.clear();
    layer.rich_text = rich_text_document_from_layer_defaults(layer);
    layer.rich_text.plain_text.clear();
    layer.rich_text.ranges.clear();
    layer.rich_text.blocks.clear();
    layer.rich_text.selection = {0, 0};
}

static bool text_has_visible_name_content(const QString &text)
{
    for (const QChar ch : text) {
        if (ch.isSpace()) continue;
        const QChar::Category category = ch.category();
        if (category == QChar::Mark_NonSpacing ||
            category == QChar::Mark_SpacingCombining ||
            category == QChar::Mark_Enclosing ||
            category == QChar::Other_Format ||
            category == QChar::Other_Control)
            continue;
        return true;
    }
    return false;
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
                  (type == LayerType::Image) ? editor_text_std("OBSTitles.Image") :
                  (type == LayerType::Adjustment) ? editor_text_std("OBSTitles.AdjustmentLayer") :
                  (type == LayerType::ColorSolid) ? editor_text_std("OBSTitles.ColorSolid") :
                  (type == LayerType::Group) ? editor_text_std("OBSTitles.Group") : editor_text_std("OBSTitles.Shape");
    }
    l->name = unique_layer_name(l->name);
    l->type = type;
    l->text_content = (type == LayerType::Text) ? editor_text_std("OBSTitles.NewText") :
                      (type == LayerType::Ticker) ? editor_text_std("OBSTitles.NewTickerText") : "";
    l->rich_text = rich_text_document_from_layer_defaults(*l);
    if (type == LayerType::Text) {
        set_new_text_layer_contents_empty(*l);
        set_text_layer_direction_defaults(*l, false, false);
    }
    l->clock_format = (type == LayerType::Clock) ? "H:i:s" : l->clock_format;
    l->position.static_value.x = title_->width / 2.0;
    l->position.static_value.y = title_->height / 2.0;
    const bool full_canvas_layer = type == LayerType::Adjustment || type == LayerType::ColorSolid;
    l->rect_width = full_canvas_layer ? static_cast<float>(title_->width) : title_->width * 0.5f;
    l->rect_height = full_canvas_layer ? static_cast<float>(title_->height) :
                     ((type == LayerType::Image) ? title_->height * 0.4f : 160.0f);
    l->size.static_value.x = l->rect_width;
    l->size.static_value.y = l->rect_height;
    if (type == LayerType::Image) {
        l->image_width = l->rect_width;
        l->image_height = l->rect_height;
        l->image_size.static_value.x = l->image_width;
        l->image_size.static_value.y = l->image_height;
    }
    if (type == LayerType::Shape || type == LayerType::SolidRect ||
        type == LayerType::Adjustment || type == LayerType::ColorSolid)
        l->lock_aspect_ratio = false;
    l->origin_prop.static_value.x = l->origin_x;
    l->origin_prop.static_value.y = l->origin_y;
    apply_new_layer_defaults(*l);
    if (type == LayerType::ColorSolid) {
        l->shape_type = ShapeType::Rectangle;
        l->fill_type = 0;
        l->outline_enabled = false;
        l->stroke_width = 0.0f;
    }
    set_channel_statics(*l, true, l->text_color);
    set_channel_statics(*l, false, l->fill_color);
    l->out_time = title_->duration;
    return l;
}

void TitleEditor::create_shape_layer_from_canvas(ShapeType shape_type, const QPointF &canvas_pt)
{
    if (!title_) return;
    if (shape_type == ShapeType::Line || shape_type == ShapeType::Path)
        shape_type = ShapeType::Rectangle;

    auto layer = create_basic_layer(LayerType::Shape, shape_display_name(shape_type));
    if (!layer) return;
    layer->shape_type = shape_type;
    layer->position.static_value.x = canvas_pt.x();
    layer->position.static_value.y = canvas_pt.y();
    if (shape_type == ShapeType::Ellipse || shape_type == ShapeType::Triangle ||
        shape_type == ShapeType::Star || shape_type == ShapeType::Polygon ||
        shape_type == ShapeType::Diamond) {
        const float size = std::min(layer->rect_width, layer->rect_height);
        layer->rect_width = size;
        layer->rect_height = size;
        layer->size.static_value.x = layer->rect_width;
        layer->size.static_value.y = layer->rect_height;
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


void TitleEditor::create_pen_path_layer_from_canvas(const std::vector<BezierPathPoint> &canvas_points, bool closed)
{
    if (!title_ || canvas_points.size() < 2)
        return;

    QPainterPath canvas_path;
    canvas_path.moveTo(canvas_points.front().x, canvas_points.front().y);
    const size_t segment_count = closed ? canvas_points.size() : canvas_points.size() - 1;
    for (size_t i = 0; i < segment_count; ++i) {
        const size_t next_index = (i + 1) % canvas_points.size();
        const BezierPathPoint &from = canvas_points[i];
        const BezierPathPoint &to = canvas_points[next_index];
        const QPointF from_anchor(from.x, from.y);
        const QPointF to_anchor(to.x, to.y);
        const QPointF control1 = from.has_out ? QPointF(from.out_x, from.out_y) : from_anchor;
        const QPointF control2 = to.has_in ? QPointF(to.in_x, to.in_y) : to_anchor;
        if (from.has_out || to.has_in)
            canvas_path.cubicTo(control1, control2, to_anchor);
        else
            canvas_path.lineTo(to_anchor);
    }
    if (closed)
        canvas_path.closeSubpath();

    QRectF bounds = canvas_path.boundingRect();
    if (bounds.width() < 1.0) {
        const double center_x = bounds.center().x();
        bounds.setLeft(center_x - 0.5);
        bounds.setWidth(1.0);
    }
    if (bounds.height() < 1.0) {
        const double center_y = bounds.center().y();
        bounds.setTop(center_y - 0.5);
        bounds.setHeight(1.0);
    }
    const double width = bounds.width();
    const double height = bounds.height();

    auto layer = create_basic_layer(LayerType::Shape, bgl_tr("OBSTitles.Path"));
    if (!layer)
        return;
    layer->shape_type = ShapeType::Path;
    layer->path_closed = closed;
    layer->path_points = canvas_points;
    layer->position.static_value.x = bounds.center().x();
    layer->position.static_value.y = bounds.center().y();
    layer->rect_width = (float)width;
    layer->rect_height = (float)height;
    layer->size.static_value.x = width;
    layer->size.static_value.y = height;
    layer->origin_x = 0.5f;
    layer->origin_y = 0.5f;
    layer->origin_prop.static_value = {0.5, 0.5};

    auto normalize = [&](double x, double y) {
        return QPointF((x - bounds.left()) / width,
                       (y - bounds.top()) / height);
    };
    for (auto &point : layer->path_points) {
        const QPointF anchor = normalize(point.x, point.y);
        const QPointF in_handle = point.has_in ? normalize(point.in_x, point.in_y) : anchor;
        const QPointF out_handle = point.has_out ? normalize(point.out_x, point.out_y) : anchor;
        point.x = anchor.x(); point.y = anchor.y();
        point.in_x = in_handle.x(); point.in_y = in_handle.y();
        point.out_x = out_handle.x(); point.out_y = out_handle.y();
    }

    title_->add_layer(layer);
    layers_->refresh();
    timeline_->set_title(title_);
    on_layer_selected(layer->id);
    force_next_title_visual_update();
    on_title_modified();
}


void TitleEditor::create_text_layer_from_canvas(LayerType type, const QPointF &canvas_pt)
{
    if (!title_) return;
    if (type != LayerType::Text && type != LayerType::Clock && type != LayerType::Ticker)
        type = LayerType::Text;

    auto layer = create_basic_layer(type, text_tool_display_name(type));
    if (!layer) return;
    layer->position.static_value.x = canvas_pt.x();
    layer->position.static_value.y = canvas_pt.y();
    if (type == LayerType::Text) {
        set_new_text_layer_contents_empty(*layer);
        set_text_layer_direction_defaults(*layer, false, false);
    }

    canvas_created_shape_layer_id_ = layer->id;
    title_->add_layer(layer);
    if (type == LayerType::Text)
        pending_text_layer_auto_names_.insert(layer->id);
    layers_->refresh();
    on_layer_selected(layer->id);
}



void TitleEditor::create_image_layer_from_canvas(const QPointF &canvas_pt)
{
    if (!title_) return;

    auto layer = create_basic_layer(LayerType::Image, bgl_tr("OBSTitles.Image"));
    if (!layer) return;
    layer->position.static_value.x = canvas_pt.x();
    layer->position.static_value.y = canvas_pt.y();
    layer->image_path.clear();
    layer->image_size_auto_fit = true;
    layer->lock_aspect_ratio = true;

    canvas_created_shape_layer_id_ = layer->id;
    title_->add_layer(layer);
    layers_->refresh();
    on_layer_selected(layer->id);
}

void TitleEditor::choose_image_file_for_layer(const std::string &layer_id)
{
    if (!title_ || layer_id.empty()) return;
    auto layer = title_->find_layer(layer_id);
    if (!layer || layer->type != LayerType::Image) return;

    const QString path = QFileDialog::getOpenFileName(
        this, bgl_tr("OBSTitles.ChooseImage"), QString(),
        bgl_tr("OBSTitles.ImageFileFilter"));
    if (path.isEmpty()) {
        // Keep the empty image layer as a usable canvas placeholder.
        force_next_title_visual_update();
        if (canvas_) canvas_->refresh_preview();
        return;
    }

    bgs::apply_exposed_image_cue_value(*layer, path.toStdString());
    layer->image_path = path.toStdString();
    layer->name = unique_layer_name(QFileInfo(path).completeBaseName().toStdString(), {layer_id});
    layers_->refresh();
    on_layer_selected(layer_id);
    force_next_title_visual_update();
    on_title_modified();
}

void TitleEditor::create_image_layer_from_external_source(const QString &image_path, const QPointF &canvas_pt)
{
    if (!title_ || image_path.trimmed().isEmpty())
        return;

    auto layer = create_basic_layer(LayerType::Image, QFileInfo(image_path).completeBaseName());
    if (!layer)
        return;

    layer->image_path = image_path.toStdString();
    layer->lock_aspect_ratio = true;
    layer->position.static_value.x = canvas_pt.x();
    layer->position.static_value.y = canvas_pt.y();

    QSize image_size = editor_image_intrinsic_size(image_path);
    if (image_size.isValid() && !image_size.isEmpty()) {
        layer->image_width = (float)image_size.width();
        layer->image_height = (float)image_size.height();
        layer->image_size.static_value.x = layer->image_width;
        layer->image_size.static_value.y = layer->image_height;
        const double max_w = title_->width * 0.65;
        const double max_h = title_->height * 0.65;
        const double scale = std::min({1.0, max_w / std::max(1, image_size.width()), max_h / std::max(1, image_size.height())});
        layer->rect_width = (float)std::max(1.0, image_size.width() * scale);
        layer->rect_height = (float)std::max(1.0, image_size.height() * scale);
        layer->size.static_value.x = layer->rect_width;
        layer->size.static_value.y = layer->rect_height;
    }

    title_->add_layer(layer);
    layers_->refresh();
    on_layer_selected(layer->id);
    force_next_title_visual_update();
    on_title_modified();
}

void TitleEditor::create_text_layer_from_external_source(const QString &text, const QPointF &canvas_pt)
{
    if (!title_ || text.isEmpty())
        return;

    const QString normalized = text.left(20000);
    auto layer = create_basic_layer(LayerType::Text, QString());
    if (!layer)
        return;

    layer->position.static_value.x = canvas_pt.x();
    layer->position.static_value.y = canvas_pt.y();
    layer->text_content = normalized.toStdString();
    layer->rich_text = rich_text_document_from_layer_defaults(*layer);
    rich_text_document_replace_text(layer->rich_text, layer->text_content);
    rich_text_document_sync_layer_mirrors(*layer);
    set_text_layer_direction_defaults(*layer, normalized.contains(QLatin1Char('\n')), text_has_rtl_direction(normalized));
    layer->name = unique_layer_name(normalized.simplified().left(40).toStdString());

    title_->add_layer(layer);
    layers_->refresh();
    on_layer_selected(layer->id);
    force_next_title_visual_update();
    on_title_modified();
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
        layer->position.static_value.x = rtl ? rect.right() : rect.left();
        layer->position.static_value.y = rect.top();
    } else {
        layer->position.static_value.x = rect.center().x();
        layer->position.static_value.y = rect.center().y();
    }
    layer->rect_width = (float)width;
    layer->rect_height = (float)height;
    layer->size.static_value.x = layer->rect_width;
    layer->size.static_value.y = layer->rect_height;
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
    const bool is_image_layer = layer->type == LayerType::Image;
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
        force_next_title_visual_update();
        on_title_modified();
        return;
    }

    layers_->refresh();
    timeline_->set_title(title_);
    on_layer_selected(layer_id);
    force_next_title_visual_update();
    on_title_modified();
    if (is_text_layer && canvas_) {
        canvas_->begin_text_edit_for_layer(layer_id);
    } else if (is_image_layer) {
        // Let the canvas/layer list paint the new image box before opening the picker.
        // Cancelling intentionally keeps the layer as an empty image placeholder.
        QTimer::singleShot(0, this, [this, layer_id]() {
            choose_image_file_for_layer(layer_id);
        });
    }
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
    const auto register_editor_shortcut = [this](QAction *action,
                                                  const QList<QKeySequence> &shortcuts) {
        if (!action)
            return;
        action->setShortcuts(shortcuts);
        action->setShortcutContext(Qt::WindowShortcut);
        /* The menu bar is embedded in the editor layout instead of being the
         * QMainWindow's native menu bar. Associate every shortcut action with
         * the editor itself so Qt does not limit it to an open QMenu.
         * WindowShortcut is required because the GPU canvas is QWindow-backed
         * and therefore is not always a QWidget child of the editor. */
        addAction(action);
    };
    auto *file_menu = menu_bar->addMenu(bgl_tr("OBSTitles.FileMenu"));
    QAction *new_action = file_menu->addAction(bgl_tr("OBSTitles.New"));
    register_editor_shortcut(new_action, {QKeySequence(QKeySequence::New)});
    connect(new_action, &QAction::triggered, this, &TitleEditor::new_title_contents);
    QAction *save_action = file_menu->addAction(obs_icon("save.svg"), bgl_tr("OBSTitles.Save"));
    register_editor_shortcut(save_action, {QKeySequence(QKeySequence::Save)});
    connect(save_action, &QAction::triggered, this, &TitleEditor::save_title);
    QAction *save_as_new_action = file_menu->addAction(bgl_tr("OBSTitles.SaveAsNew"));
    connect(save_as_new_action, &QAction::triggered, this, &TitleEditor::save_title_as_new);
    QAction *save_library_action = file_menu->addAction(bgl_tr("OBSTitles.SaveInLibrary"));
    connect(save_library_action, &QAction::triggered, this, [this]() { export_title_template(true); });
    QAction *save_asset_action = file_menu->addAction(bgl_tr("OBSTitles.SaveAsAsset"));
    connect(save_asset_action, &QAction::triggered, this, &TitleEditor::save_title_as_asset);
    QAction *export_action = file_menu->addAction(obs_icon("export.svg"), bgl_tr("OBSTitles.Export"));
    connect(export_action, &QAction::triggered, this, [this]() { export_title_template(false); });
    file_menu->addSeparator();
    QAction *exit_action = file_menu->addAction(obs_icon("file-exit.svg"), bgl_tr("OBSTitles.Exit"));
    connect(exit_action, &QAction::triggered, this, &TitleEditor::close);

    auto *edit_menu = menu_bar->addMenu(bgl_tr("OBSTitles.EditMenu"));
    edit_menu->addAction(act_undo_ = new QAction(obs_icon("undo.svg"), bgl_tr("OBSTitles.Undo"), this));
    register_editor_shortcut(act_undo_, {QKeySequence(QKeySequence::Undo)});
    connect(act_undo_, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget()))
            return;
        if (undo_index_ > 0)
            restore_undo_snapshot(undo_index_ - 1);
    });
    edit_menu->addAction(act_redo_ = new QAction(obs_icon("redo.svg"), bgl_tr("OBSTitles.Redo"), this));
    register_editor_shortcut(act_redo_, {QKeySequence(QKeySequence::Redo),
                                            QKeySequence(QStringLiteral("Ctrl+Shift+Z"))});
    connect(act_redo_, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget()))
            return;
        if (undo_index_ + 1 < (int)undo_stack_.size())
            restore_undo_snapshot(undo_index_ + 1);
    });
    edit_menu->addSeparator();
    QAction *copy_action = edit_menu->addAction(bgl_tr("OBSTitles.Copy"));
    register_editor_shortcut(copy_action, {QKeySequence(QKeySequence::Copy)});
    connect(copy_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_transition_target_selection()) {
            timeline_->copy_transition_selection();
            return;
        }
        if (timeline_ && timeline_->has_selected_keyframes()) {
            timeline_->copy_keyframe_selection();
            return;
        }
        copy_selected_layer();
    });
    QAction *cut_action = edit_menu->addAction(bgl_tr("OBSTitles.Cut"));
    register_editor_shortcut(cut_action, {QKeySequence(QKeySequence::Cut)});
    connect(cut_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_transition_target_selection()) {
            timeline_->cut_transition_selection();
            return;
        }
        if (timeline_ && timeline_->has_selected_keyframes()) {
            timeline_->cut_keyframe_selection();
            return;
        }
        cut_selected_layer();
    });
    QAction *paste_action = edit_menu->addAction(bgl_tr("OBSTitles.Paste"));
    register_editor_shortcut(paste_action, {QKeySequence(QKeySequence::Paste)});
    connect(paste_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_transition_target_selection()) {
            timeline_->paste_transition_to_selection();
            return;
        }
        if (timeline_ && timeline_->has_keyframe_clipboard()) {
            timeline_->paste_keyframes_at_playhead();
            return;
        }
        if (!layer_clipboard_.empty())
            paste_layer_from_clipboard();
        else
            paste_external_clipboard_to_canvas();
    });
    QAction *delete_action = edit_menu->addAction(bgl_tr("OBSTitles.Delete"));
    register_editor_shortcut(delete_action, {QKeySequence(QKeySequence::Delete),
                                                QKeySequence(Qt::Key_Backspace)});
    connect(delete_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        if (timeline_ && timeline_->has_transition_target_selection()) {
            timeline_->delete_transition_selection();
            return;
        }
        if (timeline_ && timeline_->has_selected_keyframes()) {
            timeline_->delete_keyframe_selection();
            return;
        }
        delete_selected_layer();
    });
    QAction *duplicate_action = edit_menu->addAction(bgl_tr("OBSTitles.DuplicateLayer"));
    register_editor_shortcut(duplicate_action,
                             {QKeySequence(Qt::CTRL | Qt::Key_D)});
    connect(duplicate_action, &QAction::triggered, this, [this]() {
        if (editor_focus_accepts_text(focusWidget())) return;
        duplicate_selected_layers();
    });
    QAction *group_action = edit_menu->addAction(bgl_tr("OBSTitles.GroupLayers"));
    register_editor_shortcut(group_action, {QKeySequence(Qt::CTRL | Qt::Key_G)});
    connect(group_action, &QAction::triggered, this, [this]() {
        if (!editor_focus_accepts_text(focusWidget()))
            group_selected_layers();
    });
    QAction *ungroup_action = edit_menu->addAction(bgl_tr("OBSTitles.UngroupLayers"));
    register_editor_shortcut(ungroup_action,
                             {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G)});
    connect(ungroup_action, &QAction::triggered, this, [this]() {
        if (!editor_focus_accepts_text(focusWidget()))
            ungroup_selected_layers();
    });
    edit_menu->addSeparator();
    QAction *preferences_action = edit_menu->addAction(bgl_tr("OBSTitles.Preferences"));
    connect(preferences_action, &QAction::triggered, this, &TitleEditor::show_preferences);

    auto *view_menu = menu_bar->addMenu(bgl_tr("OBSTitles.View"));
    act_rulers_visible_ = view_menu->addAction(bgl_tr("OBSTitles.Rulers"));
    act_rulers_visible_->setCheckable(true);
    register_editor_shortcut(act_rulers_visible_,
                             {QKeySequence(Qt::CTRL | Qt::Key_R)});
    act_rulers_visible_->setToolTip(bgl_tr("OBSTitles.RulersTooltip"));
    connect(act_rulers_visible_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_rulers_visible(visible);
    });

    act_guides_visible_ = view_menu->addAction(bgl_tr("OBSTitles.Guides"));
    act_guides_visible_->setCheckable(true);
    act_guides_visible_->setChecked(true);
    register_editor_shortcut(act_guides_visible_,
                             {QKeySequence(Qt::CTRL | Qt::Key_QuoteLeft)});
    act_guides_visible_->setToolTip(bgl_tr("OBSTitles.GuidesTooltip"));
    connect(act_guides_visible_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_guides_visible(visible);
    });

    act_guides_locked_ = view_menu->addAction(bgl_tr("OBSTitles.LockGuides"));
    act_guides_locked_->setCheckable(true);
    act_guides_locked_->setToolTip(bgl_tr("OBSTitles.LockGuidesTooltip"));
    connect(act_guides_locked_, &QAction::toggled, this, [this](bool locked) {
        if (canvas_) canvas_->set_guides_locked(locked);
    });

    act_guide_coordinates_ = view_menu->addAction(bgl_tr("OBSTitles.ShowGuideCoordinates"));
    act_guide_coordinates_->setCheckable(true);
    act_guide_coordinates_->setChecked(true);
    connect(act_guide_coordinates_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_show_guide_coordinates(visible);
    });

