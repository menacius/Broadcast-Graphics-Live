QWidget *TitleEditor::create_effects_presets_panel()
{
    effects_presets_panel_ = new EffectsPresetsPanel(this);
    connect(effects_presets_panel_, &EffectsPresetsPanel::effect_preset_activated,
            this, [this](const QString &file_path) {
                apply_effect_preset_to_layer(file_path, sel_layer_id_);
            });
    return effects_presets_panel_;
}

QWidget *TitleEditor::create_prerender_panel()
{
    prerender_panel_ = new PrerenderDock(this);
    connect(prerender_panel_, &PrerenderDock::cacheWorkAreaRequested, this, [this]() {
        if (timeline_) timeline_->update();
    });
    connect(prerender_panel_, &PrerenderDock::cacheEntireTimelineRequested, this, [this]() {
        if (timeline_) timeline_->update();
    });
    return prerender_panel_;
}

void TitleEditor::activate_editor_tool(EditorTool tool)
{
    current_editor_tool_ = tool;
    if (tools_sidebar_) {
        switch (tool) {
        case EditorTool::Selection:
            tools_sidebar_->activate_selection_tool();
            return;
        case EditorTool::DirectSelection:
            tools_sidebar_->activate_direct_selection_tool();
            return;
        case EditorTool::FreeTransform:
            tools_sidebar_->activate_free_transform_tool();
            return;
        case EditorTool::Shape:
            tools_sidebar_->activate_shape_tool(tools_sidebar_->selected_shape());
            return;
        case EditorTool::Pen:
            tools_sidebar_->activate_pen_tool();
            return;
        case EditorTool::Text:
            tools_sidebar_->activate_text_tool(tools_sidebar_->selected_text_layer_type());
            return;
        case EditorTool::Image:
            tools_sidebar_->activate_image_tool();
            return;
        case EditorTool::ColorPicker:
            tools_sidebar_->activate_color_picker_tool();
            return;
        case EditorTool::Gradient:
            tools_sidebar_->activate_gradient_tool();
            return;
        }
    }

    if (!canvas_)
        return;
    switch (tool) {
    case EditorTool::Selection:
        canvas_->set_selection_tool_active();
        break;
    case EditorTool::DirectSelection:
        canvas_->set_direct_selection_tool_active();
        break;
    case EditorTool::FreeTransform:
        canvas_->set_free_transform_tool_active(0);
        break;
    case EditorTool::Shape:
        canvas_->set_shape_tool_active(ShapeType::Rectangle);
        break;
    case EditorTool::Pen:
        canvas_->set_pen_tool_active();
        break;
    case EditorTool::Text:
        canvas_->set_text_tool_active(LayerType::Text);
        break;
    case EditorTool::Image:
        canvas_->set_image_tool_active();
        break;
    case EditorTool::ColorPicker:
        canvas_->set_color_picker_tool_active();
        break;
    case EditorTool::Gradient:
        canvas_->set_gradient_tool_active();
        break;
    }
}

void TitleEditor::handle_gradient_editor_tool_request(bool active)
{
    if (canvas_)
        canvas_->set_gradient_editor_active(active);

    if (active) {
        if (!gradient_editor_tool_override_active_) {
            tool_before_gradient_editor_ = current_editor_tool_;
            gradient_editor_tool_override_active_ = true;
        }
        /* CanvasPreview suspends (rather than commits) the inline QTextEdit,
         * so the selected rich-text range remains canonical while the popup
         * and on-canvas handles edit the same gradient. */
        activate_editor_tool(EditorTool::Gradient);
        return;
    }

    if (!gradient_editor_tool_override_active_)
        return;

    const EditorTool restore_tool = tool_before_gradient_editor_;
    gradient_editor_tool_override_active_ = false;
    activate_editor_tool(restore_tool);
}

QWidget *TitleEditor::create_styles_panel()
{
    auto *tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("BroadcastGraphicsLiveStylesTabs"));
    tabs->setDocumentMode(true);

    auto *text_styles = new obsbgs::StylePresetPanel(obsbgs::StylePresetKind::Text, tabs);
    text_styles->setCreatePresetCallback([this](const QString &name, const QString &category) {
        Layer fallback;
        if (title_ && !sel_layer_id_.empty()) {
            if (auto layer = title_->find_layer(sel_layer_id_)) {
                if (active_text_edit_layer_id_ == layer->id) {
                    RichTextCharFormatSummary summary = summarize_rich_text_char_format(
                        *layer, true, std::max(0.0, playhead_ - layer->in_time));
                    if (summary.valid) {
                        Layer inline_style = *layer;
                        inline_style.font_family = summary.format.font_family;
                        inline_style.font_style = summary.format.font_style;
                        inline_style.font_size = summary.format.font_size;
                        inline_style.font_bold = summary.format.bold;
                        inline_style.font_italic = summary.format.italic;
                        inline_style.text_underline = summary.format.underline;
                        inline_style.text_strikethrough = summary.format.strikethrough;
                        inline_style.char_tracking = summary.format.tracking;
                        inline_style.char_scale_x = summary.format.scale_x;
                        inline_style.char_scale_y = summary.format.scale_y;
                        inline_style.baseline_shift = summary.format.baseline_shift;
                        inline_style.fill_type = summary.format.fill.type;
                        inline_style.text_color = summary.format.fill.color;
                        inline_style.fill_color = summary.format.fill.color;
                        inline_style.gradient_type = summary.format.fill.gradient_type;
                        inline_style.gradient_spread = summary.format.fill.gradient_spread;
                        inline_style.gradient_start_color = summary.format.fill.gradient_start_color;
                        inline_style.gradient_end_color = summary.format.fill.gradient_end_color;
                        inline_style.gradient_start_pos = summary.format.fill.gradient_start_pos;
                        inline_style.gradient_end_pos = summary.format.fill.gradient_end_pos;
                        inline_style.gradient_start_opacity = summary.format.fill.gradient_start_opacity;
                        inline_style.gradient_end_opacity = summary.format.fill.gradient_end_opacity;
                        inline_style.gradient_opacity = summary.format.fill.gradient_opacity;
                        inline_style.gradient_angle = summary.format.fill.gradient_angle;
                        inline_style.gradient_center_x = summary.format.fill.gradient_center_x;
                        inline_style.gradient_center_y = summary.format.fill.gradient_center_y;
                        inline_style.gradient_scale = summary.format.fill.gradient_scale;
                        inline_style.gradient_focal_x = summary.format.fill.gradient_focal_x;
                        inline_style.gradient_focal_y = summary.format.fill.gradient_focal_y;
                        return obsbgs::StylePresetLibrary::makeTextPreset(inline_style, name, category);
                    }
                }
                return obsbgs::StylePresetLibrary::makeTextPreset(*layer, name, category);
            }
        }
        return obsbgs::StylePresetLibrary::makeTextPreset(fallback, name, category);
    });
    text_styles->setApplyPresetCallback([this](const obsbgs::StylePreset &preset) {
        if (!title_ || sel_layer_id_.empty()) return;
        if (auto layer = title_->find_layer(sel_layer_id_)) {
            if (layer->type != LayerType::Text && layer->type != LayerType::Clock && layer->type != LayerType::Ticker)
                return;
            push_undo_snapshot();
            const bool apply_inline = active_text_edit_layer_id_ == layer->id;
            if (apply_inline) {
                RichTextCharFormat fmt = layer_char_format_for_editor(*layer);
                if (!obsbgs::StylePresetLibrary::textPresetToCharFormat(preset, fmt))
                    return;
                const uint32_t mask = obsbgs::StylePresetLibrary::textPresetCharMask();
                apply_rich_text_format_to_layer_range(*layer, fmt, mask, true);
                if (canvas_) canvas_->apply_active_text_char_format(layer->id, fmt, mask);
                on_title_modified(false);
                update_layer_panels(layer, playhead_);
                if (layers_) layers_->refresh();
                if (canvas_) canvas_->update();
                return;
            }
            RichTextCharFormat preset_char = layer_char_format_for_editor(*layer);
            if (!obsbgs::StylePresetLibrary::textPresetToCharFormat(preset, preset_char))
                return;
            RichTextParagraphFormat preset_paragraph = layer_paragraph_format_for_editor(*layer);
            const auto payload = preset.payload;
            preset_paragraph.align_h = payload.value(QStringLiteral("alignH")).toInt(preset_paragraph.align_h);
            preset_paragraph.align_v = std::clamp(
                payload.value(QStringLiteral("alignV")).toInt(preset_paragraph.align_v), 0, 3);
            preset_paragraph.line_spacing = float(payload.value(QStringLiteral("leading")).toDouble(preset_paragraph.line_spacing));
            preset_paragraph.space_before = float(payload.value(QStringLiteral("paragraphBefore")).toDouble(preset_paragraph.space_before));
            preset_paragraph.space_after = float(payload.value(QStringLiteral("paragraphAfter")).toDouble(preset_paragraph.space_after));
            preset_paragraph.indent_left = float(payload.value(QStringLiteral("paragraphLeft")).toDouble(preset_paragraph.indent_left));
            preset_paragraph.indent_right = float(payload.value(QStringLiteral("paragraphRight")).toDouble(preset_paragraph.indent_right));
            preset_paragraph.indent_first_line = float(payload.value(QStringLiteral("paragraphFirst")).toDouble(preset_paragraph.indent_first_line));
            if (obsbgs::StylePresetLibrary::applyTextPreset(preset, *layer)) {
                apply_rich_text_format_to_layer_range(
                    *layer, preset_char,
                    obsbgs::StylePresetLibrary::textPresetCharMask(), false);
                apply_rich_text_paragraph_format_to_layer(
                    *layer, preset_paragraph,
                    RichTextParagraphAlignH | RichTextParagraphAlignV |
                        RichTextParagraphLineSpacing | RichTextParagraphSpaceBefore |
                        RichTextParagraphSpaceAfter | RichTextParagraphIndentLeft |
                        RichTextParagraphIndentRight | RichTextParagraphIndentFirstLine,
                    false);
                on_title_modified(false);
                update_layer_panels(layer, playhead_);
                if (layers_) layers_->refresh();
                if (canvas_) canvas_->update();
            }
        }
    });

    auto *gradient_styles = new obsbgs::StylePresetPanel(obsbgs::StylePresetKind::Gradient, tabs);
    gradient_styles->setCreatePresetCallback([this](const QString &name, const QString &category) {
        Layer fallback;
        fallback.fill_type = 1;
        if (title_ && !sel_layer_id_.empty()) {
            if (auto layer = title_->find_layer(sel_layer_id_)) {
                if (active_text_edit_layer_id_ == layer->id &&
                    (layer->type == LayerType::Text || layer->type == LayerType::Clock || layer->type == LayerType::Ticker)) {
                    RichTextCharFormatSummary summary = summarize_rich_text_char_format(
                        *layer, true, std::max(0.0, playhead_ - layer->in_time));
                    if (summary.valid) {
                        Layer inline_gradient = *layer;
                        inline_gradient.fill_type = summary.format.fill.type;
                        inline_gradient.text_color = summary.format.fill.color;
                        inline_gradient.fill_color = summary.format.fill.color;
                        inline_gradient.gradient_type = summary.format.fill.gradient_type;
                        inline_gradient.gradient_spread = summary.format.fill.gradient_spread;
                        inline_gradient.gradient_start_color = summary.format.fill.gradient_start_color;
                        inline_gradient.gradient_end_color = summary.format.fill.gradient_end_color;
                        inline_gradient.gradient_start_pos = summary.format.fill.gradient_start_pos;
                        inline_gradient.gradient_end_pos = summary.format.fill.gradient_end_pos;
                        inline_gradient.gradient_start_opacity = summary.format.fill.gradient_start_opacity;
                        inline_gradient.gradient_end_opacity = summary.format.fill.gradient_end_opacity;
                        inline_gradient.gradient_opacity = summary.format.fill.gradient_opacity;
                        inline_gradient.gradient_angle = summary.format.fill.gradient_angle;
                        inline_gradient.gradient_center_x = summary.format.fill.gradient_center_x;
                        inline_gradient.gradient_center_y = summary.format.fill.gradient_center_y;
                        inline_gradient.gradient_scale = summary.format.fill.gradient_scale;
                        inline_gradient.gradient_focal_x = summary.format.fill.gradient_focal_x;
                        inline_gradient.gradient_focal_y = summary.format.fill.gradient_focal_y;
                        return obsbgs::StylePresetLibrary::makeGradientPreset(inline_gradient, name, category);
                    }
                }
                return obsbgs::StylePresetLibrary::makeGradientPreset(*layer, name, category);
            }
        }
        return obsbgs::StylePresetLibrary::makeGradientPreset(fallback, name, category);
    });
    gradient_styles->setApplyPresetCallback([this](const obsbgs::StylePreset &preset) {
        if (!title_ || sel_layer_id_.empty()) return;
        if (auto layer = title_->find_layer(sel_layer_id_)) {
            push_undo_snapshot();
            const bool apply_inline = active_text_edit_layer_id_ == layer->id &&
                                      (layer->type == LayerType::Text || layer->type == LayerType::Clock || layer->type == LayerType::Ticker);
            if (apply_inline) {
                RichTextCharFormat fmt = layer_char_format_for_editor(*layer);
                if (!obsbgs::StylePresetLibrary::gradientPresetToCharFormat(preset, fmt))
                    return;
                const uint32_t mask = obsbgs::StylePresetLibrary::gradientPresetCharMask();
                apply_rich_text_format_to_layer_range(*layer, fmt, mask, true);
                if (canvas_) canvas_->apply_active_text_char_format(layer->id, fmt, mask);
                on_title_modified(false);
                update_layer_panels(layer, playhead_);
                update_sidebar_color_swatches(layer);
                if (layers_) layers_->refresh();
                if (canvas_) canvas_->update();
                return;
            }
            const bool rich_text_layer = layer->type == LayerType::Text ||
                                         layer->type == LayerType::Clock ||
                                         layer->type == LayerType::Ticker;
            RichTextCharFormat preset_fill;
            if (rich_text_layer) {
                preset_fill = layer_char_format_for_editor(*layer);
                if (!obsbgs::StylePresetLibrary::gradientPresetToCharFormat(preset, preset_fill))
                    return;
            }
            if (obsbgs::StylePresetLibrary::applyGradientPreset(preset, *layer)) {
                if (rich_text_layer) {
                    apply_rich_text_format_to_layer_range(
                        *layer, preset_fill,
                        obsbgs::StylePresetLibrary::gradientPresetCharMask(), false);
                }
                on_title_modified(false);
                update_layer_panels(layer, playhead_);
                update_sidebar_color_swatches(layer);
                if (layers_) layers_->refresh();
                if (canvas_) canvas_->update();
            }
        }
    });


    auto *assets = new obsbgs::AssetLibraryPanel(false, tabs, false);
    assets->setInsertCallback([this](const std::string &asset_id) {
        const QPointF point = canvas_ ? canvas_->view_center_canvas_point()
                                      : QPointF(title_ ? title_->width * 0.5 : 960.0,
                                                title_ ? title_->height * 0.5 : 540.0);
        insert_asset_layer(asset_id, point);
    });
    assets->setEditCallback([this](const std::string &asset_id) {
        edit_asset(asset_id);
    });
    tabs->addTab(create_color_swatches_panel(), bgl_tr("OBSTitles.ColorSwatches"));
    tabs->addTab(gradient_styles, bgl_tr("OBSTitles.Gradients"));
    tabs->addTab(text_styles, bgl_tr("OBSTitles.TextStyles"));
    tabs->addTab(assets, bgl_tr("OBSTitles.Assets"));

    return tabs;
}

QWidget *TitleEditor::create_color_swatches_panel()
{
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(8);

    auto *recent_label = new QLabel(bgl_tr("OBSTitles.RecentColors"), panel);
    QFont section_font = recent_label->font();
    section_font.setBold(true);
    recent_label->setFont(section_font);
    layout->addWidget(recent_label);

    recent_color_swatches_grid_ = new ResponsiveSwatchGrid(panel);
    recent_color_swatch_buttons_.clear();
    recent_color_swatch_buttons_.reserve(16);
    for (int i = 0; i < 16; ++i) {
        auto *swatch = new QToolButton(recent_color_swatches_grid_);
        swatch->setObjectName(QStringLiteral("BroadcastGraphicsLiveRecentColorSwatch"));
        swatch->setFixedSize(24, 24);
        swatch->setAutoRaise(false);
        connect(swatch, &QToolButton::clicked, this, [this, i]() {
            QStringList recent_hexes = bgl_load_recent_color_hexes();
            if (title_) {
                for (const auto &hex : title_->editor_recent_color_hexes) {
                    const QString value = QString::fromStdString(hex);
                    if (!recent_hexes.contains(value, Qt::CaseInsensitive))
                        recent_hexes << value;
                }
            }
            if (i >= recent_hexes.size())
                return;
            QColor color;
            if (!editor_parse_recent_color_hex(recent_hexes[i], color))
                return;
            apply_picked_color_to_selection(color);
            remember_recent_color(color);
        });
        swatch->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(swatch, &QToolButton::customContextMenuRequested, this, [this, swatch, i](const QPoint &pos) {
            QStringList recent_hexes = bgl_load_recent_color_hexes();
            if (title_) {
                for (const auto &hex : title_->editor_recent_color_hexes) {
                    const QString value = QString::fromStdString(hex);
                    if (!recent_hexes.contains(value, Qt::CaseInsensitive))
                        recent_hexes << value;
                }
            }
            if (i >= recent_hexes.size())
                return;
            QColor color;
            const bool has_color = editor_parse_recent_color_hex(recent_hexes[i], color);
            QMenu menu(swatch);
            QAction *save_action = nullptr;
            if (has_color)
                save_action = menu.addAction(bgl_tr("OBSTitles.AddToLibrary"));
            QAction *remove_action = menu.addAction(bgl_tr("OBSTitles.Delete"));
            QAction *selected = menu.exec(swatch->mapToGlobal(pos));
            if (selected == save_action)
                show_add_color_to_library_dialog(color);
            else if (selected == remove_action)
                remove_recent_color(i);
        });
        recent_color_swatches_grid_->addSwatch(swatch);
        recent_color_swatch_buttons_.push_back(swatch);
    }
    layout->addWidget(recent_color_swatches_grid_, 0);

    auto *library_label = new QLabel(bgl_tr("OBSTitles.ColorLibraries"), panel);
    library_label->setFont(section_font);
    layout->addWidget(library_label);

    auto *library_toolbar = new QWidget(panel);
    auto *library_toolbar_layout = new QHBoxLayout(library_toolbar);
    library_toolbar_layout->setContentsMargins(0, 0, 0, 0);
    library_toolbar_layout->setSpacing(4);
    color_library_combo_ = new QComboBox(library_toolbar);
    color_library_combo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    color_library_add_button_ = new QToolButton(library_toolbar);
    color_library_add_button_->setIcon(obs_icon("add.svg"));
    color_library_add_button_->setToolTip(bgl_tr("OBSTitles.New"));
    color_library_rename_button_ = new QToolButton(library_toolbar);
    color_library_rename_button_->setIcon(obs_icon("settings.svg"));
    color_library_rename_button_->setToolTip(bgl_tr("OBSTitles.Rename"));
    color_library_delete_button_ = new QToolButton(library_toolbar);
    color_library_delete_button_->setIcon(obs_icon("delete.svg"));
    color_library_delete_button_->setToolTip(bgl_tr("OBSTitles.Delete"));
    for (auto *button : {color_library_add_button_, color_library_rename_button_, color_library_delete_button_})
        button->setFixedSize(26, 24);
    library_toolbar_layout->addWidget(color_library_combo_, 1);
    library_toolbar_layout->addWidget(color_library_add_button_);
    library_toolbar_layout->addWidget(color_library_rename_button_);
    library_toolbar_layout->addWidget(color_library_delete_button_);
    layout->addWidget(library_toolbar);

    auto *library_scroll = new QScrollArea(panel);
    library_scroll->setWidgetResizable(true);
    library_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    library_scroll->setMinimumHeight(140);
    color_library_swatch_widget_ = new ResponsiveSwatchGrid(library_scroll);
    color_library_swatches_grid_ = static_cast<ResponsiveSwatchGrid *>(color_library_swatch_widget_);
    library_scroll->setWidget(color_library_swatch_widget_);
    layout->addWidget(library_scroll, 1);

    connect(color_library_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0 || index >= (int)color_libraries_.size())
            return;
        selected_color_library_index_ = index;
        persist_selected_color_library();
        refresh_color_library_controls();
    });
    connect(color_library_add_button_, &QToolButton::clicked, this, &TitleEditor::create_color_library);
    connect(color_library_rename_button_, &QToolButton::clicked, this, &TitleEditor::rename_current_color_library);
    connect(color_library_delete_button_, &QToolButton::clicked, this, &TitleEditor::delete_current_color_library);

    load_color_libraries();
    refresh_color_library_controls();
    refresh_color_swatches_panel();
    return panel;
}

void TitleEditor::remember_recent_color(const QColor &color)
{
    if (!color.isValid() || color.alpha() <= 0)
        return;

    const QString hex = editor_recent_color_hex(color);
    if (hex.isEmpty())
        return;

    QStringList recent_hexes = bgl_load_recent_color_hexes();
    for (int i = recent_hexes.size() - 1; i >= 0; --i) {
        if (recent_hexes[i].compare(hex, Qt::CaseInsensitive) == 0)
            recent_hexes.removeAt(i);
    }
    recent_hexes.prepend(hex);
    while (recent_hexes.size() > 16)
        recent_hexes.removeLast();
    bgl_save_recent_color_hexes(recent_hexes);

    if (title_) {
        title_->editor_recent_color_hexes.clear();
        for (const QString &entry : recent_hexes)
            title_->editor_recent_color_hexes.push_back(entry.toStdString());
    }

    refresh_color_swatches_panel();
    persist_recent_colors();
}

void TitleEditor::remove_recent_color(int index)
{
    QStringList recent_hexes = bgl_load_recent_color_hexes();
    if (title_) {
        for (const auto &hex : title_->editor_recent_color_hexes) {
            const QString value = QString::fromStdString(hex);
            if (!recent_hexes.contains(value, Qt::CaseInsensitive))
                recent_hexes << value;
        }
    }
    if (index < 0 || index >= recent_hexes.size())
        return;

    recent_hexes.removeAt(index);
    bgl_save_recent_color_hexes(recent_hexes);
    if (title_) {
        title_->editor_recent_color_hexes.clear();
        for (const QString &hex : recent_hexes)
            title_->editor_recent_color_hexes.push_back(hex.toStdString());
    }
    refresh_color_swatches_panel();
    persist_recent_colors();
}

void TitleEditor::persist_recent_colors()
{
    if (!title_)
        return;

    auto stored = TitleDataStore::instance().get_title(editing_title_id_.empty() ? title_->id : editing_title_id_);
    if (!stored)
        return;

    stored->editor_recent_color_hexes = title_->editor_recent_color_hexes;
    QStringList recent_hexes;
    for (const auto &hex : title_->editor_recent_color_hexes)
        recent_hexes << QString::fromStdString(hex);
    bgl_save_recent_color_hexes(recent_hexes);
    TitleDataStore::instance().save_async();
}

void TitleEditor::load_color_libraries()
{
    color_libraries_.clear();

    ColorLibrary open_color;
    open_color.name = QStringLiteral("Open Color");
    open_color.slug = QStringLiteral("open-color");
    open_color.built_in = true;
    for (const auto &entry : bgl_open_color_palette())
        open_color.colors.push_back({entry.first, entry.second});
    color_libraries_.push_back(open_color);

    QFile file(editor_color_libraries_path());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        const QJsonArray palettes = doc.object().value(QStringLiteral("palettes")).toArray();
        for (const QJsonValue &value : palettes) {
            const QJsonObject object = value.toObject();
            ColorLibrary library;
            library.name = object.value(QStringLiteral("name")).toString().trimmed();
            library.slug = object.value(QStringLiteral("slug")).toString().trimmed();
            library.built_in = false;
            if (library.name.isEmpty())
                continue;
            if (library.slug.isEmpty())
                library.slug = editor_slugify(library.name);

            const QJsonArray colors = object.value(QStringLiteral("colors")).toArray();
            for (int i = 0; i < colors.size(); ++i) {
                const QJsonObject color_object = colors[i].toObject();
                const QString hex = color_object.value(QStringLiteral("hex")).toString();
                QColor color;
                if (!editor_parse_recent_color_hex(hex, color))
                    continue;
                const QString color_name = color_object.value(QStringLiteral("name")).toString(
                    QStringLiteral("%1 %2").arg(library.name).arg(i + 1));
                library.colors.push_back({color_name, color});
            }
            color_libraries_.push_back(library);
        }
    }

    restore_selected_color_library();
    selected_color_library_index_ = std::clamp(selected_color_library_index_, 0, (int)color_libraries_.size() - 1);
}

void TitleEditor::save_color_libraries() const
{
    QJsonArray palettes;
    for (const ColorLibrary &library : color_libraries_) {
        if (library.built_in)
            continue;
        QJsonObject object;
        object.insert(QStringLiteral("name"), library.name);
        object.insert(QStringLiteral("slug"), library.slug.isEmpty() ? editor_slugify(library.name) : library.slug);
        object.insert(QStringLiteral("type"), QStringLiteral("categorical"));
        QJsonArray colors;
        for (const ColorLibraryColor &entry : library.colors) {
            QJsonObject color;
            if (!entry.name.isEmpty())
                color.insert(QStringLiteral("name"), entry.name);
            color.insert(QStringLiteral("hex"), editor_recent_color_hex(entry.color));
            colors.append(color);
        }
        object.insert(QStringLiteral("colors"), colors);
        palettes.append(object);
    }

    QJsonObject root;
    root.insert(QStringLiteral("palettes"), palettes);
    QFile file(editor_color_libraries_path());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void TitleEditor::restore_selected_color_library()
{
    const QString saved_slug = bgl_load_selected_color_library_slug().trimmed();
    if (saved_slug.isEmpty())
        return;

    for (int i = 0; i < (int)color_libraries_.size(); ++i) {
        if (color_libraries_[i].slug.compare(saved_slug, Qt::CaseInsensitive) == 0) {
            selected_color_library_index_ = i;
            return;
        }
    }
}

void TitleEditor::persist_selected_color_library() const
{
    if (selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
        return;
    const QString slug = color_libraries_[selected_color_library_index_].slug.trimmed();
    if (!slug.isEmpty())
        bgl_save_selected_color_library_slug(slug);
}

void TitleEditor::refresh_color_library_controls()
{
    if (!color_library_combo_)
        return;

    QSignalBlocker blocker(color_library_combo_);
    color_library_combo_->clear();
    for (const ColorLibrary &library : color_libraries_)
        color_library_combo_->addItem(library.name, library.slug);
    selected_color_library_index_ = std::clamp(selected_color_library_index_, 0, std::max(0, (int)color_libraries_.size() - 1));
    color_library_combo_->setCurrentIndex(selected_color_library_index_);

    const bool editable = selected_color_library_index_ >= 0 &&
        selected_color_library_index_ < (int)color_libraries_.size() &&
        !color_libraries_[selected_color_library_index_].built_in;
    if (color_library_rename_button_)
        color_library_rename_button_->setEnabled(editable);
    if (color_library_delete_button_)
        color_library_delete_button_->setEnabled(editable);
    refresh_color_library_swatches();
}

void TitleEditor::refresh_color_library_swatches()
{
    if (!color_library_swatches_grid_ || !color_library_swatch_widget_)
        return;

    color_library_swatches_grid_->clearSwatches();

    if (selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
        return;

    const auto &colors = color_libraries_[selected_color_library_index_].colors;
    for (int i = 0; i < (int)colors.size(); ++i) {
        const ColorLibraryColor entry = colors[i];
        auto *swatch = new QToolButton(color_library_swatch_widget_);
        swatch->setObjectName(QStringLiteral("BroadcastGraphicsLiveLibraryColorSwatch"));
        swatch->setFixedSize(24, 24);
        swatch->setAutoRaise(false);
        swatch->setToolTip(bgl_color_swatch_tooltip(entry.name, entry.color, editor_recent_color_hex(entry.color)));
        swatch->setStyleSheet(editor_color_swatch_style(entry.color));
        connect(swatch, &QToolButton::clicked, this, [this, color = entry.color]() {
            apply_picked_color_to_selection(color);
            remember_recent_color(color);
        });
        swatch->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(swatch, &QToolButton::customContextMenuRequested, this, [this, swatch, i](const QPoint &pos) {
            if (selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
                return;
            if (color_libraries_[selected_color_library_index_].built_in)
                return;
            QMenu menu(swatch);
            QAction *remove_action = menu.addAction(bgl_tr("OBSTitles.Delete"));
            if (menu.exec(swatch->mapToGlobal(pos)) == remove_action)
                remove_color_from_current_library(i);
        });
        color_library_swatches_grid_->addSwatch(swatch);
    }
}

void TitleEditor::create_color_library()
{
    prompt_create_color_library();
}

int TitleEditor::prompt_create_color_library()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, bgl_tr("OBSTitles.ColorLibraries"),
                                               bgl_tr("OBSTitles.Name"), QLineEdit::Normal,
                                               QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return -1;

    ColorLibrary library;
    library.name = name;
    library.slug = editor_slugify(name);
    int suffix = 2;
    auto slug_exists = [&]() {
        return std::any_of(color_libraries_.begin(), color_libraries_.end(), [&](const ColorLibrary &candidate) {
            return candidate.slug.compare(library.slug, Qt::CaseInsensitive) == 0;
        });
    };
    const QString base_slug = library.slug;
    while (slug_exists())
        library.slug = QStringLiteral("%1-%2").arg(base_slug).arg(suffix++);
    color_libraries_.push_back(library);
    selected_color_library_index_ = (int)color_libraries_.size() - 1;
    save_color_libraries();
    persist_selected_color_library();
    refresh_color_library_controls();
    return selected_color_library_index_;
}

void TitleEditor::rename_current_color_library()
{
    if (selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
        return;
    ColorLibrary &library = color_libraries_[selected_color_library_index_];
    if (library.built_in)
        return;

    bool ok = false;
    const QString name = QInputDialog::getText(this, bgl_tr("OBSTitles.ColorLibraries"),
                                               bgl_tr("OBSTitles.Name"), QLineEdit::Normal,
                                               library.name, &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    library.name = name;
    const QString base_slug = editor_slugify(name);
    library.slug = base_slug;
    int suffix = 2;
    auto slug_exists = [&]() {
        for (int i = 0; i < (int)color_libraries_.size(); ++i) {
            if (i == selected_color_library_index_)
                continue;
            if (color_libraries_[i].slug.compare(library.slug, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    while (slug_exists())
        library.slug = QStringLiteral("%1-%2").arg(base_slug).arg(suffix++);
    save_color_libraries();
    persist_selected_color_library();
    refresh_color_library_controls();
}

void TitleEditor::delete_current_color_library()
{
    if (selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
        return;
    if (color_libraries_[selected_color_library_index_].built_in)
        return;

    if (QMessageBox::question(this, bgl_tr("OBSTitles.ColorLibraries"),
                              bgl_tr("OBSTitles.DeleteColorLibraryQuestionFormat").arg(color_libraries_[selected_color_library_index_].name)) != QMessageBox::Yes)
        return;
    color_libraries_.erase(color_libraries_.begin() + selected_color_library_index_);
    selected_color_library_index_ = std::clamp(selected_color_library_index_, 0, std::max(0, (int)color_libraries_.size() - 1));
    save_color_libraries();
    persist_selected_color_library();
    refresh_color_library_controls();
}

void TitleEditor::add_color_to_current_library(const QColor &color)
{
    if (!color.isValid() || selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
        return;
    add_color_to_library(selected_color_library_index_, color, editor_recent_color_hex(color));
}

bool TitleEditor::show_add_color_to_library_dialog(const QColor &color)
{
    if (!color.isValid() || color.alpha() <= 0)
        return false;
    load_color_libraries();

    QDialog dialog(this);
    bgl_apply_brand_icon(&dialog);
    dialog.setWindowTitle(bgl_tr("OBSTitles.AddToLibrary"));
    auto *root = new QHBoxLayout(&dialog);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(12);

    auto *left = new QWidget(&dialog);
    auto *left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(8);

    auto *swatch = new QLabel(left);
    swatch->setFixedSize(96, 96);
    swatch->setStyleSheet(QStringLiteral("QLabel{background:%1;border:1px solid %2;border-radius:4px;}")
                               .arg(color.name(QColor::HexArgb),
                                    qApp->palette().color(QPalette::Mid).name(QColor::HexRgb)));
    swatch->setToolTip(bgl_color_swatch_tooltip(editor_recent_color_hex(color), color, editor_recent_color_hex(color)));
    auto *name_edit = new QLineEdit(editor_recent_color_hex(color), left);
    auto *hex_label = new QLabel(editor_recent_color_hex(color), left);
    hex_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    left_layout->addWidget(swatch, 0, Qt::AlignLeft);
    left_layout->addWidget(name_edit);
    left_layout->addWidget(hex_label);
    left_layout->addStretch(1);

    auto *right = new QWidget(&dialog);
    auto *right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);
    right_layout->setSpacing(8);
    auto *library_list = new QListWidget(right);
    library_list->addItem(bgl_tr("OBSTitles.AddNewLibrary"));
    for (int i = 0; i < (int)color_libraries_.size(); ++i) {
        if (color_libraries_[i].built_in)
            continue;
        auto *item = new QListWidgetItem(color_libraries_[i].name, library_list);
        item->setData(Qt::UserRole, i);
    }
    library_list->setCurrentRow(library_list->count() > 1 ? 1 : 0);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, right);
    right_layout->addWidget(library_list, 1);
    right_layout->addWidget(buttons);

    root->addWidget(left);
    root->addWidget(right, 1);

    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    int library_index = -1;
    if (library_list->currentRow() == 0) {
        library_index = prompt_create_color_library();
    } else if (auto *item = library_list->currentItem()) {
        library_index = item->data(Qt::UserRole).toInt();
    }
    if (library_index < 0)
        return false;

    add_color_to_library(library_index, color, name_edit->text().trimmed());
    selected_color_library_index_ = library_index;
    persist_selected_color_library();
    refresh_color_library_controls();
    return true;
}

void TitleEditor::add_color_to_library(int library_index, const QColor &color, const QString &name)
{
    if (!color.isValid() || library_index < 0 || library_index >= (int)color_libraries_.size())
        return;
    ColorLibrary &library = color_libraries_[library_index];
    if (library.built_in)
        return;

    const QString hex = editor_recent_color_hex(color);
    library.colors.erase(std::remove_if(library.colors.begin(), library.colors.end(), [&](const ColorLibraryColor &entry) {
        return editor_recent_color_hex(entry.color).compare(hex, Qt::CaseInsensitive) == 0;
    }), library.colors.end());
    library.colors.push_back({name.trimmed().isEmpty() ? hex : name.trimmed(), color});
    save_color_libraries();
    refresh_color_library_swatches();
}

void TitleEditor::remove_color_from_current_library(int index)
{
    if (selected_color_library_index_ < 0 || selected_color_library_index_ >= (int)color_libraries_.size())
        return;
    ColorLibrary &library = color_libraries_[selected_color_library_index_];
    if (library.built_in || index < 0 || index >= (int)library.colors.size())
        return;

    library.colors.erase(library.colors.begin() + index);
    save_color_libraries();
    refresh_color_library_swatches();
}

void TitleEditor::refresh_color_swatches_panel()
{
    QStringList recent_hexes = bgl_load_recent_color_hexes();
    if (title_) {
        for (const auto &hex : title_->editor_recent_color_hexes) {
            const QString value = QString::fromStdString(hex);
            if (!recent_hexes.contains(value, Qt::CaseInsensitive))
                recent_hexes << value;
        }
    }
    for (int i = 0; i < (int)recent_color_swatch_buttons_.size(); ++i) {
        auto *button = recent_color_swatch_buttons_[i];
        if (!button)
            continue;

        QColor color;
        const bool has_color = i < recent_hexes.size() && editor_parse_recent_color_hex(recent_hexes[i], color);
        button->setEnabled(has_color);
        button->setToolTip(has_color ? bgl_color_swatch_tooltip(bgl_tr("OBSTitles.RecentColors"), color, editor_recent_color_hex(color)) : QString());
        button->setStyleSheet(editor_color_swatch_style(color, has_color));
    }
}

void TitleEditor::create_docked_panel_menu(QMenuBar *menu_bar)
{
    if (!menu_bar) return;

    auto *windows_menu = menu_bar->addMenu(bgl_tr("OBSTitles.Windows"));

    act_lock_panels_ = windows_menu->addAction(bgl_tr("OBSTitles.LockPanels"));
    act_lock_panels_->setCheckable(true);
    connect(act_lock_panels_, &QAction::toggled, this, &TitleEditor::set_panels_locked);

    QAction *reset_layout_action = windows_menu->addAction(bgl_tr("OBSTitles.ResetDefaultLayout"));
    connect(reset_layout_action, &QAction::triggered, this, &TitleEditor::reset_default_layout);

    windows_menu->addSeparator();
    act_tools_visible_ = windows_menu->addAction(bgl_tr("OBSTitles.Tools"));
    act_tools_visible_->setCheckable(true);
    act_tools_visible_->setChecked(true);
    connect(act_tools_visible_, &QAction::triggered, this, [this](bool visible) {
        if (tools_dock_) tools_dock_->setVisible(visible);
    });

    act_graphic_props_visible_ = windows_menu->addAction(bgl_tr("OBSTitles.GraphicProperties"));
    act_graphic_props_visible_->setCheckable(true);
    act_graphic_props_visible_->setChecked(true);
    connect(act_graphic_props_visible_, &QAction::triggered, this, [this](bool visible) {
        if (graphic_props_dock_) graphic_props_dock_->setVisible(visible);
    });

    act_layer_props_visible_ = windows_menu->addAction(bgl_tr("OBSTitles.LayerProperties"));
    act_layer_props_visible_->setCheckable(true);
    act_layer_props_visible_->setChecked(true);
    connect(act_layer_props_visible_, &QAction::triggered, this, [this](bool visible) {
        if (layer_props_dock_) layer_props_dock_->setVisible(visible);
    });

    act_effects_visible_ = windows_menu->addAction(bgl_tr("OBSTitles.EffectSettings"));
    act_effects_visible_->setCheckable(true);
    act_effects_visible_->setChecked(true);
    connect(act_effects_visible_, &QAction::triggered, this, [this](bool visible) {
        if (effects_dock_) effects_dock_->setVisible(visible);
    });

    act_effects_presets_visible_ = windows_menu->addAction(bgl_tr("OBSTitles.EffectsAndPresets"));
    act_effects_presets_visible_->setCheckable(true);
    act_effects_presets_visible_->setChecked(true);
    connect(act_effects_presets_visible_, &QAction::triggered, this, [this](bool visible) {
        if (effects_presets_dock_) effects_presets_dock_->setVisible(visible);
    });

    act_styles_visible_ = windows_menu->addAction(bgl_tr("OBSTitles.Libraries"));
    act_styles_visible_->setCheckable(true);
    act_styles_visible_->setChecked(true);
    connect(act_styles_visible_, &QAction::triggered, this, [this](bool visible) {
        if (styles_dock_) styles_dock_->setVisible(visible);
    });

