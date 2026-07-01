                label->setVisible(show_exposed_options);
        }
        if (chk_exposed_single_value_) {
            chk_exposed_single_value_->setVisible(show_exposed_options);
            chk_exposed_single_value_->setEnabled(show_exposed_options);
            if (auto *label = live_form->labelForField(chk_exposed_single_value_))
                label->setVisible(show_exposed_options);
        }
        if (chk_ignore_persistence_) {
            chk_ignore_persistence_->setVisible(show_ignore_persistence);
            if (auto *label = live_form->labelForField(chk_ignore_persistence_))
                label->setVisible(show_ignore_persistence);
        }
        if (live_edit_box_)
            live_edit_box_->setVisible(!is_fixed_canvas_layer &&
                                       (show_scene_mask || show_expose_to_dock || show_ignore_persistence));
    }
    rect_box_->setVisible(is_text_like || is_rect || is_image);
    rect_box_->setTitle(QString());
    if (auto *shape_title = rect_box_->findChild<QLabel *>(QStringLiteral("OBSTitlesShapePanelTitle"))) {
        shape_title->setText(is_rect ? bgl_tr("OBSTitles.Shape")
                            : is_image ? bgl_tr("OBSTitles.ImageSize")
                                       : (is_clock ? bgl_tr("OBSTitles.ClockBox")
                                                   : (is_ticker ? bgl_tr("OBSTitles.TickerBox")
                                                                : bgl_tr("OBSTitles.TextBox"))));
    }
    const bool is_shape_layer = layer_->type == LayerType::Shape || layer_->type == LayerType::SolidRect;
    const ShapeType current_shape = layer_->type == LayerType::SolidRect ? ShapeType::RoundedRectangle : layer_->shape_type;
    const bool show_corner_radius = is_image ||
                                    (is_shape_layer &&
                                     (current_shape == ShapeType::Rectangle ||
                                      current_shape == ShapeType::RoundedRectangle));
    const bool show_star_controls = is_shape_layer && current_shape == ShapeType::Star;
    const bool show_polygon_controls = is_shape_layer && current_shape == ShapeType::Polygon;
    const bool show_roundness = is_shape_layer &&
                                current_shape != ShapeType::Ellipse &&
                                current_shape != ShapeType::Line &&
                                current_shape != ShapeType::Rectangle &&
                                current_shape != ShapeType::RoundedRectangle;
    const bool show_transform_scale = !is_shape_layer;
    const bool show_transform_size = is_shape_layer;
    const bool show_shape_panel_size = !is_shape_layer && (is_text_like || is_image);
    auto set_visible = [](QWidget *widget, bool visible) {
        if (widget) widget->setVisible(visible);
    };
    set_visible(btn_kf_scale_x_, show_transform_scale);
    set_visible(transform_scale_label_, show_transform_scale);
    set_visible(transform_scale_field_x_, show_transform_scale);
    set_visible(chk_scale_lock_, show_transform_scale);
    set_visible(transform_scale_field_y_, show_transform_scale);
    set_visible(btn_kf_transform_size_, show_transform_size);
    set_visible(transform_size_label_, show_transform_size);
    set_visible(transform_size_field_w_, show_transform_size);
    set_visible(chk_transform_size_lock_, show_transform_size);
    set_visible(transform_size_field_h_, show_transform_size);
    set_visible(row_shape_scale_options_, is_shape_layer || is_image);
    set_visible(chk_shape_scale_stroke_, is_shape_layer);
    set_visible(chk_shape_scale_corners_, is_shape_layer || is_image);
    set_visible(btn_kf_width_, show_shape_panel_size);
    set_visible(shape_size_label_, show_shape_panel_size);
    set_visible(shape_size_field_w_, show_shape_panel_size);
    set_visible(shape_size_field_h_, show_shape_panel_size);
    set_visible(chk_size_lock_, show_shape_panel_size && is_image);
    if (row_rect_corners_) row_rect_corners_->setVisible(show_corner_radius);
    if (chk_corner_lock_) chk_corner_lock_->setVisible(show_corner_radius);
    if (spn_corner_bevel_roundness_) spn_corner_bevel_roundness_->setVisible(show_corner_radius);
    if (grp_shape_type_) {
        for (auto *button : grp_shape_type_->buttons())
            button->setVisible(is_shape_layer);
    }
    if (auto *shape_types_row = rect_box_->findChild<QWidget *>(QStringLiteral("OBSTitlesShapeTypeButtonsRow")))
        shape_types_row->setVisible(is_shape_layer);
    if (btn_shape_defaults_) {
        btn_shape_defaults_->setVisible(is_shape_layer || is_image);
        btn_shape_defaults_->setToolTip(is_image ? bgl_tr("OBSTitles.RestoreImageSizeDefaults")
                                                    : bgl_tr("OBSTitles.RestoreShapeDefaults"));
    }
    if (spn_shape_points_) spn_shape_points_->setVisible(show_star_controls);
    if (spn_shape_sides_) spn_shape_sides_->setVisible(show_polygon_controls);
    if (spn_shape_inner_radius_) spn_shape_inner_radius_->setVisible(show_star_controls);
    if (spn_shape_outer_radius_) spn_shape_outer_radius_->setVisible(show_star_controls);
    if (spn_shape_roundness_) spn_shape_roundness_->setVisible(show_roundness);
    const bool supports_fill_type = is_rect || is_text_like;
    const bool solid_fill_active = is_rect && layer_->fill_type == 0;
    if (cmb_fill_type_) cmb_fill_type_->setVisible(false);
    if (btn_fill_color_) btn_fill_color_->setVisible(false);
    if (gradient_box_) gradient_box_->setVisible(false);
    const bool supports_text_box_auto_size = is_text || is_clock;
    if (chk_text_box_width_to_text_) chk_text_box_width_to_text_->setVisible(supports_text_box_auto_size);
    if (chk_text_box_height_to_text_) chk_text_box_height_to_text_->setVisible(supports_text_box_auto_size);
    if (spn_max_text_box_width_) spn_max_text_box_width_->setVisible(supports_text_box_auto_size);
    if (spn_max_text_box_height_) spn_max_text_box_height_->setVisible(supports_text_box_auto_size);
    btn_kf_text_color_->setVisible(false);
    const bool gradient_text_active = is_text_like && layer_->fill_type == 1;
    if (btn_text_color_) btn_text_color_->setEnabled(!gradient_text_active);
    if (btn_kf_text_color_) btn_kf_text_color_->setEnabled(!gradient_text_active);
    btn_kf_fill_color_->setVisible(false);
    if (row_fill_type_) row_fill_type_->setVisible(false);
    if (row_fill_color_) row_fill_color_->setVisible(false);
    const bool fill_controls_enabled = !is_scene_mask_layer;
    if (cmb_fill_type_) cmb_fill_type_->setEnabled(fill_controls_enabled);
    if (row_fill_type_) row_fill_type_->setEnabled(fill_controls_enabled);
    if (btn_fill_color_) btn_fill_color_->setEnabled(fill_controls_enabled);
    if (row_fill_color_) row_fill_color_->setEnabled(fill_controls_enabled);
    if (gradient_box_) gradient_box_->setEnabled(fill_controls_enabled);
    if (btn_kf_fill_color_) btn_kf_fill_color_->setEnabled(fill_controls_enabled);
    if (outline_box_) outline_box_->setVisible(false);
    if (background_gradient_box_) background_gradient_box_->setVisible(false);
    if (auto *form = rect_box_->findChild<QFormLayout *>()) {
        if (auto *label = form->labelForField(row_rect_corners_)) label->setVisible(show_corner_radius);
        if (auto *label = form->labelForField(spn_corner_bevel_roundness_)) label->setVisible(show_corner_radius);
        if (auto *label = form->labelForField(spn_shape_points_)) label->setVisible(show_star_controls);
        if (auto *label = form->labelForField(spn_shape_sides_)) label->setVisible(show_polygon_controls);
        if (auto *label = form->labelForField(spn_shape_inner_radius_)) label->setVisible(show_star_controls);
        if (auto *label = form->labelForField(spn_shape_outer_radius_)) label->setVisible(show_star_controls);
        if (auto *label = form->labelForField(spn_shape_roundness_)) label->setVisible(show_roundness);
        if (auto *label = form->labelForField(row_fill_type_))
            label->setVisible(false);
        if (auto *label = form->labelForField(row_fill_color_))
            label->setVisible(false);
        for (QWidget *field : std::initializer_list<QWidget *>{chk_text_box_width_to_text_, spn_max_text_box_width_,
                                                               chk_text_box_height_to_text_, spn_max_text_box_height_})
            if (auto *label = form->labelForField(field)) label->setVisible(supports_text_box_auto_size);
    }
    image_box_->setVisible(is_image);
    if (image_box_size_box_) image_box_size_box_->setVisible(is_image);
    if (shadow_box_) shadow_box_->setVisible(false);

    double lt = std::clamp(playhead_ - layer_->in_time, 0.0,
                           std::max(0.0, layer_->out_time - layer_->in_time));
    spn_px_->setValue(layer_->position.is_animated()
                      ? layer_->position.evaluate(lt).x
                      : layer_->position.static_value.x);
    spn_py_->setValue(layer_->position.is_animated()
                      ? layer_->position.evaluate(lt).y
                      : layer_->position.static_value.y);
    spn_scale_x_->setValue((layer_->scale.is_animated()
                            ? layer_->scale.evaluate(lt).x
                            : layer_->scale.static_value.x) * 100.0);
    spn_scale_y_->setValue((layer_->scale.is_animated()
                            ? layer_->scale.evaluate(lt).y
                            : layer_->scale.static_value.y) * 100.0);
    if (chk_scale_lock_) chk_scale_lock_->setChecked(layer_->scale_lock);
    if (spn_transform_size_w_) spn_transform_size_w_->setValue(eval_box_width(*layer_, lt));
    if (spn_transform_size_h_) spn_transform_size_h_->setValue(eval_box_height(*layer_, lt));
    if (chk_transform_size_lock_) chk_transform_size_lock_->setChecked(layer_->lock_aspect_ratio);
    if (chk_shape_scale_stroke_) chk_shape_scale_stroke_->setChecked(layer_->scale_stroke_with_shape);
    if (chk_shape_scale_corners_) chk_shape_scale_corners_->setChecked(layer_->scale_corners_with_shape);
    spn_rot_->setValue(layer_->rotation.is_animated()
                       ? layer_->rotation.evaluate(lt)
                       : layer_->rotation.static_value);
    spn_opacity_->setValue(layer_->opacity.is_animated()
                           ? layer_->opacity.evaluate(lt)
                           : layer_->opacity.static_value);
    if (spn_appearance_opacity_) spn_appearance_opacity_->setValue(spn_opacity_->value() * 100.0);
    const bool supports_fill_appearance = is_text_like || is_rect;
    if (btn_appearance_fill_color_) {
        if (layer_->fill_type == 1)
            style_gradient_button(btn_appearance_fill_color_,
                                  layer_->gradient_start_color,
                                  layer_->gradient_end_color,
                                  layer_->gradient_type);
        else
            style_color_button(btn_appearance_fill_color_,
                               is_text_like ? eval_text_color(*layer_, lt) : eval_fill_color(*layer_, lt));
        btn_appearance_fill_color_->setText(QString());
        btn_appearance_fill_color_->setEnabled(supports_fill_appearance && fill_controls_enabled);
    }
    if (btn_kf_appearance_fill_) btn_kf_appearance_fill_->setEnabled(supports_fill_appearance && fill_controls_enabled);
    if (btn_appearance_stroke_color_) {
        if (layer_->stroke_fill_type == 2)
            style_gradient_button(btn_appearance_stroke_color_,
                                  layer_->stroke_gradient_start_color,
                                  layer_->stroke_gradient_end_color,
                                  layer_->stroke_gradient_type);
        else
            style_color_button(btn_appearance_stroke_color_, layer_->stroke_color);
        btn_appearance_stroke_color_->setText(QString());
        btn_appearance_stroke_color_->setEnabled(supports_outline);
        btn_appearance_stroke_color_->setVisible(supports_outline);
    }
    if (btn_appearance_stroke_label_) btn_appearance_stroke_label_->setVisible(supports_outline);
    if (spn_appearance_stroke_width_) {
        spn_appearance_stroke_width_->setValue(layer_->stroke_width);
        spn_appearance_stroke_width_->setEnabled(supports_outline);
        spn_appearance_stroke_width_->setVisible(supports_outline);
    }
    if (btn_kf_appearance_stroke_) {
        btn_kf_appearance_stroke_->setEnabled(false);
        btn_kf_appearance_stroke_->setVisible(supports_outline);
    }
    spn_origin_x_->setValue(eval_origin_x(*layer_, lt));
    spn_origin_y_->setValue(eval_origin_y(*layer_, lt));
    const int anchor_index = anchor_index_from_layer(*layer_);
    cmb_anchor_->setCurrentIndex(anchor_index);
    if (btn_anchor_grid_) { btn_anchor_grid_->setProperty("active_index", anchor_index); btn_anchor_grid_->update(); }

    if (is_image) {
        double raw_w = eval_image_width(*layer_, lt);
        double raw_h = eval_image_height(*layer_, lt);
        if (raw_w <= 0.0 || raw_h <= 0.0) {
            const QSize intrinsic = editor_image_intrinsic_size(QString::fromStdString(layer_->image_path));
            if (intrinsic.isValid() && !intrinsic.isEmpty()) {
                raw_w = intrinsic.width();
                raw_h = intrinsic.height();
            }
        }
        const bgs::ImageDisplaySize display = bgs::calculate_image_display_size(
            layer_->image_box_mode, layer_->image_size_auto_fit,
            eval_box_width(*layer_, lt), eval_box_height(*layer_, lt), raw_w, raw_h);
        spn_layer_w_->setValue(display.width);
        spn_layer_h_->setValue(display.height);
    } else {
        spn_layer_w_->setValue(eval_box_width(*layer_, lt));
        spn_layer_h_->setValue(eval_box_height(*layer_, lt));
    }
    if (spn_image_box_w_) spn_image_box_w_->setValue(eval_box_width(*layer_, lt));
    if (spn_image_box_h_) spn_image_box_h_->setValue(eval_box_height(*layer_, lt));
    if (chk_size_lock_) chk_size_lock_->setChecked(layer_->lock_aspect_ratio);
    if (chk_image_box_size_lock_) chk_image_box_size_lock_->setChecked(layer_->image_box_lock_aspect_ratio);
    if (chk_corner_lock_) chk_corner_lock_->setChecked(layer_->corner_radius_locked);
    if (spn_rect_corner_tl_) spn_rect_corner_tl_->setValue(layer_->corner_radius_tl);
    if (spn_rect_corner_tr_) spn_rect_corner_tr_->setValue(layer_->corner_radius_tr);
    if (spn_rect_corner_br_) spn_rect_corner_br_->setValue(layer_->corner_radius_br);
    if (spn_rect_corner_bl_) spn_rect_corner_bl_->setValue(layer_->corner_radius_bl);
    if (spn_corner_bevel_roundness_) spn_corner_bevel_roundness_->setValue(layer_->corner_bevel_roundness);
    if (cmb_shape_type_) {
        int shape_idx = cmb_shape_type_->findData((int)current_shape);
        cmb_shape_type_->setCurrentIndex(shape_idx >= 0 ? shape_idx : 0);
    }
    if (grp_shape_type_) {
        if (auto *button = grp_shape_type_->button((int)current_shape))
            button->setChecked(true);
    }
    if (spn_shape_points_) spn_shape_points_->setValue(layer_->shape_points);
    if (spn_shape_sides_) spn_shape_sides_->setValue(layer_->shape_sides);
    if (spn_shape_inner_radius_) spn_shape_inner_radius_->setValue(layer_->shape_inner_radius);
    if (spn_shape_outer_radius_) spn_shape_outer_radius_->setValue(layer_->shape_outer_radius);
    if (spn_shape_roundness_) {
        const bool rectangle_roundness = current_shape == ShapeType::Rectangle ||
                                         current_shape == ShapeType::RoundedRectangle;
        double roundness = rectangle_roundness ? layer_->corner_radius : layer_->shape_roundness;
        if (current_shape == ShapeType::Path && !layer_->path_points.empty())
            roundness = layer_->path_points.front().corner_radius;
        spn_shape_roundness_->setValue(roundness);
    }
    if (btn_text_external_binding_) {
        const auto *binding = external_binding_for_property(*layer_, ExternalPropertyPaths::TextContent);
        QSignalBlocker block(btn_text_external_binding_);
        btn_text_external_binding_->setChecked(binding != nullptr);
        btn_text_external_binding_->setProperty("externalDataBound", binding != nullptr);
        btn_text_external_binding_->setToolTip(binding
            ? QStringLiteral("External data: %1 · %2")
                .arg(QString::fromStdString(binding->source_id),
                     QString::fromStdString(binding->field_path))
            : QStringLiteral("Bind text content to external data"));
    }
    if (btn_image_external_binding_) {
        const auto *binding = external_binding_for_property(*layer_, ExternalPropertyPaths::ImagePath);
        QSignalBlocker block(btn_image_external_binding_);
        btn_image_external_binding_->setChecked(binding != nullptr);
        btn_image_external_binding_->setProperty("externalDataBound", binding != nullptr);
        btn_image_external_binding_->setToolTip(binding
            ? QStringLiteral("External data: %1 · %2")
                .arg(QString::fromStdString(binding->source_id),
                     QString::fromStdString(binding->field_path))
            : QStringLiteral("Bind image path to external data"));
    }

    edit_image_path_->setText(QString::fromStdString(layer_->image_path));
    set_image_preview_label(lbl_image_preview_, QString::fromStdString(layer_->image_path));
    if (cmb_image_scale_filter_) {
        QSignalBlocker block(cmb_image_scale_filter_);
        int filter_index = cmb_image_scale_filter_->findData((int)layer_->scale_filter);
        cmb_image_scale_filter_->setCurrentIndex(filter_index >= 0 ? filter_index : 1);
    }
    if (cmb_image_box_mode_) {
        QSignalBlocker block(cmb_image_box_mode_);
        int mode_index = cmb_image_box_mode_->findData((int)layer_->image_box_mode);
        cmb_image_box_mode_->setCurrentIndex(mode_index >= 0 ? mode_index : 0);
    }
    if (chk_image_crop_when_outside_box_) {
        QSignalBlocker block(chk_image_crop_when_outside_box_);
        chk_image_crop_when_outside_box_->setChecked(layer_->image_crop_when_outside_box);
    }
    const bool stretch_image_size = is_image && layer_->image_size_auto_fit &&
                                    layer_->image_box_mode == ImageBoxMode::StretchToFill;
    if (spn_layer_w_) spn_layer_w_->setEnabled(!stretch_image_size);
    if (spn_layer_h_) spn_layer_h_->setEnabled(!stretch_image_size);
    if (chk_size_lock_) chk_size_lock_->setEnabled(!stretch_image_size);
    if (btn_kf_width_) btn_kf_width_->setEnabled(!stretch_image_size);
    if (btn_image_anchor_grid_) {
        int image_anchor_x = layer_->image_anchor_x < 0.25f ? 0 : (layer_->image_anchor_x > 0.75f ? 2 : 1);
        int image_anchor_y = layer_->image_anchor_y < 0.25f ? 0 : (layer_->image_anchor_y > 0.75f ? 2 : 1);
        btn_image_anchor_grid_->setProperty("active_index", image_anchor_y * 3 + image_anchor_x);
        btn_image_anchor_grid_->update();
    }
    style_color_button(btn_text_color_, eval_text_color(*layer_, lt));
    style_color_button(btn_fill_color_, eval_fill_color(*layer_, lt));
    if (cmb_fill_type_) {
        int fill_idx = cmb_fill_type_->findData(layer_->fill_type);
        cmb_fill_type_->setCurrentIndex(fill_idx >= 0 ? fill_idx : 0);
    }
    if (cmb_gradient_type_) {
        int gradient_idx = cmb_gradient_type_->findData(layer_->gradient_type);
        cmb_gradient_type_->setCurrentIndex(gradient_idx >= 0 ? gradient_idx : 0);
    }
    if (cmb_gradient_spread_) {
        int spread_idx = cmb_gradient_spread_->findData(layer_->gradient_spread);
        cmb_gradient_spread_->setCurrentIndex(spread_idx >= 0 ? spread_idx : 0);
    }
    if (btn_gradient_start_color_) style_color_button(btn_gradient_start_color_, layer_->gradient_start_color);
    if (btn_gradient_end_color_) style_color_button(btn_gradient_end_color_, layer_->gradient_end_color);
    if (spn_gradient_start_pos_) spn_gradient_start_pos_->setValue(layer_->gradient_start_pos);
    if (spn_gradient_end_pos_) spn_gradient_end_pos_->setValue(layer_->gradient_end_pos);
    if (spn_gradient_start_opacity_) spn_gradient_start_opacity_->setValue(layer_->gradient_start_opacity);
    if (spn_gradient_end_opacity_) spn_gradient_end_opacity_->setValue(layer_->gradient_end_opacity);
    if (spn_gradient_opacity_) spn_gradient_opacity_->setValue(layer_->gradient_opacity);
    if (spn_gradient_angle_) spn_gradient_angle_->setValue(layer_->gradient_angle);
    if (spn_gradient_center_x_) spn_gradient_center_x_->setValue(layer_->gradient_center_x);
    if (spn_gradient_center_y_) spn_gradient_center_y_->setValue(layer_->gradient_center_y);
    if (spn_gradient_scale_) spn_gradient_scale_->setValue(layer_->gradient_scale);
    if (spn_gradient_focal_x_) spn_gradient_focal_x_->setValue(layer_->gradient_focal_x);
    if (spn_gradient_focal_y_) spn_gradient_focal_y_->setValue(layer_->gradient_focal_y);
    if (chk_text_box_width_to_text_) chk_text_box_width_to_text_->setChecked(layer_->text_box_width_to_text);
    if (chk_text_box_height_to_text_) chk_text_box_height_to_text_->setChecked(layer_->text_box_height_to_text);
    if (spn_max_text_box_width_) { spn_max_text_box_width_->setValue(layer_->max_text_box_width); spn_max_text_box_width_->setEnabled(true); }
    if (spn_max_text_box_height_) { spn_max_text_box_height_->setValue(layer_->max_text_box_height); spn_max_text_box_height_->setEnabled(true); }

    auto set_kf_icon = [](QPushButton *button, bool active, bool has_keyframes) {
        if (!button) return;
        const bool outlined = has_keyframes && !active;
        button->setIcon(keyframe_diamond_icon(active, outlined));
        button->setProperty("active", active);
        button->setProperty("outlined", outlined);
        button->style()->unpolish(button);
        button->style()->polish(button);
    };
    auto set_prop_kf_icon = [&](QPushButton *button, const AnimatedProperty &prop) {
        set_kf_icon(button, keyframe_at_time(prop, lt), prop.is_animated());
    };
    auto set_group_kf_icon = [&](QPushButton *button, std::initializer_list<const AnimatedProperty *> props) {
        set_kf_icon(button, any_keyframe_at_time(props, lt), any_keyframes(props));
    };
    auto set_vec_kf_icon = [&](QPushButton *button, const AnimatedVec2Property &prop) {
        set_kf_icon(button, keyframe_at_time(prop, lt), prop.is_animated());
    };
    set_vec_kf_icon(btn_kf_pos_x_, layer_->position);
    set_vec_kf_icon(btn_kf_pos_y_, layer_->position);
    set_vec_kf_icon(btn_kf_scale_x_, layer_->scale);
    set_vec_kf_icon(btn_kf_scale_y_, layer_->scale);
    set_vec_kf_icon(btn_kf_transform_size_, layer_->size);
    set_prop_kf_icon(btn_kf_rotation_, layer_->rotation);
    set_prop_kf_icon(btn_kf_opacity_, layer_->opacity);
    set_vec_kf_icon(btn_kf_origin_x_, layer_->origin_prop);
    set_vec_kf_icon(btn_kf_origin_y_, layer_->origin_prop);
    set_prop_kf_icon(btn_kf_paragraph_indent_left_, layer_->paragraph_indent_left_prop);
    set_prop_kf_icon(btn_kf_paragraph_indent_right_, layer_->paragraph_indent_right_prop);
    set_prop_kf_icon(btn_kf_paragraph_indent_first_line_, layer_->paragraph_indent_first_line_prop);
    set_prop_kf_icon(btn_kf_font_size_, layer_->font_size_prop);
    set_prop_kf_icon(btn_kf_char_scale_x_, layer_->char_scale_x_prop);
    set_prop_kf_icon(btn_kf_char_scale_y_, layer_->char_scale_y_prop);
    set_prop_kf_icon(btn_kf_char_tracking_, layer_->char_tracking_prop);
    set_prop_kf_icon(btn_kf_baseline_shift_, layer_->baseline_shift_prop);
    set_prop_kf_icon(btn_kf_paragraph_space_before_, layer_->paragraph_space_before_prop);
    set_prop_kf_icon(btn_kf_paragraph_space_after_, layer_->paragraph_space_after_prop);
    set_vec_kf_icon(btn_kf_width_, is_image ? layer_->image_size : layer_->size);
    set_vec_kf_icon(btn_kf_image_box_size_, layer_->size);
    set_group_kf_icon(btn_kf_text_color_, {&layer_->text_color_a, &layer_->text_color_r,
                                           &layer_->text_color_g, &layer_->text_color_b});
    set_group_kf_icon(btn_kf_fill_color_, {&layer_->fill_color_a, &layer_->fill_color_r,
                                           &layer_->fill_color_g, &layer_->fill_color_b});
    if (is_text_like)
        set_group_kf_icon(btn_kf_appearance_fill_, {&layer_->text_color_a, &layer_->text_color_r,
                                                    &layer_->text_color_g, &layer_->text_color_b});
    else
        set_group_kf_icon(btn_kf_appearance_fill_, {&layer_->fill_color_a, &layer_->fill_color_r,
                                                    &layer_->fill_color_g, &layer_->fill_color_b});
    set_kf_icon(btn_kf_appearance_stroke_, false, false);

    const QString panel_text = is_clock
        ? QString::fromStdString(layer_->clock_format)
        : QString::fromStdString((rich_text_document_ensure_canonical(*layer_), layer_->rich_text.plain_text));
    /* Property refreshes are triggered after every live text mutation. Avoid
     * replacing an already-identical QTextDocument: setPlainText() resets the
     * cursor/selection to position zero, which made typing in Text, Ticker and
     * Clock layers jump to the beginning after every character. When an
     * external change really did alter the value, preserve the user's logical
     * selection as far as the new text permits. */
    if (txt_content_->toPlainText() != panel_text) {
        const QTextCursor previous_cursor = txt_content_->textCursor();
        const int previous_anchor = previous_cursor.anchor();
        const int previous_position = previous_cursor.position();
        QSignalBlocker blocker(txt_content_);
        txt_content_->setPlainText(panel_text);
        QTextCursor restored = txt_content_->textCursor();
        const int text_length = panel_text.size();
        restored.setPosition(std::clamp(previous_anchor, 0, text_length));
        restored.setPosition(std::clamp(previous_position, 0, text_length),
                             QTextCursor::KeepAnchor);
        txt_content_->setTextCursor(restored);
    }
    if (cmb_ticker_playback_mode_) {
        const int playback_idx = cmb_ticker_playback_mode_->findData(layer_->ticker_playback_mode);
        cmb_ticker_playback_mode_->setCurrentIndex(playback_idx >= 0 ? playback_idx : 0);
    }
    if (btn_ticker_pause_)
        btn_ticker_pause_->setVisible(layer_->ticker_playback_mode != static_cast<int>(TickerPlaybackMode::CustomPlayback));
    if (spn_ticker_completion_) {
        const double completion = layer_->ticker_completion_prop.is_animated()
            ? layer_->ticker_completion_prop.evaluate(lt) : layer_->ticker_completion;
        spn_ticker_completion_->setValue(std::clamp(completion, 0.0, 100.0));
    }
    set_prop_kf_icon(btn_kf_ticker_completion_, layer_->ticker_completion_prop);
    update_ticker_runtime_button();
    int ticker_style_idx = cmb_ticker_style_->findData(layer_->ticker_style);
    cmb_ticker_style_->setCurrentIndex(ticker_style_idx >= 0 ? ticker_style_idx : 0);
    spn_ticker_speed_->setValue(layer_->ticker_speed);
    spn_ticker_line_hold_->setValue(layer_->ticker_line_hold);
    cmb_ticker_direction_->clear();
    if (layer_->ticker_style == 0) {
        cmb_ticker_direction_->addItem(bgl_tr("OBSTitles.LeftToRight"), 0);
        cmb_ticker_direction_->addItem(bgl_tr("OBSTitles.RightToLeft"), 1);
    } else {
        cmb_ticker_direction_->addItem(bgl_tr("OBSTitles.TopToBottom"), 0);
        cmb_ticker_direction_->addItem(bgl_tr("OBSTitles.BottomToTop"), 1);
    }
    int ticker_direction_idx = cmb_ticker_direction_->findData(layer_->ticker_direction);
    cmb_ticker_direction_->setCurrentIndex(ticker_direction_idx >= 0 ? ticker_direction_idx : 0);
    int fi = cmb_font_->findText(QString::fromStdString(layer_->font_family));
    if (fi >= 0) cmb_font_->setCurrentIndex(fi);
    populate_font_style_combo(cmb_font_style_, QString::fromStdString(layer_->font_family), QString::fromStdString(layer_->font_style));
    spn_size_->setValue((int)std::clamp(std::round(layer_->font_size_prop.is_animated() ? layer_->font_size_prop.evaluate(lt) : (double)layer_->font_size), 1.0, 512.0));
    chk_bold_->setChecked(layer_->font_bold);
    chk_italic_->setChecked(layer_->font_italic);
    if (chk_font_kerning_) chk_font_kerning_->setChecked(layer_->font_kerning);
    if (cmb_kerning_mode_) {
        int ki = cmb_kerning_mode_->findData(layer_->kerning_mode);
        cmb_kerning_mode_->setCurrentIndex(ki >= 0 ? ki : 0);
    }
    if (spn_kerning_value_) {
        spn_kerning_value_->setValue(layer_->manual_kerning);
        spn_kerning_value_->setEnabled(layer_->kerning_mode == 2);
    }
    if (spn_text_leading_) spn_text_leading_->setValue(layer_->text_leading);
    if (spn_char_tracking_) spn_char_tracking_->setValue(layer_->char_tracking_prop.is_animated() ? layer_->char_tracking_prop.evaluate(lt) : (double)layer_->char_tracking);
    if (spn_char_scale_x_) spn_char_scale_x_->setValue((layer_->char_scale_x_prop.is_animated() ? layer_->char_scale_x_prop.evaluate(lt) : (double)layer_->char_scale_x) * 100.0);
    if (spn_char_scale_y_) spn_char_scale_y_->setValue((layer_->char_scale_y_prop.is_animated() ? layer_->char_scale_y_prop.evaluate(lt) : (double)layer_->char_scale_y) * 100.0);
    if (spn_baseline_shift_) spn_baseline_shift_->setValue(layer_->baseline_shift_prop.is_animated() ? layer_->baseline_shift_prop.evaluate(lt) : (double)layer_->baseline_shift);
    if (btn_all_caps_) btn_all_caps_->setChecked(layer_->text_style == 1);
    if (btn_small_caps_) btn_small_caps_->setChecked(layer_->text_style == 2);
    if (btn_superscript_) btn_superscript_->setChecked(layer_->text_style == 3);
    if (btn_subscript_) btn_subscript_->setChecked(layer_->text_style == 4);
    if (btn_underline_) btn_underline_->setChecked(layer_->text_underline);
    const bool inline_text_properties = is_text_like &&
                                        active_text_edit_layer_id_ == layer_->id;
    const bool keyframe_text_defaults_enabled = !inline_text_properties;
    for (QPushButton *button : {btn_kf_font_size_, btn_kf_char_scale_x_,
                                btn_kf_char_scale_y_, btn_kf_char_tracking_,
                                btn_kf_baseline_shift_, btn_kf_paragraph_indent_left_,
                                btn_kf_paragraph_indent_right_,
                                btn_kf_paragraph_indent_first_line_,
                                btn_kf_paragraph_space_before_,
                                btn_kf_paragraph_space_after_, btn_kf_text_color_,
                                btn_kf_appearance_fill_}) {
        if (button)
            button->setEnabled(keyframe_text_defaults_enabled);
    }
    /* Rich text is the canonical source both during inline editing and when
     * the textbox is only selected. Outside inline mode summarize the complete
     * textbox, so every property that differs between runs displays its mixed
     * state instead of silently falling back to the layer default. */
    const bool use_rich_char_summary = is_text_like;
    bool rich_font_family_mixed = false;
    if (use_rich_char_summary) {
        const bool active = inline_text_properties;
        RichTextCharFormatSummary summary = summarize_rich_text_char_format(
            *layer_, active, std::max(0.0, playhead_ - layer_->in_time));
        const RichTextCharFormat &fmt = summary.format;
        rich_font_family_mixed = summary.mixed & RichTextCharFontFamily;
        int rich_fi = cmb_font_->findText(QString::fromStdString(fmt.font_family));
        if (rich_fi >= 0) cmb_font_->setCurrentIndex(rich_fi);
        populate_font_style_combo(cmb_font_style_, QString::fromStdString(fmt.font_family), QString::fromStdString(fmt.font_style));
        int rich_style_i = cmb_font_style_->findText(QString::fromStdString(fmt.font_style));
        if (rich_style_i >= 0) cmb_font_style_->setCurrentIndex(rich_style_i);
        spn_size_->setValue(fmt.font_size);
        chk_bold_->setChecked(fmt.bold);
        chk_italic_->setChecked(fmt.italic);
        if (chk_font_kerning_) chk_font_kerning_->setChecked(fmt.kerning);
        if (cmb_kerning_mode_) {
            int rich_kerning_i = cmb_kerning_mode_->findData(fmt.kerning_mode);
            cmb_kerning_mode_->setCurrentIndex(rich_kerning_i >= 0 ? rich_kerning_i : 0);
        }
        if (spn_kerning_value_) {
            spn_kerning_value_->setValue(fmt.manual_kerning);
            spn_kerning_value_->setEnabled(fmt.kerning_mode == 2);
        }
        if (spn_char_tracking_) spn_char_tracking_->setValue(fmt.tracking);
        if (spn_char_scale_x_) spn_char_scale_x_->setValue(fmt.scale_x * 100.0);
        if (spn_char_scale_y_) spn_char_scale_y_->setValue(fmt.scale_y * 100.0);
        if (spn_baseline_shift_) spn_baseline_shift_->setValue(fmt.baseline_shift);
        if (btn_underline_) btn_underline_->setChecked(fmt.underline);
        if (btn_strikethrough_) btn_strikethrough_->setChecked(fmt.strikethrough);
        if (btn_all_caps_) btn_all_caps_->setChecked(fmt.text_style == 1);
        if (btn_small_caps_) btn_small_caps_->setChecked(fmt.text_style == 2);
        if (btn_superscript_) btn_superscript_->setChecked(fmt.text_style == 3);
        if (btn_subscript_) btn_subscript_->setChecked(fmt.text_style == 4);
        if (btn_ligatures_) btn_ligatures_->setChecked(fmt.ligatures);
        if (btn_stylistic_alternates_) btn_stylistic_alternates_->setChecked(fmt.stylistic_alternates);
        if (btn_fractions_) btn_fractions_->setChecked(fmt.fractions);
        if (btn_opentype_features_) btn_opentype_features_->setChecked(fmt.opentype_features);
        int rich_text_style_i = cmb_text_style_->findData(fmt.text_style);
        cmb_text_style_->setCurrentIndex(rich_text_style_i >= 0 ? rich_text_style_i : 0);
        style_color_button(btn_text_color_, fmt.fill.color);
        if (btn_appearance_fill_color_) {
            if (fmt.fill.type == 1) {
                style_gradient_button(btn_appearance_fill_color_,
                                      fmt.fill.gradient_start_color,
                                      fmt.fill.gradient_end_color,
                                      fmt.fill.gradient_type);
            } else {
                style_color_button(btn_appearance_fill_color_, fmt.fill.color);
            }
            btn_appearance_fill_color_->setText(QString());
        }
        if (btn_appearance_stroke_color_) {
            if (fmt.stroke.fill.type == 1) {
                style_gradient_button(btn_appearance_stroke_color_,
                                      fmt.stroke.fill.gradient_start_color,
                                      fmt.stroke.fill.gradient_end_color,
                                      fmt.stroke.fill.gradient_type);
            } else {
                style_color_button(btn_appearance_stroke_color_,
                                   fmt.stroke.fill.color);
            }
            btn_appearance_stroke_color_->setText(QString());
        }
        if (btn_outline_color_)
            style_color_button(btn_outline_color_, fmt.stroke.fill.color);
        if (spn_appearance_stroke_width_)
            spn_appearance_stroke_width_->setValue(fmt.stroke.width);
        if (spn_outline_width_)
            spn_outline_width_->setValue(fmt.stroke.width);
        if (cmb_fill_type_) {
            int rich_fill_i = cmb_fill_type_->findData(fmt.fill.type);
            cmb_fill_type_->setCurrentIndex(rich_fill_i >= 0 ? rich_fill_i : 0);
        }
        if (cmb_gradient_type_) {
            int rich_gradient_i = cmb_gradient_type_->findData(fmt.fill.gradient_type);
            cmb_gradient_type_->setCurrentIndex(rich_gradient_i >= 0 ? rich_gradient_i : 0);
        }
        if (cmb_gradient_spread_) {
            int rich_spread_i = cmb_gradient_spread_->findData(fmt.fill.gradient_spread);
            cmb_gradient_spread_->setCurrentIndex(rich_spread_i >= 0 ? rich_spread_i : 0);
        }
        if (btn_gradient_start_color_) style_color_button(btn_gradient_start_color_, fmt.fill.gradient_start_color);
        if (btn_gradient_end_color_) style_color_button(btn_gradient_end_color_, fmt.fill.gradient_end_color);
        if (spn_gradient_start_pos_) spn_gradient_start_pos_->setValue(fmt.fill.gradient_start_pos);
        if (spn_gradient_end_pos_) spn_gradient_end_pos_->setValue(fmt.fill.gradient_end_pos);
        if (spn_gradient_start_opacity_) spn_gradient_start_opacity_->setValue(fmt.fill.gradient_start_opacity);
        if (spn_gradient_end_opacity_) spn_gradient_end_opacity_->setValue(fmt.fill.gradient_end_opacity);
        if (spn_gradient_opacity_) spn_gradient_opacity_->setValue(fmt.fill.gradient_opacity);
        if (spn_gradient_angle_) spn_gradient_angle_->setValue(fmt.fill.gradient_angle);
        if (spn_gradient_center_x_) spn_gradient_center_x_->setValue(fmt.fill.gradient_center_x);
        if (spn_gradient_center_y_) spn_gradient_center_y_->setValue(fmt.fill.gradient_center_y);
        if (spn_gradient_scale_) spn_gradient_scale_->setValue(fmt.fill.gradient_scale);
        if (spn_gradient_focal_x_) spn_gradient_focal_x_->setValue(fmt.fill.gradient_focal_x);
        if (spn_gradient_focal_y_) spn_gradient_focal_y_->setValue(fmt.fill.gradient_focal_y);

        const bool fill_type_mixed = summary.fill_mixed & RichTextFillTypeMixed;
        const bool fill_swatch_mixed =
            (summary.mixed & RichTextCharFillColor) != 0;
        const bool solid_color_mixed = fill_type_mixed ||
            (summary.fill_mixed & RichTextFillColorMixed);
        const bool gradient_start_color_mixed = fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientStartColorMixed);
        const bool gradient_end_color_mixed = fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientEndColorMixed);
        if (solid_color_mixed) style_color_button_mixed(btn_text_color_);
        if (fill_swatch_mixed)
            style_color_button_mixed(btn_appearance_fill_color_);
        const bool stroke_mixed =
            (summary.mixed & RichTextCharStroke) != 0;
        if (stroke_mixed) {
            style_color_button_mixed(btn_appearance_stroke_color_);
            if (btn_outline_color_)
                style_color_button_mixed(btn_outline_color_);
        }
        if (gradient_start_color_mixed) style_color_button_mixed(btn_gradient_start_color_);
        if (gradient_end_color_mixed) style_color_button_mixed(btn_gradient_end_color_);

        set_combo_mixed(cmb_font_, summary.mixed & RichTextCharFontFamily);
        set_combo_mixed(cmb_font_style_, summary.mixed & (RichTextCharFontFamily | RichTextCharFontStyle | RichTextCharBold | RichTextCharItalic));
        set_spin_mixed(spn_size_, summary.mixed & RichTextCharFontSize);
        set_button_mixed(chk_bold_, summary.mixed & RichTextCharBold);
        set_button_mixed(chk_italic_, summary.mixed & RichTextCharItalic);
        set_button_mixed(chk_font_kerning_, summary.kerning_mixed & RichTextKerningEnabledMixed);
        set_combo_mixed(cmb_kerning_mode_, summary.kerning_mixed & RichTextKerningModeMixed);
        set_spin_mixed(spn_kerning_value_, summary.kerning_mixed & RichTextKerningValueMixed);
        set_spin_mixed(spn_char_tracking_, summary.mixed & RichTextCharTracking);
        set_spin_mixed(spn_char_scale_x_, summary.mixed & RichTextCharScaleX);
        set_spin_mixed(spn_char_scale_y_, summary.mixed & RichTextCharScaleY);
        set_spin_mixed(spn_baseline_shift_, summary.mixed & RichTextCharBaselineShift);
        set_spin_mixed(spn_appearance_stroke_width_,
                       summary.mixed & RichTextCharStroke);
        set_spin_mixed(spn_outline_width_,
                       summary.mixed & RichTextCharStroke);
        set_button_mixed(btn_underline_, summary.mixed & RichTextCharUnderline);
        set_button_mixed(btn_strikethrough_, summary.mixed & RichTextCharStrikethrough);
        set_button_mixed(btn_all_caps_, summary.mixed & RichTextCharTextStyle);
        set_button_mixed(btn_small_caps_, summary.mixed & RichTextCharTextStyle);
        set_button_mixed(btn_superscript_, summary.mixed & RichTextCharTextStyle);
        set_button_mixed(btn_subscript_, summary.mixed & RichTextCharTextStyle);
        set_button_mixed(btn_ligatures_, summary.mixed & RichTextCharLigatures);
        set_button_mixed(btn_stylistic_alternates_, summary.mixed & RichTextCharStylisticAlternates);
        set_button_mixed(btn_fractions_, summary.mixed & RichTextCharFractions);
        set_button_mixed(btn_opentype_features_, summary.mixed & RichTextCharOpenTypeFeatures);
        set_combo_mixed(cmb_text_style_, summary.mixed & RichTextCharTextStyle);
        set_combo_mixed(cmb_fill_type_, fill_type_mixed);
        set_combo_mixed(cmb_gradient_type_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientTypeMixed));
        set_combo_mixed(cmb_gradient_spread_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientSpreadMixed));
        set_spin_mixed(spn_gradient_start_pos_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientStartPosMixed));
        set_spin_mixed(spn_gradient_end_pos_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientEndPosMixed));
        set_spin_mixed(spn_gradient_start_opacity_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientStartOpacityMixed));
        set_spin_mixed(spn_gradient_end_opacity_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientEndOpacityMixed));
        set_spin_mixed(spn_gradient_opacity_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientOpacityMixed));
        set_spin_mixed(spn_gradient_angle_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientAngleMixed));
        set_spin_mixed(spn_gradient_center_x_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientCenterXMixed));
        set_spin_mixed(spn_gradient_center_y_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientCenterYMixed));
        set_spin_mixed(spn_gradient_scale_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientScaleMixed));
        set_spin_mixed(spn_gradient_focal_x_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientFocalXMixed));
        set_spin_mixed(spn_gradient_focal_y_, fill_type_mixed ||
            (summary.fill_mixed & RichTextFillGradientFocalYMixed));
    }
    if (!use_rich_char_summary) {
        if (btn_ligatures_) btn_ligatures_->setChecked(layer_->text_ligatures);
        if (btn_stylistic_alternates_) btn_stylistic_alternates_->setChecked(layer_->text_stylistic_alternates);
        if (btn_fractions_) btn_fractions_->setChecked(layer_->text_fractions);
        if (btn_opentype_features_) btn_opentype_features_->setChecked(layer_->text_opentype_features);
        int style_idx = cmb_text_style_->findData(layer_->text_style);
        cmb_text_style_->setCurrentIndex(style_idx >= 0 ? style_idx : 0);
    }
    int overflow_idx = cmb_text_overflow_->findData(layer_->text_overflow_mode);
    cmb_text_overflow_->setCurrentIndex(overflow_idx >= 0 ? overflow_idx : 0);
    spn_text_fit_min_scale_->setValue(layer_->text_fit_min_scale);
    bool is_fit = layer_->text_overflow_mode == 2 && !is_ticker;
    spn_text_fit_min_scale_->setVisible(is_fit);
    lbl_text_fit_scale_->setVisible(is_fit);
    if (auto *form = qobject_cast<QFormLayout *>(dynamic_text_box_ ? dynamic_text_box_->layout() : nullptr)) {
        if (auto *label = form->labelForField(spn_text_fit_min_scale_))
            label->setVisible(is_fit);
    }
    if (lbl_text_fit_scale_) {
        QFont preview_font = font_for_layer(*layer_);
        QRectF preview_rect(0, 0, eval_box_width(*layer_, lt), eval_box_height(*layer_, lt));
        double scale = horizontal_fit_scale(preview_font, preview_rect, display_text_for_style(*layer_), *layer_, lt);
        lbl_text_fit_scale_->setText(bgl_tr("OBSTitles.ScalePercentFormat").arg((int)std::round(scale * 100.0)));
    }
    {
        QSignalBlocker block(chk_expose_text_);
        chk_expose_text_->setChecked(layer_->expose_text);
    }
    if (chk_exposed_hide_if_empty_) {
        QSignalBlocker block(chk_exposed_hide_if_empty_);
        chk_exposed_hide_if_empty_->setEnabled(layer_->expose_text);
        chk_exposed_hide_if_empty_->setChecked(layer_->exposed_hide_if_empty);
    }
    if (chk_exposed_single_value_) {
        QSignalBlocker block(chk_exposed_single_value_);
        chk_exposed_single_value_->setEnabled(layer_->expose_text);
        chk_exposed_single_value_->setChecked(layer_->exposed_single_value);
    }
    if (chk_ignore_persistence_) {
        const bool can_ignore_persistence = !layer_->expose_text;
        QSignalBlocker block(chk_ignore_persistence_);
        chk_ignore_persistence_->setEnabled(can_ignore_persistence);
        chk_ignore_persistence_->setChecked(can_ignore_persistence && layer_->ignore_persistence);
        chk_ignore_persistence_->setToolTip(can_ignore_persistence
            ? bgl_tr("OBSTitles.IgnorePersistenceTooltip")
            : bgl_tr("OBSTitles.IgnorePersistenceDisabledTooltip"));
    }
    if (chk_scene_mask_) chk_scene_mask_->setChecked(layer_->use_as_scene_mask);
    RichTextParagraphFormat displayed_paragraph;
    RichTextParagraphFormatSummary paragraph_summary;
    if (is_text_like) {
        paragraph_summary = summarize_rich_text_paragraph_format(
            *layer_, inline_text_properties, lt);
        displayed_paragraph = paragraph_summary.format;
    }
    if (grp_text_align_) {
        QSignalBlocker block(grp_text_align_);
        const int align_h = is_text_like ? displayed_paragraph.align_h : layer_->align_h;
        const bool mixed = is_text_like &&
            (paragraph_summary.mixed & RichTextParagraphAlignH);
        if (!mixed) {
            if (auto *button = grp_text_align_->button(align_h))
                button->setChecked(true);
            else if (auto *fallback = grp_text_align_->button(1))
                fallback->setChecked(true);
        }
        set_button_group_mixed(grp_text_align_, mixed);
    }
    if (grp_text_valign_) {
        QSignalBlocker block(grp_text_valign_);
        /* Vertical alignment positions the complete textbox and therefore
         * remains an object-level property during inline editing. */
        if (auto *button = grp_text_valign_->button(layer_->align_v))
            button->setChecked(true);
        else if (auto *fallback = grp_text_valign_->button(1))
            fallback->setChecked(true);
    }
    if (spn_text_leading_ && is_text_like)
        spn_text_leading_->setValue(displayed_paragraph.line_spacing);
    if (spn_paragraph_indent_left_)
        spn_paragraph_indent_left_->setValue(is_text_like ? displayed_paragraph.indent_left
                                                          : eval_paragraph_indent_left(*layer_, lt));
    if (spn_paragraph_indent_right_)
        spn_paragraph_indent_right_->setValue(is_text_like ? displayed_paragraph.indent_right
                                                           : eval_paragraph_indent_right(*layer_, lt));
    if (spn_paragraph_indent_first_line_)
        spn_paragraph_indent_first_line_->setValue(is_text_like ? displayed_paragraph.indent_first_line
                                                                : eval_paragraph_indent_first_line(*layer_, lt));
    if (spn_paragraph_space_before_)
        spn_paragraph_space_before_->setValue(is_text_like ? displayed_paragraph.space_before
                                                           : layer_->paragraph_space_before_prop.evaluate(lt));
    if (spn_paragraph_space_after_)
        spn_paragraph_space_after_->setValue(is_text_like ? displayed_paragraph.space_after
                                                          : layer_->paragraph_space_after_prop.evaluate(lt));
    if (chk_paragraph_hyphenate_)
        chk_paragraph_hyphenate_->setChecked(is_text_like
            ? displayed_paragraph.hyphenate : layer_->paragraph_hyphenate);

    if (is_text_like) {
        set_spin_mixed(spn_text_leading_,
                       paragraph_summary.mixed & RichTextParagraphLineSpacing);
        set_spin_mixed(spn_paragraph_indent_left_,
                       paragraph_summary.mixed & RichTextParagraphIndentLeft);
        set_spin_mixed(spn_paragraph_indent_right_,
                       paragraph_summary.mixed & RichTextParagraphIndentRight);
        set_spin_mixed(spn_paragraph_indent_first_line_,
                       paragraph_summary.mixed & RichTextParagraphIndentFirstLine);
        set_spin_mixed(spn_paragraph_space_before_,
                       paragraph_summary.mixed & RichTextParagraphSpaceBefore);
        set_spin_mixed(spn_paragraph_space_after_,
                       paragraph_summary.mixed & RichTextParagraphSpaceAfter);
        set_button_mixed(chk_paragraph_hyphenate_,
                         paragraph_summary.mixed & RichTextParagraphHyphenate);
    }

    QFontDatabase fdb;
    cmb_font_->setToolTip(rich_font_family_mixed
        ? bgl_tr("OBSTitles.MixedValues")
        : (fdb.families().contains(QString::fromStdString(layer_->font_family))
            ? QString()
            : bgl_tr("OBSTitles.FontMissingWarningFormat").arg(
                  QString::fromStdString(layer_->font_family))));

    if (text_animators_box_ && is_text_like)
        refresh_text_animator_controls();
    loading_values_ = false;
}

void PropertiesPanel::remember_next_color_popup_position(const QPoint &global_pos)
{
    pending_color_popup_position_ = global_pos;
    pending_color_popup_position_valid_ = true;
}


bool PropertiesPanel::apply_external_picked_color(const QColor &color, bool commit)
{
    if (!color.isValid() || !layer_ || external_gradient_stop_index_ < 0 ||
        external_gradient_layer_id_ != layer_->id)
        return false;

    const uint32_t argb = argb_from_color(color);
    auto &stops = external_gradient_stroke_ ? layer_->stroke_gradient_stops
                                            : layer_->gradient_stops;
    if (external_gradient_stop_index_ == 0) {
        if (external_gradient_stroke_)
            layer_->stroke_gradient_start_color = argb;
        else
            layer_->gradient_start_color = argb;
    } else if (external_gradient_stop_index_ == 1) {
        if (external_gradient_stroke_)
            layer_->stroke_gradient_end_color = argb;
        else
            layer_->gradient_end_color = argb;
    } else {
        const int extra = external_gradient_stop_index_ - 2;
        if (extra < 0 || extra >= static_cast<int>(stops.size()))
            return false;
        stops[extra].color = argb;
    }

    if (commit)
        emit property_changed(true);
    else
        emit live_visual_changed();
    emit gradient_model_refresh_requested();
    return true;
}

void PropertiesPanel::open_foreground_color_selector()
{
    if (btn_appearance_fill_color_ && btn_appearance_fill_color_->isEnabled())
        btn_appearance_fill_color_->click();
}

void PropertiesPanel::open_background_color_selector()
{
    if (btn_appearance_stroke_color_ && btn_appearance_stroke_color_->isEnabled())
        btn_appearance_stroke_color_->click();
}

void PropertiesPanel::swap_foreground_background_colors()
{
    return;
}
