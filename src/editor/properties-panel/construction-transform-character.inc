    auto *image_form_widget = new QWidget(image_content);
    image_form_widget->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *image_form = new QFormLayout(image_form_widget);
    style_form(image_form);
    edit_image_path_ = new QLineEdit(image_form_widget);
    edit_image_path_->setFixedHeight(22);
    edit_image_path_->setStyleSheet(control_style);
    btn_pick_image_ = new QPushButton(bgl_tr("OBSTitles.Browse"), image_form_widget);
    style_push_button(btn_pick_image_);
    spn_layer_w_->setToolTip(bgl_tr("OBSTitles.ImageWidthTooltip"));
    spn_layer_h_->setToolTip(bgl_tr("OBSTitles.ImageHeightTooltip"));
    cmb_image_scale_filter_ = new QComboBox(image_form_widget);
    cmb_image_scale_filter_->setFixedHeight(22);
    cmb_image_scale_filter_->setStyleSheet(control_style);
    cmb_image_scale_filter_->addItem(bgl_tr("OBSTitles.ScaleFilterDisable"), (int)ImageScaleFilter::Disable);
    cmb_image_scale_filter_->addItem(bgl_tr("OBSTitles.ScaleFilterBilinear"), (int)ImageScaleFilter::Bilinear);
    cmb_image_scale_filter_->addItem(bgl_tr("OBSTitles.ScaleFilterBicubic"), (int)ImageScaleFilter::Bicubic);
    cmb_image_scale_filter_->addItem(bgl_tr("OBSTitles.ScaleFilterLanczos"), (int)ImageScaleFilter::Lanczos);
    cmb_image_scale_filter_->addItem(bgl_tr("OBSTitles.ScaleFilterArea"), (int)ImageScaleFilter::Area);
    cmb_image_box_mode_ = new QComboBox(image_form_widget);
    cmb_image_box_mode_->setFixedHeight(22);
    cmb_image_box_mode_->setStyleSheet(control_style);
    cmb_image_box_mode_->addItem(bgl_tr("OBSTitles.ImageBoxFitImageToBox"), (int)ImageBoxMode::FitImageToBox);
    cmb_image_box_mode_->addItem(bgl_tr("OBSTitles.ImageBoxFillHorizontal"), (int)ImageBoxMode::FillHorizontal);
    cmb_image_box_mode_->addItem(bgl_tr("OBSTitles.ImageBoxFillVertical"), (int)ImageBoxMode::FillVertical);
    cmb_image_box_mode_->addItem(bgl_tr("OBSTitles.ImageBoxFitToLongSide"), (int)ImageBoxMode::FitToLongSide);
    cmb_image_box_mode_->addItem(bgl_tr("OBSTitles.ImageBoxFitToShortSide"), (int)ImageBoxMode::FitToShortSide);
    cmb_image_box_mode_->addItem(bgl_tr("OBSTitles.ImageBoxStretchToFill"), (int)ImageBoxMode::StretchToFill);
    chk_image_crop_when_outside_box_ = new BglSwitch(bgl_tr("OBSTitles.ImageBoxCropWhenOutside"), image_form_widget);
    style_checkbox(chk_image_crop_when_outside_box_);
    btn_image_anchor_grid_ = new AnchorGridButton(image_form_widget);
    btn_image_anchor_grid_->setFixedSize(36, 36);
    btn_image_anchor_grid_->setToolTip(bgl_tr("OBSTitles.ImageBoxAnchorTooltip"));
    add_form_row(image_form, bgl_tr("OBSTitles.PathLabel"), edit_image_path_);
    add_form_row(image_form, "", btn_pick_image_);
    add_form_row(image_form, bgl_tr("OBSTitles.ImageBoxMode"), cmb_image_box_mode_);
    add_form_row(image_form, QString(), chk_image_crop_when_outside_box_);
    add_form_row(image_form, bgl_tr("OBSTitles.ImageBoxAnchor"), btn_image_anchor_grid_);
    add_form_row(image_form, bgl_tr("OBSTitles.Filtering"), cmb_image_scale_filter_);
    image_content_layout->addWidget(image_form_widget, 1);
    image_layout->addWidget(image_content);
    BglCollapsiblePanel *shape_panel = bgl_panel_for_content(rect_box_);
    const int image_index = shape_panel ? vl->indexOf(shape_panel) : -1;
    bgl_add_panel_section(vl, image_box_, bgl_tr("OBSTitles.Image"), nullptr, image_index);

    image_box_size_box_ = new QWidget(inner);
    image_box_size_box_->setStyleSheet(QStringLiteral("background:%1;").arg(section_bg_name));
    auto *image_box_size_layout = new QVBoxLayout(image_box_size_box_);
    image_box_size_layout->setContentsMargins(10, 8, 10, 10);
    image_box_size_layout->setSpacing(8);
    auto *image_box_size_grid = new QGridLayout();
    image_box_size_grid->setContentsMargins(0, 0, 0, 0);
    image_box_size_grid->setHorizontalSpacing(8);
    image_box_size_grid->setVerticalSpacing(8);
    image_box_size_grid->setColumnMinimumWidth(0, 24);
    image_box_size_grid->setColumnMinimumWidth(1, 82);
    image_box_size_grid->setColumnMinimumWidth(2, 86);
    image_box_size_grid->setColumnMinimumWidth(3, 22);
    image_box_size_grid->setColumnMinimumWidth(4, 86);
    image_box_size_grid->setColumnStretch(5, 1);
    auto make_image_box_size_field = [&](const QString &label, QDoubleSpinBox *spin) {
        auto *field = new QWidget(image_box_size_box_);
        field->setObjectName(QStringLiteral("OBSTitlesShapeNumericField"));
        field->setStyleSheet(QStringLiteral("QWidget#OBSTitlesShapeNumericField{background:%1;border:1px solid %2;"
                             "border-radius:2px;}").arg(control_bg_name, border_name));
        auto *field_layout = new QHBoxLayout(field);
        field_layout->setContentsMargins(5, 0, 0, 0);
        field_layout->setSpacing(2);
        auto *field_label = new NumericDragLabel(label, spin, field,
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
        field_label->setFixedWidth(16);
        field_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        field_label->setStyleSheet(QStringLiteral("color:%1;background:transparent;font-size:12px;").arg(control_text_name));
        field_layout->addWidget(field_label);
        field_layout->addWidget(spin, 1);
        field->setFixedSize(104, 24);
        return field;
    };
    chk_image_box_size_lock_ = new TransformLockCheckBox(image_box_size_box_);
    chk_image_box_size_lock_->setText(QString());
    chk_image_box_size_lock_->setToolTip(bgl_tr("OBSTitles.LockAspectRatio"));
    chk_image_box_size_lock_->setFixedSize(24, 24);
    chk_image_box_size_lock_->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *image_box_size_label = new QLabel(bgl_tr("OBSTitles.Size"), image_box_size_box_);
    image_box_size_label->setStyleSheet(QStringLiteral("color:%1;background:transparent;font-size:13px;").arg(panel_text_name));
    image_box_size_grid->addWidget(btn_kf_image_box_size_, 0, 0, Qt::AlignCenter);
    image_box_size_grid->addWidget(image_box_size_label, 0, 1);
    image_box_size_grid->addWidget(make_image_box_size_field(bgl_tr("OBSTitles.W"), spn_image_box_w_), 0, 2);
    image_box_size_grid->addWidget(chk_image_box_size_lock_, 0, 3, Qt::AlignCenter);
    image_box_size_grid->addWidget(make_image_box_size_field(bgl_tr("OBSTitles.H"), spn_image_box_h_), 0, 4);
    image_box_size_layout->addLayout(image_box_size_grid);
    BglCollapsiblePanel *image_panel = bgl_panel_for_content(image_box_);
    const int image_box_size_index = image_panel ? vl->indexOf(image_panel) + 1 : -1;
    bgl_add_panel_section(vl, image_box_size_box_, bgl_tr("OBSTitles.ImageBoxSize"), nullptr,
                          image_box_size_index);

    vl->addStretch();
    setWidget(inner);

    /* ── Connect signals → property_changed ── */
    auto emit_change = [this]() { if (!loading_values_) emit property_changed(!numeric_label_dragging_); };
    auto can_edit = [this]() { return layer_ && !loading_values_; };
    auto text_edit_active = [this]() {
        return layer_ && active_text_edit_layer_id_ == layer_->id;
    };
    auto apply_text_char_format = [this](const RichTextCharFormat &format, uint32_t mask) {
        if (!layer_ || loading_values_) return;
        const bool active = active_text_edit_layer_id_ == layer_->id;
        apply_rich_text_format_to_layer_range(*layer_, format, mask, active);
        emit text_char_format_changed(layer_->id, format, mask);
    };
    auto current_text_char_format = [this]() {
        RichTextCharFormat fmt;
        if (!layer_) return fmt;
        const bool active = active_text_edit_layer_id_ == layer_->id;
        RichTextCharFormatSummary summary = summarize_rich_text_char_format(
            *layer_, active, std::max(0.0, playhead_ - layer_->in_time));
        return summary.valid ? summary.format : fmt;
    };
    auto current_text_paragraph_format = [this]() {
        if (!layer_) return RichTextParagraphFormat();
        rich_text_document_ensure_canonical(*layer_);
        if (active_text_edit_layer_id_ == layer_->id) {
            const size_t offset = std::min(layer_->rich_text.selection.anchor,
                                           layer_->rich_text.selection.head);
            return rich_text_paragraph_format_at(layer_->rich_text, offset);
        }
        return layer_->rich_text.default_paragraph_format;
    };
    auto apply_text_paragraph_format = [this](const RichTextParagraphFormat &format, uint32_t mask,
                                              bool force_object_level = false) {
        if (!layer_ || loading_values_) return;
        const bool active = !force_object_level && active_text_edit_layer_id_ == layer_->id;
        apply_rich_text_paragraph_format_to_layer(*layer_, format, mask, active);
        /* Reuse the canonical text-adapter refresh signal; mask 0 means the
         * paragraph model changed without a character-style mutation. */
        emit text_char_format_changed(layer_->id, RichTextCharFormat(), 0);
    };
    auto apply_text_fill_format = [this, apply_text_char_format]() {
        if (!layer_ || loading_values_) return;
        const bool active = active_text_edit_layer_id_ == layer_->id;
        RichTextCharFormatSummary summary = summarize_rich_text_char_format(
            *layer_, active, std::max(0.0, playhead_ - layer_->in_time));
        RichTextCharFormat fmt = summary.valid ? summary.format : RichTextCharFormat();
        if (active) {
            /* While editing inline, the controls describe the selected run.
             * Never re-read the layer mirrors here: those mirrors represent
             * document defaults and would erase the previous inline change. */
            if (cmb_fill_type_) fmt.fill.type = cmb_fill_type_->currentData().toInt();
            if (cmb_gradient_type_) fmt.fill.gradient_type = cmb_gradient_type_->currentData().toInt();
            if (cmb_gradient_spread_) fmt.fill.gradient_spread = cmb_gradient_spread_->currentData().toInt();
            if (spn_gradient_start_pos_) fmt.fill.gradient_start_pos = (float)spn_gradient_start_pos_->value();
            if (spn_gradient_end_pos_) fmt.fill.gradient_end_pos = (float)spn_gradient_end_pos_->value();
            if (spn_gradient_start_opacity_) fmt.fill.gradient_start_opacity = (float)spn_gradient_start_opacity_->value();
            if (spn_gradient_end_opacity_) fmt.fill.gradient_end_opacity = (float)spn_gradient_end_opacity_->value();
            if (spn_gradient_opacity_) fmt.fill.gradient_opacity = (float)spn_gradient_opacity_->value();
            if (spn_gradient_angle_) fmt.fill.gradient_angle = (float)spn_gradient_angle_->value();
            if (spn_gradient_center_x_) fmt.fill.gradient_center_x = (float)spn_gradient_center_x_->value();
            if (spn_gradient_center_y_) fmt.fill.gradient_center_y = (float)spn_gradient_center_y_->value();
            if (spn_gradient_scale_) fmt.fill.gradient_scale = (float)spn_gradient_scale_->value();
            if (spn_gradient_focal_x_) fmt.fill.gradient_focal_x = (float)spn_gradient_focal_x_->value();
            if (spn_gradient_focal_y_) fmt.fill.gradient_focal_y = (float)spn_gradient_focal_y_->value();
        } else {
            fmt.fill.type = layer_->fill_type;
            fmt.fill.color = layer_->text_color;
            fmt.fill.gradient_type = layer_->gradient_type;
            fmt.fill.gradient_spread = layer_->gradient_spread;
            fmt.fill.gradient_start_color = layer_->gradient_start_color;
            fmt.fill.gradient_end_color = layer_->gradient_end_color;
            fmt.fill.gradient_start_pos = layer_->gradient_start_pos;
            fmt.fill.gradient_end_pos = layer_->gradient_end_pos;
            fmt.fill.gradient_start_opacity = layer_->gradient_start_opacity;
            fmt.fill.gradient_end_opacity = layer_->gradient_end_opacity;
            fmt.fill.gradient_opacity = layer_->gradient_opacity;
            fmt.fill.gradient_angle = layer_->gradient_angle;
            fmt.fill.gradient_center_x = layer_->gradient_center_x;
            fmt.fill.gradient_center_y = layer_->gradient_center_y;
            fmt.fill.gradient_scale = layer_->gradient_scale;
            fmt.fill.gradient_focal_x = layer_->gradient_focal_x;
            fmt.fill.gradient_focal_y = layer_->gradient_focal_y;
        }
        apply_text_char_format(fmt, RichTextCharFillColor);
    };
    auto local_time = [this]() {
        return layer_ ? std::clamp(playhead_ - layer_->in_time, 0.0,
                                   std::max(0.0, layer_->out_time - layer_->in_time)) : 0.0;
    };
    auto fit_image_size_to_current_image = [this, local_time]() {
        if (!layer_ || layer_->image_path.empty()) return;
        const QSize image_size = editor_image_intrinsic_size(QString::fromStdString(layer_->image_path));
        if (!image_size.isValid() || image_size.isEmpty()) return;
        const double t = local_time();
        layer_->image_width = (float)image_size.width();
        layer_->image_height = (float)image_size.height();
        set_animated_x(layer_->image_size, t, layer_->image_width);
        set_animated_y(layer_->image_size, t, layer_->image_height);
        layer_->image_size_auto_fit = true;
        layer_->lock_aspect_ratio = layer_->image_box_mode != ImageBoxMode::StretchToFill;
    };
    auto refresh_image_size_controls = [this, local_time]() {
        if (!layer_ || layer_->type != LayerType::Image) return;
        const double t = local_time();
        double raw_w = eval_image_width(*layer_, t);
        double raw_h = eval_image_height(*layer_, t);
        if (raw_w <= 0.0 || raw_h <= 0.0) {
            const QSize intrinsic = editor_image_intrinsic_size(QString::fromStdString(layer_->image_path));
            if (intrinsic.isValid() && !intrinsic.isEmpty()) {
                raw_w = intrinsic.width();
                raw_h = intrinsic.height();
            }
        }
        const bgs::ImageDisplaySize display = bgs::calculate_image_display_size(
            layer_->image_box_mode, layer_->image_size_auto_fit,
            eval_box_width(*layer_, t), eval_box_height(*layer_, t), raw_w, raw_h);
        if (spn_layer_w_) {
            QSignalBlocker block(spn_layer_w_);
            spn_layer_w_->setValue(display.width);
        }
        if (spn_layer_h_) {
            QSignalBlocker block(spn_layer_h_);
            spn_layer_h_->setValue(display.height);
        }
        const bool stretch = layer_->image_size_auto_fit &&
                             layer_->image_box_mode == ImageBoxMode::StretchToFill;
        if (spn_layer_w_) spn_layer_w_->setEnabled(!stretch);
        if (spn_layer_h_) spn_layer_h_->setEnabled(!stretch);
        if (chk_size_lock_) chk_size_lock_->setEnabled(!stretch);
        if (btn_kf_width_) btn_kf_width_->setEnabled(!stretch);
    };
    auto update_text_box_auto_controls = [this]() {
        if (spn_max_text_box_width_)
            spn_max_text_box_width_->setEnabled(true);
        if (spn_max_text_box_height_)
            spn_max_text_box_height_->setEnabled(true);
    };
    auto install_delete_all_keyframes_menu =
        [this, can_edit, emit_change, menu_style](QPushButton *button, auto props_for_layer) {
            if (!button) return;
            button->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(button, &QPushButton::customContextMenuRequested,
                    this, [this, button, props_for_layer, can_edit, emit_change, menu_style](const QPoint &pos) {
                        if (!layer_) return;
                        std::vector<AnimatedProperty *> props = props_for_layer();
                        bool has_keyframes = false;
                        for (auto *prop : props) {
                            if (prop && prop->is_animated()) {
                                has_keyframes = true;
                                break;
                            }
                        }

                        QMenu menu(button);
                        menu.setStyleSheet(menu_style);
                        QAction *delete_all = menu.addAction(bgl_tr("OBSTitles.DeleteAllKeyframes"));
                        delete_all->setEnabled(can_edit() && has_keyframes);
                        if (menu.exec(button->mapToGlobal(pos)) != delete_all || !can_edit()) return;

                        bool changed = false;
                        for (auto *prop : props) {
                            if (!prop || prop->keyframes.empty()) continue;
                            prop->keyframes.clear();
                            changed = true;
                        }
                        if (!changed) return;
                        load_values();
                        emit_change();
                    });
        };
    auto install_prop_delete_all = [&](QPushButton *button, AnimatedProperty Layer::*prop) {
        install_delete_all_keyframes_menu(button, [this, prop]() {
            return layer_ ? std::vector<AnimatedProperty *>{&(layer_.get()->*prop)}
                          : std::vector<AnimatedProperty *>{};
        });
    };
    auto install_vec_delete_all = [&](QPushButton *button, AnimatedVec2Property Layer::*prop) {
        if (!button) return;
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QPushButton::customContextMenuRequested,
                this, [this, button, prop, can_edit, emit_change, menu_style](const QPoint &pos) {
                    if (!layer_) return;
                    auto &vec = layer_.get()->*prop;
                    QMenu menu(button);
                    menu.setStyleSheet(menu_style);
                    QAction *delete_all = menu.addAction(bgl_tr("OBSTitles.DeleteAllKeyframes"));
                    delete_all->setEnabled(can_edit() && !vec.keyframes.empty());
                    if (menu.exec(button->mapToGlobal(pos)) != delete_all || !can_edit()) return;
                    if (vec.keyframes.empty()) return;
                    vec.keyframes.clear();
                    load_values();
                    emit_change();
                });
    };
    auto install_vec_delete_all_resolved = [&](QPushButton *button, auto prop_for_layer) {
        if (!button) return;
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QPushButton::customContextMenuRequested,
                this, [this, button, prop_for_layer, can_edit, emit_change, menu_style](const QPoint &pos) {
                    if (!layer_) return;
                    AnimatedVec2Property *vec = prop_for_layer();
                    QMenu menu(button);
                    menu.setStyleSheet(menu_style);
                    QAction *delete_all = menu.addAction(bgl_tr("OBSTitles.DeleteAllKeyframes"));
                    delete_all->setEnabled(can_edit() && vec && !vec->keyframes.empty());
                    if (menu.exec(button->mapToGlobal(pos)) != delete_all || !can_edit() || !vec) return;
                    if (vec->keyframes.empty()) return;
                    vec->keyframes.clear();
                    load_values();
                    emit_change();
                });
    };
    auto install_group_delete_all = [&](QPushButton *button, std::initializer_list<AnimatedProperty Layer::*> props) {
        std::vector<AnimatedProperty Layer::*> prop_members(props);
        install_delete_all_keyframes_menu(button, [this, prop_members]() {
            std::vector<AnimatedProperty *> result;
            if (!layer_) return result;
            result.reserve(prop_members.size());
            for (auto prop : prop_members)
                result.push_back(&(layer_.get()->*prop));
            return result;
        });
    };

    install_vec_delete_all(btn_kf_pos_x_, &Layer::position);
    install_vec_delete_all(btn_kf_pos_y_, &Layer::position);
    install_vec_delete_all(btn_kf_scale_x_, &Layer::scale);
    install_vec_delete_all(btn_kf_scale_y_, &Layer::scale);
    install_vec_delete_all(btn_kf_transform_size_, &Layer::size);
    install_prop_delete_all(btn_kf_rotation_, &Layer::rotation);
    install_prop_delete_all(btn_kf_opacity_, &Layer::opacity);
    install_vec_delete_all(btn_kf_origin_x_, &Layer::origin_prop);
    install_vec_delete_all(btn_kf_origin_y_, &Layer::origin_prop);
    install_prop_delete_all(btn_kf_paragraph_indent_left_, &Layer::paragraph_indent_left_prop);
    install_prop_delete_all(btn_kf_paragraph_indent_right_, &Layer::paragraph_indent_right_prop);
    install_prop_delete_all(btn_kf_paragraph_indent_first_line_, &Layer::paragraph_indent_first_line_prop);
    install_prop_delete_all(btn_kf_font_size_, &Layer::font_size_prop);
    install_prop_delete_all(btn_kf_ticker_completion_, &Layer::ticker_completion_prop);
    install_prop_delete_all(btn_kf_char_scale_x_, &Layer::char_scale_x_prop);
    install_prop_delete_all(btn_kf_char_scale_y_, &Layer::char_scale_y_prop);
    install_prop_delete_all(btn_kf_char_tracking_, &Layer::char_tracking_prop);
    install_prop_delete_all(btn_kf_baseline_shift_, &Layer::baseline_shift_prop);
    install_prop_delete_all(btn_kf_paragraph_space_before_, &Layer::paragraph_space_before_prop);
    install_prop_delete_all(btn_kf_paragraph_space_after_, &Layer::paragraph_space_after_prop);
    install_vec_delete_all_resolved(btn_kf_width_, [this]() -> AnimatedVec2Property * {
        if (!layer_) return nullptr;
        return layer_->type == LayerType::Image ? &layer_->image_size : &layer_->size;
    });
    install_vec_delete_all(btn_kf_image_box_size_, &Layer::size);
    install_group_delete_all(btn_kf_text_color_, {&Layer::text_color_a, &Layer::text_color_r,
                                                  &Layer::text_color_g, &Layer::text_color_b});
    install_group_delete_all(btn_kf_fill_color_, {&Layer::fill_color_a, &Layer::fill_color_r,
                                                  &Layer::fill_color_g, &Layer::fill_color_b});

    connect(spn_px_,       QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) {
                    const double t = local_time();
                    set_animated_value(layer_->position, t, {v, layer_->position.evaluate(t).y});
                    emit_change();
                }
            });
    connect(spn_py_,       QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) {
                    const double t = local_time();
                    set_animated_value(layer_->position, t, {layer_->position.evaluate(t).x, v});
                    emit_change();
                }
            });
    connect(spn_scale_x_,  QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (!can_edit()) return;
                const double scale = v / 100.0;
                const double t = local_time();
                Vec2Value next = layer_->scale.evaluate(t);
                next.x = scale;
                if (layer_->scale_lock) {
                    QSignalBlocker blocker(spn_scale_y_);
                    spn_scale_y_->setValue(v);
                    next.y = scale;
                }
                set_animated_value(layer_->scale, t, next);
                emit_change();
            });
    connect(spn_scale_y_,  QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (!can_edit()) return;
                const double scale = v / 100.0;
                const double t = local_time();
                Vec2Value next = layer_->scale.evaluate(t);
                next.y = scale;
                if (layer_->scale_lock) {
                    QSignalBlocker blocker(spn_scale_x_);
                    spn_scale_x_->setValue(v);
                    next.x = scale;
                }
                set_animated_value(layer_->scale, t, next);
                emit_change();
            });
    connect(chk_scale_lock_, &QCheckBox::toggled,
            this, [this, can_edit, local_time, emit_change](bool locked) {
                if (!can_edit()) return;
                layer_->scale_lock = locked;
                if (locked) {
                    const double t = local_time();
                    const double scale = spn_scale_x_->value() / 100.0;
                    QSignalBlocker blocker(spn_scale_y_);
                    spn_scale_y_->setValue(spn_scale_x_->value());
                    set_animated_value(layer_->scale, t, {scale, scale});
                }
                emit_change();
            });
    connect(spn_rot_,      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) { set_animated_value(layer_->rotation, local_time(), v); emit_change(); }
            });
    connect(spn_opacity_,  QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) { set_animated_value(layer_->opacity, local_time(), v); emit_change(); }
            });
    connect(spn_origin_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) {
                    const double t = local_time();
                    layer_->origin_x = (float)v;
                    set_animated_value(layer_->origin_prop, t, {v, layer_->origin_prop.evaluate(t).y});
                    emit_change();
                }
            });
    connect(spn_origin_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) {
                    const double t = local_time();
                    layer_->origin_y = (float)v;
                    set_animated_value(layer_->origin_prop, t, {layer_->origin_prop.evaluate(t).x, v});
                    emit_change();
                }
            });
    connect(cmb_anchor_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, local_time, emit_change](int idx) {
                if (!can_edit()) return;
                double t = local_time();
                QPointF next = anchor_point_from_index(idx);
                double w = eval_box_width(*layer_, t);
                double h = eval_box_height(*layer_, t);
                QPointF keep = rotated_scaled_delta((next.x() - layer_->origin_x) * w,
                                                    (next.y() - layer_->origin_y) * h,
                                                    layer_->rotation.evaluate(t),
                                                    layer_->scale.evaluate(t).x,
                                                    layer_->scale.evaluate(t).y);
                layer_->origin_x = (float)next.x();
                layer_->origin_y = (float)next.y();
                set_animated_value(layer_->origin_prop, t, {next.x(), next.y()});
                const Vec2Value pos = layer_->position.evaluate(t);
                set_animated_value(layer_->position, t, {pos.x + keep.x(), pos.y + keep.y()});
                load_values();
                emit_change();
            });
    connect(btn_transform_defaults_, &QPushButton::clicked,
            this, [this, can_edit, local_time, emit_change]() {
                if (!can_edit()) return;
                const double t = local_time();
                set_animated_value(layer_->position, t, {0.0, 0.0});
                set_animated_value(layer_->scale, t, {1.0, 1.0});
                set_animated_value(layer_->rotation, t, 0.0);
                set_animated_value(layer_->opacity, t, 1.0);
                layer_->origin_x = 0.5f;
                layer_->origin_y = 0.5f;
                set_animated_value(layer_->origin_prop, t, {0.5, 0.5});
                layer_->scale_lock = true;
                load_values();
                emit_change();
            });
    connect(txt_content_, &QTextEdit::textChanged,
            this, [this, can_edit, emit_change]() {
                if (!can_edit()) return;
                std::string value = txt_content_->toPlainText().toStdString();
                if (layer_->type == LayerType::Clock) {
                    layer_->clock_format = value.empty() ? "H:i:s" : value;
                } else {
                    rich_text_document_ensure_canonical(*layer_);
                    const RichTextCharFormat insertion_format =
                        insertion_format_for_text_replace(layer_->rich_text);
                    const uint32_t insertion_mask =
                        insertion_mask_for_text_replace(layer_->rich_text);
                    rich_text_document_replace_text(layer_->rich_text, value,
                                                    insertion_format, insertion_mask);
                    rich_text_document_sync_layer_mirrors(*layer_);
                }
                emit_change();
            });
    connect(chk_text_box_width_to_text_, &QCheckBox::toggled,
            this, [this, can_edit, update_text_box_auto_controls, emit_change](bool v) {
                update_text_box_auto_controls();
                if (can_edit()) { layer_->text_box_width_to_text = v; emit_change(); }
            });
    connect(chk_text_box_height_to_text_, &QCheckBox::toggled,
            this, [this, can_edit, update_text_box_auto_controls, emit_change](bool v) {
                update_text_box_auto_controls();
                if (can_edit()) { layer_->text_box_height_to_text = v; emit_change(); }
            });
    connect(spn_max_text_box_width_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->max_text_box_width = (float)v; emit_change(); }
            });
    connect(spn_max_text_box_height_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->max_text_box_height = (float)v; emit_change(); }
            });
    connect(cmb_font_, &QComboBox::currentTextChanged,
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](const QString &s){
                if (!can_edit()) return;
                RichTextCharFormat fmt = current_text_char_format();
                populate_font_style_combo(cmb_font_style_, s, QString::fromStdString(fmt.font_style));
                fmt.font_family = s.toStdString();
                fmt.font_style = cmb_font_style_->currentText().toStdString();
                QFontDatabase fdb;
                fmt.bold = fdb.bold(s, cmb_font_style_->currentText());
                fmt.italic = fdb.italic(s, cmb_font_style_->currentText());
                apply_text_char_format(fmt, RichTextCharFontFamily | RichTextCharFontStyle |
                                       RichTextCharBold | RichTextCharItalic);
                emit_change();
            });
    connect(cmb_font_style_, &QComboBox::currentTextChanged,
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](const QString &s){
                if (!can_edit()) return;
                RichTextCharFormat fmt = current_text_char_format();
                fmt.font_style = s.toStdString();
                QFontDatabase fdb;
                const QString family = QString::fromStdString(fmt.font_family);
                fmt.bold = fdb.bold(family, s);
                fmt.italic = fdb.italic(family, s);
                apply_text_char_format(fmt, RichTextCharFontStyle | RichTextCharBold | RichTextCharItalic);
                emit_change();
            });
    connect(spn_size_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, local_time, emit_change, apply_text_char_format, current_text_char_format](int v){
                if (!can_edit()) return;
                if (!text_edit_active()) {
                    layer_->font_size = v;
                    set_animated_value(layer_->font_size_prop, local_time(), v);
                }
                RichTextCharFormat fmt = current_text_char_format();
                fmt.font_size = v;
                apply_text_char_format(fmt, RichTextCharFontSize);
                emit_change();
            });
    connect(chk_bold_, &QToolButton::toggled,
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){
                if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.bold = v; apply_text_char_format(fmt, RichTextCharBold); emit_change(); }
            });
    connect(chk_italic_, &QToolButton::toggled,
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){
                if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.italic = v; apply_text_char_format(fmt, RichTextCharItalic); emit_change(); }
            });
    connect(chk_font_kerning_, &QToolButton::toggled,
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){
                if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.kerning = v; apply_text_char_format(fmt, RichTextCharKerning); emit_change(); }
            });
    connect(cmb_kerning_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](int idx) {
                if (!can_edit()) return;
                RichTextCharFormat fmt = current_text_char_format();
                fmt.kerning_mode = cmb_kerning_mode_->itemData(idx).toInt();
                fmt.kerning = fmt.kerning_mode != 2;
                if (chk_font_kerning_) chk_font_kerning_->setChecked(fmt.kerning);
                if (spn_kerning_value_) spn_kerning_value_->setEnabled(fmt.kerning_mode == 2);
                apply_text_char_format(fmt, RichTextCharKerning);
                emit_change();
            });
    connect(spn_kerning_value_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](double v) {
                if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.manual_kerning = (float)v; apply_text_char_format(fmt, RichTextCharKerning); emit_change(); }
            });
    connect(spn_text_leading_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, current_text_paragraph_format, apply_text_paragraph_format](double v){
                if (!can_edit()) return;
                RichTextParagraphFormat fmt = current_text_paragraph_format();
                fmt.line_spacing = (float)v;
                apply_text_paragraph_format(fmt, RichTextParagraphLineSpacing);
                emit_change();
            });
    connect(spn_char_tracking_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, local_time, emit_change, apply_text_char_format, current_text_char_format](double v){
                if (!can_edit()) return;
                if (!text_edit_active()) {
                    layer_->char_tracking = (float)v;
                    set_animated_value(layer_->char_tracking_prop, local_time(), v);
                }
                RichTextCharFormat fmt = current_text_char_format();
                fmt.tracking = (float)v;
                apply_text_char_format(fmt, RichTextCharTracking);
                emit_change();
            });
    connect(spn_char_scale_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, local_time, emit_change, apply_text_char_format, current_text_char_format](double v){
                if (!can_edit()) return;
                if (!text_edit_active()) {
                    layer_->char_scale_x = (float)(v / 100.0);
                    set_animated_value(layer_->char_scale_x_prop, local_time(), v / 100.0);
                }
                RichTextCharFormat fmt = current_text_char_format();
                fmt.scale_x = (float)(v / 100.0);
                apply_text_char_format(fmt, RichTextCharScaleX);
                emit_change();
            });
    connect(spn_char_scale_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, local_time, emit_change, apply_text_char_format, current_text_char_format](double v){
                if (!can_edit()) return;
                if (!text_edit_active()) {
                    layer_->char_scale_y = (float)(v / 100.0);
                    set_animated_value(layer_->char_scale_y_prop, local_time(), v / 100.0);
                }
                RichTextCharFormat fmt = current_text_char_format();
                fmt.scale_y = (float)(v / 100.0);
                apply_text_char_format(fmt, RichTextCharScaleY);
                emit_change();
            });
    connect(spn_baseline_shift_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, local_time, emit_change, apply_text_char_format, current_text_char_format](double v){
                if (!can_edit()) return;
                if (!text_edit_active()) {
                    layer_->baseline_shift = (float)v;
                    set_animated_value(layer_->baseline_shift_prop, local_time(), v);
                }
                RichTextCharFormat fmt = current_text_char_format();
                fmt.baseline_shift = (float)v;
                apply_text_char_format(fmt, RichTextCharBaselineShift);
                emit_change();
            });
    auto set_exclusive_text_style = [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](int style, bool checked) {
        if (!can_edit()) return;
        RichTextCharFormat fmt = current_text_char_format();
        fmt.text_style = checked ? style : 0;
        apply_text_char_format(fmt, RichTextCharTextStyle);
        emit_change();
        load_values();
    };
    connect(btn_all_caps_, &QToolButton::toggled, this, [set_exclusive_text_style](bool v){ set_exclusive_text_style(1, v); });
    connect(btn_small_caps_, &QToolButton::toggled, this, [set_exclusive_text_style](bool v){ set_exclusive_text_style(2, v); });
    connect(btn_superscript_, &QToolButton::toggled, this, [set_exclusive_text_style](bool v){ set_exclusive_text_style(3, v); });
    connect(btn_subscript_, &QToolButton::toggled, this, [set_exclusive_text_style](bool v){ set_exclusive_text_style(4, v); });
    connect(btn_underline_, &QToolButton::toggled, this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){ if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.underline = v; apply_text_char_format(fmt, RichTextCharUnderline); emit_change(); }});
    connect(btn_strikethrough_, &QToolButton::toggled, this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){ if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.strikethrough = v; apply_text_char_format(fmt, RichTextCharStrikethrough); emit_change(); }});
    connect(btn_ligatures_, &QToolButton::toggled, this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){ if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.ligatures = v; apply_text_char_format(fmt, RichTextCharLigatures); emit_change(); }});
    connect(btn_stylistic_alternates_, &QToolButton::toggled, this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){ if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.stylistic_alternates = v; apply_text_char_format(fmt, RichTextCharStylisticAlternates); emit_change(); }});
    connect(btn_fractions_, &QToolButton::toggled, this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){ if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.fractions = v; apply_text_char_format(fmt, RichTextCharFractions); emit_change(); }});
    connect(btn_opentype_features_, &QToolButton::toggled, this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){ if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.opentype_features = v; apply_text_char_format(fmt, RichTextCharOpenTypeFeatures); emit_change(); }});
    connect(cmb_text_style_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](int idx) {
                if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.text_style = cmb_text_style_->itemData(idx).toInt(); apply_text_char_format(fmt, RichTextCharTextStyle); emit_change(); }
            });
    connect(cmb_text_overflow_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->text_overflow_mode = cmb_text_overflow_->itemData(idx).toInt(); emit_change(); }
            });
    connect(spn_text_fit_min_scale_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->text_fit_min_scale = (float)v; emit_change(); }
            });
    connect(btn_ticker_pause_, &QToolButton::clicked, this, [this]() {
        if (loading_values_ || !layer_ || layer_->type != LayerType::Ticker)
            return;
        const std::string title_id = title_ ? title_->id : std::string();
        const bool title_cued = title_ &&
            (title_->current_cue_row >= 0 || title_->pending_cue_row >= 0);
        bgs::ticker_runtime::toggle_pause(title_id, *layer_, title_cued);
        const TickerRuntimeSnapshot state =
            bgs::ticker_runtime::status(title_id, *layer_, title_cued);
        BGL_LOG_INFO("Ticker", QStringLiteral(
            "action=toggle source=properties title=%1 layer=%2 paused=%3")
            .arg(QString::fromStdString(title_id))
            .arg(QString::fromStdString(layer_->id))
            .arg(state.paused ? 1 : 0));
        TitleDataStore::instance().touch_runtime_change();
        load_values();
        emit runtime_visual_changed();
    });
    connect(cmb_ticker_playback_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (!can_edit() || !layer_ || layer_->type != LayerType::Ticker)
                    return;
                layer_->ticker_playback_mode =
                    cmb_ticker_playback_mode_->itemData(idx).toInt();
                const std::string title_id = title_ ? title_->id : std::string();
                const bool title_cued = title_ &&
                    (title_->current_cue_row >= 0 || title_->pending_cue_row >= 0);
                bgs::ticker_runtime::reset_for_mode_change(title_id, *layer_, title_cued);
                BGL_LOG_INFO("Ticker", QStringLiteral(
                    "action=mode-change source=properties title=%1 layer=%2 mode=%3")
                    .arg(QString::fromStdString(title_id))
                    .arg(QString::fromStdString(layer_->id))
                    .arg(layer_->ticker_playback_mode));
                emit_change();
                load_values();
            });
    connect(spn_ticker_completion_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v) {
                if (!can_edit() || !layer_ || layer_->type != LayerType::Ticker) return;
                layer_->ticker_completion = v;
                if (layer_->ticker_completion_prop.is_animated())
                    set_animated_value(layer_->ticker_completion_prop, local_time(), v);
                else
                    layer_->ticker_completion_prop.static_value = v;
                emit_change();
            });
    connect(btn_kf_ticker_completion_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit() || !layer_ || layer_->type != LayerType::Ticker) return;
        toggle_keyframe(layer_->ticker_completion_prop, local_time(), spn_ticker_completion_->value());
        load_values();
        emit_change();
    });
    connect(cmb_ticker_style_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->ticker_style = cmb_ticker_style_->itemData(idx).toInt(); emit_change(); load_values(); }
            });
    connect(spn_ticker_speed_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->ticker_speed = v; emit_change(); }
            });
    connect(spn_ticker_line_hold_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->ticker_line_hold = v; emit_change(); }
            });
    connect(cmb_ticker_direction_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->ticker_direction = cmb_ticker_direction_->itemData(idx).toInt(); emit_change(); }
            });
    connect(chk_expose_text_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) {
                    layer_->expose_text = v;
                    if (v) layer_->ignore_persistence = false;
                    load_values();
                    emit_change();
                }
            });
    connect(chk_exposed_hide_if_empty_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) {
                    layer_->exposed_hide_if_empty = v;
                    if (!v) layer_->live_cue_hidden_if_empty = false;
                    load_values();
                    emit_change();
                }
            });
    connect(chk_exposed_single_value_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) {
                    layer_->exposed_single_value = v;
                    load_values();
                    emit_change();
                }
            });
    connect(chk_ignore_persistence_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) {
                    layer_->ignore_persistence = !layer_->expose_text && v;
                    load_values();
                    emit_change();
                }
            });
    connect(chk_scene_mask_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) { layer_->use_as_scene_mask = v; load_values(); emit_change(); }
            });
    auto connect_alignment_group = [this, can_edit, emit_change, current_text_paragraph_format, apply_text_paragraph_format](QButtonGroup *group, bool horizontal) {
        if (!group) return;
        for (auto *button : group->buttons()) {
            connect(button, &QAbstractButton::clicked, this,
                    [can_edit, emit_change, current_text_paragraph_format, apply_text_paragraph_format, group, horizontal, button]() {
                if (!can_edit()) return;
                const int value = group->id(button);
                RichTextParagraphFormat fmt = current_text_paragraph_format();
                if (horizontal)
                    fmt.align_h = value;
                else
                    fmt.align_v = value;
                apply_text_paragraph_format(fmt, horizontal ? RichTextParagraphAlignH
                                                             : RichTextParagraphAlignV,
                                            !horizontal);
                emit_change();
            });
        }
    };
    connect_alignment_group(grp_text_align_, true);
    connect_alignment_group(grp_text_valign_, false);
    auto connect_keyframed_paragraph_spin = [this, can_edit, text_edit_active, local_time, emit_change,
                                              current_text_paragraph_format, apply_text_paragraph_format]
                                             (QDoubleSpinBox *spin, float Layer::*field,
                                              AnimatedProperty Layer::*prop, uint32_t mask) {
        if (!spin) return;
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, can_edit, text_edit_active, local_time, emit_change,
                 current_text_paragraph_format, apply_text_paragraph_format, field, prop, mask](double value) {
            if (!can_edit()) return;
            RichTextParagraphFormat fmt = current_text_paragraph_format();
            if (field == &Layer::paragraph_indent_left)
                fmt.indent_left = (float)value;
            else if (field == &Layer::paragraph_indent_right)
                fmt.indent_right = (float)value;
            else if (field == &Layer::paragraph_indent_first_line)
                fmt.indent_first_line = (float)value;
            else if (field == &Layer::paragraph_space_before)
                fmt.space_before = (float)value;
            else if (field == &Layer::paragraph_space_after)
                fmt.space_after = (float)value;
            apply_text_paragraph_format(fmt, mask);
            if (!text_edit_active())
                set_animated_value(layer_.get()->*prop, local_time(), value);
            emit_change();
        });
    };
    connect_keyframed_paragraph_spin(spn_paragraph_indent_left_, &Layer::paragraph_indent_left,
                                     &Layer::paragraph_indent_left_prop, RichTextParagraphIndentLeft);
    connect_keyframed_paragraph_spin(spn_paragraph_indent_right_, &Layer::paragraph_indent_right,
                                     &Layer::paragraph_indent_right_prop, RichTextParagraphIndentRight);
    connect_keyframed_paragraph_spin(spn_paragraph_indent_first_line_, &Layer::paragraph_indent_first_line,
                                     &Layer::paragraph_indent_first_line_prop, RichTextParagraphIndentFirstLine);
    connect_keyframed_paragraph_spin(spn_paragraph_space_before_, &Layer::paragraph_space_before,
                                     &Layer::paragraph_space_before_prop, RichTextParagraphSpaceBefore);
    connect_keyframed_paragraph_spin(spn_paragraph_space_after_, &Layer::paragraph_space_after,
                                     &Layer::paragraph_space_after_prop, RichTextParagraphSpaceAfter);
    connect(chk_paragraph_hyphenate_, &QCheckBox::toggled,
            this, [can_edit, emit_change, current_text_paragraph_format, apply_text_paragraph_format](bool v) {
                if (!can_edit()) return;
                RichTextParagraphFormat fmt = current_text_paragraph_format();
                fmt.hyphenate = v;
                /* Qt's temporary adapter exposes hyphenation as a document-wide
                 * wrap policy, not a per-block property. Keep this canonical at
                 * textbox level until the Phase 12B shaper supports paragraph-local
                 * hyphenation. */
                apply_text_paragraph_format(fmt, RichTextParagraphHyphenate, true);
                emit_change();
            });
    connect(btn_text_color_, &QPushButton::clicked,
            this, [this, can_edit, text_edit_active, local_time, emit_change, apply_text_char_format, current_text_char_format]() {
                if (!can_edit()) return;
                const RichTextCharFormat current = current_text_char_format();
                QColor initial = color_from_argb(text_edit_active()
                    ? current.fill.color
                    : eval_text_color(*layer_, local_time()));
                QColor picked = bgl_pick_color(initial, this, bgl_tr("OBSTitles.TextColor"));
                if (!picked.isValid()) return;
                RichTextCharFormat fmt = current_text_char_format();
                fmt.fill.type = 0;
                fmt.fill.color = argb_from_color(picked);
                apply_text_char_format(fmt, RichTextCharFillColor);
                if (!text_edit_active())
                    set_color_channels_at(*layer_, true, local_time(), fmt.fill.color);
                style_color_button(btn_text_color_, fmt.fill.color);
                emit_change();
            });
    auto open_color_selector = [this, can_edit, text_edit_active, emit_change, apply_text_char_format, current_text_char_format, apply_text_fill_format, local_time, control_style,
                                panel_bg_name, panel_text_name, control_bg_name, control_text_name,
                                button_bg_name, button_text_name, border_name, highlight_name,
                                highlighted_text_name, subtle_text_name, hover_bg_name, section_bg_name](bool stroke) {
        if (!can_edit()) return;
        const bool text_like = layer_->type == LayerType::Text ||
                               layer_->type == LayerType::Clock ||
                               layer_->type == LayerType::Ticker;
        const bool text_fill = !stroke && text_like;
        const bool inline_text_fill = text_fill && text_edit_active();
        const bool inline_text_stroke = stroke && text_like && text_edit_active();
        const RichTextCharFormat initial_inline_format =
            (inline_text_fill || inline_text_stroke)
                ? current_text_char_format()
                : RichTextCharFormat();
        const RichTextFill initial_inline_fill = initial_inline_format.fill;
        const RichTextStroke initial_inline_stroke = initial_inline_format.stroke;

