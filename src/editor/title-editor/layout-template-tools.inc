            if (result_layer->fill_type == 1) {
                editor_remap_gradient_to_canvas_bounds(
                    result_layer->gradient_type, style_source_rect, style_source_world, bounds,
                    result_layer->gradient_center_x, result_layer->gradient_center_y,
                    result_layer->gradient_focal_x, result_layer->gradient_focal_y,
                    result_layer->gradient_scale, result_layer->gradient_angle);
            }
            if (result_layer->stroke_fill_type == 2) {
                editor_remap_gradient_to_canvas_bounds(
                    result_layer->stroke_gradient_type, style_source_rect, style_source_world, bounds,
                    result_layer->stroke_gradient_center_x, result_layer->stroke_gradient_center_y,
                    result_layer->stroke_gradient_focal_x, result_layer->stroke_gradient_focal_y,
                    result_layer->stroke_gradient_scale, result_layer->stroke_gradient_angle);
            }
            if (result_layer->background_fill_type == 1) {
                editor_remap_gradient_to_canvas_bounds(
                    result_layer->background_gradient_type, style_source_rect, style_source_world, bounds,
                    result_layer->background_gradient_center_x, result_layer->background_gradient_center_y,
                    result_layer->background_gradient_focal_x, result_layer->background_gradient_focal_y,
                    result_layer->background_gradient_scale, result_layer->background_gradient_angle);
            }
            for (LayerEffect &effect : result_layer->effects) {
                const bool uses_gradient =
                    (effect.type == LayerEffectType::BackgroundColor && effect.effect_fill_type == 1) ||
                    (effect.type == LayerEffectType::Outline && effect.effect_fill_type == 2);
                if (!effect.effect_owned_style_loaded || !uses_gradient)
                    continue;
                editor_remap_gradient_to_canvas_bounds(
                    effect.effect_gradient_type, style_source_rect, style_source_world, bounds,
                    effect.effect_gradient_center_x, effect.effect_gradient_center_y,
                    effect.effect_gradient_focal_x, effect.effect_gradient_focal_y,
                    effect.effect_gradient_scale, effect.effect_gradient_angle);
            }
        }
    }

    std::vector<std::shared_ptr<Layer>> next_layers;
    next_layers.reserve(title_->layers.size() - shapes.size() + (result_layer ? 1 : 0));
    const std::string topmost_selected_id = shapes.back()->id;
    for (const auto &layer : title_->layers) {
        if (!layer)
            continue;
        if (selected_ids.find(layer->id) == selected_ids.end()) {
            next_layers.push_back(layer);
            continue;
        }
        if (result_layer && layer->id == topmost_selected_id)
            next_layers.push_back(result_layer);
    }
    title_->layers = std::move(next_layers);

    for (const auto &layer : title_->layers) {
        if (!layer)
            continue;
        if (selected_ids.find(layer->parent_id) != selected_ids.end())
            layer->parent_id = result_layer ? result_layer->id : std::string();
        if (selected_ids.find(layer->transform_parent_id) != selected_ids.end())
            layer->transform_parent_id = result_layer ? result_layer->id : std::string();
        if (selected_ids.find(layer->mask_source_id) != selected_ids.end()) {
            layer->mask_source_id = result_layer ? result_layer->id : std::string();
            if (!result_layer)
                layer->mask_mode = MaskMode::None;
        }
    }

    const std::vector<std::string> selection = result_layer
        ? std::vector<std::string>{result_layer->id}
        : std::vector<std::string>{};
    sel_layer_id_ = result_layer ? result_layer->id : std::string();
    layers_->refresh();
    layers_->set_selected_layers(selection);
    canvas_->set_title(title_, true);
    canvas_->set_selected_layers(selection);
    timeline_->set_title(title_);
    timeline_->set_selected_layers(selection);
    if (result_layer)
        update_layer_panels(result_layer, playhead_);
    else
        update_layer_panels(nullptr, playhead_);
    force_next_title_visual_update();
    on_title_modified();
}

std::vector<std::shared_ptr<Layer>> TitleEditor::clone_layers_for_insert(const std::vector<std::shared_ptr<Layer>> &layers,
                                                                         bool suffix_name) const
{
    std::map<std::string, std::string> cloned_ids_by_original;
    std::vector<std::shared_ptr<Layer>> clones;
    clones.reserve(layers.size());
    std::set<std::string> reserved_names;

    for (const auto &layer : layers) {
        if (!layer) continue;
        auto clone = std::make_shared<Layer>(*layer);
        clone->id = TitleDataStore::make_uuid();
        if (suffix_name)
            clone->name = clone->name.empty() ? editor_text_std("OBSTitles.LayerCopy")
                                              : clone->name + editor_text_std("OBSTitles.CopySuffix");
        clone->name = unique_layer_name(clone->name, {}, &reserved_names);
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

        auto transform_parent_clone = cloned_ids_by_original.find(clone->transform_parent_id);
        if (transform_parent_clone != cloned_ids_by_original.end()) {
            clone->transform_parent_id = transform_parent_clone->second;
        } else if (!clone->transform_parent_id.empty() &&
                   (!title_ || !title_->find_layer(clone->transform_parent_id))) {
            clone->transform_parent_id.clear();
        }

        auto mask_clone = cloned_ids_by_original.find(clone->mask_source_id);
        if (mask_clone != cloned_ids_by_original.end()) {
            clone->mask_source_id = mask_clone->second;
        } else if (!clone->mask_source_id.empty() && (!title_ || !title_->find_layer(clone->mask_source_id))) {
            clone->mask_source_id.clear();
            clone->mask_mode = MaskMode::None;
        }

        auto asset_owner_clone = cloned_ids_by_original.find(clone->asset_owner_id);
        if (asset_owner_clone != cloned_ids_by_original.end()) {
            clone->asset_owner_id = asset_owner_clone->second;
        } else if (!clone->asset_owner_id.empty() &&
                   (!title_ || !title_->find_layer(clone->asset_owner_id))) {
            clone->asset_owner_id.clear();
        }
    }

    return clones;
}

void TitleEditor::apply_picked_color_to_selection(const QColor &color, bool commit)
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
            const bool apply_inline_range = active_text_edit_layer_id_ == layer->id;
            RichTextCharFormat fmt;
            if (apply_inline_range) {
                /* The Color Swatches dock is outside the QTextEdit, but an
                 * active inline selection remains the edit target. Use the
                 * canonical selection summary instead of the layer mirrors;
                 * the latter represent the whole-text defaults and previously
                 * caused a swatch click to recolor the complete textbox. */
                const RichTextCharFormatSummary summary =
                    summarize_rich_text_char_format(*layer, true, local_time);
                fmt = summary.valid ? summary.format
                                    : layer_char_format_for_editor(*layer);
            } else {
                fmt = layer_char_format_for_editor(*layer);
            }
            fmt.fill.type = 0;
            fmt.fill.color = argb;
            apply_rich_text_format_to_layer_range(
                *layer, fmt, RichTextCharFillColor, apply_inline_range);
            if (apply_inline_range) {
                if (canvas_)
                    canvas_->apply_active_text_char_format(
                        layer->id, fmt, RichTextCharFillColor);
            } else {
                set_color_channels_at(*layer, true, local_time, argb);
            }
            last_changed = layer;
        } else if (layer->type == LayerType::Shape || layer->type == LayerType::SolidRect) {
            layer->fill_type = 0;
            layer->fill_color = argb;
            set_color_channels_at(*layer, false, local_time, argb);
            last_changed = layer;
        }
    }

    if (!last_changed) return;
    force_next_title_visual_update();
    if (canvas_)
        canvas_->refresh_preview();
    if (commit) {
        on_title_modified();
        update_layer_panels(last_changed, playhead_);
    }
    if (reopen_color_tab_after_canvas_pick_) {
        reopen_color_tab_after_canvas_pick_ = false;
        if (props_) props_->open_foreground_color_selector();
    }
}

void TitleEditor::duplicate_selected_layers()
{
    if (!title_) return;
    const auto requested_ids = selected_layer_ids_for_operation();
    if (requested_ids.empty()) return;
    const auto ids = editor_selection_with_group_descendants(title_, requested_ids);

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
    clone_ids.reserve(requested_ids.size());
    for (const auto &requested_id : requested_ids) {
        const auto found = clones_by_original.find(requested_id);
        if (found != clones_by_original.end() && found->second)
            clone_ids.push_back(found->second->id);
    }

    sel_layer_id_ = clone_ids.empty() ? std::string() : clone_ids.back();
    layers_->refresh();
    layers_->set_selected_layers(clone_ids);
    canvas_->set_selected_layers(clone_ids);
    timeline_->set_title(title_);
    timeline_->set_selected_layers(clone_ids);
    if (!sel_layer_id_.empty()) {
        if (auto layer = title_->find_layer(sel_layer_id_)) update_layer_panels(layer, playhead_);
    }
    on_title_modified();
}


void TitleEditor::group_selected_layers()
{
    if (!title_)
        return;
    const auto selected_ids = selected_layer_ids_for_operation();
    auto roots = editor_top_level_selected_layers(title_, selected_ids);
    if (roots.size() < 2)
        return;

    std::string common_parent = roots.front()->parent_id;
    for (const auto &layer : roots) {
        if (!layer || layer->parent_id != common_parent) {
            common_parent.clear();
            break;
        }
    }

    QRectF canvas_bounds;
    bool have_bounds = false;
    int insertion_index = -1;
    for (const auto &layer : roots) {
        if (!layer)
            continue;
        const QRectF bounds = editor_layer_canvas_bounds_for_grouping(
            title_, *layer, playhead_);
        if (bounds.isValid() && !bounds.isEmpty()) {
            canvas_bounds = have_bounds ? canvas_bounds.united(bounds) : bounds;
            have_bounds = true;
        }
        auto it = std::find(title_->layers.begin(), title_->layers.end(), layer);
        if (it != title_->layers.end())
            insertion_index = std::max(insertion_index,
                static_cast<int>(std::distance(title_->layers.begin(), it)));
    }
    if (!have_bounds)
        return;

    auto group = std::make_shared<Layer>();
    group->id = TitleDataStore::make_uuid();
    group->name = unique_layer_name(editor_text_std("OBSTitles.Group"));
    group->type = LayerType::Group;
    /* A newly-created group starts closed.  The user can advance the caret to
     * group-keyframes-only or to the full child-editing state explicitly. */
    group->group_collapsed = true;
    group->properties_expanded = false;
    group->parent_id = common_parent;
    group->in_time = 0.0;
    group->out_time = title_->duration;
    group->origin_x = 0.5f;
    group->origin_y = 0.5f;
    group->origin_prop.static_value = {0.5, 0.5};
    group->rect_width = static_cast<float>(std::max(1.0, canvas_bounds.width()));
    group->rect_height = static_cast<float>(std::max(1.0, canvas_bounds.height()));
    group->size.static_value = {group->rect_width, group->rect_height};

    QPointF group_local_center = canvas_bounds.center();
    if (!common_parent.empty()) {
        if (const Layer *parent = editor_layer_by_id_for_parenting(
                title_, common_parent)) {
            bool invertible = false;
            const QTransform inverse = editor_layer_world_transform_for_parenting(
                title_, *parent, playhead_).inverted(&invertible);
            if (invertible)
                group_local_center = inverse.map(canvas_bounds.center());
        }
    }
    group->position.static_value = {group_local_center.x(),
                                    group_local_center.y()};

    if (insertion_index < 0 ||
        insertion_index >= static_cast<int>(title_->layers.size()))
        title_->layers.push_back(group);
    else
        title_->layers.insert(title_->layers.begin() + insertion_index + 1,
                              group);

    for (const auto &layer : roots) {
        if (!layer)
            continue;
        if (layer->parent_id == common_parent && layer->transform_parent_id.empty()) {
            /* The new group is a pure translation inside the same parent.
             * Offset the complete position track, not just the current frame,
             * so grouping never changes animated artwork. */
            editor_offset_position_track(layer->position,
                                         -group_local_center.x(),
                                         -group_local_center.y());
            layer->parent_id = group->id;
        } else {
            const QTransform world = editor_layer_world_transform_for_parenting(
                title_, *layer, playhead_);
            editor_set_local_transform_from_world(title_, *layer, world,
                                                  group->id, layer->transform_parent_id,
                                                  playhead_);
        }
    }

    sel_layer_id_ = group->id;
    layers_->refresh();
    layers_->set_selected_layer(group->id);
    canvas_->set_selected_layer(group->id);
    timeline_->set_title(title_);
    timeline_->set_selected_layer(group->id);
    update_layer_panels(group, playhead_);
    force_next_title_visual_update();
    on_title_modified();
}

void TitleEditor::ungroup_selected_layers()
{
    if (!title_)
        return;
    const auto selected_ids = selected_layer_ids_for_operation();
    std::vector<std::shared_ptr<Layer>> groups;
    for (const auto &id : selected_ids) {
        auto layer = title_->find_layer(id);
        if (layer && layer->type == LayerType::Group)
            groups.push_back(layer);
    }
    if (groups.empty())
        return;

    std::vector<std::string> next_selection;
    std::set<std::string> removed_group_ids;
    for (const auto &group : groups) {
        if (group)
            removed_group_ids.insert(group->id);
    }

    std::map<std::string, QTransform> detached_transform_worlds;
    for (const auto &layer : title_->layers) {
        if (!layer || removed_group_ids.find(layer->id) != removed_group_ids.end())
            continue;
        if (removed_group_ids.find(layer->transform_parent_id) != removed_group_ids.end()) {
            detached_transform_worlds.emplace(
                layer->id, editor_layer_world_transform_for_parenting(
                    title_, *layer, playhead_));
        }
    }

    std::set<std::string> reparented_children;
    for (const auto &group : groups) {
        if (!group)
            continue;
        std::vector<std::shared_ptr<Layer>> children;
        for (const auto &candidate : title_->layers) {
            if (candidate && candidate->parent_id == group->id)
                children.push_back(candidate);
        }
        for (const auto &child : children) {
            const auto detached = detached_transform_worlds.find(child->id);
            const QTransform world = detached != detached_transform_worlds.end()
                ? detached->second
                : editor_layer_world_transform_for_parenting(title_, *child, playhead_);
            const std::string transform_parent_id =
                removed_group_ids.find(child->transform_parent_id) == removed_group_ids.end()
                    ? child->transform_parent_id : std::string();
            editor_set_local_transform_from_world(title_, *child, world,
                                                  group->parent_id,
                                                  transform_parent_id,
                                                  playhead_);
            reparented_children.insert(child->id);
            next_selection.push_back(child->id);
        }
    }

    for (const auto &[layer_id, world] : detached_transform_worlds) {
        if (reparented_children.find(layer_id) != reparented_children.end())
            continue;
        auto layer = title_->find_layer(layer_id);
        if (!layer)
            continue;
        const std::string group_parent_id =
            removed_group_ids.find(layer->parent_id) == removed_group_ids.end()
                ? layer->parent_id : std::string();
        editor_set_local_transform_from_world(title_, *layer, world,
                                              group_parent_id, std::string(),
                                              playhead_);
    }

    title_->layers.erase(std::remove_if(title_->layers.begin(),
                                        title_->layers.end(),
        [&](const std::shared_ptr<Layer> &layer) {
            return !layer || removed_group_ids.find(layer->id) !=
                             removed_group_ids.end();
        }), title_->layers.end());

    for (auto &layer : title_->layers) {
        if (!layer)
            continue;
        if (removed_group_ids.find(layer->transform_parent_id) !=
            removed_group_ids.end())
            layer->transform_parent_id.clear();
        if (removed_group_ids.find(layer->mask_source_id) !=
            removed_group_ids.end()) {
            layer->mask_source_id.clear();
            layer->mask_mode = MaskMode::None;
        }
    }

    sel_layer_id_ = next_selection.empty() ? std::string()
                                           : next_selection.back();
    layers_->refresh();
    layers_->set_selected_layers(next_selection);
    canvas_->set_selected_layers(next_selection);
    timeline_->set_title(title_);
    timeline_->set_selected_layers(next_selection);
    update_layer_panels(sel_layer_id_.empty() ? nullptr
                                              : title_->find_layer(sel_layer_id_),
                        playhead_);
    force_next_title_visual_update();
    on_title_modified();
}

void TitleEditor::add_selected_layers_to_group(const std::string &group_id)
{
    if (!title_ || group_id.empty())
        return;
    auto group = title_->find_layer(group_id);
    if (!group || group->type != LayerType::Group || group->locked)
        return;

    const auto selected_ids = selected_layer_ids_for_operation();
    auto roots = editor_top_level_selected_layers(title_, selected_ids);
    bool changed = false;
    for (const auto &layer : roots) {
        if (!layer || layer->id == group_id || layer->locked ||
            editor_layer_has_ancestor_for_grouping(title_, *group,
                                                   layer->id))
            continue;
        const QTransform world = editor_layer_world_transform_for_parenting(
            title_, *layer, playhead_);
        editor_set_local_transform_from_world(title_, *layer, world,
                                              group_id, layer->transform_parent_id,
                                              playhead_);
        changed = true;
    }
    if (!changed)
        return;

    /* Adding artwork to an existing group must not silently change its
     * presentation/editing state. */
    sel_layer_id_ = group_id;
    layers_->refresh();
    layers_->set_selected_layer(group_id);
    canvas_->set_selected_layer(group_id);
    timeline_->set_title(title_);
    timeline_->set_selected_layer(group_id);
    update_layer_panels(group, playhead_);
    force_next_title_visual_update();
    on_title_modified();
}

void TitleEditor::remove_selected_layers_from_group()
{
    if (!title_)
        return;
    const auto selected_ids = selected_layer_ids_for_operation();
    bool changed = false;
    for (const auto &id : selected_ids) {
        auto layer = title_->find_layer(id);
        if (!layer || layer->locked || layer->parent_id.empty())
            continue;
        auto parent = title_->find_layer(layer->parent_id);
        if (!parent || parent->type != LayerType::Group)
            continue;
        const QTransform world = editor_layer_world_transform_for_parenting(
            title_, *layer, playhead_);
        editor_set_local_transform_from_world(title_, *layer, world,
                                              parent->parent_id, layer->transform_parent_id,
                                              playhead_);
        changed = true;
    }
    if (!changed)
        return;

    layers_->refresh();
    layers_->set_selected_layers(selected_ids);
    canvas_->set_selected_layers(selected_ids);
    timeline_->set_title(title_);
    timeline_->set_selected_layers(selected_ids);
    force_next_title_visual_update();
    on_title_modified();
}

void TitleEditor::copy_selected_layer()
{
    if (!title_) return;
    const auto requested_ids = selected_layer_ids_for_operation();
    if (requested_ids.empty()) return;
    const auto ids = editor_selection_with_group_descendants(title_, requested_ids);

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

    std::set<std::string> clipboard_ids;
    for (const auto &layer : clipboard_layers)
        if (layer) clipboard_ids.insert(layer->id);
    std::vector<std::string> pasted_ids;
    pasted_ids.reserve(pasted_layers.size());
    for (size_t index = 0; index < pasted_layers.size() &&
                           index < clipboard_layers.size(); ++index) {
        const auto &source = clipboard_layers[index];
        const auto &pasted = pasted_layers[index];
        if (!source || !pasted)
            continue;
        if (source->parent_id.empty() ||
            clipboard_ids.find(source->parent_id) == clipboard_ids.end())
            pasted_ids.push_back(pasted->id);
    }
    if (pasted_ids.empty()) {
        for (const auto &layer : pasted_layers)
            if (layer) pasted_ids.push_back(layer->id);
    }

    sel_layer_id_ = pasted_ids.empty() ? std::string() : pasted_ids.back();
    layers_->refresh();
    layers_->set_selected_layers(pasted_ids);
    canvas_->set_selected_layers(pasted_ids);
    timeline_->set_title(title_);
    timeline_->set_selected_layers(pasted_ids);
    if (!sel_layer_id_.empty()) {
        if (auto layer = title_->find_layer(sel_layer_id_)) update_layer_panels(layer, playhead_);
    }
    on_title_modified();
}



bool TitleEditor::paste_external_clipboard_to_canvas()
{
    if (!title_)
        return false;
    const QMimeData *mime = QApplication::clipboard() ? QApplication::clipboard()->mimeData() : nullptr;
    if (!mime)
        return false;

    const QPointF canvas_pt = canvas_ ? canvas_->view_center_canvas_point()
                                      : QPointF(title_->width * 0.5, title_->height * 0.5);

    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            if (!url.isLocalFile())
                continue;
            const QString path = url.toLocalFile();
            QImageReader reader(path);
            if (!reader.canRead())
                continue;
            create_image_layer_from_external_source(path, canvas_pt);
            return true;
        }
    }

    if (mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) {
            QString base_dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
            if (base_dir.isEmpty())
                base_dir = QDir::tempPath();
            QDir dir(base_dir + QStringLiteral("/obs-bgs-pasted-assets"));
            if (!dir.exists())
                dir.mkpath(QStringLiteral("."));
            const QString path = dir.filePath(QStringLiteral("pasted-image-%1.png")
                .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz"))));
            if (image.save(path, "PNG")) {
                create_image_layer_from_external_source(path, canvas_pt);
                return true;
            }
        }
    }

    if (mime->hasText()) {
        const QString text = mime->text().trimmed();
        if (!text.isEmpty()) {
            create_text_layer_from_external_source(text, canvas_pt);
            return true;
        }
    }

    return false;
}

void TitleEditor::delete_selected_layer()
{
    if (!title_) return;
    const auto requested_ids = selected_layer_ids_for_operation();
    if (requested_ids.empty()) return;
    const auto ids = editor_selection_with_group_descendants(title_, requested_ids);

    std::set<std::string> removed_ids(ids.begin(), ids.end());
    std::vector<std::pair<std::shared_ptr<Layer>, QTransform>> detached_transform_children;
    for (const auto &layer : title_->layers) {
        if (!layer || removed_ids.find(layer->id) != removed_ids.end())
            continue;
        if (removed_ids.find(layer->transform_parent_id) != removed_ids.end()) {
            detached_transform_children.push_back({
                layer, editor_layer_world_transform_for_parenting(title_, *layer, playhead_)});
        }
    }

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

    title_->layers = std::move(remaining);
    for (const auto &[layer, world] : detached_transform_children) {
        if (!layer)
            continue;
        const std::string group_parent_id =
            removed_ids.find(layer->parent_id) == removed_ids.end()
                ? layer->parent_id : std::string();
        editor_set_local_transform_from_world(title_, *layer, world,
                                              group_parent_id, std::string(),
                                              playhead_);
    }
    for (auto &layer : title_->layers) {
        if (!layer) continue;
        if (removed_ids.find(layer->parent_id) != removed_ids.end())
            layer->parent_id.clear();
        if (removed_ids.find(layer->transform_parent_id) != removed_ids.end())
            layer->transform_parent_id.clear();
        if (removed_ids.find(layer->mask_source_id) != removed_ids.end()) {
            layer->mask_source_id.clear();
            layer->mask_mode = MaskMode::None;
        }
    }
    sel_layer_id_.clear();
    layers_->refresh();

    if (!title_->layers.empty()) {
        const int select_index = std::clamp(first_removed_index, 0, (int)title_->layers.size() - 1);
        on_layer_selected(title_->layers[(size_t)select_index]->id);
    } else {
        layers_->set_selected_layers({});
        canvas_->set_selected_layers({});
        timeline_->set_title(title_);
        timeline_->set_selected_layers({});
        update_layer_panels(nullptr, playhead_);

        /* Deleting the final layer is a special hard-empty transition.  Clear
         * the editor artwork synchronously and discard every RAM/disk frame for
         * this title before any delayed invalidation or repaint can reuse the
         * last valid cached image.  The CanvasPreview itself stays alive and
         * continues painting the checkerboard/editor chrome. */
        canvas_->clear_rendered_frame();
        CacheManager::instance().removeTitleCache(QString::fromStdString(title_->id), true);
    }

    force_next_title_visual_update();
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
    title_->live_text_row_ids = snapshot->live_text_row_ids;
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
            ? bgl_tr("OBSTitles.PropertiesNamed").arg(QString::fromStdString(title_->name))
            : bgl_tr("OBSTitles.Properties"));
    }
    if (dirty_indicator_)
        dirty_indicator_->setVisible(dirty_);
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
    setWindowTitle(bgl_editor_caption(bgl_tr(dirty_ ? "OBSTitles.EditorModifiedTitle" : "OBSTitles.EditorWindowTitle")));
}

bool TitleEditor::confirm_save_before_close()
{
    if (!dirty_)
        return true;

    QMessageBox dialog(this);
    dialog.setIcon(QMessageBox::Warning);
    dialog.setWindowTitle(bgl_tr("OBSTitles.UnsavedChangesTitle"));
    dialog.setText(bgl_tr("OBSTitles.UnsavedChangesPrompt"));
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
    if (!playing_) {
        full_loop_playback_ = false;
        manual_reverse_playback_ = false;
        playback_reverse_ = false;
    }
    playing_ = !playing_;
    if (canvas_) canvas_->set_playback_active(playing_);
    if (playing_) {
        const CachePlaybackSettings cache_settings = CacheManager::instance().playbackSettings();
        if (cache_settings.from_beginning)
            on_playhead_changed(0.0);
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
    if (canvas_) canvas_->set_playback_active(true);
    act_play_->setText("⏸");
    act_play_->setIcon(obs_icon("pause.svg"));
    playback_clock_.restart();
    play_timer_->start();
}

void TitleEditor::reverse_play()
{
    if (!title_) return;
    if (playing_ && manual_reverse_playback_) {
        playing_ = false;
        if (canvas_) canvas_->set_playback_active(false);
        manual_reverse_playback_ = false;
        play_timer_->stop();
        act_play_->setText("▶");
        act_play_->setIcon(obs_icon("play.svg"));
        return;
    }
    full_loop_playback_ = false;
    manual_reverse_playback_ = true;
    playback_reverse_ = true;
    if (playhead_ <= 0.0)
        on_playhead_changed(title_->duration);
    playing_ = true;
    if (canvas_) canvas_->set_playback_active(true);
    act_play_->setText("⏸");
    act_play_->setIcon(obs_icon("pause.svg"));
    playback_clock_.restart();
    play_timer_->start();
}

void TitleEditor::go_to_start()
{
    manual_reverse_playback_ = false;
    playback_reverse_ = false;
    on_playhead_changed(0.0);
}

void TitleEditor::go_to_end()
{
    if (!title_) return;
    manual_reverse_playback_ = false;
    playback_reverse_ = false;
    on_playhead_changed(title_->duration);
}

void TitleEditor::step_backward()
{
    if (!title_) return;
    on_playhead_changed(std::max(0.0, snap_to_obs_frame(playhead_ - obs_frame_duration())));
}


void TitleEditor::insert_asset_layer(const std::string &asset_id,
                                     const QPointF &canvas_pt)
{
    if (!title_ || asset_id.empty())
        return;
    auto asset_title = TitleDataStore::instance().get_title(asset_id);
    if (!asset_title || !asset_title->is_asset || asset_title->layers.empty())
        return;
    if (asset_title->id == title_->id) {
        QMessageBox::warning(this, bgl_tr("OBSTitles.Assets"),
                             bgl_tr("OBSTitles.AssetRecursiveInsertError"));
        return;
    }

    auto bounds_title = clone_title(*asset_title);
    QRectF source_bounds = editor_title_animation_bounds(bounds_title);
    if (!source_bounds.isValid() || source_bounds.isEmpty()) {
        const double center_x = static_cast<double>(asset_title->width) * 0.5;
        const double center_y = static_cast<double>(asset_title->height) * 0.5;
        source_bounds = QRectF(center_x - 16.0, center_y - 16.0, 32.0, 32.0);
    }

    push_undo_snapshot();

    auto asset_layer = std::make_shared<Layer>();
    asset_layer->id = TitleDataStore::make_uuid();
    asset_layer->name = unique_layer_name(asset_title->name);
    asset_layer->type = LayerType::Asset;
    asset_layer->group_collapsed = true;
    asset_layer->properties_expanded = false;
    asset_layer->asset_title_id = asset_title->id;
    asset_layer->asset_category = asset_title->asset_category;
    asset_layer->asset_animated = bgs::asset_runtime::title_has_timeline_animation(*asset_title);
    asset_layer->asset_playback_mode = 0;
    asset_layer->asset_playback_offset = 0.0;
    asset_layer->asset_duration = std::max(0.1, asset_title->duration);
    asset_layer->asset_source_playback_mode = std::clamp(asset_title->playback_mode, 0, 2);
    asset_layer->asset_source_loop_type = std::clamp(asset_title->loop_type, 0, 1);
    asset_layer->asset_source_loop_start = std::clamp(asset_title->loop_start, 0.0, asset_layer->asset_duration);
    asset_layer->asset_source_loop_end = std::clamp(asset_title->loop_end,
        asset_layer->asset_source_loop_start, asset_layer->asset_duration);
    asset_layer->asset_source_pause_time = std::clamp(asset_title->pause_time,
        0.0, asset_layer->asset_duration);
    asset_layer->asset_pause_duration = 1.0;
    asset_layer->asset_loop_count = 1;
    asset_layer->asset_loop = false;
    asset_layer->in_time = 0.0;
    asset_layer->out_time = title_->duration;
    asset_layer->origin_x = 0.5f;
    asset_layer->origin_y = 0.5f;
    asset_layer->origin_prop.static_value = {0.5, 0.5};
    asset_layer->rect_width = static_cast<float>(std::max(1.0, source_bounds.width()));
    asset_layer->rect_height = static_cast<float>(std::max(1.0, source_bounds.height()));
    asset_layer->size.static_value = {asset_layer->rect_width,
                                      asset_layer->rect_height};
    asset_layer->position.static_value = {canvas_pt.x(), canvas_pt.y()};

    std::map<std::string, std::string> id_map;
    for (const auto &source : asset_title->layers) {
        if (source)
            id_map[source->id] = TitleDataStore::make_uuid();
    }

    std::vector<std::shared_ptr<Layer>> clones;
    clones.reserve(asset_title->layers.size());
    const QPointF source_center = source_bounds.center();
    for (const auto &source : asset_title->layers) {
        if (!source)
            continue;
        auto clone = std::make_shared<Layer>(*source);
        clone->id = id_map[source->id];
        const auto owner_it = id_map.find(source->asset_owner_id);
        clone->asset_owner_id = owner_it != id_map.end()
            ? owner_it->second : asset_layer->id;
        clone->asset_source_layer_id = source->id;
        clone->group_collapsed = source->type == LayerType::Asset
            ? true : source->group_collapsed;

        const auto parent_it = id_map.find(source->parent_id);
        clone->parent_id = parent_it != id_map.end()
            ? parent_it->second : asset_layer->id;
        const auto transform_it = id_map.find(source->transform_parent_id);
        clone->transform_parent_id = transform_it != id_map.end()
            ? transform_it->second : std::string();
        const auto mask_it = id_map.find(source->mask_source_id);
        if (mask_it != id_map.end()) {
            clone->mask_source_id = mask_it->second;
        } else {
            clone->mask_source_id.clear();
            clone->mask_mode = MaskMode::None;
        }

        if (source->parent_id.empty()) {
            editor_offset_position_track(clone->position,
                                         -source_center.x(),
                                         -source_center.y());
        }
        clones.push_back(clone);
    }

    for (auto &clone : clones)
        title_->layers.push_back(std::move(clone));
    title_->layers.push_back(asset_layer);

    sel_layer_id_ = asset_layer->id;
    layers_->refresh();
    layers_->set_selected_layer(asset_layer->id);
    canvas_->set_selected_layer(asset_layer->id);
    timeline_->set_title(title_);
    timeline_->set_selected_layer(asset_layer->id);
    update_layer_panels(asset_layer, playhead_);
    force_next_title_visual_update();
    on_title_modified(false);
}

void TitleEditor::edit_asset(const std::string &asset_id)
{
    if (asset_id.empty())
        return;
    auto asset = TitleDataStore::instance().get_title(asset_id);
    if (!asset || !asset->is_asset) {
        QMessageBox::warning(this, bgl_tr("OBSTitles.EditAsset"),
                             bgl_tr("OBSTitles.AssetNotFound"));
        return;
    }
    if (editing_title_id_ == asset_id)
        return;

    /* Switching the editor to an asset must never silently discard the title
     * currently being edited. Ask only when the current title is dirty. */
    if (dirty_) {
        QMessageBox dialog(this);
        dialog.setIcon(QMessageBox::Warning);
        dialog.setWindowTitle(bgl_tr("OBSTitles.UnsavedChangesTitle"));
        dialog.setText(bgl_tr("OBSTitles.SaveBeforeEditAssetPrompt"));
        dialog.setStandardButtons(
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        dialog.setDefaultButton(QMessageBox::Save);
        dialog.setEscapeButton(QMessageBox::Cancel);
        const auto result = static_cast<QMessageBox::StandardButton>(
            dialog.exec());
        if (result == QMessageBox::Save) {
            if (!save_title())
                return;
        } else if (result != QMessageBox::Discard) {
            return;
        }
    }

    open_title(asset_id);
    show();
    raise();
    activateWindow();
}

void TitleEditor::open_asset_overrides_dialog(
    const std::string &asset_layer_id)
{
    if (!title_)
        return;
    auto asset_layer = title_->find_layer(asset_layer_id);
    if (!asset_layer || asset_layer->type != LayerType::Asset)
        return;

    QDialog dialog(this);
    bgl_apply_brand_icon(&dialog);
    dialog.setWindowTitle(bgl_tr("OBSTitles.AssetLayerProperties"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();

    auto *playback = new QComboBox(&dialog);
    playback->addItem(bgl_tr("OBSTitles.AssetPlaybackSynchronized"), 0);
    playback->addItem(bgl_tr("OBSTitles.AssetPlaybackIndependent"), 1);
    playback->setCurrentIndex(std::clamp(asset_layer->asset_playback_mode, 0, 1));
    form->addRow(bgl_tr("OBSTitles.AssetPlayback"), playback);

    auto *offset = new QDoubleSpinBox(&dialog);
    offset->setRange(-86400.0, 86400.0);
    offset->setDecimals(3);
    offset->setSuffix(QStringLiteral(" s"));
    offset->setValue(asset_layer->asset_playback_offset);
    form->addRow(bgl_tr("OBSTitles.AssetPlaybackOffset"), offset);

    auto *pause_duration = new TimecodeSpinBox(&dialog);
    pause_duration->setRange(0.0, 86400.0);
    pause_duration->setValue(asset_layer->asset_pause_duration);
    form->addRow(bgl_tr("OBSTitles.AssetPauseFor"), pause_duration);

    auto *loop_count = new QSpinBox(&dialog);
    loop_count->setRange(1, 1000000);
    loop_count->setSuffix(bgl_tr("OBSTitles.AssetLoopTimesSuffix"));
    loop_count->setValue(std::max(1, asset_layer->asset_loop_count));
    form->addRow(bgl_tr("OBSTitles.AssetLoopCount"), loop_count);

    auto *loop = new BglSwitch(bgl_tr("OBSTitles.AssetLoop"), &dialog);
    loop->setChecked(asset_layer->asset_loop);
    form->addRow(QString(), loop);
    layout->addLayout(form);

    auto set_form_field_visible = [form](QWidget *field, bool visible) {
        if (!field) return;
        field->setVisible(visible);
        if (auto *label = qobject_cast<QWidget *>(form->labelForField(field)))
            label->setVisible(visible);
    };
    auto refresh_playback_controls = [=]() {
        const bool has_animation = asset_layer->asset_animated;
        const bool independent = has_animation && playback->currentData().toInt() == 1;
        set_form_field_visible(playback, has_animation);
        set_form_field_visible(offset, independent);
        set_form_field_visible(pause_duration,
            independent && asset_layer->asset_source_playback_mode == 2);
        set_form_field_visible(loop_count,
            independent && asset_layer->asset_source_playback_mode == 1);
        set_form_field_visible(loop, independent);
    };
    connect(playback, QOverload<int>::of(&QComboBox::currentIndexChanged),
            &dialog, [refresh_playback_controls](int) {
                refresh_playback_controls();
            });
    refresh_playback_controls();

    auto *overrides_label = new QLabel(bgl_tr("OBSTitles.AssetExposedOverrides"), &dialog);
    QFont label_font = overrides_label->font();
    label_font.setBold(true);
    overrides_label->setFont(label_font);
    layout->addWidget(overrides_label);

    auto *overrides_form = new QFormLayout();
    std::vector<std::function<void()>> apply_overrides;
    int exposed_count = 0;
    for (const auto &child : title_->layers) {
        if (!child || !child->expose_text ||
            !editor_layer_has_ancestor_for_grouping(title_, *child,
                                                    asset_layer->id))
            continue;
        if (child->type == LayerType::Text || child->type == LayerType::Ticker ||
            child->type == LayerType::Clock) {
            auto *edit = new QLineEdit(QString::fromStdString(child->text_content), &dialog);
            overrides_form->addRow(QString::fromStdString(child->name), edit);
            apply_overrides.emplace_back([child, edit]() {
                const std::string text = edit->text().toStdString();
                rich_text_document_ensure_canonical(*child);
                rich_text_document_replace_text(child->rich_text, text);
                child->text_content = text;
            });
            ++exposed_count;
        } else if (child->type == LayerType::Image) {
            auto *row = new QWidget(&dialog);
            auto *row_layout = new QHBoxLayout(row);
            row_layout->setContentsMargins(0, 0, 0, 0);
            auto *edit = new QLineEdit(QString::fromStdString(child->image_path), row);
            auto *browse = new QToolButton(row);
            browse->setText(QStringLiteral("…"));
            connect(browse, &QToolButton::clicked, &dialog, [edit, &dialog]() {
                const QString path = QFileDialog::getOpenFileName(
                    &dialog, bgl_tr("OBSTitles.SelectImage"), edit->text(),
                    bgl_tr("OBSTitles.ImageFileFilter"));
                if (!path.isEmpty())
                    edit->setText(path);
            });
            row_layout->addWidget(edit, 1);
            row_layout->addWidget(browse);
            overrides_form->addRow(QString::fromStdString(child->name), row);
            apply_overrides.emplace_back([child, edit]() {
                bgs::apply_exposed_image_cue_value(*child,
                                                    edit->text().toStdString());
            });
            ++exposed_count;
        }
    }
    if (exposed_count == 0) {
        auto *none = new QLabel(bgl_tr("OBSTitles.AssetNoExposedProperties"), &dialog);
        none->setWordWrap(true);
        layout->addWidget(none);
    } else {
        layout->addLayout(overrides_form);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         Qt::Horizontal, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;

    push_undo_snapshot();
    asset_layer->asset_playback_mode = asset_layer->asset_animated
        ? playback->currentData().toInt() : 0;
    asset_layer->asset_playback_offset = offset->value();
    asset_layer->asset_pause_duration = pause_duration->value();
    asset_layer->asset_loop_count = loop_count->value();
    asset_layer->asset_loop = loop->isChecked();
    bgs::asset_runtime::reset_layer(title_->id, asset_layer->id);
    for (const auto &apply : apply_overrides)
        apply();
    update_layer_panels(asset_layer, playhead_);
    layers_->refresh();
    canvas_->refresh_preview();
    timeline_->set_title(title_);
    on_title_modified(false);
}
