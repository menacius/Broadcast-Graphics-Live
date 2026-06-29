        // Route history shortcuts before the focused canvas/viewport can consume
        // them. Text-entry widgets retain their own native document undo/redo.
        if (in_editor && !editing_value && !key_event->isAutoRepeat()) {
            const bool undo_shortcut = key_event->key() == Qt::Key_Z &&
                shortcut_modifiers == Qt::ControlModifier;
            const bool redo_shortcut =
                (key_event->key() == Qt::Key_Z &&
                 shortcut_modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) ||
                (key_event->key() == Qt::Key_Y &&
                 shortcut_modifiers == Qt::ControlModifier);
            if (undo_shortcut) {
                if (undo_index_ > 0)
                    restore_undo_snapshot(undo_index_ - 1);
                key_event->accept();
                return true;
            }
            if (redo_shortcut) {
                if (undo_index_ + 1 < (int)undo_stack_.size())
                    restore_undo_snapshot(undo_index_ + 1);
                key_event->accept();
                return true;
            }

            if (key_event->matches(QKeySequence::New)) {
                new_title_contents();
                key_event->accept();
                return true;
            }
            if (key_event->matches(QKeySequence::Save)) {
                save_title();
                key_event->accept();
                return true;
            }
            if (key_event->matches(QKeySequence::Copy)) {
                if (timeline_ && timeline_->has_transition_target_selection())
                    timeline_->copy_transition_selection();
                else if (timeline_ && timeline_->has_selected_keyframes())
                    timeline_->copy_keyframe_selection();
                else
                    copy_selected_layer();
                key_event->accept();
                return true;
            }
            if (key_event->matches(QKeySequence::Cut)) {
                if (timeline_ && timeline_->has_transition_target_selection())
                    timeline_->cut_transition_selection();
                else if (timeline_ && timeline_->has_selected_keyframes())
                    timeline_->cut_keyframe_selection();
                else
                    cut_selected_layer();
                key_event->accept();
                return true;
            }
            if (key_event->matches(QKeySequence::Paste)) {
                if (timeline_ && timeline_->has_transition_target_selection())
                    timeline_->paste_transition_to_selection();
                else if (timeline_ && timeline_->has_keyframe_clipboard())
                    timeline_->paste_keyframes_at_playhead();
                else if (!layer_clipboard_.empty())
                    paste_layer_from_clipboard();
                else
                    paste_external_clipboard_to_canvas();
                key_event->accept();
                return true;
            }
            if (key_event->key() == Qt::Key_Delete ||
                key_event->key() == Qt::Key_Backspace) {
                if (timeline_ && timeline_->has_transition_target_selection())
                    timeline_->delete_transition_selection();
                else if (timeline_ && timeline_->has_selected_keyframes())
                    timeline_->delete_keyframe_selection();
                else
                    delete_selected_layer();
                key_event->accept();
                return true;
            }
            if (key_event->key() == Qt::Key_D &&
                shortcut_modifiers == Qt::ControlModifier) {
                duplicate_selected_layers();
                key_event->accept();
                return true;
            }
            if (key_event->key() == Qt::Key_G &&
                shortcut_modifiers == Qt::ControlModifier) {
                group_selected_layers();
                key_event->accept();
                return true;
            }
            if (key_event->key() == Qt::Key_G &&
                shortcut_modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) {
                ungroup_selected_layers();
                key_event->accept();
                return true;
            }
            if (key_event->key() == Qt::Key_R &&
                shortcut_modifiers == Qt::ControlModifier &&
                act_rulers_visible_) {
                act_rulers_visible_->toggle();
                key_event->accept();
                return true;
            }
            if (key_event->key() == Qt::Key_QuoteLeft &&
                shortcut_modifiers == Qt::ControlModifier &&
                act_guides_visible_) {
                act_guides_visible_->toggle();
                key_event->accept();
                return true;
            }
            if (key_event->key() == Qt::Key_Semicolon &&
                shortcut_modifiers == Qt::ControlModifier &&
                act_snap_enabled_) {
                act_snap_enabled_->toggle();
                key_event->accept();
                return true;
            }
        }

        if (in_editor && !editing_value && tool_modifiers == Qt::NoModifier &&
            !key_event->isAutoRepeat() && tools_sidebar_) {
            switch (key_event->key()) {
            case Qt::Key_V:
                tools_sidebar_->activate_selection_tool();
                key_event->accept();
                return true;
            case Qt::Key_A:
                tools_sidebar_->activate_direct_selection_tool();
                key_event->accept();
                return true;
            case Qt::Key_E:
                tools_sidebar_->activate_free_transform_tool();
                key_event->accept();
                return true;
            case Qt::Key_P:
                tools_sidebar_->activate_pen_tool();
                key_event->accept();
                return true;
            case Qt::Key_M:
                tools_sidebar_->activate_shape_tool(ShapeType::Rectangle);
                key_event->accept();
                return true;
            case Qt::Key_L:
                tools_sidebar_->activate_shape_tool(ShapeType::Ellipse);
                key_event->accept();
                return true;
            case Qt::Key_T:
                tools_sidebar_->activate_text_tool(LayerType::Text);
                key_event->accept();
                return true;
            case Qt::Key_I:
                tools_sidebar_->activate_color_picker_tool();
                key_event->accept();
                return true;
            case Qt::Key_G:
                tools_sidebar_->activate_gradient_tool();
                key_event->accept();
                return true;
            default:
                break;
            }
        }

        if (in_editor && key_event->key() == Qt::Key_Space &&
            shortcut_modifiers == Qt::NoModifier && !key_event->isAutoRepeat() &&
            QApplication::mouseButtons() == Qt::NoButton) {
            if (!editing_value) {
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
    QWidget *fw = focusWidget();
    bool editing_value = editor_focus_accepts_text(fw);

    // QKeyEvent::matches() only accepts QKeySequence::StandardKey in Qt 6.
    // Check the custom Undo/Redo bindings explicitly instead. Text-editing
    // widgets keep their own local undo stack.
    const Qt::KeyboardModifiers shortcut_modifiers =
        ev->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier |
                           Qt::AltModifier | Qt::MetaModifier);
    if (!editing_value && ev->key() == Qt::Key_Z &&
        shortcut_modifiers == Qt::ControlModifier) {
        if (undo_index_ > 0) restore_undo_snapshot(undo_index_ - 1);
        ev->accept();
        return;
    }
    if (!editing_value &&
        ((ev->key() == Qt::Key_Z &&
          shortcut_modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) ||
         (ev->key() == Qt::Key_Y &&
          shortcut_modifiers == Qt::ControlModifier))) {
        if (undo_index_ + 1 < (int)undo_stack_.size())
            restore_undo_snapshot(undo_index_ + 1);
        ev->accept();
        return;
    }

    if (!editing_value && ev->key() == Qt::Key_Escape) {
        close();
        ev->accept();
        return;
    }
    if (!editing_value && timeline_ && timeline_->has_transition_target_selection()) {
        if (ev->matches(QKeySequence::Copy)) {
            timeline_->copy_transition_selection();
            ev->accept();
            return;
        }
        if (ev->matches(QKeySequence::Cut)) {
            timeline_->cut_transition_selection();
            ev->accept();
            return;
        }
        if (ev->matches(QKeySequence::Paste)) {
            timeline_->paste_transition_to_selection();
            ev->accept();
            return;
        }
        if (ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) {
            timeline_->delete_transition_selection();
            ev->accept();
            return;
        }
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
    if (!editing_value && ev->matches(QKeySequence::Paste)) {
        if (!layer_clipboard_.empty()) {
            paste_layer_from_clipboard();
            ev->accept();
            return;
        }
        if (paste_external_clipboard_to_canvas()) {
            ev->accept();
            return;
        }
    }
    if (!editing_value && ev->key() == Qt::Key_Delete && !sel_layer_id_.empty()) {
        delete_selected_layer();
        ev->accept();
        return;
    }
    if (ev->key() == Qt::Key_Space && shortcut_modifiers == Qt::NoModifier &&
        !ev->isAutoRepeat() && QApplication::mouseButtons() == Qt::NoButton) {
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
        begin_shutdown();
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
    dst.gradient_spread = src.gradient_spread;
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
    dst.outline_alignment = src.outline_alignment;
    dst.outline_antialias = src.outline_antialias;
    dst.scale_stroke_with_shape = src.scale_stroke_with_shape;
    dst.scale_corners_with_shape = src.scale_corners_with_shape;
    dst.stroke_gradient_type = src.stroke_gradient_type;
    dst.stroke_gradient_spread = src.stroke_gradient_spread;
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
    dst.background_padding_left = src.background_padding_left;
    dst.background_padding_right = src.background_padding_right;
    dst.background_padding_top = src.background_padding_top;
    dst.background_padding_bottom = src.background_padding_bottom;
    dst.background_corner_radius = src.background_corner_radius;
    dst.background_corner_radius_tl = src.background_corner_radius_tl;
    dst.background_corner_radius_tr = src.background_corner_radius_tr;
    dst.background_corner_radius_br = src.background_corner_radius_br;
    dst.background_corner_radius_bl = src.background_corner_radius_bl;
    dst.background_corner_type = src.background_corner_type;
    dst.background_fill_type = src.background_fill_type;
    dst.background_stroke_color = src.background_stroke_color;
    dst.background_stroke_width = src.background_stroke_width;
    dst.background_stroke_opacity = src.background_stroke_opacity;
    dst.background_stroke_fill_type = src.background_stroke_fill_type;
    dst.background_gradient_type = src.background_gradient_type;
    dst.background_gradient_spread = src.background_gradient_spread;
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

    // New-layer defaults should persist visual base styling only. Do not copy
    // the layer effect stack or mask/effect interaction flags into defaults.
    dst.effects.clear();
    dst.effect_stack_respects_masks = false;
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
    if (title_) {
        title_->editor_default_style_enabled = true;
        title_->editor_default_foreground_color = argb_from_color(default_foreground_color_);
        title_->editor_default_background_color = argb_from_color(default_background_color_);
    }
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
    if (title_ && title_->editor_default_style_enabled) {
        default_new_layer_style_ = title_->editor_default_layer_style;
        default_new_layer_style_.effects.clear();
        default_new_layer_style_.effect_stack_respects_masks = false;
        default_foreground_color_ = color_from_argb(title_->editor_default_foreground_color);
        default_background_color_ = color_from_argb(title_->editor_default_background_color);
        return;
    }

    if (g_editor_session_new_layer_defaults_initialized) {
        default_new_layer_style_ = g_editor_session_new_layer_style;
        default_new_layer_style_.effects.clear();
        default_new_layer_style_.effect_stack_respects_masks = false;
        return;
    }

    g_editor_session_new_layer_style = default_new_layer_style_;
    g_editor_session_new_layer_style.effects.clear();
    g_editor_session_new_layer_style.effect_stack_respects_masks = false;
    g_editor_session_new_layer_defaults_initialized = true;
}


void TitleEditor::save_new_layer_defaults() const
{
    Layer persisted = default_new_layer_style_;
    persisted.effects.clear();
    persisted.effect_stack_respects_masks = false;

    g_editor_session_new_layer_style = persisted;
    g_editor_session_new_layer_defaults_initialized = true;

    if (title_) {
        title_->editor_default_style_enabled = true;
        title_->editor_default_layer_style = persisted;
        title_->editor_default_foreground_color = argb_from_color(default_foreground_color_);
        title_->editor_default_background_color = argb_from_color(default_background_color_);
    }
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
        RichTextFill text_fill;
        bool text_fill_mixed = false;
        if (is_text_like) {
            const bool inline_selection = active_text_edit_layer_id_ == layer->id;
            const RichTextCharFormatSummary summary =
                summarize_rich_text_char_format(*layer, inline_selection, lt);
            text_fill = summary.valid ? summary.format.fill
                                      : layer_char_format_for_editor(*layer).fill;
            text_fill_mixed = summary.valid &&
                ((summary.mixed & RichTextCharFillColor) != 0);
        }
        const uint32_t fg = is_text_like ? text_fill.color : eval_fill_color(*layer, lt);
        const uint32_t bg = eval_outline_color(*layer, lt);
        // Show the actual active fill in the sidebar swatch for every layer type that
        // supports gradients, including Text / Clock / Ticker. Previously text-like
        // layers were forced to a solid color fallback, so their gradient state was
        // stored on the layer but never visible in the sidebar swatch.
        const int displayed_fill_type = is_text_like ? text_fill.type : layer->fill_type;
        if (displayed_fill_type == 1) {
            const uint32_t start = is_text_like ? text_fill.gradient_start_color
                                                : layer->gradient_start_color;
            const uint32_t end = is_text_like ? text_fill.gradient_end_color
                                              : layer->gradient_end_color;
            const int gradient_type = is_text_like ? text_fill.gradient_type
                                                   : layer->gradient_type;
            tools_sidebar_->set_foreground_gradient(color_from_argb(start),
                                                    color_from_argb(end),
                                                    gradient_type);
        } else {
            tools_sidebar_->set_foreground_color(color_from_argb(fg));
        }
        tools_sidebar_->set_foreground_mixed(text_fill_mixed);
        if (layer->stroke_fill_type == 2) {
            tools_sidebar_->set_background_gradient(color_from_argb(layer->stroke_gradient_start_color),
                                                    color_from_argb(layer->stroke_gradient_end_color),
                                                    layer->stroke_gradient_type);
        } else {
            tools_sidebar_->set_background_color(color_from_argb(bg));
        }
        tools_sidebar_->set_background_mixed(false);
    } else {
        if (default_new_layer_style_.fill_type == 1) {
            tools_sidebar_->set_foreground_gradient(color_from_argb(default_new_layer_style_.gradient_start_color),
                                                    color_from_argb(default_new_layer_style_.gradient_end_color),
                                                    default_new_layer_style_.gradient_type);
        } else {
            tools_sidebar_->set_foreground_color(default_foreground_color_);
        }
        tools_sidebar_->set_foreground_mixed(false);
        if (default_new_layer_style_.stroke_fill_type == 2) {
            tools_sidebar_->set_background_gradient(color_from_argb(default_new_layer_style_.stroke_gradient_start_color),
                                                    color_from_argb(default_new_layer_style_.stroke_gradient_end_color),
                                                    default_new_layer_style_.stroke_gradient_type);
        } else {
            tools_sidebar_->set_background_color(default_background_color_);
        }
        tools_sidebar_->set_background_mixed(false);
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
    auto *label = new QLabel(foreground ? bgl_tr("OBSTitles.DefaultForegroundColor")
                                         : bgl_tr("OBSTitles.DefaultBackgroundColor"), color_tab);
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
        QColor picked = bgl_pick_color(target, &popup,
            foreground ? bgl_tr("OBSTitles.ForegroundColor") : bgl_tr("OBSTitles.BackgroundColor"));
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
    tabs->addTab(color_tab, bgl_tr("OBSTitles.Color"));

    auto *gradient_tab = new QWidget(tabs);
    auto *gradient_layout = new QVBoxLayout(gradient_tab);
    auto *gradient_label = new QLabel(bgl_tr("OBSTitles.GradientDefaultsHint"), gradient_tab);
    gradient_label->setWordWrap(true);
    gradient_layout->addWidget(gradient_label);
    gradient_layout->addStretch(1);
    tabs->addTab(gradient_tab, bgl_tr("OBSTitles.Gradient"));

    auto *swatches_tab = new QWidget(tabs);
    auto *swatches_layout = new QVBoxLayout(swatches_tab);
    auto *swatches_label = new QLabel(bgl_tr("OBSTitles.SavedDefaultsHint"), swatches_tab);
    swatches_label->setWordWrap(true);
    swatches_layout->addWidget(swatches_label);
    swatches_layout->addStretch(1);
    tabs->addTab(swatches_tab, bgl_tr("OBSTitles.Swatches"));

    update_controls();
    popup.adjustSize();
    const QPoint cursor_pos = QCursor::pos();
    const QPoint desired_pos(cursor_pos.x() + 14, cursor_pos.y() - popup.height() / 2);
    popup.move(clamp_popup_position_to_screen(desired_pos, popup.size(), this));
    popup.exec();
}

void TitleEditor::apply_effect_preset_to_layer(const QString &file_path, const std::string &layer_id)
{
    if (!title_ || !effects_panel_ || layer_id.empty())
        return;

    const auto layer = title_->find_layer(layer_id);
    if (!layer || layer->locked)
        return;

    on_layer_selected(layer_id);
    effects_panel_->add_effect_from_preset_file(file_path);
}

void TitleEditor::apply_transition_preset_to_layer(const QString &file_path,
                                                   const std::string &layer_id,
                                                   int edge_value)
{
    if (!title_ || layer_id.empty())
        return;

    auto layer = title_->find_layer(layer_id);
    if (!layer || layer->locked)
        return;

    bgs::transitions::TransitionPresetDescriptor descriptor;
    QString error;
    if (!bgs::transitions::load_transition_preset_file(file_path, &descriptor, &error)) {
        QMessageBox::warning(this, bgl_tr("OBSTitles.Transitions"), error);
        return;
    }

    const bool text_layer = layer->type == LayerType::Text ||
                            layer->type == LayerType::Clock ||
                            layer->type == LayerType::Ticker;
    if (descriptor.transition.kind == LayerTransitionKind::Text && !text_layer) {
        QMessageBox::information(this, bgl_tr("OBSTitles.Transitions"),
                                 bgl_tr("OBSTitles.TextTransitionRequiresText"));
        return;
    }

    const LayerTransitionEdge edge = edge_value == static_cast<int>(LayerTransitionEdge::Out)
        ? LayerTransitionEdge::Out : LayerTransitionEdge::In;
    LayerTransition transition = descriptor.transition;
    transition.id = TitleDataStore::make_uuid();
    transition.edge = edge;

    const double frame = obs_frame_duration();
    const double layer_duration = std::max(frame, layer->out_time - layer->in_time);
    const LayerTransition *other = find_layer_transition(
        layer->transitions,
        edge == LayerTransitionEdge::In ? LayerTransitionEdge::Out : LayerTransitionEdge::In);
    const double maximum_duration = std::max(frame, layer_duration - (other ? other->duration : 0.0));
    transition.duration = std::clamp(transition.duration, frame, maximum_duration);

    if (LayerTransition *existing = find_layer_transition(layer->transitions, edge))
        *existing = transition;
    else
        layer->transitions.push_back(std::move(transition));

    on_layer_selected(layer_id);
    force_next_title_visual_update();
    on_title_modified();
    if (timeline_) timeline_->update();
}

void TitleEditor::edit_layer_transition(const std::string &layer_id, int edge_value)
{
    if (!title_ || layer_id.empty())
        return;
    auto layer = title_->find_layer(layer_id);
    if (!layer || layer->locked)
        return;

    const LayerTransitionEdge edge = edge_value == static_cast<int>(LayerTransitionEdge::Out)
        ? LayerTransitionEdge::Out : LayerTransitionEdge::In;
    LayerTransition *transition = find_layer_transition(layer->transitions, edge);
    if (!transition)
        return;

    const double frame = obs_frame_duration();
    const double layer_duration = std::max(frame, layer->out_time - layer->in_time);
    const LayerTransition *other = find_layer_transition(
        layer->transitions,
        edge == LayerTransitionEdge::In ? LayerTransitionEdge::Out : LayerTransitionEdge::In);
    const double maximum_duration = std::max(frame, layer_duration - (other ? other->duration : 0.0));

    TransitionEditorDialog dialog(*transition, maximum_duration, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    LayerTransition edited = dialog.transition();
    edited.id = transition->id;
    edited.preset_id = transition->preset_id;
    edited.edge = edge;
    edited.duration = std::clamp(edited.duration, frame, maximum_duration);
    *transition = std::move(edited);

    on_layer_selected(layer_id);
    force_next_title_visual_update();
    on_title_modified();
    if (timeline_) timeline_->update();
}

void TitleEditor::update_layer_panels(std::shared_ptr<Layer> layer, double playhead)
{
    QScopedValueRollback<bool> panel_update_guard(updating_layer_panels_, true);
    if (layer_props_dock_) {
        layer_props_dock_->setWindowTitle(layer
            ? bgl_tr("OBSTitles.PropertiesNamed").arg(QString::fromStdString(layer->name))
            : bgl_tr("OBSTitles.PropertiesNoSelection"));
    }
    if (props_) props_->set_layer(layer, playhead);
    update_sidebar_color_swatches(layer);
    if (effects_panel_) effects_panel_->set_layer(layer, playhead);
    update_corner_toolbar();
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
    /* Timeline drags, jog/step actions and every other externally requested
     * playhead move are editor/scrub updates. They remain monitor-capped even
     * if the transport happened to be armed when the interaction began. */
    apply_playhead_change(t, false);
}

void TitleEditor::apply_playhead_change(double t, bool playback_frame)
{
    if (shutting_down_)
        return;
    t = title_ ? std::clamp(snap_to_obs_frame(t), 0.0, title_->duration) : snap_to_obs_frame(t);
    if (std::abs(t - playhead_) < 1e-9) {
        if (canvas_)
            canvas_->set_playhead(t, playback_frame);
        return;
    }
    playhead_ = t;
    canvas_->set_playhead(t, playback_frame);
    timeline_->set_playhead(t);
    if (prerender_panel_) prerender_panel_->setPlayhead(t);
    if (!cache_reprioritize_clock_.isValid() || cache_reprioritize_clock_.elapsed() >= 80) {
        CacheManager::instance().reprioritize(title_, t);
        cache_reprioritize_clock_.restart();
    }

    if (!sel_layer_id_.empty() && title_) {
        auto l = title_->find_layer(sel_layer_id_);
        if (l) {
            QScopedValueRollback<bool> panel_update_guard(updating_layer_panels_, true);
            if (props_)
                props_->update_playhead(t);
            if (effects_panel_)
                effects_panel_->update_playhead(t);
            update_sidebar_color_swatches(l);
        }
    }

    if (time_lbl_)
        time_lbl_->setText(bgl_tr("OBSTitles.TimeFpsFormat").arg(format_timecode(t)).arg(obs_frame_rate(), 0, 'f', 2));
}

void TitleEditor::on_title_modified(bool push_undo)
{
    if (shutting_down_)
        return;
    const bool force_visual_update = force_next_visual_update_;
    force_next_visual_update_ = false;
    /* Several Qt controls emit value-changed signals while a newly selected
     * layer is being reflected into the properties/effects panels. Selection
     * itself is UI state and must never dirty, save, invalidate, or rerender
     * the title. Compare against the last rendered/restored visual identity
     * before entering the modification pipeline. Genuine edits have already
     * changed the model here and therefore continue normally. */
    if (!force_visual_update && title_ && CacheManager::instance().visualStateCurrent(*title_)) {
        BGL_LOG_TRACE("Editor", QStringLiteral("Ignored selection-only property notification title=%1")
                                   .arg(QString::fromStdString(title_->id)));
        return;
    }
    if (title_) set_dirty(true);
    schedule_cache_invalidation();
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

void TitleEditor::force_next_title_visual_update()
{
    force_next_visual_update_ = true;
}

void TitleEditor::schedule_cache_invalidation()
{
    if (!title_)
        return;
    CacheManager::instance().setInteractiveBypass(true);
    if (cache_invalidation_timer_)
        cache_invalidation_timer_->start();
}

/* ══════════════════════════════════════════════════════════════════
 *  CanvasPreview
 * ══════════════════════════════════════════════════════════════════ */
