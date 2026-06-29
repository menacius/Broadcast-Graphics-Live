                const QPoint popup_pos = btn_appearance_stroke_label_->mapToGlobal(
                    QPoint(0, btn_appearance_stroke_label_->height() + 2));
                popup.adjustSize();
                popup.move(clamp_popup_position_to_screen(popup_pos, popup.size(), btn_appearance_stroke_label_));
                popup.exec();
            });
    connect(spn_appearance_stroke_width_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, apply_text_char_format,
                   current_text_char_format, emit_change](double v) {
                if (!can_edit()) return;
                const bool text_like = layer_->type == LayerType::Text ||
                                       layer_->type == LayerType::Clock ||
                                       layer_->type == LayerType::Ticker;
                if (text_like && text_edit_active()) {
                    RichTextCharFormat fmt = current_text_char_format();
                    fmt.stroke.width = (float)v;
                    fmt.stroke.enabled = v > 0.0;
                    if (v > 0.0 && fmt.stroke.fill.color == 0)
                        fmt.stroke.fill.color = layer_->stroke_color;
                    apply_text_char_format(fmt, RichTextCharStroke);
                } else {
                    layer_->stroke_width = (float)v;
                    layer_->outline_enabled = v > 0.0 && layer_->stroke_fill_type != 0;
                }
                emit_change();
            });
    connect(spn_appearance_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v) {
                if (!can_edit()) return;
                const double opacity = v / 100.0;
                set_animated_value(layer_->opacity, local_time(), opacity);
                if (spn_opacity_) {
                    QSignalBlocker block(spn_opacity_);
                    spn_opacity_->setValue(opacity);
                }
                emit_change();
            });
    auto shape_resize_factor = [](double old_w, double old_h, double new_w, double new_h) {
        constexpr double kEpsilon = 1e-6;
        if (old_w <= kEpsilon || old_h <= kEpsilon || new_w <= kEpsilon || new_h <= kEpsilon)
            return 1.0;
        const double sx = new_w / old_w;
        const double sy = new_h / old_h;
        const bool changed_x = std::abs(new_w - old_w) > kEpsilon;
        const bool changed_y = std::abs(new_h - old_h) > kEpsilon;
        if (changed_x && changed_y)
            return std::sqrt(std::max(0.0, sx * sy));
        if (changed_x)
            return sx;
        if (changed_y)
            return sy;
        return 1.0;
    };
    auto apply_shape_size_metric_scaling = [this, shape_resize_factor](double old_w, double old_h, double new_w, double new_h) {
        if (!layer_ || (layer_->type != LayerType::Shape && layer_->type != LayerType::SolidRect))
            return;
        const double factor = shape_resize_factor(old_w, old_h, new_w, new_h);
        if (!std::isfinite(factor) || factor <= 0.0 || std::abs(factor - 1.0) < 1e-6)
            return;

        if (layer_->scale_stroke_with_shape &&
            layer_->outline_enabled &&
            layer_->stroke_fill_type != 0 &&
            layer_->stroke_width > 0.0f) {
            layer_->stroke_width = (float)std::clamp((double)layer_->stroke_width * factor, 0.0, 512.0);
            if (spn_appearance_stroke_width_) {
                QSignalBlocker block(spn_appearance_stroke_width_);
                spn_appearance_stroke_width_->setValue(layer_->stroke_width);
            }
            if (spn_outline_width_) {
                QSignalBlocker block(spn_outline_width_);
                spn_outline_width_->setValue(layer_->stroke_width);
            }
        }

        if (layer_->scale_corners_with_shape) {
            const float tl = (float)std::clamp((double)layer_->corner_radius_tl * factor, 0.0, 9999.0);
            const float tr = (float)std::clamp((double)layer_->corner_radius_tr * factor, 0.0, 9999.0);
            const float br = (float)std::clamp((double)layer_->corner_radius_br * factor, 0.0, 9999.0);
            const float bl = (float)std::clamp((double)layer_->corner_radius_bl * factor, 0.0, 9999.0);
            set_layer_corner_radii(*layer_, tl, tr, br, bl);
            layer_->shape_roundness = (float)std::clamp((double)layer_->shape_roundness * factor, 0.0, 9999.0);
            layer_->shape_inner_roundness = (float)std::clamp((double)layer_->shape_inner_roundness * factor, 0.0, 9999.0);
            for (auto &point : layer_->path_points)
                point.corner_radius = std::clamp(point.corner_radius * factor, 0.0, 9999.0);
            if (spn_rect_corner_tl_) { QSignalBlocker block(spn_rect_corner_tl_); spn_rect_corner_tl_->setValue(layer_->corner_radius_tl); }
            if (spn_rect_corner_tr_) { QSignalBlocker block(spn_rect_corner_tr_); spn_rect_corner_tr_->setValue(layer_->corner_radius_tr); }
            if (spn_rect_corner_br_) { QSignalBlocker block(spn_rect_corner_br_); spn_rect_corner_br_->setValue(layer_->corner_radius_br); }
            if (spn_rect_corner_bl_) { QSignalBlocker block(spn_rect_corner_bl_); spn_rect_corner_bl_->setValue(layer_->corner_radius_bl); }
            if (spn_shape_roundness_) {
                QSignalBlocker block(spn_shape_roundness_);
                spn_shape_roundness_->setValue(layer_->shape_roundness);
            }
        }
    };
    connect(spn_layer_w_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_shape_size_metric_scaling](double v){
                if (!can_edit()) return;
                double t = local_time();
                if (layer_->type == LayerType::Image) {
                    double raw_w = eval_image_width(*layer_, t);
                    double raw_h = eval_image_height(*layer_, t);
                    const bgs::ImageDisplaySize current = bgs::calculate_image_display_size(
                        layer_->image_box_mode, layer_->image_size_auto_fit,
                        eval_box_width(*layer_, t), eval_box_height(*layer_, t), raw_w, raw_h);
                    const double old_w = current.width;
                    const double old_h = current.height;
                    layer_->image_size_auto_fit = false;
                    layer_->image_width = (float)v;
                    set_animated_x(layer_->image_size, t, v);
                    if (layer_->lock_aspect_ratio && old_w > 0.0) {
                        layer_->image_height = (float)(v * old_h / old_w);
                        set_animated_y(layer_->image_size, t, layer_->image_height);
                        QSignalBlocker block(spn_layer_h_);
                        spn_layer_h_->setValue(layer_->image_height);
                    } else {
                        layer_->image_height = (float)old_h;
                        set_animated_y(layer_->image_size, t, old_h);
                    }
                    emit_change();
                    return;
                }
                double old_w = eval_box_width(*layer_, t);
                double old_h = eval_box_height(*layer_, t);
                layer_->rect_width = (float)v;
                set_animated_x(layer_->size, t, v);
                if (spn_transform_size_w_) {
                    QSignalBlocker block(spn_transform_size_w_);
                    spn_transform_size_w_->setValue(v);
                }
                const bool lock_size = layer_->lock_aspect_ratio &&
                                       (layer_->type == LayerType::Image ||
                                        layer_->type == LayerType::Shape ||
                                        layer_->type == LayerType::SolidRect);
                if (lock_size && old_w > 0.0) {
                    layer_->rect_height = (float)(v * old_h / old_w);
                    set_animated_y(layer_->size, t, layer_->rect_height);
                    QSignalBlocker block(spn_layer_h_);
                    spn_layer_h_->setValue(layer_->rect_height);
                    if (spn_transform_size_h_) {
                        QSignalBlocker transform_block(spn_transform_size_h_);
                        spn_transform_size_h_->setValue(layer_->rect_height);
                    }
                }
                apply_shape_size_metric_scaling(old_w, old_h, eval_box_width(*layer_, t), eval_box_height(*layer_, t));
                emit_change();
            });
    connect(spn_layer_h_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_shape_size_metric_scaling](double v){
                if (!can_edit()) return;
                double t = local_time();
                if (layer_->type == LayerType::Image) {
                    double raw_w = eval_image_width(*layer_, t);
                    double raw_h = eval_image_height(*layer_, t);
                    const bgs::ImageDisplaySize current = bgs::calculate_image_display_size(
                        layer_->image_box_mode, layer_->image_size_auto_fit,
                        eval_box_width(*layer_, t), eval_box_height(*layer_, t), raw_w, raw_h);
                    const double old_w = current.width;
                    const double old_h = current.height;
                    layer_->image_size_auto_fit = false;
                    layer_->image_height = (float)v;
                    set_animated_y(layer_->image_size, t, v);
                    if (layer_->lock_aspect_ratio && old_h > 0.0) {
                        layer_->image_width = (float)(v * old_w / old_h);
                        set_animated_x(layer_->image_size, t, layer_->image_width);
                        QSignalBlocker block(spn_layer_w_);
                        spn_layer_w_->setValue(layer_->image_width);
                    } else {
                        layer_->image_width = (float)old_w;
                        set_animated_x(layer_->image_size, t, old_w);
                    }
                    emit_change();
                    return;
                }
                double old_w = eval_box_width(*layer_, t);
                double old_h = eval_box_height(*layer_, t);
                layer_->rect_height = (float)v;
                set_animated_y(layer_->size, t, v);
                if (spn_transform_size_h_) {
                    QSignalBlocker block(spn_transform_size_h_);
                    spn_transform_size_h_->setValue(v);
                }
                const bool lock_size = layer_->lock_aspect_ratio &&
                                       (layer_->type == LayerType::Image ||
                                        layer_->type == LayerType::Shape ||
                                        layer_->type == LayerType::SolidRect);
                if (lock_size && old_h > 0.0) {
                    layer_->rect_width = (float)(v * old_w / old_h);
                    set_animated_x(layer_->size, t, layer_->rect_width);
                    QSignalBlocker block(spn_layer_w_);
                    spn_layer_w_->setValue(layer_->rect_width);
                    if (spn_transform_size_w_) {
                        QSignalBlocker transform_block(spn_transform_size_w_);
                        spn_transform_size_w_->setValue(layer_->rect_width);
                    }
                }
                apply_shape_size_metric_scaling(old_w, old_h, eval_box_width(*layer_, t), eval_box_height(*layer_, t));
                emit_change();
            });
    connect(spn_transform_size_w_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_shape_size_metric_scaling](double v){
                if (!can_edit()) return;
                double t = local_time();
                double old_w = eval_box_width(*layer_, t);
                double old_h = eval_box_height(*layer_, t);
                layer_->rect_width = (float)v;
                set_animated_x(layer_->size, t, v);
                if (spn_layer_w_) {
                    QSignalBlocker block(spn_layer_w_);
                    spn_layer_w_->setValue(v);
                }
                const bool lock_size = layer_->lock_aspect_ratio &&
                                       (layer_->type == LayerType::Shape ||
                                        layer_->type == LayerType::SolidRect);
                if (lock_size && old_w > 0.0) {
                    layer_->rect_height = (float)(v * old_h / old_w);
                    set_animated_y(layer_->size, t, layer_->rect_height);
                    if (spn_transform_size_h_) {
                        QSignalBlocker block(spn_transform_size_h_);
                        spn_transform_size_h_->setValue(layer_->rect_height);
                    }
                    if (spn_layer_h_) {
                        QSignalBlocker block(spn_layer_h_);
                        spn_layer_h_->setValue(layer_->rect_height);
                    }
                }
                apply_shape_size_metric_scaling(old_w, old_h, eval_box_width(*layer_, t), eval_box_height(*layer_, t));
                emit_change();
            });
    connect(spn_transform_size_h_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_shape_size_metric_scaling](double v){
                if (!can_edit()) return;
                double t = local_time();
                double old_w = eval_box_width(*layer_, t);
                double old_h = eval_box_height(*layer_, t);
                layer_->rect_height = (float)v;
                set_animated_y(layer_->size, t, v);
                if (spn_layer_h_) {
                    QSignalBlocker block(spn_layer_h_);
                    spn_layer_h_->setValue(v);
                }
                const bool lock_size = layer_->lock_aspect_ratio &&
                                       (layer_->type == LayerType::Shape ||
                                        layer_->type == LayerType::SolidRect);
                if (lock_size && old_h > 0.0) {
                    layer_->rect_width = (float)(v * old_w / old_h);
                    set_animated_x(layer_->size, t, layer_->rect_width);
                    if (spn_transform_size_w_) {
                        QSignalBlocker block(spn_transform_size_w_);
                        spn_transform_size_w_->setValue(layer_->rect_width);
                    }
                    if (spn_layer_w_) {
                        QSignalBlocker block(spn_layer_w_);
                        spn_layer_w_->setValue(layer_->rect_width);
                    }
                }
                apply_shape_size_metric_scaling(old_w, old_h, eval_box_width(*layer_, t), eval_box_height(*layer_, t));
                emit_change();
            });
    connect(spn_image_box_w_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, refresh_image_size_controls](double v) {
                if (!can_edit() || layer_->type != LayerType::Image) return;
                const double t = local_time();
                const double old_w = eval_box_width(*layer_, t);
                const double old_h = eval_box_height(*layer_, t);
                layer_->rect_width = (float)v;
                set_animated_x(layer_->size, t, v);
                if (layer_->image_box_lock_aspect_ratio && old_w > 0.0) {
                    layer_->rect_height = (float)(v * old_h / old_w);
                    set_animated_y(layer_->size, t, layer_->rect_height);
                    QSignalBlocker block(spn_image_box_h_);
                    spn_image_box_h_->setValue(layer_->rect_height);
                }
                refresh_image_size_controls();
                emit_change();
            });
    connect(spn_image_box_h_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, refresh_image_size_controls](double v) {
                if (!can_edit() || layer_->type != LayerType::Image) return;
                const double t = local_time();
                const double old_w = eval_box_width(*layer_, t);
                const double old_h = eval_box_height(*layer_, t);
                layer_->rect_height = (float)v;
                set_animated_y(layer_->size, t, v);
                if (layer_->image_box_lock_aspect_ratio && old_h > 0.0) {
                    layer_->rect_width = (float)(v * old_w / old_h);
                    set_animated_x(layer_->size, t, layer_->rect_width);
                    QSignalBlocker block(spn_image_box_w_);
                    spn_image_box_w_->setValue(layer_->rect_width);
                }
                refresh_image_size_controls();
                emit_change();
            });
    auto set_corner_spin_values = [this](double tl, double tr, double br, double bl, QDoubleSpinBox *except = nullptr) {
        auto set_spin = [except](QDoubleSpinBox *spin, double value) {
            if (!spin || spin == except) return;
            QSignalBlocker blocker(spin);
            spin->setValue(value);
        };
        set_spin(spn_rect_corner_tl_, tl);
        set_spin(spn_rect_corner_tr_, tr);
        set_spin(spn_rect_corner_br_, br);
        set_spin(spn_rect_corner_bl_, bl);
    };
    auto connect_corner_spin = [this, can_edit, emit_change, set_corner_spin_values](QDoubleSpinBox *spin, int corner_index) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, can_edit, emit_change, set_corner_spin_values, spin, corner_index](double v) {
                    if (!can_edit()) return;
                    if (chk_corner_lock_ && chk_corner_lock_->isChecked()) {
                        set_layer_all_corner_radii(*layer_, (float)v);
                        set_corner_spin_values(v, v, v, v, spin);
                    } else {
                        switch (corner_index) {
                        case 0: layer_->corner_radius_tl = (float)v; break;
                        case 1: layer_->corner_radius_tr = (float)v; break;
                        case 2: layer_->corner_radius_br = (float)v; break;
                        case 3: layer_->corner_radius_bl = (float)v; break;
                        default: break;
                        }
                        set_layer_corner_radii(*layer_, layer_->corner_radius_tl, layer_->corner_radius_tr,
                                               layer_->corner_radius_br, layer_->corner_radius_bl);
                    }
                    emit_change();
                });
    };
    connect_corner_spin(spn_rect_corner_tl_, 0);
    connect_corner_spin(spn_rect_corner_tr_, 1);
    connect_corner_spin(spn_rect_corner_br_, 2);
    connect_corner_spin(spn_rect_corner_bl_, 3);
    connect(chk_corner_lock_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change, set_corner_spin_values](bool locked) {
                if (!can_edit()) return;
                layer_->corner_radius_locked = locked;
                if (locked) {
                    const double radius = spn_rect_corner_tl_ ? spn_rect_corner_tl_->value() : layer_->corner_radius_tl;
                    set_layer_all_corner_radii(*layer_, (float)radius);
                    set_corner_spin_values(radius, radius, radius, radius);
                }
                emit_change();
            });
    connect(spn_corner_bevel_roundness_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double value) {
                if (!can_edit()) return;
                layer_->corner_bevel_roundness = (float)std::clamp(value, -100.0, 100.0);
                emit_change();
            });
    connect(cmb_shape_type_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx){
                if (!can_edit()) return;
                const ShapeType previous_shape = layer_->shape_type;
                const ShapeType next_shape = (ShapeType)cmb_shape_type_->itemData(idx).toInt();
                if (next_shape == ShapeType::Path && previous_shape != ShapeType::Path) {
                    if (!bgs::ensure_editable_path(*layer_)) {
                        load_values();
                        return;
                    }
                } else {
                    layer_->shape_type = next_shape;
                }
                if (previous_shape == ShapeType::Path && next_shape != ShapeType::Path) {
                    layer_->path_points.clear();
                    layer_->path_closed = true;
                }
                if (next_shape == ShapeType::Rectangle) {
                    set_layer_all_corner_radii(*layer_, 0.0f);
                    layer_->corner_radius_locked = true;
                    layer_->shape_roundness = 0.0f;
                    layer_->shape_inner_roundness = 0.0f;
                } else if (next_shape == ShapeType::RoundedRectangle &&
                           previous_shape != ShapeType::Rectangle &&
                           previous_shape != ShapeType::RoundedRectangle &&
                           layer_->corner_radius <= 0.0f) {
                    set_layer_all_corner_radii(*layer_, 18.0f);
                    layer_->corner_radius_locked = true;
                    layer_->shape_roundness = layer_->corner_radius;
                    layer_->shape_inner_roundness = layer_->shape_roundness;
                } else if (next_shape == ShapeType::Star && previous_shape != ShapeType::Star) {
                    layer_->shape_inner_roundness = layer_->shape_roundness;
                }
                load_values();
                emit_change();
            });
    connect(grp_shape_type_, &QButtonGroup::idClicked,
            this, [this](int shape_id) {
                if (!cmb_shape_type_ || loading_values_) return;
                const int idx = cmb_shape_type_->findData(shape_id);
                if (idx >= 0 && cmb_shape_type_->currentIndex() != idx)
                    cmb_shape_type_->setCurrentIndex(idx);
            });
    connect(chk_size_lock_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool locked) {
                if (can_edit()) {
                    layer_->lock_aspect_ratio = locked;
                    if (chk_transform_size_lock_) {
                        QSignalBlocker block(chk_transform_size_lock_);
                        chk_transform_size_lock_->setChecked(locked);
                    }
                    emit_change();
                }
            });
    connect(chk_transform_size_lock_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool locked) {
                if (can_edit()) {
                    layer_->lock_aspect_ratio = locked;
                    if (chk_size_lock_) {
                        QSignalBlocker block(chk_size_lock_);
                        chk_size_lock_->setChecked(locked);
                    }
                    emit_change();
                }
            });
    connect(chk_image_box_size_lock_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool locked) {
                if (can_edit() && layer_->type == LayerType::Image) {
                    layer_->image_box_lock_aspect_ratio = locked;
                    emit_change();
                }
            });
    connect(chk_shape_scale_stroke_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool enabled) {
                if (can_edit()) {
                    layer_->scale_stroke_with_shape = enabled;
                    emit_change();
                }
            });
    connect(chk_shape_scale_corners_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool enabled) {
                if (can_edit()) {
                    layer_->scale_corners_with_shape = enabled;
                    emit_change();
                }
            });
    connect(btn_shape_defaults_, &QPushButton::clicked,
            this, [this, can_edit, local_time, emit_change]() {
                if (!can_edit()) return;
                const double t = local_time();
                if (layer_->type == LayerType::Image) {
                    QSize intrinsic = editor_image_intrinsic_size(QString::fromStdString(layer_->image_path));
                    if (!intrinsic.isValid() || intrinsic.isEmpty())
                        intrinsic = QSize(std::max(1, (int)std::lround(layer_->image_width)),
                                          std::max(1, (int)std::lround(layer_->image_height)));
                    layer_->image_width = (float)intrinsic.width();
                    layer_->image_height = (float)intrinsic.height();
                    set_animated_x(layer_->image_size, t, layer_->image_width);
                    set_animated_y(layer_->image_size, t, layer_->image_height);
                    layer_->image_size_auto_fit = true;
                    layer_->lock_aspect_ratio = layer_->image_box_mode != ImageBoxMode::StretchToFill;
                    load_values();
                    emit_change();
                    return;
                }
                const double default_w = title_ ? std::max(1.0, title_->width * 0.5) : 960.0;
                const double default_h = 160.0;
                layer_->shape_type = ShapeType::Rectangle;
                layer_->rect_width = (float)default_w;
                layer_->rect_height = (float)default_h;
                set_animated_x(layer_->size, t, default_w);
                set_animated_y(layer_->size, t, default_h);
                set_layer_all_corner_radii(*layer_, 0.0f);
                layer_->corner_radius_locked = true;
                layer_->corner_bevel_roundness = 100.0f;
                layer_->shape_points = 5;
                layer_->shape_sides = 6;
                layer_->shape_inner_radius = 0.20f;
                layer_->shape_outer_radius = 0.5f;
                layer_->shape_roundness = 0.0f;
                layer_->shape_inner_roundness = 0.0f;
                layer_->lock_aspect_ratio = false;
                layer_->scale_stroke_with_shape = false;
                layer_->scale_corners_with_shape = false;
                load_values();
                emit_change();
            });
    connect(spn_shape_points_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, can_edit, emit_change](int v){ if (can_edit()) { layer_->shape_points = v; emit_change(); } });
    connect(spn_shape_sides_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, can_edit, emit_change](int v){ if (can_edit()) { layer_->shape_sides = v; emit_change(); } });
    connect(spn_shape_inner_radius_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){ if (can_edit()) { layer_->shape_inner_radius = (float)v; emit_change(); } });
    connect(spn_shape_outer_radius_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){ if (can_edit()) { layer_->shape_outer_radius = (float)v; emit_change(); } });
    connect(spn_shape_roundness_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){
                if (!can_edit()) return;
                const bool rectangle_shape = layer_->type == LayerType::SolidRect ||
                                             (layer_->type == LayerType::Shape &&
                                              (layer_->shape_type == ShapeType::Rectangle ||
                                               layer_->shape_type == ShapeType::RoundedRectangle));
                layer_->shape_roundness = (float)v;
                if (layer_->type == LayerType::Shape && layer_->shape_type == ShapeType::Star)
                    layer_->shape_inner_roundness = (float)v;
                if (rectangle_shape) {
                    set_layer_all_corner_radii(*layer_, (float)v);
                } else if (layer_->type == LayerType::Shape && layer_->shape_type == ShapeType::Path) {
                    for (auto &point : layer_->path_points)
                        point.corner_radius = std::max(0.0, v);
                }
                emit_change();
            });
    connect(btn_fill_color_, &QPushButton::clicked,
            this, [this, can_edit, local_time, emit_change]() {
                if (!can_edit()) return;
                QColor initial = color_from_argb(eval_fill_color(*layer_, local_time()));
                QColor picked = bgl_pick_color(initial, this, bgl_tr("OBSTitles.FillColor"));
                if (!picked.isValid()) return;
                layer_->fill_color = argb_from_color(picked);
                set_color_channels_at(*layer_, false, local_time(), layer_->fill_color);
                style_color_button(btn_fill_color_, layer_->fill_color);
                emit_change();
            });
    connect(cmb_fill_type_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](int idx) {
                if (!can_edit()) return;
                if (!text_edit_active())
                    layer_->fill_type = cmb_fill_type_->itemData(idx).toInt();
                apply_text_fill_format();
                load_values();
                emit_change();
            });
    connect(cmb_gradient_type_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](int idx) {
                if (can_edit()) { if (!text_edit_active()) layer_->gradient_type = cmb_gradient_type_->itemData(idx).toInt(); apply_text_fill_format(); emit_change(); }
            });
    connect(cmb_gradient_spread_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](int idx) {
                if (can_edit()) { if (!text_edit_active()) layer_->gradient_spread = cmb_gradient_spread_->itemData(idx).toInt(); apply_text_fill_format(); emit_change(); }
            });
    auto connect_gradient_color = [this, can_edit, text_edit_active, emit_change, apply_text_fill_format,
                                   apply_text_char_format, current_text_char_format](QPushButton *button, uint32_t Layer::*member,
                                                                 const char *title_key, bool text_fill) {
        connect(button, &QPushButton::clicked, this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format,
                                                     apply_text_char_format, current_text_char_format,
                                                     button, member, title_key, text_fill]() {
            if (!can_edit()) return;
            uint32_t initial_argb = (*layer_).*member;
            if (text_fill && text_edit_active()) {
                const RichTextCharFormat fmt = current_text_char_format();
                initial_argb = member == &Layer::gradient_start_color
                    ? fmt.fill.gradient_start_color
                    : fmt.fill.gradient_end_color;
            }
            QColor picked = bgl_pick_color(color_from_argb(initial_argb), this, bgl_tr(title_key));
            if (!picked.isValid()) return;
            const uint32_t argb = argb_from_color(picked);
            if (text_fill && text_edit_active()) {
                RichTextCharFormat fmt = current_text_char_format();
                if (member == &Layer::gradient_start_color)
                    fmt.fill.gradient_start_color = argb;
                else
                    fmt.fill.gradient_end_color = argb;
                apply_text_char_format(fmt, RichTextCharFillColor);
            } else {
                (*layer_).*member = argb;
                if (text_fill) apply_text_fill_format();
            }
            style_color_button(button, argb);
            emit_change();
        });
    };
    connect_gradient_color(btn_gradient_start_color_, &Layer::gradient_start_color, "OBSTitles.StartColorLabel", true);
    connect_gradient_color(btn_gradient_end_color_, &Layer::gradient_end_color, "OBSTitles.EndColorLabel", true);
    connect(spn_gradient_start_pos_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](double v) { if (can_edit()) { if (!text_edit_active()) layer_->gradient_start_pos = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_end_pos_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](double v) { if (can_edit()) { if (!text_edit_active()) layer_->gradient_end_pos = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_start_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](double v) { if (can_edit()) { if (!text_edit_active()) layer_->gradient_start_opacity = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_end_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](double v) { if (can_edit()) { if (!text_edit_active()) layer_->gradient_end_opacity = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](double v) { if (can_edit()) { if (!text_edit_active()) layer_->gradient_opacity = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_angle_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](double v) { if (can_edit()) { if (!text_edit_active()) layer_->gradient_angle = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_center_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](double v) { if (can_edit()) { if (!text_edit_active()) layer_->gradient_center_x = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_center_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](double v) { if (can_edit()) { if (!text_edit_active()) layer_->gradient_center_y = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_scale_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](double v) { if (can_edit()) { if (!text_edit_active()) layer_->gradient_scale = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_focal_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](double v) { if (can_edit()) { if (!text_edit_active()) layer_->gradient_focal_x = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_focal_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, text_edit_active, emit_change, apply_text_fill_format](double v) { if (can_edit()) { if (!text_edit_active()) layer_->gradient_focal_y = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(edit_image_path_, &QLineEdit::textChanged,
            this, [this, can_edit, emit_change, fit_image_size_to_current_image](const QString &path){
                set_image_preview_label(lbl_image_preview_, path);
                if (can_edit()) {
                    layer_->image_path = path.toStdString();
                    fit_image_size_to_current_image();
                    emit_change();
                }
            });
    connect(cmb_image_scale_filter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx){
                if (!can_edit() || idx < 0) return;
                layer_->scale_filter = (ImageScaleFilter)cmb_image_scale_filter_->itemData(idx).toInt();
                emit_change();
            });
    connect(cmb_image_box_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change, refresh_image_size_controls](int idx){
                if (!can_edit() || idx < 0) return;
                layer_->image_box_mode = (ImageBoxMode)cmb_image_box_mode_->itemData(idx).toInt();
                layer_->image_size_auto_fit = true;
                layer_->lock_aspect_ratio = layer_->image_box_mode != ImageBoxMode::StretchToFill;
                if (chk_size_lock_) {
                    QSignalBlocker block(chk_size_lock_);
                    chk_size_lock_->setChecked(layer_->lock_aspect_ratio);
                }
                refresh_image_size_controls();
                emit_change();
            });
    connect(chk_image_crop_when_outside_box_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool enabled) {
                if (!can_edit() || layer_->type != LayerType::Image) return;
                layer_->image_crop_when_outside_box = enabled;
                emit_change();
            });
    if (auto *image_anchor_button = static_cast<AnchorGridButton *>(btn_image_anchor_grid_)) {
        image_anchor_button->anchor_selected = [this, can_edit, emit_change](int i) {
            if (!can_edit()) return;
            QPointF next = anchor_point_from_index(i);
            layer_->image_anchor_x = (float)next.x();
            layer_->image_anchor_y = (float)next.y();
            emit_change();
        };
    }
    connect(btn_pick_image_, &QPushButton::clicked,
            this, [this, can_edit, fit_image_size_to_current_image, emit_change]() {
                if (!can_edit()) return;
                QString path = QFileDialog::getOpenFileName(
                    this, bgl_tr("OBSTitles.ChooseImage"),
                    QString::fromStdString(layer_->image_path),
                    bgl_tr("OBSTitles.ImageFileFilter"));
                if (path.isEmpty()) return;
                layer_->image_path = path.toStdString();
                fit_image_size_to_current_image();
                load_values();
                emit_change();
            });

    auto toggle_vec2_keyframe = [](AnimatedVec2Property &prop, double time, double x_value, double y_value) {
        toggle_keyframe(prop, time, {x_value, y_value});
    };

    connect(btn_kf_pos_x_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->position, local_time(), spn_px_->value(), spn_py_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_pos_y_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->position, local_time(), spn_px_->value(), spn_py_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_scale_x_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->scale, local_time(),
                             spn_scale_x_->value() / 100.0, spn_scale_y_->value() / 100.0);
        load_values();
        emit_change();
    });
    connect(btn_kf_scale_y_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->scale, local_time(),
                             spn_scale_x_->value() / 100.0, spn_scale_y_->value() / 100.0);
        load_values();
        emit_change();
    });
    connect(btn_kf_transform_size_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->size, local_time(),
                             spn_transform_size_w_->value(), spn_transform_size_h_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_rotation_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->rotation, local_time(), spn_rot_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_opacity_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->opacity, local_time(), spn_opacity_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_origin_x_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->origin_prop, local_time(),
                             spn_origin_x_->value(), spn_origin_y_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_origin_y_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->origin_prop, local_time(),
                             spn_origin_x_->value(), spn_origin_y_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_paragraph_indent_left_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit() || active_text_edit_layer_id_ == layer_->id) return;
        toggle_keyframe(layer_->paragraph_indent_left_prop, local_time(), spn_paragraph_indent_left_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_paragraph_indent_right_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit() || active_text_edit_layer_id_ == layer_->id) return;
        toggle_keyframe(layer_->paragraph_indent_right_prop, local_time(), spn_paragraph_indent_right_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_paragraph_indent_first_line_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit() || active_text_edit_layer_id_ == layer_->id) return;
        toggle_keyframe(layer_->paragraph_indent_first_line_prop, local_time(), spn_paragraph_indent_first_line_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_font_size_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit() || active_text_edit_layer_id_ == layer_->id) return;
        toggle_keyframe(layer_->font_size_prop, local_time(), spn_size_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_char_scale_x_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit() || active_text_edit_layer_id_ == layer_->id) return;
        toggle_keyframe(layer_->char_scale_x_prop, local_time(), spn_char_scale_x_->value() / 100.0);
        load_values();
        emit_change();
    });
    connect(btn_kf_char_scale_y_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit() || active_text_edit_layer_id_ == layer_->id) return;
        toggle_keyframe(layer_->char_scale_y_prop, local_time(), spn_char_scale_y_->value() / 100.0);
        load_values();
        emit_change();
    });
    connect(btn_kf_char_tracking_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit() || active_text_edit_layer_id_ == layer_->id) return;
        toggle_keyframe(layer_->char_tracking_prop, local_time(), spn_char_tracking_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_baseline_shift_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit() || active_text_edit_layer_id_ == layer_->id) return;
        toggle_keyframe(layer_->baseline_shift_prop, local_time(), spn_baseline_shift_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_paragraph_space_before_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit() || active_text_edit_layer_id_ == layer_->id) return;
        toggle_keyframe(layer_->paragraph_space_before_prop, local_time(), spn_paragraph_space_before_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_paragraph_space_after_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit() || active_text_edit_layer_id_ == layer_->id) return;
        toggle_keyframe(layer_->paragraph_space_after_prop, local_time(), spn_paragraph_space_after_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_width_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        if (layer_->type == LayerType::Image) {
            layer_->image_size_auto_fit = false;
            toggle_vec2_keyframe(layer_->image_size, local_time(),
                                 spn_layer_w_->value(), spn_layer_h_->value());
        } else {
            toggle_vec2_keyframe(layer_->size, local_time(),
                                 spn_layer_w_->value(), spn_layer_h_->value());
        }
        load_values();
        emit_change();
    });
    connect(btn_kf_image_box_size_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit() || layer_->type != LayerType::Image) return;
        toggle_vec2_keyframe(layer_->size, local_time(),
                             spn_image_box_w_->value(), spn_image_box_h_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_text_color_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit() || active_text_edit_layer_id_ == layer_->id) return;
        double t = local_time();
        uint32_t color = eval_text_color(*layer_, t);
        if (any_keyframe_at_time({&layer_->text_color_a, &layer_->text_color_r,
                                  &layer_->text_color_g, &layer_->text_color_b}, t)) {
            remove_keyframe_at(layer_->text_color_a, t);
            remove_keyframe_at(layer_->text_color_r, t);
            remove_keyframe_at(layer_->text_color_g, t);
            remove_keyframe_at(layer_->text_color_b, t);
        } else {
            add_or_replace_keyframe(layer_->text_color_a, t, (color >> 24) & 0xFF);
            add_or_replace_keyframe(layer_->text_color_r, t, (color >> 16) & 0xFF);
            add_or_replace_keyframe(layer_->text_color_g, t, (color >> 8) & 0xFF);
            add_or_replace_keyframe(layer_->text_color_b, t, color & 0xFF);
        }
        load_values();
        emit_change();
    });

