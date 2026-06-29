    connect(canvas_, &CanvasPreview::layer_clicked,
            this, [this](const std::string &id) {
                if (timeline_) timeline_->clear_transition_target_selection();
                on_layer_selected(id);
            });
    connect(canvas_, &CanvasPreview::layers_selected,
            this, [this](const std::vector<std::string> &ids) {
                sel_layer_id_ = ids.size() == 1 ? ids.back() : std::string();
                layers_->set_selected_layers(ids);
                canvas_->set_selected_layers(ids);
                timeline_->clear_transition_target_selection();
                timeline_->set_selected_layers(ids);
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
    connect(canvas_, &CanvasPreview::corner_context_changed,
            this, &TitleEditor::update_corner_toolbar);
    connect(canvas_, &CanvasPreview::text_edit_changed,
            this, [this](const std::string &layer_id) {
                if (!title_) return;
                active_text_edit_layer_id_ = layer_id;
                if (props_) props_->set_active_text_edit_layer(layer_id);
                /* CanvasPreview has already marked and scheduled the edited
                 * GPU frame. Avoid on_title_modified() here: it resets the
                 * Timeline/title panels, repositions the inline editor and
                 * serializes the whole title for every character. Cache
                 * invalidation, panel reflection and live publication are
                 * independently coalesced below. */
                set_dirty(true);
                schedule_cache_invalidation();
                pending_inline_text_refresh_layer_id_ = layer_id;
                if (inline_text_panel_refresh_timer_)
                    inline_text_panel_refresh_timer_->start();
                if (live_editing_ && inline_text_live_publish_timer_)
                    inline_text_live_publish_timer_->start();
            });
    connect(canvas_, &CanvasPreview::text_edit_cursor_changed,
            this, [this](const std::string &layer_id) {
                if (!title_) return;
                active_text_edit_layer_id_ = layer_id;
                if (props_) props_->set_active_text_edit_layer(layer_id);
                if (pending_inline_text_refresh_layer_id_ == layer_id) {
                    pending_inline_text_refresh_layer_id_.clear();
                    if (inline_text_panel_refresh_timer_)
                        inline_text_panel_refresh_timer_->stop();
                }
                if (auto layer = title_->find_layer(layer_id))
                    update_layer_panels(layer, playhead_);
            });
    connect(canvas_, &CanvasPreview::text_edit_committed,
            this, [this](const std::string &layer_id) {
                if (!title_) return;
                if (inline_text_panel_refresh_timer_)
                    inline_text_panel_refresh_timer_->stop();
                if (inline_text_live_publish_timer_)
                    inline_text_live_publish_timer_->stop();
                pending_inline_text_refresh_layer_id_.clear();
                active_text_edit_layer_id_.clear();
                if (props_) props_->set_active_text_edit_layer(std::string());
                bool renamed_layer = false;
                if (auto layer = title_->find_layer(layer_id)) {
                    if (layer->type == LayerType::Text &&
                        pending_text_layer_auto_names_.find(layer_id) != pending_text_layer_auto_names_.end()) {
                        const QString text = !layer->rich_text.empty()
                            ? QString::fromStdString(layer->rich_text.plain_text)
                            : QString::fromStdString(layer->text_content);
                        if (text_has_visible_name_content(text)) {
                            const std::string next_name = unique_layer_name(text.simplified().toStdString(), {layer_id});
                            if (layer->name != next_name) {
                                layer->name = next_name;
                                renamed_layer = true;
                            }
                        }
                        pending_text_layer_auto_names_.erase(layer_id);
                    }
                }
                on_title_modified();
                if (renamed_layer && layers_)
                    layers_->refresh();
                if (auto layer = title_->find_layer(layer_id))
                    update_layer_panels(layer, playhead_);
            });
    connect(canvas_, &CanvasPreview::color_picker_previewed,
            this, [this](const QColor &color) {
                if (!color.isValid())
                    return;
                if (props_ && props_->apply_external_picked_color(color, false))
                    return;
                apply_picked_color_to_selection(color, false);
            });
    connect(canvas_, &CanvasPreview::color_picked,
            this, [this](const QColor &color, bool foreground) {
                if (!color.isValid())
                    return;
                if (foreground) {
                    default_foreground_color_ = color;
                    if (!(props_ && props_->apply_external_picked_color(color, true)))
                        apply_picked_color_to_selection(color, true);
                } else {
                    default_background_color_ = color;
                }
                remember_recent_color(color);
                save_sidebar_default_colors();
                update_sidebar_color_swatches(nullptr);
                if (tools_sidebar_)
                    tools_sidebar_->activate_selection_tool();
                else if (canvas_) {
                    current_editor_tool_ = EditorTool::Selection;
                    canvas_->set_selection_tool_active();
                }
            });
    connect(canvas_, &CanvasPreview::layer_structure_changed,
            this, [this]() {
                layers_->refresh();
                timeline_->set_title(title_);
            });
    connect(canvas_, &CanvasPreview::duplicate_layers_requested,
            this, &TitleEditor::duplicate_selected_layers);
    connect(canvas_, &CanvasPreview::copy_layers_requested,
            this, &TitleEditor::copy_selected_layer);
    connect(canvas_, &CanvasPreview::cut_layers_requested,
            this, &TitleEditor::cut_selected_layer);
    connect(canvas_, &CanvasPreview::paste_layers_requested,
            this, [this]() {
                if (!layer_clipboard_.empty())
                    paste_layer_from_clipboard();
                else
                    paste_external_clipboard_to_canvas();
            });
    connect(canvas_, &CanvasPreview::delete_layers_requested,
            this, &TitleEditor::delete_selected_layer);
    connect(canvas_, &CanvasPreview::group_layers_requested,
            this, &TitleEditor::group_selected_layers);
    connect(canvas_, &CanvasPreview::ungroup_layers_requested,
            this, &TitleEditor::ungroup_selected_layers);
    connect(canvas_, &CanvasPreview::add_layers_to_group_requested,
            this, &TitleEditor::add_selected_layers_to_group);
    connect(canvas_, &CanvasPreview::remove_layers_from_group_requested,
            this, &TitleEditor::remove_selected_layers_from_group);
    connect(canvas_, &CanvasPreview::flip_layers_requested,
            this, &TitleEditor::flip_selected_layers);
    connect(canvas_, &CanvasPreview::rotate_layers_requested,
            this, &TitleEditor::rotate_selected_layers);
    connect(canvas_, &CanvasPreview::layer_order_requested,
            this, &TitleEditor::reorder_selected_layers);
    connect(canvas_, &CanvasPreview::set_layers_locked_requested,
            this, [this](bool locked) {
                if (!title_) return;
                bool changed = false;
                for (const auto &id : selected_layer_ids_for_operation()) {
                    if (auto layer = title_->find_layer(id);
                        layer && layer->locked != locked) {
                        layer->locked = locked;
                        changed = true;
                    }
                }
                if (!changed) return;
                layers_->refresh();
                canvas_->refresh_preview();
                on_title_modified();
            });
    connect(canvas_, &CanvasPreview::set_layers_visible_requested,
            this, [this](bool visible) {
                if (!title_) return;
                bool changed = false;
                for (const auto &id : selected_layer_ids_for_operation()) {
                    if (auto layer = title_->find_layer(id);
                        layer && layer->visible != visible) {
                        layer->visible = visible;
                        changed = true;
                    }
                }
                if (!changed) return;
                layers_->refresh();
                canvas_->refresh_preview();
                timeline_->set_title(title_);
                on_title_modified();
            });
    connect(canvas_, &CanvasPreview::shape_drawing_started,
            this, &TitleEditor::create_shape_layer_from_canvas);
    connect(canvas_, &CanvasPreview::text_drawing_started,
            this, &TitleEditor::create_text_layer_from_canvas);
    connect(canvas_, &CanvasPreview::image_drawing_started,
            this, &TitleEditor::create_image_layer_from_canvas);
    connect(canvas_, &CanvasPreview::external_image_layer_requested,
            this, &TitleEditor::create_image_layer_from_external_source);
    connect(canvas_, &CanvasPreview::external_text_layer_requested,
            this, &TitleEditor::create_text_layer_from_external_source);
    connect(canvas_, &CanvasPreview::asset_layer_requested,
            this, [this](const QString &asset_id, const QPointF &canvas_pt) {
                insert_asset_layer(asset_id.toStdString(), canvas_pt);
            });
    connect(canvas_, &CanvasPreview::edit_asset_requested,
            this, [this](const QString &asset_id) {
                edit_asset(asset_id.toStdString());
            });
    connect(canvas_, &CanvasPreview::effect_preset_dropped,
            this, &TitleEditor::apply_effect_preset_to_layer);
    connect(canvas_, &CanvasPreview::shape_drawing_changed,
            this, &TitleEditor::update_canvas_created_shape);
    connect(canvas_, &CanvasPreview::shape_drawing_finished,
            this, &TitleEditor::finish_canvas_created_shape);
    connect(canvas_, &CanvasPreview::pen_path_finished,
            this, &TitleEditor::create_pen_path_layer_from_canvas);
    if (tools_sidebar_) {
        connect(tools_sidebar_, &ToolsSidebar::selection_tool_requested, this, [this]() {
            current_editor_tool_ = EditorTool::Selection;
            if (canvas_) canvas_->set_selection_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::direct_selection_tool_requested, this, [this]() {
            current_editor_tool_ = EditorTool::DirectSelection;
            if (canvas_) canvas_->set_direct_selection_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::free_transform_tool_requested, this, [this](int mode) {
            current_editor_tool_ = EditorTool::FreeTransform;
            if (canvas_) canvas_->set_free_transform_tool_active(mode);
        });
        connect(tools_sidebar_, &ToolsSidebar::shape_tool_requested, this, [this](ShapeType shape_type) {
            current_editor_tool_ = EditorTool::Shape;
            if (tools_sidebar_) tools_sidebar_->set_selected_shape(shape_type);
            if (canvas_) canvas_->set_shape_tool_active(shape_type);
        });
        connect(tools_sidebar_, &ToolsSidebar::pen_tool_requested, this, [this]() {
            current_editor_tool_ = EditorTool::Pen;
            if (canvas_) canvas_->set_pen_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::text_tool_requested, this, [this](LayerType type) {
            current_editor_tool_ = EditorTool::Text;
            if (tools_sidebar_) tools_sidebar_->set_selected_text_layer_type(type);
            if (canvas_) canvas_->set_text_tool_active(type);
        });
        connect(tools_sidebar_, &ToolsSidebar::image_tool_requested, this, [this]() {
            current_editor_tool_ = EditorTool::Image;
            if (canvas_) canvas_->set_image_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::color_picker_tool_requested, this, [this]() {
            current_editor_tool_ = EditorTool::ColorPicker;
            reopen_color_tab_after_canvas_pick_ = false;
            if (canvas_) canvas_->set_color_picker_tool_active();
        });
        connect(tools_sidebar_, &ToolsSidebar::gradient_tool_requested, this, [this]() {
            current_editor_tool_ = EditorTool::Gradient;
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
        set_animated_x(layer->position, lt, x);
        set_animated_y(layer->position, lt, y);
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
        Vec2Value scale = layer->scale.evaluate(lt);
        if (horizontal)
            scale.x = -scale.x;
        else
            scale.y = -scale.y;
        set_animated_value(layer->scale, lt, scale);
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

void TitleEditor::reorder_selected_layers(int action)
{
    if (!title_)
        return;
    const auto requested_ids = selected_layer_ids_for_operation();
    if (!editor_reorder_selected_group_siblings(title_, requested_ids, action))
        return;

    layers_->refresh();
    layers_->set_selected_layers(requested_ids);
    canvas_->set_selected_layers(requested_ids);
    timeline_->set_title(title_);
    timeline_->set_selected_layers(requested_ids);
    force_next_title_visual_update();
    on_title_modified();
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
    if (!title_) return;

    // Multi-selection clears sel_layer_id_. Use the common operation selection
    // path so alignment works consistently from both the canvas and layer list.
    const auto ids = selected_layer_ids_for_operation();
    if (ids.empty()) return;

    struct Entry {
        std::shared_ptr<Layer> layer;
        double lt = 0.0;
        double anchor_x = 0.0;
        double anchor_y = 0.0;
        double left_offset = 0.0;
        double right_offset = 0.0;
        double top_offset = 0.0;
        double bottom_offset = 0.0;
    };

    std::vector<Entry> entries;
    entries.reserve(ids.size());

    double min_left = std::numeric_limits<double>::infinity();
    double max_right = -std::numeric_limits<double>::infinity();
    double min_top = std::numeric_limits<double>::infinity();
    double max_bottom = -std::numeric_limits<double>::infinity();
    double min_anchor_x = std::numeric_limits<double>::infinity();
    double max_anchor_x = -std::numeric_limits<double>::infinity();
    double min_anchor_y = std::numeric_limits<double>::infinity();
    double max_anchor_y = -std::numeric_limits<double>::infinity();

    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;

        const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                     std::max(0.0, layer->out_time - layer->in_time));
        const double width = eval_box_width(*layer, lt);
        const double height = eval_box_height(*layer, lt);
        const Vec2Value scale = layer->scale.evaluate(lt);
        const Vec2Value position = layer->position.evaluate(lt);
        const double origin_x = eval_origin_x(*layer, lt);
        const double origin_y = eval_origin_y(*layer, lt);
        const double rotation = layer->rotation.evaluate(lt) * 3.14159265358979323846 / 180.0;
        const double cos_r = std::cos(rotation);
        const double sin_r = std::sin(rotation);

        const double local_left = -origin_x * width;
        const double local_right = (1.0 - origin_x) * width;
        const double local_top = -origin_y * height;
        const double local_bottom = (1.0 - origin_y) * height;
        const double xs[4] = {local_left, local_right, local_right, local_left};
        const double ys[4] = {local_top, local_top, local_bottom, local_bottom};

        double left_offset = std::numeric_limits<double>::infinity();
        double right_offset = -std::numeric_limits<double>::infinity();
        double top_offset = std::numeric_limits<double>::infinity();
        double bottom_offset = -std::numeric_limits<double>::infinity();
        for (int i = 0; i < 4; ++i) {
            const double sx = xs[i] * scale.x;
            const double sy = ys[i] * scale.y;
            const double tx = sx * cos_r - sy * sin_r;
            const double ty = sx * sin_r + sy * cos_r;
            left_offset = std::min(left_offset, tx);
            right_offset = std::max(right_offset, tx);
            top_offset = std::min(top_offset, ty);
            bottom_offset = std::max(bottom_offset, ty);
        }

        min_left = std::min(min_left, position.x + left_offset);
        max_right = std::max(max_right, position.x + right_offset);
        min_top = std::min(min_top, position.y + top_offset);
        max_bottom = std::max(max_bottom, position.y + bottom_offset);
        min_anchor_x = std::min(min_anchor_x, position.x);
        max_anchor_x = std::max(max_anchor_x, position.x);
        min_anchor_y = std::min(min_anchor_y, position.y);
        max_anchor_y = std::max(max_anchor_y, position.y);

        entries.push_back({layer, lt, position.x, position.y,
                           left_offset, right_offset, top_offset, bottom_offset});
    }

    if (entries.empty()) return;
    const bool align_to_selection = alignment_target_ == 0 || alignment_target_ == 4;
    const bool selection_anchors = alignment_target_ == 4;
    if (align_to_selection && entries.size() < 2) return;

    double target_left = selection_anchors ? min_anchor_x : min_left;
    double target_hcenter = selection_anchors ? (min_anchor_x + max_anchor_x) / 2.0
                                              : (min_left + max_right) / 2.0;
    double target_right = selection_anchors ? max_anchor_x : max_right;
    double target_top = selection_anchors ? min_anchor_y : min_top;
    double target_vcenter = selection_anchors ? (min_anchor_y + max_anchor_y) / 2.0
                                              : (min_top + max_bottom) / 2.0;
    double target_bottom = selection_anchors ? max_anchor_y : max_bottom;

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
            double next_x = entry.anchor_x;
            if (selection_anchors) {
                if (x_mode == 0) next_x = target_left;
                if (x_mode == 1) next_x = target_hcenter;
                if (x_mode == 2) next_x = target_right;
            } else {
                if (x_mode == 0) next_x = target_left - entry.left_offset;
                if (x_mode == 1) next_x = target_hcenter - (entry.left_offset + entry.right_offset) / 2.0;
                if (x_mode == 2) next_x = target_right - entry.right_offset;
            }
            set_animated_x(entry.layer->position, entry.lt, next_x);
        }
        if (y_mode >= 0) {
            double next_y = entry.anchor_y;
            if (selection_anchors) {
                if (y_mode == 0) next_y = target_top;
                if (y_mode == 1) next_y = target_vcenter;
                if (y_mode == 2) next_y = target_bottom;
            } else {
                if (y_mode == 0) next_y = target_top - entry.top_offset;
                if (y_mode == 1) next_y = target_vcenter - (entry.top_offset + entry.bottom_offset) / 2.0;
                if (y_mode == 2) next_y = target_bottom - entry.bottom_offset;
            }
            set_animated_y(entry.layer->position, entry.lt, next_y);
        }
        last_layer = entry.layer;
    }

    if (!last_layer) return;
    on_title_modified();
    update_layer_panels(last_layer, playhead_);
    if (canvas_) canvas_->update();
    if (timeline_) timeline_->update();
}

void TitleEditor::distribute_selected_layers(bool horizontal)
{
    if (!title_) return;

    // Multi-selection intentionally clears sel_layer_id_. Always use the shared
    // operation-selection helper so canvas/layer-list multi-selection works.
    const auto ids = selected_layer_ids_for_operation();
    if (ids.size() < 3) return;

    struct Entry {
        std::shared_ptr<Layer> layer;
        double lt = 0.0;
        double anchor = 0.0;
        double start = 0.0;
        double end = 0.0;
        double start_offset = 0.0;
    };

    std::vector<Entry> entries;
    entries.reserve(ids.size());
    for (const auto &id : ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked) continue;

        const double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                     std::max(0.0, layer->out_time - layer->in_time));
        const double width = eval_box_width(*layer, lt);
        const double height = eval_box_height(*layer, lt);
        const Vec2Value scale = layer->scale.evaluate(lt);
        const Vec2Value position = layer->position.evaluate(lt);
        const double origin_x = eval_origin_x(*layer, lt);
        const double origin_y = eval_origin_y(*layer, lt);
        const double rotation_radians = layer->rotation.evaluate(lt) * 3.14159265358979323846 / 180.0;
        const double cos_r = std::cos(rotation_radians);
        const double sin_r = std::sin(rotation_radians);

        // Build the axis-aligned bounds from this layer's own transformed
        // corners. Do not use the aggregate selection bounds as an item in
        // the distribution calculation.
        const double local_left = -origin_x * width;
        const double local_right = (1.0 - origin_x) * width;
        const double local_top = -origin_y * height;
        const double local_bottom = (1.0 - origin_y) * height;
        const double xs[4] = {local_left, local_right, local_right, local_left};
        const double ys[4] = {local_top, local_top, local_bottom, local_bottom};
        double start_offset = std::numeric_limits<double>::max();
        double end_offset = std::numeric_limits<double>::lowest();
        for (int corner = 0; corner < 4; ++corner) {
            const double sx = xs[corner] * scale.x;
            const double sy = ys[corner] * scale.y;
            const double axis_offset = horizontal
                ? (sx * cos_r - sy * sin_r)
                : (sx * sin_r + sy * cos_r);
            start_offset = std::min(start_offset, axis_offset);
            end_offset = std::max(end_offset, axis_offset);
        }
        const double axis_position = horizontal ? position.x : position.y;

        entries.push_back({layer, lt, axis_position, axis_position + start_offset,
                           axis_position + end_offset, start_offset});
    }

    if (entries.size() < 3) return;

    if (distribute_to_anchors_) {
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            return a.anchor < b.anchor;
        });

        const double step = (entries.back().anchor - entries.front().anchor) /
                            static_cast<double>(entries.size() - 1);
        for (size_t i = 1; i + 1 < entries.size(); ++i) {
            const double next_position = entries.front().anchor + step * static_cast<double>(i);
            if (horizontal)
                set_animated_x(entries[i].layer->position, entries[i].lt, next_position);
            else
                set_animated_y(entries[i].layer->position, entries[i].lt, next_position);
        }
    } else {
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
            if (a.start != b.start) return a.start < b.start;
            return a.end < b.end;
        });

        // Use only each object's individual bounds. The usable interval is
        // the space between the trailing bound of the first object and the
        // leading bound of the last object; the aggregate selection bound is
        // never treated as an additional bound.
        double intermediate_extent = 0.0;
        for (size_t i = 1; i + 1 < entries.size(); ++i)
            intermediate_extent += entries[i].end - entries[i].start;
        const double available_between_outer_objects =
            entries.back().start - entries.front().end;
        const double gap = (available_between_outer_objects - intermediate_extent) /
                           static_cast<double>(entries.size() - 1);

        double next_start = entries.front().end + gap;
        for (size_t i = 1; i + 1 < entries.size(); ++i) {
            const double next_position = next_start - entries[i].start_offset;
            if (horizontal)
                set_animated_x(entries[i].layer->position, entries[i].lt, next_position);
            else
                set_animated_y(entries[i].layer->position, entries[i].lt, next_position);
            next_start += (entries[i].end - entries[i].start) + gap;
        }
    }

    on_title_modified();
    update_layer_panels(entries.back().layer, playhead_);
    if (canvas_) canvas_->update();
    if (timeline_) timeline_->update();
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

    act_go_start_ = new QAction(obs_icon("go-to-start.svg"), bgl_tr("OBSTitles.GoToStart"), this);
    act_prev_kf_ = new QAction(obs_icon("previous-keyframe.svg"), bgl_tr("OBSTitles.PreviousKeyframe"), this);
    act_step_back_ = new QAction(obs_icon("step-backward.svg"), bgl_tr("OBSTitles.StepBackward"), this);
    act_rew_ = new QAction(obs_icon("play-reverse.svg"), bgl_tr("OBSTitles.PlayReverse"), this);
    act_play_ = new QAction(obs_icon("play.svg"), bgl_tr("OBSTitles.Play"), this);
    act_play_->setToolTip(bgl_tr("OBSTitles.PlayTooltip"));
    act_go_end_ = new QAction(obs_icon("go-to-end.svg"), bgl_tr("OBSTitles.GoToEnd"), this);
    act_next_kf_ = new QAction(obs_icon("next-keyframe.svg"), bgl_tr("OBSTitles.NextKeyframe"), this);

    connect(act_go_start_, &QAction::triggered, this, &TitleEditor::go_to_start);
    connect(act_prev_kf_, &QAction::triggered, this, &TitleEditor::previous_keyframe);
    connect(act_step_back_, &QAction::triggered, this, &TitleEditor::step_backward);
    connect(act_rew_, &QAction::triggered, this, &TitleEditor::reverse_play);
    connect(act_play_, &QAction::triggered, this, &TitleEditor::play_pause);
    connect(act_go_end_, &QAction::triggered, this, &TitleEditor::go_to_end);
    connect(act_next_kf_, &QAction::triggered, this, &TitleEditor::next_keyframe);

    toolbar_->addSeparator();
    auto *align_target = new QToolButton(toolbar_);
    align_target->setIcon(obs_icon("alignment-target.svg"));
    align_target->setIconSize(toolbar_->iconSize());
    align_target->setToolButtonStyle(Qt::ToolButtonIconOnly);
    align_target->setToolTip(bgl_tr("OBSTitles.AlignmentTarget"));
    align_target->setAccessibleName(bgl_tr("OBSTitles.AlignmentTarget"));
    align_target->setPopupMode(QToolButton::InstantPopup);
    align_target->setFixedSize(kEditorToolbarButtonExtent, kEditorToolbarButtonExtent);
    align_target->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;}"));
    auto *align_menu = new QMenu(align_target);
    QAction *target_selection_bounds = align_menu->addAction(bgl_tr("OBSTitles.AlignToSelectionBounds"));
    QAction *target_selection_anchors = align_menu->addAction(bgl_tr("OBSTitles.AlignToSelectionAnchors"));
    QAction *target_title_safe = align_menu->addAction(bgl_tr("OBSTitles.AlignToTitleSafeGuides"));
    QAction *target_action_safe = align_menu->addAction(bgl_tr("OBSTitles.AlignToActionSafeGuides"));
    QAction *target_artboard = align_menu->addAction(bgl_tr("OBSTitles.AlignToArtboard"));
    target_selection_bounds->setCheckable(true);
    target_selection_anchors->setCheckable(true);
    target_title_safe->setCheckable(true);
    target_action_safe->setCheckable(true);
    target_artboard->setCheckable(true);
    target_artboard->setChecked(true);
    auto update_alignment_target = [this, align_target, target_selection_bounds, target_selection_anchors, target_title_safe, target_action_safe, target_artboard](int target) {
        alignment_target_ = target;
        target_selection_bounds->setChecked(target == 0);
        target_selection_anchors->setChecked(target == 4);
        target_title_safe->setChecked(target == 1);
        target_action_safe->setChecked(target == 2);
        target_artboard->setChecked(target == 3);
        QString tooltip = bgl_tr("OBSTitles.AlignToArtboard");
        if (target == 0)
            tooltip = bgl_tr("OBSTitles.AlignToSelectionBounds");
        else if (target == 4)
            tooltip = bgl_tr("OBSTitles.AlignToSelectionAnchors");
        else if (target == 1)
            tooltip = bgl_tr("OBSTitles.AlignToTitleSafeGuides");
        else if (target == 2)
            tooltip = bgl_tr("OBSTitles.AlignToActionSafeGuides");
        align_target->setToolTip(tooltip);
    };
    connect(target_selection_bounds, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(0); });
    connect(target_selection_anchors, &QAction::triggered, this, [update_alignment_target]() { update_alignment_target(4); });
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
    add_align_action("align-left.svg", bgl_tr("OBSTitles.AlignLeft"), 0, -1);
    add_align_action("align-horizontal-center.svg", bgl_tr("OBSTitles.AlignHorizontalCenter"), 1, -1);
    add_align_action("align-right.svg", bgl_tr("OBSTitles.AlignRight"), 2, -1);
    add_align_action("align-top.svg", bgl_tr("OBSTitles.AlignTop"), -1, 0);
    add_align_action("align-vertical-center.svg", bgl_tr("OBSTitles.AlignVerticalCenter"), -1, 1);
    add_align_action("align-bottom.svg", bgl_tr("OBSTitles.AlignBottom"), -1, 2);
    add_align_action("align-center-artboard.svg", bgl_tr("OBSTitles.AlignCenterToArtboard"), 1, 1);

    auto *distribute_mode = new QToolButton(toolbar_);
    distribute_mode->setIcon(obs_icon("distribute-mode.svg"));
    distribute_mode->setIconSize(toolbar_->iconSize());
    distribute_mode->setToolButtonStyle(Qt::ToolButtonIconOnly);
    distribute_mode->setPopupMode(QToolButton::InstantPopup);
    distribute_mode->setFixedSize(kEditorToolbarButtonExtent, kEditorToolbarButtonExtent);
    distribute_mode->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;}"));

    auto *distribute_menu = new QMenu(distribute_mode);
    auto *distribute_group = new QActionGroup(distribute_menu);
    distribute_group->setExclusive(true);
    QAction *distribute_bounds = distribute_menu->addAction(bgl_tr("OBSTitles.DistributeToBounds"));
    QAction *distribute_anchors = distribute_menu->addAction(bgl_tr("OBSTitles.DistributeToAnchors"));
    distribute_bounds->setCheckable(true);
    distribute_anchors->setCheckable(true);
    distribute_group->addAction(distribute_bounds);
    distribute_group->addAction(distribute_anchors);
    distribute_bounds->setChecked(true);
    distribute_mode->setToolTip(bgl_tr("OBSTitles.DistributeToBounds"));
    distribute_mode->setAccessibleName(bgl_tr("OBSTitles.DistributeMode"));
    connect(distribute_bounds, &QAction::triggered, this, [this, distribute_mode]() {
        distribute_to_anchors_ = false;
        distribute_mode->setToolTip(bgl_tr("OBSTitles.DistributeToBounds"));
    });
    connect(distribute_anchors, &QAction::triggered, this, [this, distribute_mode]() {
        distribute_to_anchors_ = true;
        distribute_mode->setToolTip(bgl_tr("OBSTitles.DistributeToAnchors"));
    });
    distribute_mode->setMenu(distribute_menu);
    toolbar_->addWidget(distribute_mode);

    auto add_distribute_action = [this](const char *icon_name, const QString &tip, bool horizontal) {
        QAction *action = toolbar_->addAction(obs_icon(icon_name), tip);
        action->setToolTip(tip);
        connect(action, &QAction::triggered, this, [this, horizontal]() {
            distribute_selected_layers(horizontal);
        });
        return action;
    };
    add_distribute_action("distribute-horizontal.svg", bgl_tr("OBSTitles.DistributeHorizontally"), true);
    add_distribute_action("distribute-vertical.svg", bgl_tr("OBSTitles.DistributeVertically"), false);

    toolbar_->addSeparator();
    auto add_flip_action = [this](const char *icon_name, const QString &text, bool horizontal) {
        QAction *action = toolbar_->addAction(obs_icon(icon_name), text);
        action->setToolTip(text);
        connect(action, &QAction::triggered, this, [this, horizontal]() {
            flip_selected_layers(horizontal);
        });
        return action;
    };
    add_flip_action("flip-horizontal.svg", bgl_tr("OBSTitles.FlipHorizontal"), true);
    add_flip_action("flip-vertical.svg", bgl_tr("OBSTitles.FlipVertical"), false);

    toolbar_->addSeparator();
    auto *rotation_degrees = new QDoubleSpinBox(toolbar_);
    rotation_degrees->setRange(-9999.0, 9999.0);
    rotation_degrees->setDecimals(1);
    rotation_degrees->setSingleStep(1.0);
    rotation_degrees->setValue(90.0);
    rotation_degrees->setSuffix(QStringLiteral("°"));
    rotation_degrees->setToolTip(bgl_tr("OBSTitles.RotateDegreesTooltip"));
    rotation_degrees->setAccessibleName(bgl_tr("OBSTitles.RotateDegrees"));
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
    add_rotate_action("rotate-left.svg", bgl_tr("OBSTitles.RotateLeft"), -1.0);
    add_rotate_action("rotate-right.svg", bgl_tr("OBSTitles.RotateRight"), 1.0);

    act_safe_guides_ = new QAction(obs_icon("safe.svg"), bgl_tr("OBSTitles.Safe"), this);
    act_safe_guides_->setCheckable(true);
    act_safe_guides_->setToolTip(bgl_tr("OBSTitles.SafeTooltip"));
    connect(act_safe_guides_, &QAction::toggled, this, [this](bool visible) {
        if (canvas_) canvas_->set_safe_guides_visible(visible);
    });


    /*
     * Keep the context controls in one QWidgetAction.  QToolBar owns widgets
     * through QWidgetAction; toggling only a child widget that was hidden when
     * it was added can leave the toolbar action with a zero-sized geometry on
     * Windows.  The action itself is therefore the single source of truth for
     * visibility and layout recalculation.
     */
    dynamic_toolbar_widget_ = new QWidget(toolbar_);
    dynamic_toolbar_widget_->setObjectName(QStringLiteral("canvasDynamicToolbar"));
    dynamic_toolbar_widget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto *dynamic_toolbar_layout = new QHBoxLayout(dynamic_toolbar_widget_);
    dynamic_toolbar_layout->setContentsMargins(2, 0, 2, 0);
    dynamic_toolbar_layout->setSpacing(2);

    transform_toolbar_widget_ = new QWidget(dynamic_toolbar_widget_);
    transform_toolbar_widget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto *transform_toolbar_layout = new QHBoxLayout(transform_toolbar_widget_);
    transform_toolbar_layout->setContentsMargins(2, 0, 2, 0);
    transform_toolbar_layout->setSpacing(3);
    transform_toolbar_mode_ = new QComboBox(transform_toolbar_widget_);
    transform_toolbar_mode_->addItems({bgl_tr("OBSTitles.FreeTransformMode"),
                                       bgl_tr("OBSTitles.PerspectiveDistortMode"),
                                       bgl_tr("OBSTitles.FreeDistortMode")});
    transform_toolbar_mode_->setToolTip(bgl_tr("OBSTitles.FreeTransformToolTooltip"));
    transform_toolbar_keyframe_ = new QToolButton(transform_toolbar_widget_);
    transform_toolbar_keyframe_->setIcon(obs_icon("keyframe-outline.svg"));
    transform_toolbar_keyframe_->setCheckable(true);
    transform_toolbar_keyframe_->setAutoRaise(true);
    transform_toolbar_keyframe_->setToolTip(QStringLiteral("Toggle Free Transform keyframe"));
    transform_toolbar_reset_ = new QToolButton(transform_toolbar_widget_);
    transform_toolbar_reset_->setIcon(obs_icon("undo.svg"));
    transform_toolbar_reset_->setAutoRaise(true);
    transform_toolbar_reset_->setToolTip(QStringLiteral("Reset Free Transform"));
    transform_toolbar_layout->addWidget(transform_toolbar_mode_);
    transform_toolbar_layout->addWidget(transform_toolbar_keyframe_);
    transform_toolbar_layout->addWidget(transform_toolbar_reset_);
    dynamic_toolbar_layout->addWidget(transform_toolbar_widget_);

    boolean_toolbar_widget_ = new QWidget(dynamic_toolbar_widget_);
    boolean_toolbar_widget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto *boolean_toolbar_layout = new QHBoxLayout(boolean_toolbar_widget_);
    boolean_toolbar_layout->setContentsMargins(2, 0, 2, 0);
    boolean_toolbar_layout->setSpacing(2);
    auto make_boolean_button = [this](const char *icon_name, const QString &tooltip) {
        auto *button = new QToolButton(boolean_toolbar_widget_);
        button->setIcon(obs_icon(icon_name));
        button->setIconSize(toolbar_->iconSize());
        button->setToolTip(tooltip);
        button->setAutoRaise(true);
        button->setFixedSize(kEditorToolbarButtonExtent, kEditorToolbarButtonExtent);
        return button;
    };
    boolean_union_button_ = make_boolean_button("boolean-union.svg",
                                                bgl_tr("OBSTitles.BooleanUnionTooltip"));
    boolean_subtract_button_ = make_boolean_button("boolean-subtract.svg",
                                                   bgl_tr("OBSTitles.BooleanSubtractFrontTooltip"));
    boolean_intersect_button_ = make_boolean_button("boolean-intersect.svg",
                                                    bgl_tr("OBSTitles.BooleanIntersectTooltip"));
    boolean_exclude_button_ = make_boolean_button("boolean-exclude.svg",
                                                  bgl_tr("OBSTitles.BooleanExcludeTooltip"));
    boolean_toolbar_layout->addWidget(boolean_union_button_);
    boolean_toolbar_layout->addWidget(boolean_subtract_button_);
    boolean_toolbar_layout->addWidget(boolean_intersect_button_);
    boolean_toolbar_layout->addWidget(boolean_exclude_button_);
    dynamic_toolbar_layout->addWidget(boolean_toolbar_widget_);

    point_toolbar_widget_ = new QWidget(dynamic_toolbar_widget_);
    point_toolbar_widget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto *point_toolbar_layout = new QHBoxLayout(point_toolbar_widget_);
    point_toolbar_layout->setContentsMargins(2, 0, 2, 0);
    point_toolbar_layout->setSpacing(3);
    auto make_point_button = [this](const char *icon_name, const QString &tooltip) {
        auto *button = new QToolButton(point_toolbar_widget_);
        button->setIcon(obs_icon(icon_name));
        button->setIconSize(toolbar_->iconSize());
        button->setToolTip(tooltip);
        button->setAutoRaise(true);
        button->setCheckable(true);
        button->setFixedSize(kEditorToolbarButtonExtent, kEditorToolbarButtonExtent);
        return button;
    };
    point_toolbar_corner_ = make_point_button("draw-polygon.svg", bgl_tr("OBSTitles.ConvertAnchorToCorner"));
    point_toolbar_smooth_ = make_point_button("bezier-curve.svg", bgl_tr("OBSTitles.ConvertAnchorToSmooth"));
    point_toolbar_handles_ = make_point_button("eye.svg", bgl_tr("OBSTitles.ShowSelectedPointHandles"));
    point_toolbar_handles_->setChecked(true);

    auto make_point_position = [this](const QString &prefix, const QString &tooltip) {
        auto *spin = new QDoubleSpinBox(point_toolbar_widget_);
        spin->setRange(-100000.0, 100000.0);
        spin->setDecimals(1);
        spin->setSingleStep(1.0);
        spin->setPrefix(prefix);
        spin->setSpecialValueText(QStringLiteral("—"));
        spin->setToolTip(tooltip);
        spin->setKeyboardTracking(false);
        spin->setFixedWidth(78);
        return spin;
    };
    point_toolbar_x_ = make_point_position(QStringLiteral("X: "), bgl_tr("OBSTitles.PointXTooltip"));
    point_toolbar_y_ = make_point_position(QStringLiteral("Y: "), bgl_tr("OBSTitles.PointYTooltip"));
    point_toolbar_layout->addWidget(point_toolbar_corner_);
    point_toolbar_layout->addWidget(point_toolbar_smooth_);
    point_toolbar_layout->addWidget(point_toolbar_handles_);
    point_toolbar_layout->addWidget(point_toolbar_x_);
    point_toolbar_layout->addWidget(point_toolbar_y_);
    dynamic_toolbar_layout->addWidget(point_toolbar_widget_);

    corner_toolbar_widget_ = new QWidget(dynamic_toolbar_widget_);
    corner_toolbar_widget_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    auto *corner_toolbar_layout = new QHBoxLayout(corner_toolbar_widget_);
    corner_toolbar_layout->setContentsMargins(2, 0, 2, 0);
    corner_toolbar_layout->setSpacing(5);
    corner_toolbar_label_ = new QLabel(bgl_tr("OBSTitles.Corners"), corner_toolbar_widget_);
    corner_toolbar_roundness_ = new QDoubleSpinBox(corner_toolbar_widget_);
    corner_toolbar_roundness_->setRange(-100.0, 100.0);
    corner_toolbar_roundness_->setDecimals(0);
    corner_toolbar_roundness_->setSingleStep(1.0);
    corner_toolbar_roundness_->setSuffix(QStringLiteral("%"));
    corner_toolbar_roundness_->setSpecialValueText(bgl_tr("OBSTitles.MixedValues"));
    corner_toolbar_roundness_->setToolTip(QStringLiteral("100 = round, 0 = flat bevel, -100 = inverted round"));
    corner_toolbar_roundness_->setFixedWidth(82);
    corner_toolbar_radius_ = new QDoubleSpinBox(corner_toolbar_widget_);
    corner_toolbar_radius_->setRange(-1.0, 9999.0);
    corner_toolbar_radius_->setDecimals(1);
    corner_toolbar_radius_->setSingleStep(1.0);
    corner_toolbar_radius_->setSuffix(QStringLiteral(" px"));
    corner_toolbar_radius_->setSpecialValueText(bgl_tr("OBSTitles.MixedValues"));
    corner_toolbar_radius_->setToolTip(bgl_tr("OBSTitles.CornerRadius"));
    corner_toolbar_radius_->setFixedWidth(82);
    corner_toolbar_sync_ = new BglSwitch(bgl_tr("OBSTitles.SyncCornerRadii"), corner_toolbar_widget_);
    corner_toolbar_sync_->setTristate(true);
    corner_toolbar_sync_->setToolTip(bgl_tr("OBSTitles.SyncCornerRadiiTooltip"));
    corner_toolbar_layout->addWidget(corner_toolbar_label_);
    corner_toolbar_layout->addWidget(corner_toolbar_radius_);
    corner_toolbar_layout->addWidget(corner_toolbar_roundness_);
    corner_toolbar_layout->addWidget(corner_toolbar_sync_);
    dynamic_toolbar_layout->addWidget(corner_toolbar_widget_);

    /* Insert exactly between Rotate and Undo/Redo. */
    dynamic_toolbar_action_ = toolbar_->addWidget(dynamic_toolbar_widget_);
    dynamic_toolbar_action_->setPriority(QAction::HighPriority);
    dynamic_toolbar_action_->setVisible(false);
    transform_toolbar_widget_->setVisible(false);
    boolean_toolbar_widget_->setVisible(false);
    point_toolbar_widget_->setVisible(false);
    corner_toolbar_widget_->setVisible(false);

    dynamic_toolbar_separator_ = toolbar_->addSeparator();
    dynamic_toolbar_separator_->setVisible(false);
    toolbar_->addAction(act_undo_);
    toolbar_->addAction(act_redo_);
    update_undo_redo_actions();

    auto *toolbar_spacer = new QWidget(toolbar_);
    toolbar_spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar_->addWidget(toolbar_spacer);

    connect(transform_toolbar_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int mode) {
                if (updating_corner_toolbar_ || !canvas_) return;
                canvas_->set_free_transform_tool_active(mode);
                current_editor_tool_ = EditorTool::FreeTransform;
                update_corner_toolbar();
            });
    connect(transform_toolbar_keyframe_, &QToolButton::clicked, this, [this]() {
        if (!updating_corner_toolbar_ && canvas_) canvas_->toggle_free_transform_keyframe();
    });
    connect(transform_toolbar_reset_, &QToolButton::clicked, this, [this]() {
        if (!updating_corner_toolbar_ && canvas_) canvas_->reset_free_transform();
    });

