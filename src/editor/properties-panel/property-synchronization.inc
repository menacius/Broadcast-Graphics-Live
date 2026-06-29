    connect(btn_kf_appearance_fill_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        if (active_text_edit_layer_id_ == layer_->id &&
            (layer_->type == LayerType::Text || layer_->type == LayerType::Clock ||
             layer_->type == LayerType::Ticker))
            return;
        const bool text_fill = layer_->type == LayerType::Text || layer_->type == LayerType::Clock ||
                               layer_->type == LayerType::Ticker;
        double t = local_time();
        uint32_t color = text_fill ? eval_text_color(*layer_, t) : eval_fill_color(*layer_, t);
        auto &a = text_fill ? layer_->text_color_a : layer_->fill_color_a;
        auto &r = text_fill ? layer_->text_color_r : layer_->fill_color_r;
        auto &g = text_fill ? layer_->text_color_g : layer_->fill_color_g;
        auto &b = text_fill ? layer_->text_color_b : layer_->fill_color_b;
        if (any_keyframe_at_time({&a, &r, &g, &b}, t)) {
            remove_keyframe_at(a, t);
            remove_keyframe_at(r, t);
            remove_keyframe_at(g, t);
            remove_keyframe_at(b, t);
        } else {
            add_or_replace_keyframe(a, t, (color >> 24) & 0xFF);
            add_or_replace_keyframe(r, t, (color >> 16) & 0xFF);
            add_or_replace_keyframe(g, t, (color >> 8) & 0xFF);
            add_or_replace_keyframe(b, t, color & 0xFF);
        }
        load_values();
        emit_change();
    });
    connect(btn_kf_fill_color_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        double t = local_time();
        uint32_t color = eval_fill_color(*layer_, t);
        if (any_keyframe_at_time({&layer_->fill_color_a, &layer_->fill_color_r,
                                  &layer_->fill_color_g, &layer_->fill_color_b}, t)) {
            remove_keyframe_at(layer_->fill_color_a, t);
            remove_keyframe_at(layer_->fill_color_r, t);
            remove_keyframe_at(layer_->fill_color_g, t);
            remove_keyframe_at(layer_->fill_color_b, t);
        } else {
            add_or_replace_keyframe(layer_->fill_color_a, t, (color >> 24) & 0xFF);
            add_or_replace_keyframe(layer_->fill_color_r, t, (color >> 16) & 0xFF);
            add_or_replace_keyframe(layer_->fill_color_g, t, (color >> 8) & 0xFF);
            add_or_replace_keyframe(layer_->fill_color_b, t, color & 0xFF);
        }
        load_values();
        emit_change();
    });

    auto apply_rule_editor_to_rule = [this](RichTextAutoStyleRule &rule) {
        rule.enabled = chk_auto_rule_enabled_ ? chk_auto_rule_enabled_->isChecked() : true;
        rule.display_name = edt_auto_rule_name_ ? edt_auto_rule_name_->text().trimmed().toStdString() : std::string();
        rule.style_preset_id = cmb_auto_rule_style_->currentData().toString().toStdString();
        rule.conflict_mode = cmb_auto_rule_conflict_mode_ ? cmb_auto_rule_conflict_mode_->currentData().toString().toStdString() : std::string("override_previous");
        rule.match_mode = cmb_auto_rule_match_mode_ ? cmb_auto_rule_match_mode_->currentData().toString().toStdString() : std::string("all_matches");
        rule.stop_processing = chk_auto_rule_stop_processing_ ? chk_auto_rule_stop_processing_->isChecked() : false;
        rule.require_stop_match = chk_auto_rule_require_stop_match_ ? chk_auto_rule_require_stop_match_->isChecked() : true;
        rule.include_start_marker = chk_auto_rule_include_start_marker_ ? chk_auto_rule_include_start_marker_->isChecked() : true;
        rule.include_end_marker = chk_auto_rule_include_end_marker_ ? chk_auto_rule_include_end_marker_->isChecked() : false;
        rule.excludes_rule_ids = edt_auto_rule_excludes_ ? auto_style_split_ids(edt_auto_rule_excludes_->text()) : std::vector<std::string>();
        rule.condition_type = "range_markers";
        rule.start_condition = cmb_auto_rule_start_condition_->currentData().toString().toStdString();
        rule.end_condition = cmb_auto_rule_end_condition_->currentData().toString().toStdString();
        rule.start_offset = (size_t)spn_auto_rule_start_offset_->value();
        rule.end_offset = (size_t)spn_auto_rule_end_offset_->value();
        rule.start_custom_chars = edt_auto_rule_start_chars_->text().toStdString();
        rule.end_custom_chars = edt_auto_rule_end_chars_->text().toStdString();
        rule.start = rule.start_offset;
        rule.length = rule.end_offset > rule.start_offset ? rule.end_offset - rule.start_offset : 0;
    };
    auto load_rule_to_editor = [this](const RichTextAutoStyleRule &rule) {
        if (chk_auto_rule_enabled_) chk_auto_rule_enabled_->setChecked(rule.enabled);
        if (edt_auto_rule_name_) edt_auto_rule_name_->setText(QString::fromStdString(rule.display_name));
        int idx = cmb_auto_rule_style_->findData(QString::fromStdString(rule.style_preset_id));
        if (idx >= 0) cmb_auto_rule_style_->setCurrentIndex(idx);
        int sidx = cmb_auto_rule_start_condition_->findData(QString::fromStdString(rule.start_condition.empty() ? std::string("text_start") : rule.start_condition));
        int eidx = cmb_auto_rule_end_condition_->findData(QString::fromStdString(rule.end_condition.empty() ? std::string("character_index") : rule.end_condition));
        if (sidx >= 0) cmb_auto_rule_start_condition_->setCurrentIndex(sidx);
        if (eidx >= 0) cmb_auto_rule_end_condition_->setCurrentIndex(eidx);
        spn_auto_rule_start_offset_->setValue((int)rule.start_offset);
        spn_auto_rule_end_offset_->setValue((int)(rule.end_offset ? rule.end_offset : rule.length));
        edt_auto_rule_start_chars_->setText(QString::fromStdString(rule.start_custom_chars));
        edt_auto_rule_end_chars_->setText(QString::fromStdString(rule.end_custom_chars));
        if (cmb_auto_rule_conflict_mode_) { int cidx = cmb_auto_rule_conflict_mode_->findData(QString::fromStdString(rule.conflict_mode.empty() ? std::string("override_previous") : rule.conflict_mode)); if (cidx >= 0) cmb_auto_rule_conflict_mode_->setCurrentIndex(cidx); }
        if (cmb_auto_rule_match_mode_) { int midx = cmb_auto_rule_match_mode_->findData(QString::fromStdString(rule.match_mode.empty() ? std::string("all_matches") : rule.match_mode)); if (midx >= 0) cmb_auto_rule_match_mode_->setCurrentIndex(midx); }
        if (edt_auto_rule_excludes_) edt_auto_rule_excludes_->setText(auto_style_join_ids(rule.excludes_rule_ids));
        if (chk_auto_rule_stop_processing_) chk_auto_rule_stop_processing_->setChecked(rule.stop_processing);
        if (chk_auto_rule_require_stop_match_) chk_auto_rule_require_stop_match_->setChecked(rule.require_stop_match);
        if (chk_auto_rule_include_start_marker_) chk_auto_rule_include_start_marker_->setChecked(rule.include_start_marker);
        if (chk_auto_rule_include_end_marker_) chk_auto_rule_include_end_marker_->setChecked(rule.include_end_marker);
        if (spn_auto_rule_chars_) spn_auto_rule_chars_->setValue((int)(rule.end_offset ? rule.end_offset : rule.length));
    };
    auto refresh_auto_rule_list = [this]() {
        if (!lst_auto_style_rules_ || !layer_) return;
        obsbgs::StylePresetLibrary library;
        QSignalBlocker blocker(lst_auto_style_rules_);
        lst_auto_style_rules_->clear();
        int i = 1;
        for (const auto &rule : layer_->rich_text.auto_style_rules) {
            const QString preset_name = library.displayNameForId(QString::fromStdString(rule.style_preset_id));
            const QString display_name = rule.display_name.empty() ? preset_name : QString::fromStdString(rule.display_name);
            const QString rule_id = QString::fromStdString(rule.rule_id.empty() ? std::to_string(i) : rule.rule_id);
            const QString from = auto_style_marker_label(rule.start_condition, rule.start_offset, rule.start_custom_chars);
            const QString to = auto_style_marker_label(rule.end_condition, rule.end_offset ? rule.end_offset : rule.length, rule.end_custom_chars);
            auto *item = new QListWidgetItem(QStringLiteral("%1. %2  •  %3 → %4  •  %5%6")
                                             .arg(i++).arg(display_name).arg(from).arg(to)
                                             .arg(QString::fromStdString(rule.conflict_mode.empty() ? std::string("override_previous") : rule.conflict_mode))
                                             .arg(rule.enabled ? QString() : QStringLiteral("  [off]")),
                                             lst_auto_style_rules_);
            item->setData(Qt::UserRole, i - 2);
            item->setToolTip(QStringLiteral("Rule ID: %1\nPreset: %2\nRange: %3 → %4\nConflict mode: %5\n%6")
                             .arg(rule_id, preset_name, from, to,
                                  QString::fromStdString(rule.conflict_mode.empty() ? std::string("override_previous") : rule.conflict_mode),
                                  rule.enabled ? QStringLiteral("Enabled") : QStringLiteral("Disabled")));
        }
    };
    auto cache_text_preset_format = [](const std::string &preset_id, RichTextCharFormat &format, uint32_t &mask) {
        mask = 0;
        if (preset_id.empty()) return;
        obsbgs::StylePresetLibrary library;
        obsbgs::StylePreset preset;
        if (library.findById(QString::fromStdString(preset_id), &preset) &&
            preset.kind == obsbgs::StylePresetKind::Text &&
            obsbgs::StylePresetLibrary::textPresetToCharFormat(preset, format))
            mask = obsbgs::StylePresetLibrary::textPresetCharMask();
    };
    auto cache_rule_format = [cache_text_preset_format](RichTextAutoStyleRule &rule) {
        cache_text_preset_format(rule.style_preset_id, rule.cached_format, rule.cached_mask);
    };
    auto cache_default_auto_style = [this, cache_text_preset_format]() {
        if (!layer_) return;
        cache_text_preset_format(layer_->rich_text.auto_default_style_preset_id,
                                 layer_->rich_text.auto_default_style_cached_format,
                                 layer_->rich_text.auto_default_style_cached_mask);
    };
    auto invalidate_auto_text_styling = [this, cache_default_auto_style, cache_rule_format]() {
        if (!layer_) return;
        if (layer_->rich_text.empty())
            layer_->rich_text = rich_text_document_from_layer_defaults(*layer_);
        if (layer_->rich_text.plain_text != layer_->text_content) {
            const RichTextCharFormat insertion_format =
                insertion_format_for_text_replace(layer_->rich_text);
            const uint32_t insertion_mask =
                insertion_mask_for_text_replace(layer_->rich_text);
            rich_text_document_replace_text(layer_->rich_text, layer_->text_content,
                                            insertion_format, insertion_mask);
        }
        cache_default_auto_style();
        for (auto &rule : layer_->rich_text.auto_style_rules)
            cache_rule_format(rule);
        layer_->rich_text.normalize();
    };
    connect(chk_auto_style_enabled_, &QCheckBox::toggled, this, [this, invalidate_auto_text_styling](bool v){
        if (loading_values_ || !layer_) return;
        if (layer_->rich_text.empty()) layer_->rich_text = rich_text_document_from_layer_defaults(*layer_);
        layer_->rich_text.auto_style_enabled = v;
        invalidate_auto_text_styling();
        emit property_changed();
    });
    connect(cmb_auto_default_style_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, invalidate_auto_text_styling](int){
        if (loading_values_ || !layer_ || !cmb_auto_default_style_) return;
        if (layer_->rich_text.empty()) layer_->rich_text = rich_text_document_from_layer_defaults(*layer_);
        layer_->rich_text.auto_default_style_preset_id = cmb_auto_default_style_->currentData().toString().toStdString();
        invalidate_auto_text_styling();
        emit property_changed();
    });
    connect(btn_auto_rule_add_, &QPushButton::clicked, this, [this, apply_rule_editor_to_rule, cache_rule_format, invalidate_auto_text_styling, refresh_auto_rule_list](){
        if (!layer_ || !cmb_auto_rule_style_) return;
        const QString id = cmb_auto_rule_style_->currentData().toString();
        if (id.isEmpty()) return;
        if (layer_->rich_text.empty()) layer_->rich_text = rich_text_document_from_layer_defaults(*layer_);
        RichTextAutoStyleRule rule;
        rule.rule_id = std::string("rule_") + std::to_string(layer_->rich_text.auto_style_rules.size() + 1);
        rule.enabled = true;
        rule.style_preset_id = id.toStdString();
        apply_rule_editor_to_rule(rule);
        if (rule.rule_id.empty()) rule.rule_id = std::string("rule_") + std::to_string(layer_->rich_text.auto_style_rules.size() + 1);
        cache_rule_format(rule);
        layer_->rich_text.auto_style_rules.push_back(rule);
        invalidate_auto_text_styling();
        refresh_auto_rule_list();
        emit property_changed();
    });
    connect(btn_auto_rule_update_, &QPushButton::clicked, this, [this, apply_rule_editor_to_rule, cache_rule_format, invalidate_auto_text_styling, refresh_auto_rule_list](){
        if (!layer_ || !lst_auto_style_rules_ || !lst_auto_style_rules_->currentItem()) return;
        int row = lst_auto_style_rules_->currentRow();
        if (row < 0 || row >= (int)layer_->rich_text.auto_style_rules.size()) return;
        auto &rule = layer_->rich_text.auto_style_rules[(size_t)row];
        apply_rule_editor_to_rule(rule);
        cache_rule_format(rule);
        invalidate_auto_text_styling();
        refresh_auto_rule_list();
        lst_auto_style_rules_->setCurrentRow(row);
        emit property_changed();
    });
    connect(btn_auto_rule_delete_, &QPushButton::clicked, this, [this, invalidate_auto_text_styling, refresh_auto_rule_list](){
        if (!layer_ || !lst_auto_style_rules_ || !lst_auto_style_rules_->currentItem()) return;
        int row = lst_auto_style_rules_->currentRow();
        if (row < 0 || row >= (int)layer_->rich_text.auto_style_rules.size()) return;
        layer_->rich_text.auto_style_rules.erase(layer_->rich_text.auto_style_rules.begin() + row);
        invalidate_auto_text_styling();
        refresh_auto_rule_list();
        emit property_changed();
    });
    connect(btn_auto_rule_up_, &QPushButton::clicked, this, [this, invalidate_auto_text_styling, refresh_auto_rule_list](){
        if (!layer_ || !lst_auto_style_rules_) return;
        int row = lst_auto_style_rules_->currentRow();
        if (row <= 0 || row >= (int)layer_->rich_text.auto_style_rules.size()) return;
        std::swap(layer_->rich_text.auto_style_rules[(size_t)row], layer_->rich_text.auto_style_rules[(size_t)row - 1]);
        invalidate_auto_text_styling();
        refresh_auto_rule_list(); lst_auto_style_rules_->setCurrentRow(row - 1); emit property_changed();
    });
    connect(btn_auto_rule_down_, &QPushButton::clicked, this, [this, invalidate_auto_text_styling, refresh_auto_rule_list](){
        if (!layer_ || !lst_auto_style_rules_) return;
        int row = lst_auto_style_rules_->currentRow();
        if (row < 0 || row + 1 >= (int)layer_->rich_text.auto_style_rules.size()) return;
        std::swap(layer_->rich_text.auto_style_rules[(size_t)row], layer_->rich_text.auto_style_rules[(size_t)row + 1]);
        invalidate_auto_text_styling();
        refresh_auto_rule_list(); lst_auto_style_rules_->setCurrentRow(row + 1); emit property_changed();
    });
    connect(lst_auto_style_rules_, &QListWidget::currentRowChanged, this, [this, load_rule_to_editor](int row){
        if (!layer_ || row < 0 || row >= (int)layer_->rich_text.auto_style_rules.size()) return;
        const auto &rule = layer_->rich_text.auto_style_rules[(size_t)row];
        load_rule_to_editor(rule);
    });

    ticker_status_timer_ = new QTimer(this);
    ticker_status_timer_->setInterval(150);
    connect(ticker_status_timer_, &QTimer::timeout, this,
            &PropertiesPanel::update_ticker_runtime_button);
    ticker_status_timer_->start();
}

void PropertiesPanel::set_title(std::shared_ptr<Title> t)
{
    title_ = t;
    BGL_LOG_DEBUG("Properties", QStringLiteral("Properties title=%1")
        .arg(title_ ? QString::fromStdString(title_->id)
                    : QStringLiteral("<none>")));
}

void PropertiesPanel::set_active_text_edit_layer(const std::string &layer_id)
{
    if (active_text_edit_layer_id_ == layer_id) return;
    active_text_edit_layer_id_ = layer_id;
    load_values();
    emit gradient_model_refresh_requested();
}

void PropertiesPanel::update_asset_playback_controls_visibility()
{
    if (!asset_box_)
        return;
    auto set_field_visible = [this](QWidget *field, bool visible) {
        if (!field) return;
        field->setVisible(visible);
        if (auto *form = qobject_cast<QFormLayout *>(asset_box_->layout())) {
            if (auto *label = qobject_cast<QWidget *>(form->labelForField(field)))
                label->setVisible(visible);
        }
    };

    const bool is_asset = layer_ && layer_->type == LayerType::Asset;
    const bool has_animation = is_asset && layer_->asset_animated;
    const bool independent = has_animation && layer_->asset_playback_mode == 1;
    set_field_visible(cmb_asset_playback_, has_animation);
    set_field_visible(spn_asset_offset_, independent);
    set_field_visible(spn_asset_pause_duration_, independent &&
                      layer_->asset_source_playback_mode == 2);
    set_field_visible(spn_asset_loop_count_, independent &&
                      layer_->asset_source_playback_mode == 1);
    set_field_visible(chk_asset_loop_, independent);
}

void PropertiesPanel::set_layer(std::shared_ptr<Layer> layer, double t)
{
    layer_    = layer;
    playhead_ = t;
    BGL_LOG_DEBUG("Properties", QStringLiteral(
        "Properties layer=%1 type=%2 playhead=%3")
        .arg(layer_ ? QString::fromStdString(layer_->id)
                    : QStringLiteral("<none>"))
        .arg(layer_ ? static_cast<int>(layer_->type) : -1)
        .arg(playhead_, 0, 'f', 6));
    load_values();
    emit gradient_model_refresh_requested();
}

void PropertiesPanel::update_playhead(double t)
{
    playhead_ = t;
    if (!layer_ || loading_values_)
        return;

    QScopedValueRollback<bool> loading_guard(loading_values_, true);
    const double lt = std::clamp(playhead_ - layer_->in_time, 0.0,
                                 std::max(0.0, layer_->out_time - layer_->in_time));
    const bool is_text = layer_->type == LayerType::Text;
    const bool is_clock = layer_->type == LayerType::Clock;
    const bool is_ticker = layer_->type == LayerType::Ticker;
    const bool is_text_like = is_text || is_clock || is_ticker;
    const bool is_image = layer_->type == LayerType::Image;
    const bool is_rect = layer_->type == LayerType::SolidRect || layer_->type == LayerType::Shape;

    auto set_double = [](QDoubleSpinBox *spin, double value) {
        if (!spin || !std::isfinite(value))
            return;
        if (std::abs(spin->value() - value) < 0.000001)
            return;
        QSignalBlocker blocker(spin);
        spin->setValue(value);
    };
    auto set_int = [](QSpinBox *spin, int value) {
        if (!spin)
            return;
        if (spin->value() == value)
            return;
        QSignalBlocker blocker(spin);
        spin->setValue(value);
    };
    auto set_color_button = [](QPushButton *button, uint32_t argb) {
        if (!button)
            return;
        if (button->property("buttonFillKind").toString() == QStringLiteral("color") &&
            button->property("argb").isValid() && button->property("argb").toUInt() == argb)
            return;
        style_color_button(button, argb);
        button->setProperty("buttonFillKind", QStringLiteral("color"));
        button->setProperty("argb", argb);
    };
    auto set_gradient_button = [](QPushButton *button, uint32_t start_argb,
                                  uint32_t end_argb, int gradient_type) {
        if (!button)
            return;
        if (button->property("buttonFillKind").toString() == QStringLiteral("gradient") &&
            button->property("gradientStartArgb").isValid() &&
            button->property("gradientStartArgb").toUInt() == start_argb &&
            button->property("gradientEndArgb").toUInt() == end_argb &&
            button->property("gradientType").toInt() == gradient_type)
            return;
        style_gradient_button(button, start_argb, end_argb, gradient_type);
        button->setProperty("buttonFillKind", QStringLiteral("gradient"));
        button->setProperty("gradientStartArgb", start_argb);
        button->setProperty("gradientEndArgb", end_argb);
        button->setProperty("gradientType", gradient_type);
    };
    auto set_kf_icon = [](QPushButton *button, bool active, bool has_keyframes) {
        if (!button)
            return;
        const bool outlined = has_keyframes && !active;
        if (button->property("active").isValid() &&
            button->property("active").toBool() == active &&
            button->property("outlined").toBool() == outlined)
            return;
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

    const Vec2Value position = layer_->position.evaluate(lt);
    const Vec2Value scale = layer_->scale.evaluate(lt);
    set_double(spn_px_, position.x);
    set_double(spn_py_, position.y);
    set_double(spn_scale_x_, scale.x * 100.0);
    set_double(spn_scale_y_, scale.y * 100.0);
    set_double(spn_transform_size_w_, eval_box_width(*layer_, lt));
    set_double(spn_transform_size_h_, eval_box_height(*layer_, lt));
    set_double(spn_rot_, layer_->rotation.evaluate(lt));
    set_double(spn_opacity_, layer_->opacity.evaluate(lt));
    if (spn_appearance_opacity_)
        set_double(spn_appearance_opacity_, layer_->opacity.evaluate(lt) * 100.0);
    set_double(spn_origin_x_, eval_origin_x(*layer_, lt));
    set_double(spn_origin_y_, eval_origin_y(*layer_, lt));

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
        set_double(spn_layer_w_, display.width);
        set_double(spn_layer_h_, display.height);
    } else {
        set_double(spn_layer_w_, eval_box_width(*layer_, lt));
        set_double(spn_layer_h_, eval_box_height(*layer_, lt));
    }
    set_double(spn_image_box_w_, eval_box_width(*layer_, lt));
    set_double(spn_image_box_h_, eval_box_height(*layer_, lt));

    set_color_button(btn_text_color_, eval_text_color(*layer_, lt));
    set_color_button(btn_fill_color_, eval_fill_color(*layer_, lt));
    if (btn_appearance_fill_color_) {
        if (layer_->fill_type == 1)
            set_gradient_button(btn_appearance_fill_color_,
                                layer_->gradient_start_color,
                                layer_->gradient_end_color,
                                layer_->gradient_type);
        else
            set_color_button(btn_appearance_fill_color_,
                             is_text_like ? eval_text_color(*layer_, lt) : eval_fill_color(*layer_, lt));
        btn_appearance_fill_color_->setText(QString());
    }

    if (is_text_like) {
        set_int(spn_size_, (int)std::clamp(std::round(layer_->font_size_prop.evaluate(lt)), 1.0, 512.0));
        set_double(spn_char_tracking_, layer_->char_tracking_prop.evaluate(lt));
        set_double(spn_char_scale_x_, layer_->char_scale_x_prop.evaluate(lt) * 100.0);
        set_double(spn_char_scale_y_, layer_->char_scale_y_prop.evaluate(lt) * 100.0);
        set_double(spn_baseline_shift_, layer_->baseline_shift_prop.evaluate(lt));
        set_double(spn_paragraph_indent_left_, eval_paragraph_indent_left(*layer_, lt));
        set_double(spn_paragraph_indent_right_, eval_paragraph_indent_right(*layer_, lt));
        set_double(spn_paragraph_indent_first_line_, eval_paragraph_indent_first_line(*layer_, lt));
        set_double(spn_paragraph_space_before_, layer_->paragraph_space_before_prop.evaluate(lt));
        set_double(spn_paragraph_space_after_, layer_->paragraph_space_after_prop.evaluate(lt));

        if (lbl_text_fit_scale_ && layer_->text_overflow_mode == 2 && !is_ticker) {
            QFont preview_font = font_for_layer(*layer_);
            QRectF preview_rect(0, 0, eval_box_width(*layer_, lt), eval_box_height(*layer_, lt));
            double fit_scale = horizontal_fit_scale(preview_font, preview_rect,
                                                    display_text_for_style(*layer_), *layer_, lt);
            lbl_text_fit_scale_->setText(bgl_tr("OBSTitles.ScalePercentFormat")
                                             .arg((int)std::round(fit_scale * 100.0)));
        }
    }

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
    else if (is_rect)
        set_group_kf_icon(btn_kf_appearance_fill_, {&layer_->fill_color_a, &layer_->fill_color_r,
                                                    &layer_->fill_color_g, &layer_->fill_color_b});
}

void PropertiesPanel::update_ticker_runtime_button()
{
    if (!btn_ticker_pause_ || !layer_ || layer_->type != LayerType::Ticker)
        return;
    const std::string title_id = title_ ? title_->id : std::string();
    const bool title_cued = title_ &&
        (title_->current_cue_row >= 0 || title_->pending_cue_row >= 0);
    const TickerRuntimeSnapshot state =
        bgs::ticker_runtime::status(title_id, *layer_, title_cued);
    btn_ticker_pause_->setIcon(obs_icon(state.paused ? "play.svg" : "pause.svg"));
    btn_ticker_pause_->setText(state.paused
        ? bgl_tr("OBSTitles.TickerPlay")
        : bgl_tr("OBSTitles.TickerPause"));
    btn_ticker_pause_->setToolTip(state.paused
        ? bgl_tr("OBSTitles.TickerPlayTooltip")
        : bgl_tr("OBSTitles.TickerPauseTooltip"));
}

void PropertiesPanel::load_values()
{
    loading_values_ = true;
    if (!layer_) {
        if (transform_box_) transform_box_->setVisible(false);
        if (asset_box_) asset_box_->setVisible(false);
        if (appearance_box_) appearance_box_->setVisible(false);
        if (btn_transform_defaults_) btn_transform_defaults_->setEnabled(false);
        if (btn_anchor_grid_) { btn_anchor_grid_->setProperty("active_index", 4); btn_anchor_grid_->update(); }
        text_box_->setVisible(false);
        if (type_options_box_) type_options_box_->setVisible(false);
        if (paragraph_box_) paragraph_box_->setVisible(false);
        if (dynamic_text_box_) dynamic_text_box_->setVisible(false);
        if (auto_style_box_) auto_style_box_->setVisible(false);
        if (live_edit_box_) live_edit_box_->setVisible(false);
        if (bullets_box_) bullets_box_->setVisible(false);
        rect_box_->setVisible(false);
        if (btn_shape_defaults_) btn_shape_defaults_->setVisible(false);
        if (chk_size_lock_) chk_size_lock_->setVisible(false);
        if (row_shape_scale_options_) row_shape_scale_options_->setVisible(false);
        if (grp_shape_type_) {
            for (auto *button : grp_shape_type_->buttons())
                button->setVisible(false);
        }
        if (auto *shape_types_row = rect_box_ ? rect_box_->findChild<QWidget *>(QStringLiteral("OBSTitlesShapeTypeButtonsRow")) : nullptr)
            shape_types_row->setVisible(false);
        if (gradient_box_) gradient_box_->setVisible(false);
        image_box_->setVisible(false);
        if (image_box_size_box_) image_box_size_box_->setVisible(false);
        if (outline_box_) outline_box_->setVisible(false);
        if (shadow_box_) shadow_box_->setVisible(false);
        spn_px_->setValue(0.0);
        spn_py_->setValue(0.0);
        spn_rot_->setValue(0.0);
        spn_opacity_->setValue(1.0);
        spn_origin_x_->setValue(0.5);
        spn_origin_y_->setValue(0.5);
        if (chk_shape_scale_stroke_) chk_shape_scale_stroke_->setChecked(false);
        if (chk_shape_scale_corners_) chk_shape_scale_corners_->setChecked(false);
        txt_content_->clear();
        edit_image_path_->clear();
        set_image_preview_label(lbl_image_preview_, QString());
        if (cmb_image_scale_filter_) {
            QSignalBlocker block(cmb_image_scale_filter_);
            cmb_image_scale_filter_->setCurrentIndex(1);
        }
        if (cmb_image_box_mode_) {
            QSignalBlocker block(cmb_image_box_mode_);
            int mode_index = cmb_image_box_mode_->findData((int)ImageBoxMode::FitImageToBox);
            cmb_image_box_mode_->setCurrentIndex(mode_index >= 0 ? mode_index : 0);
        }
        if (chk_image_crop_when_outside_box_) {
            QSignalBlocker block(chk_image_crop_when_outside_box_);
            chk_image_crop_when_outside_box_->setChecked(false);
        }
        if (btn_image_anchor_grid_) { btn_image_anchor_grid_->setProperty("active_index", 4); btn_image_anchor_grid_->update(); }
        style_color_button(btn_text_color_, 0xFFFFFFFF);
        if (btn_text_color_) btn_text_color_->setEnabled(true);
        if (btn_kf_text_color_) btn_kf_text_color_->setEnabled(true);
        style_color_button(btn_fill_color_, 0xFF222222);
        if (cmb_fill_type_) cmb_fill_type_->setCurrentIndex(0);
        if (cmb_gradient_type_) cmb_gradient_type_->setCurrentIndex(0);
        if (cmb_gradient_spread_) cmb_gradient_spread_->setCurrentIndex(0);
        if (btn_gradient_start_color_) style_color_button(btn_gradient_start_color_, 0xFF4B6EA8);
        if (btn_gradient_end_color_) style_color_button(btn_gradient_end_color_, 0xFF1B1B1B);
        if (btn_appearance_fill_color_) { style_color_button(btn_appearance_fill_color_, 0xFF222222); btn_appearance_fill_color_->setText(QString()); }
        if (btn_appearance_stroke_color_) {
            style_color_button(btn_appearance_stroke_color_, 0xFF000000);
            btn_appearance_stroke_color_->setText(QString());
            btn_appearance_stroke_color_->setVisible(false);
        }
        if (btn_appearance_stroke_label_) btn_appearance_stroke_label_->setVisible(false);
        if (spn_appearance_stroke_width_) {
            spn_appearance_stroke_width_->setValue(0.0);
            spn_appearance_stroke_width_->setVisible(false);
        }
        if (spn_appearance_opacity_) spn_appearance_opacity_->setValue(100.0);
        if (spn_gradient_start_pos_) spn_gradient_start_pos_->setValue(0.0);
        if (spn_gradient_end_pos_) spn_gradient_end_pos_->setValue(1.0);
        if (spn_gradient_start_opacity_) spn_gradient_start_opacity_->setValue(1.0);
        if (spn_gradient_end_opacity_) spn_gradient_end_opacity_->setValue(1.0);
        if (spn_gradient_opacity_) spn_gradient_opacity_->setValue(1.0);
        if (spn_gradient_angle_) spn_gradient_angle_->setValue(0.0);
        if (spn_gradient_center_x_) spn_gradient_center_x_->setValue(0.5);
        if (spn_gradient_center_y_) spn_gradient_center_y_->setValue(0.5);
        if (spn_gradient_scale_) spn_gradient_scale_->setValue(1.0);
        if (spn_gradient_focal_x_) spn_gradient_focal_x_->setValue(0.5);
        if (spn_gradient_focal_y_) spn_gradient_focal_y_->setValue(0.5);
        spn_layer_w_->setValue(0.0);
        spn_layer_h_->setValue(0.0);
        if (spn_image_box_w_) spn_image_box_w_->setValue(0.0);
        if (spn_image_box_h_) spn_image_box_h_->setValue(0.0);
        if (spn_rect_corner_tl_) spn_rect_corner_tl_->setValue(0.0);
        if (spn_rect_corner_tr_) spn_rect_corner_tr_->setValue(0.0);
        if (spn_rect_corner_br_) spn_rect_corner_br_->setValue(0.0);
        if (spn_rect_corner_bl_) spn_rect_corner_bl_->setValue(0.0);
        if (spn_corner_bevel_roundness_) spn_corner_bevel_roundness_->setValue(100.0);
        spn_size_->setValue(72);
        if (cmb_font_style_) populate_font_style_combo(cmb_font_style_, cmb_font_->currentText(), bgl_tr("OBSTitles.Regular"));
        chk_bold_->setChecked(false);
        chk_italic_->setChecked(false);
        if (chk_font_kerning_) chk_font_kerning_->setChecked(true);
        if (spn_text_leading_) spn_text_leading_->setValue(0.0);
        if (spn_char_tracking_) spn_char_tracking_->setValue(0.0);
        if (cmb_kerning_mode_) cmb_kerning_mode_->setCurrentIndex(0);
        if (spn_kerning_value_) spn_kerning_value_->setValue(0.0);
        if (spn_scale_x_) spn_scale_x_->setValue(100.0);
        if (spn_scale_y_) spn_scale_y_->setValue(100.0);
        if (spn_transform_size_w_) spn_transform_size_w_->setValue(0.0);
        if (spn_transform_size_h_) spn_transform_size_h_->setValue(0.0);
        if (chk_scale_lock_) chk_scale_lock_->setChecked(true);
        if (chk_transform_size_lock_) chk_transform_size_lock_->setChecked(true);
        if (chk_image_box_size_lock_) chk_image_box_size_lock_->setChecked(false);
        if (spn_char_scale_x_) spn_char_scale_x_->setValue(100.0);
        if (spn_char_scale_y_) spn_char_scale_y_->setValue(100.0);
        if (spn_baseline_shift_) spn_baseline_shift_->setValue(0.0);
        for (auto *b : {btn_all_caps_, btn_small_caps_, btn_superscript_, btn_subscript_, btn_underline_,
                        btn_strikethrough_, btn_ligatures_, btn_stylistic_alternates_, btn_fractions_, btn_opentype_features_})
            if (b) b->setChecked(false);
        if (cmb_text_style_) cmb_text_style_->setCurrentIndex(0);
        if (cmb_text_overflow_) cmb_text_overflow_->setCurrentIndex(0);
        if (cmb_ticker_playback_mode_) cmb_ticker_playback_mode_->setCurrentIndex(0);
        if (btn_ticker_pause_) {
            btn_ticker_pause_->setIcon(obs_icon("pause.svg"));
            btn_ticker_pause_->setText(bgl_tr("OBSTitles.TickerPause"));
        }
        if (spn_text_fit_min_scale_) spn_text_fit_min_scale_->setValue(0.5);
        if (lbl_text_fit_scale_) lbl_text_fit_scale_->setText(bgl_tr("OBSTitles.Scale100"));
        if (chk_text_box_width_to_text_) chk_text_box_width_to_text_->setChecked(false);
        if (chk_text_box_height_to_text_) chk_text_box_height_to_text_->setChecked(false);
        if (spn_max_text_box_width_) { spn_max_text_box_width_->setValue(1920.0); spn_max_text_box_width_->setEnabled(false); }
        if (spn_max_text_box_height_) { spn_max_text_box_height_->setValue(1080.0); spn_max_text_box_height_->setEnabled(false); }
        if (grp_text_align_ && grp_text_align_->button(1)) grp_text_align_->button(1)->setChecked(true);
        if (grp_text_valign_ && grp_text_valign_->button(1)) grp_text_valign_->button(1)->setChecked(true);
        if (spn_paragraph_indent_left_) spn_paragraph_indent_left_->setValue(0.0);
        if (spn_paragraph_indent_right_) spn_paragraph_indent_right_->setValue(0.0);
        if (spn_paragraph_indent_first_line_) spn_paragraph_indent_first_line_->setValue(0.0);
        if (spn_paragraph_space_before_) spn_paragraph_space_before_->setValue(0.0);
        if (spn_paragraph_space_after_) spn_paragraph_space_after_->setValue(0.0);
        if (chk_paragraph_hyphenate_) chk_paragraph_hyphenate_->setChecked(false);
        if (cmb_anchor_) cmb_anchor_->setCurrentIndex(4);
        if (btn_anchor_grid_) { btn_anchor_grid_->setProperty("active_index", 4); btn_anchor_grid_->update(); }
        for (auto *b : {btn_kf_pos_x_, btn_kf_pos_y_, btn_kf_scale_x_, btn_kf_scale_y_,
                        btn_kf_rotation_, btn_kf_opacity_, btn_kf_origin_x_, btn_kf_origin_y_,
                        btn_kf_appearance_fill_, btn_kf_appearance_stroke_,
                        btn_kf_paragraph_indent_left_, btn_kf_paragraph_indent_right_, btn_kf_paragraph_indent_first_line_,
                        btn_kf_width_, btn_kf_transform_size_,
                        btn_kf_text_color_, btn_kf_fill_color_}) {
            if (!b) continue;
            b->setIcon(keyframe_diamond_icon(false));
            b->setProperty("active", false);
            b->setProperty("outlined", false);
            b->style()->unpolish(b);
            b->style()->polish(b);
        }
        loading_values_ = false;
        return;
    }

    const bool is_text = layer_->type == LayerType::Text;
    const bool is_clock = layer_->type == LayerType::Clock;
    const bool is_ticker = layer_->type == LayerType::Ticker;
    const bool is_text_like = is_text || is_clock || is_ticker;
    const bool is_adjustment = layer_->type == LayerType::Adjustment;
    const bool is_color_solid = layer_->type == LayerType::ColorSolid;
    const bool is_fixed_canvas_layer = false;
    const bool is_rect = layer_->type == LayerType::SolidRect ||
                         layer_->type == LayerType::Shape || is_color_solid;
    const bool is_image = layer_->type == LayerType::Image;
    const bool is_asset = layer_->type == LayerType::Asset;
    const bool is_scene_mask_layer = layer_->use_as_scene_mask;
    const bool supports_outline = is_text_like || is_rect || is_image;
    if (transform_box_) transform_box_->setVisible(!is_fixed_canvas_layer);
    if (asset_box_) asset_box_->setVisible(is_asset);
    if (is_asset) {
        if (lbl_asset_name_) {
            const QString source_name = title_ && !layer_->asset_title_id.empty()
                ? [&]() {
                    for (const auto &candidate : TitleDataStore::instance().titles())
                        if (candidate && candidate->id == layer_->asset_title_id)
                            return QString::fromStdString(candidate->name);
                    return QString::fromStdString(layer_->name);
                }()
                : QString::fromStdString(layer_->name);
            lbl_asset_name_->setText(source_name);
        }
        if (!layer_->asset_animated)
            layer_->asset_playback_mode = 0;
        if (cmb_asset_playback_) {
            const int index = cmb_asset_playback_->findData(layer_->asset_playback_mode);
            cmb_asset_playback_->setCurrentIndex(index >= 0 ? index : 0);
        }
        if (spn_asset_offset_) spn_asset_offset_->setValue(layer_->asset_playback_offset);
        if (spn_asset_pause_duration_) spn_asset_pause_duration_->setValue(layer_->asset_pause_duration);
        if (spn_asset_loop_count_) spn_asset_loop_count_->setValue(std::max(1, layer_->asset_loop_count));
        if (chk_asset_loop_) chk_asset_loop_->setChecked(layer_->asset_loop);
        update_asset_playback_controls_visibility();
    }
    if (appearance_box_) appearance_box_->setVisible(!is_adjustment);
    if (btn_transform_defaults_) btn_transform_defaults_->setEnabled(!is_fixed_canvas_layer);
    text_box_->setVisible(is_text_like);
    if (type_options_box_) type_options_box_->setVisible(is_text_like);
    if (paragraph_box_) paragraph_box_->setVisible(is_text_like);
    if (dynamic_text_box_) dynamic_text_box_->setVisible(is_text_like);
    if (auto_style_box_) auto_style_box_->setVisible(is_text_like);
    if (live_edit_box_)
        live_edit_box_->setVisible(is_rect || is_text_like || is_image ||
                                   is_adjustment || is_color_solid);
    if (bullets_box_) bullets_box_->setVisible(false);
    text_box_->setTitle("Character");
    if (row_text_color_) row_text_color_->setVisible(false);
    if (btn_kf_text_color_) btn_kf_text_color_->setVisible(false);
    if (auto *char_grid = qobject_cast<QGridLayout *>(text_box_->layout())) {
        const bool show_text_editor = !is_text;
        if (auto *label_item = char_grid->itemAtPosition(0, 0)) {
            if (auto *label = qobject_cast<QLabel *>(label_item->widget())) {
                label->setText(is_clock ? bgl_tr("OBSTitles.DateTimeFormatLabel")
                                        : bgl_tr("OBSTitles.TextLabel"));
                label->setVisible(show_text_editor);
            }
        }
        if (txt_content_) txt_content_->setVisible(show_text_editor);
        if (auto *label_item = char_grid->itemAtPosition(7, 0)) {
            if (auto *label = label_item->widget())
                label->setVisible(false);
        }
    }
    txt_content_->setPlaceholderText(is_clock ? "H:i:s" : bgl_tr("OBSTitles.EnterTextPlaceholder"));
    if (cmb_auto_default_style_ && cmb_auto_rule_style_) {
        obsbgs::StylePresetLibrary library;
        const auto presets = library.presets(obsbgs::StylePresetKind::Text);
        auto fill_combo = [&](QComboBox *combo, bool include_none, const QString &selected) {
            QSignalBlocker blocker(combo);
            combo->clear();
            if (include_none) combo->addItem("None", QString());
            for (const auto &preset : presets)
                combo->addItem(preset.name + QStringLiteral(" — ") + preset.category, preset.id);
            const int idx = combo->findData(selected);
            if (idx >= 0) combo->setCurrentIndex(idx);
        };
        fill_combo(cmb_auto_default_style_, true, QString::fromStdString(layer_->rich_text.auto_default_style_preset_id));
        const QString first_rule = layer_->rich_text.auto_style_rules.empty()
            ? QString()
            : QString::fromStdString(layer_->rich_text.auto_style_rules.front().style_preset_id);
        fill_combo(cmb_auto_rule_style_, false, first_rule);
        if (chk_auto_style_enabled_) chk_auto_style_enabled_->setChecked(layer_->rich_text.auto_style_enabled);
        QSignalBlocker list_blocker(lst_auto_style_rules_);
        lst_auto_style_rules_->clear();
        int i = 1;
        for (const auto &rule : layer_->rich_text.auto_style_rules) {
            const QString preset_name = library.displayNameForId(QString::fromStdString(rule.style_preset_id));
            const QString display_name = rule.display_name.empty() ? preset_name : QString::fromStdString(rule.display_name);
            const QString rule_id = QString::fromStdString(rule.rule_id.empty() ? std::to_string(i) : rule.rule_id);
            const QString from = auto_style_marker_label(rule.start_condition, rule.start_offset, rule.start_custom_chars);
            const QString to = auto_style_marker_label(rule.end_condition, rule.end_offset ? rule.end_offset : rule.length, rule.end_custom_chars);
            auto *item = new QListWidgetItem(QStringLiteral("%1. %2  •  %3 → %4  •  %5%6")
                                             .arg(i++).arg(display_name).arg(from).arg(to)
                                             .arg(QString::fromStdString(rule.conflict_mode.empty() ? std::string("override_previous") : rule.conflict_mode))
                                             .arg(rule.enabled ? QString() : QStringLiteral("  [off]")),
                                             lst_auto_style_rules_);
            item->setData(Qt::UserRole, i - 2);
            item->setToolTip(QStringLiteral("Rule ID: %1\nPreset: %2\nRange: %3 → %4\nConflict mode: %5\n%6")
                             .arg(rule_id, preset_name, from, to,
                                  QString::fromStdString(rule.conflict_mode.empty() ? std::string("override_previous") : rule.conflict_mode),
                                  rule.enabled ? QStringLiteral("Enabled") : QStringLiteral("Disabled")));
        }
        if (!layer_->rich_text.auto_style_rules.empty()) {
            lst_auto_style_rules_->setCurrentRow(0);
            const auto &rule = layer_->rich_text.auto_style_rules.front();
            int sidx = cmb_auto_rule_start_condition_->findData(QString::fromStdString(rule.start_condition.empty() ? std::string("text_start") : rule.start_condition));
            int eidx = cmb_auto_rule_end_condition_->findData(QString::fromStdString(rule.end_condition.empty() ? std::string("character_index") : rule.end_condition));
            if (chk_auto_rule_enabled_) chk_auto_rule_enabled_->setChecked(rule.enabled);
            if (edt_auto_rule_name_) edt_auto_rule_name_->setText(QString::fromStdString(rule.display_name));
            if (sidx >= 0) cmb_auto_rule_start_condition_->setCurrentIndex(sidx);
            if (eidx >= 0) cmb_auto_rule_end_condition_->setCurrentIndex(eidx);
            spn_auto_rule_start_offset_->setValue((int)rule.start_offset);
            spn_auto_rule_end_offset_->setValue((int)(rule.end_offset ? rule.end_offset : rule.length));
            edt_auto_rule_start_chars_->setText(QString::fromStdString(rule.start_custom_chars));
            edt_auto_rule_end_chars_->setText(QString::fromStdString(rule.end_custom_chars));
            if (cmb_auto_rule_conflict_mode_) { int cidx = cmb_auto_rule_conflict_mode_->findData(QString::fromStdString(rule.conflict_mode.empty() ? std::string("override_previous") : rule.conflict_mode)); if (cidx >= 0) cmb_auto_rule_conflict_mode_->setCurrentIndex(cidx); }
            if (cmb_auto_rule_match_mode_) { int midx = cmb_auto_rule_match_mode_->findData(QString::fromStdString(rule.match_mode.empty() ? std::string("all_matches") : rule.match_mode)); if (midx >= 0) cmb_auto_rule_match_mode_->setCurrentIndex(midx); }
            if (edt_auto_rule_excludes_) edt_auto_rule_excludes_->setText(auto_style_join_ids(rule.excludes_rule_ids));
            if (chk_auto_rule_stop_processing_) chk_auto_rule_stop_processing_->setChecked(rule.stop_processing);
            if (chk_auto_rule_require_stop_match_) chk_auto_rule_require_stop_match_->setChecked(rule.require_stop_match);
            if (chk_auto_rule_include_start_marker_) chk_auto_rule_include_start_marker_->setChecked(rule.include_start_marker);
            if (chk_auto_rule_include_end_marker_) chk_auto_rule_include_end_marker_->setChecked(rule.include_end_marker);
            if (spn_auto_rule_chars_) spn_auto_rule_chars_->setValue((int)(rule.end_offset ? rule.end_offset : rule.length));
        } else {
            if (chk_auto_rule_enabled_) chk_auto_rule_enabled_->setChecked(true);
            if (edt_auto_rule_name_) edt_auto_rule_name_->clear();
            if (cmb_auto_rule_start_condition_) cmb_auto_rule_start_condition_->setCurrentIndex(0);
            if (cmb_auto_rule_end_condition_) cmb_auto_rule_end_condition_->setCurrentIndex(cmb_auto_rule_end_condition_->findData("character_index"));
            if (spn_auto_rule_start_offset_) spn_auto_rule_start_offset_->setValue(0);
            if (spn_auto_rule_end_offset_) spn_auto_rule_end_offset_->setValue(0);
            if (edt_auto_rule_start_chars_) edt_auto_rule_start_chars_->clear();
            if (edt_auto_rule_end_chars_) edt_auto_rule_end_chars_->clear();
            if (cmb_auto_rule_conflict_mode_) cmb_auto_rule_conflict_mode_->setCurrentIndex(0);
            if (cmb_auto_rule_match_mode_) cmb_auto_rule_match_mode_->setCurrentIndex(0);
            if (edt_auto_rule_excludes_) edt_auto_rule_excludes_->clear();
            if (chk_auto_rule_stop_processing_) chk_auto_rule_stop_processing_->setChecked(false);
            if (chk_auto_rule_require_stop_match_) chk_auto_rule_require_stop_match_->setChecked(true);
            if (chk_auto_rule_include_start_marker_) chk_auto_rule_include_start_marker_->setChecked(true);
            if (chk_auto_rule_include_end_marker_) chk_auto_rule_include_end_marker_->setChecked(false);
            if (spn_auto_rule_chars_) spn_auto_rule_chars_->setValue(0);
        }
    }
    if (spn_text_fit_min_scale_) spn_text_fit_min_scale_->setVisible(is_text_like && layer_->text_overflow_mode == 2 && !is_ticker);
    if (lbl_text_fit_scale_) lbl_text_fit_scale_->setVisible(is_text_like && layer_->text_overflow_mode == 2 && !is_ticker);
    if (auto *dynamic_form = qobject_cast<QFormLayout *>(dynamic_text_box_ ? dynamic_text_box_->layout() : nullptr)) {
        const bool show_ticker_fit = is_text_like && layer_->text_overflow_mode == 2 && !is_ticker;
        if (cmb_text_style_) {
            cmb_text_style_->setVisible(false);
            if (auto *label = dynamic_form->labelForField(cmb_text_style_)) label->setVisible(false);
        }
        if (auto *label = dynamic_form->labelForField(spn_text_fit_min_scale_))
            label->setVisible(show_ticker_fit);
        if (row_ticker_playback_) {
            row_ticker_playback_->setVisible(is_ticker);
            if (auto *label = dynamic_form->labelForField(row_ticker_playback_))
                label->setVisible(is_ticker);
        }
        if (row_ticker_completion_) {
            const bool show_completion = is_ticker && layer_->ticker_playback_mode == static_cast<int>(TickerPlaybackMode::CustomPlayback);
            row_ticker_completion_->setVisible(show_completion);
            if (auto *label = dynamic_form->labelForField(row_ticker_completion_)) label->setVisible(show_completion);
        }
        if (cmb_ticker_style_) {
            cmb_ticker_style_->setVisible(is_ticker);
            if (auto *label = dynamic_form->labelForField(cmb_ticker_style_)) label->setVisible(is_ticker);
        }
        if (spn_ticker_speed_) {
            spn_ticker_speed_->setVisible(is_ticker && layer_->ticker_style != 1);
            if (auto *label = dynamic_form->labelForField(spn_ticker_speed_)) label->setVisible(is_ticker && layer_->ticker_style != 1);
        }
        if (spn_ticker_line_hold_) {
            spn_ticker_line_hold_->setVisible(is_ticker && layer_->ticker_style == 1);
            if (auto *label = dynamic_form->labelForField(spn_ticker_line_hold_)) label->setVisible(is_ticker && layer_->ticker_style == 1);
        }
        if (cmb_ticker_direction_) {
            cmb_ticker_direction_->setVisible(is_ticker);
            if (auto *label = dynamic_form->labelForField(cmb_ticker_direction_)) label->setVisible(is_ticker);
        }
    }
    if (auto *live_form = qobject_cast<QFormLayout *>(live_edit_box_ ? live_edit_box_->layout() : nullptr)) {
        const bool show_scene_mask = is_rect || is_text_like;
        const bool show_expose_to_dock = is_text || is_ticker || is_image;
        const bool show_exposed_options = show_expose_to_dock && layer_->expose_text;
        const bool show_ignore_persistence = true;
        if (chk_scene_mask_) {
            chk_scene_mask_->setVisible(show_scene_mask);
            if (auto *label = live_form->labelForField(chk_scene_mask_))
                label->setVisible(show_scene_mask);
        }
        if (chk_expose_text_) {
            chk_expose_text_->setVisible(show_expose_to_dock);
            if (auto *label = live_form->labelForField(chk_expose_text_))
                label->setVisible(show_expose_to_dock);
        }
        if (chk_exposed_hide_if_empty_) {
            chk_exposed_hide_if_empty_->setVisible(show_exposed_options);
            chk_exposed_hide_if_empty_->setEnabled(show_exposed_options);
            if (auto *label = live_form->labelForField(chk_exposed_hide_if_empty_))
