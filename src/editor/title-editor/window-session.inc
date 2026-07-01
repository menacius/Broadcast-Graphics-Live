namespace {

static QString bgl_editor_caption(const QString &base)
{
    return base + QStringLiteral(" — ") + QStringLiteral(BGL_BUILD_DISPLAY);
}
double editor_playback_ui_frame_duration()
{
    /* Playback follows the project/OBS cadence, including project rates above
     * 60 fps. The 1 ms floor only protects QTimer from an invalid zero value. */
    return std::max(1.0 / 1000.0, obs_frame_duration());
}

int editor_playback_ui_timer_interval_ms()
{
    return std::max(1, static_cast<int>(std::lround(editor_playback_ui_frame_duration() * 1000.0)));
}

QString editor_recent_color_hex(const QColor &color)
{
    if (!color.isValid())
        return QString();
    if (color.alpha() < 255) {
        return QStringLiteral("#%1%2%3%4")
            .arg(color.red(), 2, 16, QLatin1Char('0'))
            .arg(color.green(), 2, 16, QLatin1Char('0'))
            .arg(color.blue(), 2, 16, QLatin1Char('0'))
            .arg(color.alpha(), 2, 16, QLatin1Char('0'))
            .toUpper();
    }
    return color.name(QColor::HexRgb).toUpper();
}

bool editor_parse_recent_color_hex(const QString &hex, QColor &color)
{
    QString s = hex.trimmed();
    if (s.startsWith(QLatin1Char('#')))
        s.remove(0, 1);

    bool ok = false;
    if (s.size() == 6) {
        const int r = s.mid(0, 2).toInt(&ok, 16);
        if (!ok) return false;
        const int g = s.mid(2, 2).toInt(&ok, 16);
        if (!ok) return false;
        const int b = s.mid(4, 2).toInt(&ok, 16);
        if (!ok) return false;
        color = QColor(r, g, b, 255);
        return color.isValid();
    }

    if (s.size() == 8) {
        const int r = s.mid(0, 2).toInt(&ok, 16);
        if (!ok) return false;
        const int g = s.mid(2, 2).toInt(&ok, 16);
        if (!ok) return false;
        const int b = s.mid(4, 2).toInt(&ok, 16);
        if (!ok) return false;
        const int a = s.mid(6, 2).toInt(&ok, 16);
        if (!ok) return false;
        color = QColor(r, g, b, a);
        return color.isValid();
    }

    return false;
}

QString editor_color_swatch_style(const QColor &color, bool enabled = true)
{
    const QPalette pal = qApp->palette();
    const QString border = pal.color(QPalette::Mid).name(QColor::HexRgb);
    const QString highlight = pal.color(QPalette::Highlight).name(QColor::HexRgb);
    if (!enabled || !color.isValid()) {
        const QString bg = pal.color(QPalette::Window).name(QColor::HexRgb);
        return QStringLiteral("QToolButton{background:%1;border:1px dashed %2;border-radius:3px;}"
                              "QToolButton:hover{border:1px dashed %2;}")
            .arg(bg, border);
    }

    const QString bg = color.name(color.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb);
    return QStringLiteral("QToolButton{background:%1;border:1px solid %2;border-radius:3px;}"
                          "QToolButton:hover{border:2px solid %3;}")
        .arg(bg, border, highlight);
}

QString editor_slugify(QString text)
{
    text = text.trimmed().toLower();
    QString slug;
    bool previous_dash = false;
    for (const QChar ch : text) {
        const bool valid = ch.isLetterOrNumber();
        if (valid) {
            slug.append(ch);
            previous_dash = false;
        } else if (!previous_dash && !slug.isEmpty()) {
            slug.append(QLatin1Char('-'));
            previous_dash = true;
        }
    }
    while (slug.endsWith(QLatin1Char('-')))
        slug.chop(1);
    return slug.isEmpty() ? QStringLiteral("library") : slug;
}

QString editor_color_libraries_path()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/Broadcast Graphics Live");
    QDir dir(base);
    dir.mkpath(QStringLiteral("palettes"));
    return dir.filePath(QStringLiteral("palettes/user-color-libraries.palette.json"));
}

QString bgl_load_selected_color_library_slug()
{
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Color"));
    return settings.value(QStringLiteral("selectedColorLibrarySlug")).toString();
}

void bgl_save_selected_color_library_slug(const QString &slug)
{
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Color"));
    settings.setValue(QStringLiteral("selectedColorLibrarySlug"), slug);
    settings.sync();
}


const Layer *editor_layer_by_id_for_parenting(const std::shared_ptr<Title> &title, const std::string &id)
{
    if (!title || id.empty()) return nullptr;
    for (const auto &candidate : title->layers) {
        if (candidate && candidate->id == id) return candidate.get();
    }
    return nullptr;
}

static double editor_asset_time_for_parenting_impl(
    const std::shared_ptr<Title> &title, const Layer &layer, double playhead,
    std::set<std::string> &visiting)
{
    if (!title || layer.asset_owner_id.empty() ||
        !visiting.insert(layer.asset_owner_id).second)
        return playhead;
    const Layer *asset = editor_layer_by_id_for_parenting(
        title, layer.asset_owner_id);
    if (!asset || asset->type != LayerType::Asset) {
        visiting.erase(layer.asset_owner_id);
        return playhead;
    }
    const double owner_time = editor_asset_time_for_parenting_impl(
        title, *asset, playhead, visiting);
    const double resolved = bgs::asset_runtime::resolve_local_time(
        title->id, *asset, owner_time - asset->in_time);
    visiting.erase(layer.asset_owner_id);
    return resolved;
}

static double editor_asset_time_for_parenting(
    const std::shared_ptr<Title> &title, const Layer &layer, double playhead)
{
    std::set<std::string> visiting;
    return editor_asset_time_for_parenting_impl(title, layer, playhead, visiting);
}

static QTransform editor_layer_local_transform_for_parenting(
    const std::shared_ptr<Title> &title, const Layer &layer, double playhead,
    bool include_general_transitions = false)
{
    QTransform local;
    const double resolved_time = editor_asset_time_for_parenting(
        title, layer, playhead);
    const double lt = std::max(0.0, resolved_time - layer.in_time);
    const LayerTransitionVisualState transition = include_general_transitions
        ? evaluate_layer_general_transitions(
              layer.transitions, layer.in_time, layer.out_time, resolved_time)
        : LayerTransitionVisualState{};
    local.translate(layer.position.evaluate(lt).x + transition.translate_x,
                    layer.position.evaluate(lt).y + transition.translate_y);
    local.rotate(layer.rotation.evaluate(lt));
    local.scale(layer.scale.evaluate(lt).x * transition.scale,
                layer.scale.evaluate(lt).y * transition.scale);
    return local;
}

static std::vector<std::string> editor_group_chain_for_parenting(
    const std::shared_ptr<Title> &title, const std::string &group_parent_id)
{
    std::vector<std::string> chain;
    std::string cursor = group_parent_id;
    std::set<std::string> visited;
    while (!cursor.empty() && visited.insert(cursor).second) {
        const Layer *group = editor_layer_by_id_for_parenting(title, cursor);
        if (!group || !layer_type_is_container(group->type))
            break;
        chain.push_back(cursor);
        cursor = group->parent_id;
    }
    return chain;
}

static std::vector<std::string> editor_group_chain_for_layer_parenting(
    const std::shared_ptr<Title> &title, const Layer &layer)
{
    std::vector<std::string> chain;
    if (layer_type_is_container(layer.type))
        chain.push_back(layer.id);
    const auto ancestors = editor_group_chain_for_parenting(title, layer.parent_id);
    chain.insert(chain.end(), ancestors.begin(), ancestors.end());
    return chain;
}

static std::string editor_common_group_for_parenting(
    const std::shared_ptr<Title> &title, const std::string &child_group_parent_id,
    const Layer &transform_parent)
{
    const auto child_groups = editor_group_chain_for_parenting(title, child_group_parent_id);
    const auto parent_groups = editor_group_chain_for_layer_parenting(title, transform_parent);
    const std::set<std::string> parent_set(parent_groups.begin(), parent_groups.end());
    for (const auto &group_id : child_groups) {
        if (parent_set.find(group_id) != parent_set.end())
            return group_id;
    }
    return {};
}

static QTransform editor_layer_world_transform_for_parenting_impl(
    const std::shared_ptr<Title> &title, const Layer &layer, double playhead,
    std::set<std::string> &visiting, bool include_general_transitions);

static QTransform editor_parent_basis_for_parenting(
    const std::shared_ptr<Title> &title, const Layer &layer, double playhead,
    const std::string &group_parent_id, const std::string &transform_parent_id,
    std::set<std::string> &visiting, bool include_general_transitions)
{
    QTransform group_world;
    if (!group_parent_id.empty()) {
        if (const Layer *group_parent = editor_layer_by_id_for_parenting(title, group_parent_id);
            group_parent && layer_type_is_container(group_parent->type)) {
            group_world = editor_layer_world_transform_for_parenting_impl(
                title, *group_parent, playhead, visiting,
                include_general_transitions);
        }
    }

    if (transform_parent_id.empty())
        return group_world;
    const Layer *transform_parent = editor_layer_by_id_for_parenting(title, transform_parent_id);
    if (!transform_parent)
        return group_world;

    const QTransform transform_world = editor_layer_world_transform_for_parenting_impl(
        title, *transform_parent, playhead, visiting,
        include_general_transitions);
    const std::string common_group_id = editor_common_group_for_parenting(
        title, group_parent_id, *transform_parent);
    if (common_group_id.empty())
        return group_world * transform_world;

    const Layer *common_group = editor_layer_by_id_for_parenting(title, common_group_id);
    if (!common_group)
        return group_world * transform_world;
    std::set<std::string> common_visiting;
    const QTransform common_world = editor_layer_world_transform_for_parenting_impl(
        title, *common_group, playhead, common_visiting,
        include_general_transitions);
    bool invertible = false;
    const QTransform common_inverse = common_world.inverted(&invertible);
    return invertible ? group_world * common_inverse * transform_world
                      : group_world * transform_world;
}

static QTransform editor_layer_world_transform_for_parenting_impl(
    const std::shared_ptr<Title> &title, const Layer &layer, double playhead,
    std::set<std::string> &visiting, bool include_general_transitions)
{
    if (!layer.id.empty() && !visiting.insert(layer.id).second)
        return QTransform();
    const QTransform parent_basis = editor_parent_basis_for_parenting(
        title, layer, playhead, layer.parent_id, layer.transform_parent_id,
        visiting, include_general_transitions);
    /* QTransform products are applied in written order. Keep the established
     * Group contract: layer-local transform first, then the container/parent
     * basis. This is algebraically identical to the previous recursive
     * translate/rotate/scale implementation for Group-only layers. */
    const QTransform world = editor_layer_local_transform_for_parenting(
        title, layer, playhead, include_general_transitions) * parent_basis;
    if (!layer.id.empty())
        visiting.erase(layer.id);
    return world;
}

QTransform editor_layer_world_transform_for_parenting(const std::shared_ptr<Title> &title,
                                                      const Layer &layer,
                                                      double playhead,
                                                      int /*depth*/ = 0)
{
    std::set<std::string> visiting;
    return editor_layer_world_transform_for_parenting_impl(
        title, layer, playhead, visiting, false);
}

static QTransform editor_layer_world_transform_for_animation_bounds(
    const std::shared_ptr<Title> &title, const Layer &layer, double playhead)
{
    std::set<std::string> visiting;
    return editor_layer_world_transform_for_parenting_impl(
        title, layer, playhead, visiting, true);
}

bool editor_paths_geometrically_equal(const QPainterPath &a, const QPainterPath &b)
{
    if (a.isEmpty() || b.isEmpty())
        return a.isEmpty() && b.isEmpty();

    QPainterPath only_a = a.subtracted(b);
    QPainterPath only_b = b.subtracted(a);
    only_a.setFillRule(Qt::OddEvenFill);
    only_b.setFillRule(Qt::OddEvenFill);
    return only_a.isEmpty() && only_b.isEmpty();
}

int editor_boolean_gradient_type(int gradient_type)
{
    switch (std::clamp(gradient_type, 0, 4)) {
    case 1: return 1;
    case 2: return 2;
    case 4: return 1;
    case 0:
    case 3:
    default: return 0;
    }
}

double editor_normalized_degrees(double degrees)
{
    degrees = std::fmod(degrees, 360.0);
    return degrees < 0.0 ? degrees + 360.0 : degrees;
}

void editor_remap_gradient_to_canvas_bounds(int gradient_type,
                                            const QRectF &source_box,
                                            const QTransform &source_world,
                                            const QRectF &result_canvas_bounds,
                                            float &center_x, float &center_y,
                                            float &focal_x, float &focal_y,
                                            float &scale, float &angle_degrees)
{
    if (source_box.width() <= 1e-9 || source_box.height() <= 1e-9 ||
        result_canvas_bounds.width() <= 1e-9 || result_canvas_bounds.height() <= 1e-9)
        return;

    const QPointF source_center(source_box.left() + (double)center_x * source_box.width(),
                                source_box.top() + (double)center_y * source_box.height());
    const QPointF source_focal(source_box.left() + (double)focal_x * source_box.width(),
                               source_box.top() + (double)focal_y * source_box.height());
    const double source_angle = (double)angle_degrees * std::acos(-1.0) / 180.0;
    const QPointF source_axis(std::cos(source_angle), std::sin(source_angle));

    const QPointF world_center = source_world.map(source_center);
    const QPointF world_focal = source_world.map(source_focal);
    auto normalize_result_point = [&](const QPointF &point) {
        return QPointF((point.x() - result_canvas_bounds.left()) / result_canvas_bounds.width(),
                       (point.y() - result_canvas_bounds.top()) / result_canvas_bounds.height());
    };

    const QPointF normalized_center = normalize_result_point(world_center);
    const QPointF normalized_focal = normalize_result_point(world_focal);
    center_x = (float)std::clamp(normalized_center.x(), -100.0, 100.0);
    center_y = (float)std::clamp(normalized_center.y(), -100.0, 100.0);
    focal_x = (float)std::clamp(normalized_focal.x(), -100.0, 100.0);
    focal_y = (float)std::clamp(normalized_focal.y(), -100.0, 100.0);

    const int type = editor_boolean_gradient_type(gradient_type);
    double source_extent = 1.0;
    double result_base = 1.0;
    if (type == 1) {
        source_extent = std::max(1.0, std::max(source_box.width(), source_box.height()) *
                                      0.5 * std::clamp((double)scale, 0.01, 100.0));
        result_base = std::max(1.0, std::max(result_canvas_bounds.width(),
                                             result_canvas_bounds.height()) * 0.5);
    } else {
        source_extent = std::max(1.0, std::hypot(source_box.width(), source_box.height()) *
                                      0.5 * std::clamp((double)scale, 0.01, 100.0));
        result_base = std::max(1.0, std::hypot(result_canvas_bounds.width(),
                                               result_canvas_bounds.height()) * 0.5);
    }

    /* Conical gradients have no radius in the renderer, but mapping a stable
     * reference vector still preserves their canvas-space orientation. */
    if (type == 2)
        source_extent = std::max(1.0, std::hypot(source_box.width(), source_box.height()) * 0.5);

    const QPointF world_axis_point = source_world.map(source_center + source_axis * source_extent);
    const QPointF world_axis = world_axis_point - world_center;
    const double world_extent = std::hypot(world_axis.x(), world_axis.y());
    if (world_extent > 1e-9) {
        angle_degrees = (float)editor_normalized_degrees(
            std::atan2(world_axis.y(), world_axis.x()) * 180.0 / std::acos(-1.0));
        if (type != 2)
            scale = (float)std::clamp(world_extent / result_base, 0.01, 100.0);
    }
}

QString editor_font_awesome_family()
{
    static const QString family = []() {
        const QStringList preferred = {
            QStringLiteral("Font Awesome 6 Free"),
            QStringLiteral("Font Awesome 5 Free"),
            QStringLiteral("FontAwesome")
        };
        const QStringList available = QFontDatabase().families();
        for (const QString &candidate : preferred) {
            if (available.contains(candidate, Qt::CaseInsensitive))
                return candidate;
        }
        return QString();
    }();
    return family;
}

QIcon editor_font_awesome_icon(ushort codepoint, const QString &fallback, QWidget *widget)
{
    const int extent = 30;
    QPixmap pixmap(extent, extent);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    QFont font;
    const QString family = editor_font_awesome_family();
    QString glyph = fallback;
    if (!family.isEmpty()) {
        font.setFamily(family);
        font.setWeight(QFont::Black);
        glyph = QString(QChar(codepoint));
    } else {
        font.setBold(true);
    }
    font.setPixelSize(std::max(11, extent - 9));
    painter.setFont(font);
    painter.setPen((widget ? widget->palette() : qApp->palette()).color(QPalette::ButtonText));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, glyph);
    return QIcon(pixmap);
}

bool editor_parenting_would_cycle(const std::shared_ptr<Title> &title,
                                  const std::string &layer_id,
                                  const std::string &parent_id)
{
    if (!title || layer_id.empty() || parent_id.empty())
        return false;
    std::set<std::string> visited;
    std::function<bool(const std::string &)> depends_on_layer;
    depends_on_layer = [&](const std::string &candidate_id) {
        if (candidate_id.empty()) return false;
        if (candidate_id == layer_id) return true;
        if (!visited.insert(candidate_id).second) return false;
        const Layer *candidate = editor_layer_by_id_for_parenting(title, candidate_id);
        if (!candidate) return false;
        return depends_on_layer(candidate->transform_parent_id) ||
               depends_on_layer(candidate->parent_id);
    };
    return depends_on_layer(parent_id);
}

bool editor_layer_has_ancestor_for_grouping(const std::shared_ptr<Title> &title,
                                            const Layer &layer,
                                            const std::string &ancestor_id)
{
    std::string cursor = layer.parent_id;
    int guard = 0;
    while (!cursor.empty() && guard++ < 64) {
        if (cursor == ancestor_id)
            return true;
        const Layer *parent = editor_layer_by_id_for_parenting(title, cursor);
        if (!parent)
            break;
        cursor = parent->parent_id;
    }
    return false;
}

QRectF editor_layer_local_rect_for_grouping(
    const std::shared_ptr<Title> &title, const Layer &layer, double playhead)
{
    const double local_time = std::max(
        0.0, editor_asset_time_for_parenting(title, layer, playhead) -
             layer.in_time);
    const double width = std::max(0.0, eval_box_width(layer, local_time));
    const double height = std::max(0.0, eval_box_height(layer, local_time));
    const double origin_x = eval_origin_x(layer, local_time);
    const double origin_y = eval_origin_y(layer, local_time);
    return QRectF(-origin_x * width, -origin_y * height, width, height);
}

QRectF editor_layer_canvas_bounds_for_grouping(const std::shared_ptr<Title> &title,
                                                const Layer &layer,
                                                double playhead)
{
    if (!title)
        return {};
    if (layer_type_is_container(layer.type)) {
        QRectF combined;
        bool have = false;
        for (const auto &candidate : title->layers) {
            if (!candidate || layer_type_is_container(candidate->type) ||
                !editor_layer_has_ancestor_for_grouping(title, *candidate, layer.id))
                continue;
            const QRectF child = editor_layer_canvas_bounds_for_grouping(
                title, *candidate, playhead);
            if (!child.isValid() || child.isEmpty())
                continue;
            combined = have ? combined.united(child) : child;
            have = true;
        }
        if (have)
            return combined.normalized();
        const QPointF origin = editor_layer_world_transform_for_parenting(
            title, layer, playhead).map(QPointF(0.0, 0.0));
        return QRectF(origin - QPointF(16.0, 16.0), QSizeF(32.0, 32.0));
    }

    const QRectF local = editor_layer_local_rect_for_grouping(title, layer, playhead);
    if (!local.isValid() || local.isEmpty())
        return {};
    const QTransform world = editor_layer_world_transform_for_parenting(
        title, layer, playhead);
    QPolygonF polygon;
    polygon << world.map(local.topLeft()) << world.map(local.topRight())
            << world.map(local.bottomRight()) << world.map(local.bottomLeft());
    return polygon.boundingRect().normalized();
}

static QRectF editor_layer_canvas_bounds_for_animation(
    const std::shared_ptr<Title> &title, const Layer &layer, double playhead)
{
    if (!title)
        return {};

    const Layer *visibility_cursor = &layer;
    int visibility_guard = 0;
    while (visibility_cursor && visibility_guard++ < 64) {
        const double cursor_time = editor_asset_time_for_parenting(
            title, *visibility_cursor, playhead);
        if (!visibility_cursor->visible ||
            cursor_time < visibility_cursor->in_time - 1e-6 ||
            cursor_time > visibility_cursor->out_time + 1e-6)
            return {};
        visibility_cursor = visibility_cursor->parent_id.empty()
            ? nullptr
            : editor_layer_by_id_for_parenting(
                  title, visibility_cursor->parent_id);
    }

    const double resolved_time = editor_asset_time_for_parenting(
        title, layer, playhead);

    if (layer_type_is_container(layer.type)) {
        QRectF combined;
        bool have = false;
        for (const auto &candidate : title->layers) {
            if (!candidate || layer_type_is_container(candidate->type) ||
                !editor_layer_has_ancestor_for_grouping(title, *candidate, layer.id))
                continue;
            const QRectF child = editor_layer_canvas_bounds_for_animation(
                title, *candidate, playhead);
            if (!child.isValid() || child.isEmpty())
                continue;
            combined = have ? combined.united(child) : child;
            have = true;
        }
        if (have)
            return combined.normalized();
        const QPointF origin = editor_layer_world_transform_for_animation_bounds(
            title, layer, playhead).map(QPointF(0.0, 0.0));
        return QRectF(origin - QPointF(16.0, 16.0), QSizeF(32.0, 32.0));
    }

    const QRectF local = editor_layer_local_rect_for_grouping(
        title, layer, playhead);
    if (!local.isValid() || local.isEmpty())
        return {};

    const double local_time = std::max(0.0, resolved_time - layer.in_time);
    auto quad_value = [local_time](const AnimatedVec2Property &property,
                                   float legacy_x, float legacy_y) {
        if (property.is_animated() || property.static_value.x != 0.0 ||
            property.static_value.y != 0.0)
            return property.evaluate(local_time);
        return Vec2Value{legacy_x, legacy_y};
    };
    const Vec2Value qtl = quad_value(
        layer.transform_quad_tl, layer.transform_quad_tl_x,
        layer.transform_quad_tl_y);
    const Vec2Value qtr = quad_value(
        layer.transform_quad_tr, layer.transform_quad_tr_x,
        layer.transform_quad_tr_y);
    const Vec2Value qbr = quad_value(
        layer.transform_quad_br, layer.transform_quad_br_x,
        layer.transform_quad_br_y);
    const Vec2Value qbl = quad_value(
        layer.transform_quad_bl, layer.transform_quad_bl_x,
        layer.transform_quad_bl_y);

    QPolygonF local_polygon;
    local_polygon
        << local.topLeft() + QPointF(qtl.x * local.width(), qtl.y * local.height())
        << local.topRight() + QPointF(qtr.x * local.width(), qtr.y * local.height())
        << local.bottomRight() + QPointF(qbr.x * local.width(), qbr.y * local.height())
        << local.bottomLeft() + QPointF(qbl.x * local.width(), qbl.y * local.height());

    /* Text Animator displacement/scale/blur is the only text-specific
     * animation envelope. Legacy text transitions are migrated before this
     * editor path is reached. */
    QRectF text_transition_bounds = local_polygon.boundingRect();
    const double animator_padding = text_animator_stack_visual_padding(
        layer.text_animators, local.width(), local.height());
    if (animator_padding > 0.0)
        text_transition_bounds = text_transition_bounds.adjusted(
            -animator_padding, -animator_padding,
             animator_padding,  animator_padding);
    if (text_transition_bounds != local_polygon.boundingRect()) {
        local_polygon.clear();
        local_polygon << text_transition_bounds.topLeft()
                      << text_transition_bounds.topRight()
                      << text_transition_bounds.bottomRight()
                      << text_transition_bounds.bottomLeft();
    }

    const QTransform world = editor_layer_world_transform_for_animation_bounds(
        title, layer, playhead);
    QPolygonF canvas_polygon;
    for (const QPointF &point : local_polygon)
        canvas_polygon << world.map(point);
    return canvas_polygon.boundingRect().normalized();
}

QRectF editor_title_animation_bounds(const std::shared_ptr<Title> &title)
{
    if (!title)
        return {};

    /* Independent nested assets must not consult the wall clock while the
     * reusable asset envelope is calculated. Evaluate their complete source
     * timelines deterministically, then keep the resulting union fixed on the
     * Asset Layer instance. */
    for (const auto &layer : title->layers) {
        if (layer && layer->type == LayerType::Asset)
            layer->asset_playback_mode = 0;
    }

    double duration = std::max(0.0, title->duration);
    for (const auto &layer : title->layers) {
        if (!layer)
            continue;
        duration = std::max(duration, layer->out_time);
        if (layer->type == LayerType::Asset)
            duration = std::max(duration, layer->asset_duration);
    }

    const double native_step = std::max(1.0 / 240.0, obs_frame_duration());
    constexpr int kMaximumEnvelopeSamples = 12000;
    int sample_count = std::max(1, static_cast<int>(std::ceil(
        duration / native_step)));
    sample_count = std::min(sample_count, kMaximumEnvelopeSamples);

    QRectF combined;
    bool have_bounds = false;
    for (int sample = 0; sample <= sample_count; ++sample) {
        const double time = sample_count > 0
            ? duration * static_cast<double>(sample) /
                  static_cast<double>(sample_count)
            : 0.0;
        for (const auto &layer : title->layers) {
            if (!layer || !layer->parent_id.empty())
                continue;
            const QRectF bounds = editor_layer_canvas_bounds_for_animation(
                title, *layer, time);
            if (!bounds.isValid() || bounds.isEmpty())
                continue;
            combined = have_bounds ? combined.united(bounds) : bounds;
            have_bounds = true;
        }
    }
    return have_bounds ? combined.normalized() : QRectF();
}

void editor_offset_position_track(AnimatedVec2Property &position,
                                  double dx, double dy)
{
    position.static_value.x += dx;
    position.static_value.y += dy;
    for (VectorKeyframe &keyframe : position.keyframes) {
        keyframe.value.x += dx;
        keyframe.value.y += dy;
    }
}

void editor_set_local_transform_from_world(const std::shared_ptr<Title> &title,
                                           Layer &layer,
                                           const QTransform &world,
                                           const std::string &new_group_parent_id,
                                           const std::string &new_transform_parent_id,
                                           double playhead)
{
    std::set<std::string> visiting;
    const QTransform parent_world = editor_parent_basis_for_parenting(
        title, layer, playhead, new_group_parent_id, new_transform_parent_id,
        visiting, false);
    bool invertible = false;
    const QTransform parent_inverse = parent_world.inverted(&invertible);
    const QTransform local = invertible ? world * parent_inverse : world;

    const double scale_x = std::hypot(local.m11(), local.m12());
    const double determinant = local.determinant();
    double scale_y = scale_x > 1e-9
        ? determinant / scale_x
        : std::hypot(local.m21(), local.m22());
    if (std::abs(scale_y) < 1e-9)
        scale_y = scale_y < 0.0 ? -1e-9 : 1e-9;
    const double rotation = std::atan2(local.m12(), local.m11()) *
                            180.0 / std::acos(-1.0);
    const double local_time = std::clamp(
        playhead - layer.in_time, 0.0,
        std::max(0.0, layer.out_time - layer.in_time));

    layer.parent_id = new_group_parent_id;
    layer.transform_parent_id = new_transform_parent_id;
    set_animated_x(layer.position, local_time, local.dx());
    set_animated_y(layer.position, local_time, local.dy());
    set_animated_x(layer.scale, local_time, scale_x);
    set_animated_y(layer.scale, local_time, scale_y);
    set_animated_value(layer.rotation, local_time, rotation);
}

std::vector<std::shared_ptr<Layer>> editor_top_level_selected_layers(
    const std::shared_ptr<Title> &title,
    const std::vector<std::string> &ids)
{
    std::vector<std::shared_ptr<Layer>> result;
    if (!title)
        return result;
    const std::set<std::string> selected(ids.begin(), ids.end());
    for (const auto &layer : title->layers) {
        if (!layer || selected.find(layer->id) == selected.end())
            continue;
        bool has_selected_ancestor = false;
        std::string cursor = layer->parent_id;
        int guard = 0;
        while (!cursor.empty() && guard++ < 64) {
            if (selected.find(cursor) != selected.end()) {
                has_selected_ancestor = true;
                break;
            }
            const Layer *parent = editor_layer_by_id_for_parenting(title, cursor);
            if (!parent)
                break;
            cursor = parent->parent_id;
        }
        if (!has_selected_ancestor)
            result.push_back(layer);
    }
    return result;
}

std::vector<std::string> editor_selection_with_group_descendants(
    const std::shared_ptr<Title> &title,
    const std::vector<std::string> &ids)
{
    if (!title)
        return {};
    std::set<std::string> expanded(ids.begin(), ids.end());
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &layer : title->layers) {
            if (!layer || layer->parent_id.empty() ||
                expanded.find(layer->id) != expanded.end())
                continue;
            auto parent = title->find_layer(layer->parent_id);
            if (parent && layer_type_is_container(parent->type) &&
                expanded.find(parent->id) != expanded.end()) {
                expanded.insert(layer->id);
                changed = true;
            }
        }
    }
    std::vector<std::string> ordered;
    ordered.reserve(expanded.size());
    for (const auto &layer : title->layers) {
        if (layer && expanded.find(layer->id) != expanded.end())
            ordered.push_back(layer->id);
    }
    return ordered;
}


std::string editor_group_order_scope(const std::shared_ptr<Title> &title,
                                     const Layer &layer)
{
    if (!title || layer.parent_id.empty()) return {};
    const auto parent = title->find_layer(layer.parent_id);
    return parent && layer_type_is_container(parent->type) ? layer.parent_id
                                                       : std::string();
}

std::vector<std::shared_ptr<Layer>> editor_canonical_group_order(
    const std::shared_ptr<Title> &title,
    const std::map<std::string, std::vector<std::shared_ptr<Layer>>> &siblings_by_scope)
{
    std::vector<std::shared_ptr<Layer>> ordered;
    if (!title) return ordered;

    std::set<std::string> emitted;
    std::function<void(const std::shared_ptr<Layer> &)> append_subtree;
    append_subtree = [&](const std::shared_ptr<Layer> &layer) {
        if (!layer || !emitted.insert(layer->id).second)
            return;
        const auto children = siblings_by_scope.find(layer->id);
        if (children != siblings_by_scope.end()) {
            for (const auto &child : children->second)
                append_subtree(child);
        }
        ordered.push_back(layer);
    };

    const auto roots = siblings_by_scope.find(std::string());
    if (roots != siblings_by_scope.end()) {
        for (const auto &root : roots->second)
            append_subtree(root);
    }
    for (const auto &layer : title->layers)
        append_subtree(layer);
    return ordered;
}

bool editor_reorder_selected_group_siblings(const std::shared_ptr<Title> &title,
                                            const std::vector<std::string> &selected_ids,
                                            int action)
{
    if (!title || selected_ids.empty() || action == 0)
        return false;
    const auto selected_roots = editor_top_level_selected_layers(title, selected_ids);
    std::set<std::string> moving;
    for (const auto &layer : selected_roots)
        if (layer) moving.insert(layer->id);
    if (moving.empty()) return false;

    std::map<std::string, std::vector<std::shared_ptr<Layer>>> scopes;
    for (const auto &layer : title->layers) {
        if (!layer) continue;
        scopes[editor_group_order_scope(title, *layer)].push_back(layer);
    }

    bool changed = false;
    for (auto &[scope, siblings] : scopes) {
        const bool scope_has_selection = std::any_of(
            siblings.begin(), siblings.end(), [&](const auto &layer) {
                return layer && moving.find(layer->id) != moving.end();
            });
        if (!scope_has_selection) continue;

        auto is_moving = [&](const std::shared_ptr<Layer> &layer) {
            return layer && moving.find(layer->id) != moving.end();
        };
        const auto before = siblings;
        if (action >= 2 || action <= -2) {
            std::vector<std::shared_ptr<Layer>> selected;
            std::vector<std::shared_ptr<Layer>> stationary;
            selected.reserve(siblings.size());
            stationary.reserve(siblings.size());
            for (const auto &layer : siblings)
                (is_moving(layer) ? selected : stationary).push_back(layer);
            siblings.clear();
            if (action >= 2) {
                siblings.insert(siblings.end(), stationary.begin(), stationary.end());
                siblings.insert(siblings.end(), selected.begin(), selected.end());
            } else {
                siblings.insert(siblings.end(), selected.begin(), selected.end());
                siblings.insert(siblings.end(), stationary.begin(), stationary.end());
            }
        } else if (action > 0) {
            for (int i = static_cast<int>(siblings.size()) - 2; i >= 0; --i) {
                if (is_moving(siblings[static_cast<size_t>(i)]) &&
                    !is_moving(siblings[static_cast<size_t>(i + 1)]))
                    std::swap(siblings[static_cast<size_t>(i)],
                              siblings[static_cast<size_t>(i + 1)]);
            }
        } else {
            for (size_t i = 1; i < siblings.size(); ++i) {
                if (is_moving(siblings[i]) && !is_moving(siblings[i - 1]))
                    std::swap(siblings[i], siblings[i - 1]);
            }
        }
        changed = changed || siblings != before;
    }

    if (!changed) return false;
    auto reordered = editor_canonical_group_order(title, scopes);
    if (reordered.size() != title->layers.size())
        return false;
    title->layers = std::move(reordered);
    return true;
}

// Editor-session-only defaults. These intentionally live only in process memory:
// closing/reopening the editor window within the same OBS run keeps the user's
// last foreground/background and new-layer style, but restarting OBS resets them
// to the compiled/project defaults instead of reading/writing QSettings.

class BroadcastGraphicsLiveLogo final : public QWidget {
public:
    explicit BroadcastGraphicsLiveLogo(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumSize(320, 320);

        char *path = obs_module_file("icons/broadcast-graphics-live-logo.svg");
        if (!path)
            return;

        QFile file(QString::fromUtf8(path));
        bfree(path);
        if (file.open(QIODevice::ReadOnly))
            source_svg_ = file.readAll();
    }

    QSize sizeHint() const override
    {
        return QSize(460, 460);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);
        if (source_svg_.isEmpty())
            return;

        QByteArray themed_svg = source_svg_;
        const QColor icon_color = bgl_icon_color();
        const QByteArray replacement = icon_color.name(QColor::HexRgb).toUtf8();

        /* Keep the supplied artwork byte-for-byte on disk. Only its #dddddd
         * artwork color (exported by Illustrator as the equivalent #ddd) is
         * substituted in memory when the About dialog is painted. */
        themed_svg.replace("#dddddd", replacement);
        themed_svg.replace("#DDDDDD", replacement);
        themed_svg.replace("#ddd", replacement);
        themed_svg.replace("#DDD", replacement);

        QSvgRenderer renderer(themed_svg);
        if (!renderer.isValid())
            return;

        QRectF available = rect().adjusted(8, 8, -8, -8);
        QSizeF artwork_size = renderer.defaultSize();
        if (artwork_size.isEmpty())
            artwork_size = QSizeF(1920.0, 1920.0);
        artwork_size.scale(available.size(), Qt::KeepAspectRatio);
        const QRectF target(
            available.center().x() - artwork_size.width() * 0.5,
            available.center().y() - artwork_size.height() * 0.5,
            artwork_size.width(), artwork_size.height());

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&painter, target);
    }

    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::PaletteChange ||
            event->type() == QEvent::ApplicationPaletteChange ||
            event->type() == QEvent::StyleChange)
            update();
    }

private:
    QByteArray source_svg_;
};

class OmniaTvLogo final : public QWidget {
public:
    explicit OmniaTvLogo(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setMinimumSize(180, 54);
        setMaximumHeight(96);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(QStringLiteral("OmniaTV"));
        setToolTip(bgl_tr("OBSTitles.OmniaTvWebsiteTooltip"));

        normal_svg_ = load_svg("icons/omnianormal.svg");
        inverted_svg_ = load_svg("icons/omniainvert.svg");
    }

    QSize sizeHint() const override
    {
        return QSize(260, 78);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);

        const bool dark_theme = qApp &&
            qApp->palette().color(QPalette::Window).lightnessF() < 0.5;
        const QByteArray &preferred = dark_theme ? inverted_svg_ : normal_svg_;
        const QByteArray &fallback = dark_theme ? normal_svg_ : inverted_svg_;
        const QByteArray &source = preferred.isEmpty() ? fallback : preferred;
        if (source.isEmpty())
            return;

        QSvgRenderer renderer(source);
        if (!renderer.isValid())
            return;

        const QRectF available = rect().adjusted(8, 6, -8, -6);
        QSizeF artwork_size = renderer.defaultSize();
        if (artwork_size.isEmpty())
            artwork_size = QSizeF(717.95692, 215.31342);
        artwork_size.scale(available.size(), Qt::KeepAspectRatio);
        const QRectF target(
            available.center().x() - artwork_size.width() * 0.5,
            available.center().y() - artwork_size.height() * 0.5,
            artwork_size.width(), artwork_size.height());

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&painter, target);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
            open_website();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
            event->key() == Qt::Key_Space) {
            open_website();
            event->accept();
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::PaletteChange ||
            event->type() == QEvent::ApplicationPaletteChange ||
            event->type() == QEvent::StyleChange)
            update();
    }

private:
    static QByteArray load_svg(const char *relative_path)
    {
        char *path = obs_module_file(relative_path);
        if (!path)
            return {};

        QFile file(QString::fromUtf8(path));
        bfree(path);
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    static void open_website()
    {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://omniatv.com")));
    }

    QByteArray normal_svg_;
    QByteArray inverted_svg_;
};

bool g_editor_session_sidebar_colors_initialized = false;
QColor g_editor_session_foreground_color;
QColor g_editor_session_background_color;
bool g_editor_session_new_layer_defaults_initialized = false;
Layer g_editor_session_new_layer_style;

} // namespace

TitleEditor::TitleEditor(QWidget *parent)
    : QMainWindow(parent, Qt::Window)
{
    bgl_apply_brand_icon(this);
    setWindowTitle(bgl_editor_caption(bgl_tr("OBSTitles.EditorWindowTitle")));
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
                   QMainWindow::GroupedDragging);

    build_ui();
    const QPointer<TitleEditor> external_data_guard(this);
    external_data_callback_id_ =
        ExternalDataManager::instance().on_change(
            [external_data_guard](const ExternalDataUpdate &update) {
                if (!external_data_guard)
                    return;
                QTimer::singleShot(0, external_data_guard,
                    [external_data_guard, update]() {
                        if (!external_data_guard ||
                            external_data_guard->shutting_down_ ||
                            !external_data_guard->title_ ||
                            !title_uses_external_update(
                                *external_data_guard->title_, update))
                            return;
                        const QString next_visual_hash =
                            CacheManager::instance().contentHashForTitle(
                                *external_data_guard->title_);
                        if (!external_data_guard->external_data_visual_hash_.isEmpty() &&
                            external_data_guard->external_data_visual_hash_ ==
                                next_visual_hash)
                            return;
                        external_data_guard->external_data_visual_hash_ =
                            next_visual_hash;
                        if (external_data_guard->canvas_)
                            external_data_guard->canvas_->refresh_preview();
                    });
            });
    load_sidebar_default_colors();
    load_new_layer_defaults();
    update_sidebar_color_swatches(nullptr);

    play_timer_ = new QTimer(this);
    play_timer_->setTimerType(Qt::PreciseTimer);
    // Transport is always driven by the project/OBS frame rate. It must not
    // change when the editor is moved between monitors with different refresh rates.
    play_timer_->setInterval(editor_playback_ui_timer_interval_ms());
    connect(play_timer_, &QTimer::timeout, this, &TitleEditor::tick);

    gui_refresh_timer_ = new QTimer(this);
    gui_refresh_timer_->setTimerType(Qt::PreciseTimer);
    connect(gui_refresh_timer_, &QTimer::timeout, this, [this]() {
        // Never enqueue paints while Qt is reparenting/rebuilding the dock tree.
        // Updating child widgets during QMainWindow layout transitions can race
        // with Qt's internal dock layout items and lead to access violations.
        if (dock_layout_transition_ || !isVisible() || isMinimized() || !isActiveWindow())
            return;
        /* The monitor-paced timer is editing-only. During playback, the
         * project-rate play_timer_ is the sole source of canvas frame updates. */
        if (playing_)
            return;

        // High-refresh presentation is reserved for interactive UI/canvas work
        // and live clock/ticker refresh while transport is stopped. Playback
        // frames continue to advance only from play_timer_ at project FPS.
        const bool pointer_drag = QApplication::mouseButtons() != Qt::NoButton;
        const bool runtime_dynamic = !playing_ && title_has_dynamic_text_layer(title_);
        if (!pointer_drag && active_text_edit_layer_id_.empty() && !runtime_dynamic)
            return;

        if (canvas_) {
            if (runtime_dynamic)
                canvas_->refresh_runtime_dynamic_content();
            else
                canvas_->update();
        }
        if (timeline_ && pointer_drag && timeline_->underMouse())
            timeline_->update();
    });

    layout_settle_timer_ = new QTimer(this);
    layout_settle_timer_->setSingleShot(true);
    layout_settle_timer_->setInterval(90);
    connect(layout_settle_timer_, &QTimer::timeout, this, [this]() {
        dock_layout_transition_ = false;
        update_display_refresh_pacing();
        if (canvas_ && isVisible() && !isMinimized())
            canvas_->update();
    });

    cache_invalidation_timer_ = new QTimer(this);
    cache_invalidation_timer_->setSingleShot(true);
    cache_invalidation_timer_->setInterval(180);
    connect(cache_invalidation_timer_, &QTimer::timeout, this, [this]() {
        CacheManager::instance().setInteractiveBypass(false);
        if (!title_)
            return;
        CacheManager::instance().invalidateAll(title_);
        CacheManager::instance().reprioritize(title_, playhead_);
        /* Editing invalidates every affected frame. Restore the editor's
         * full-timeline prerender intent instead of leaving only a small
         * playhead neighbourhood queued. */
        CacheManager::instance().queueWholeTimeline(title_);
        if (timeline_) timeline_->update();
    });

    status_activity_timer_ = new QTimer(this);
    status_activity_timer_->setSingleShot(true);
    connect(status_activity_timer_, &QTimer::timeout, this,
            &TitleEditor::update_statusbar_autosave_summary);

    /* Inline typing owns its canvas refresh already. Rebuilding the complete
     * Properties panel, Timeline model and persisted live title on every
     * character was a much larger cost than shaping the new glyph itself.
     * Keep live output responsive, but coalesce those secondary consumers. */
    inline_text_panel_refresh_timer_ = new QTimer(this);
    inline_text_panel_refresh_timer_->setSingleShot(true);
    inline_text_panel_refresh_timer_->setInterval(120);
    connect(inline_text_panel_refresh_timer_, &QTimer::timeout, this, [this]() {
        if (shutting_down_ || !title_ ||
            pending_inline_text_refresh_layer_id_.empty())
            return;
        const std::string layer_id = pending_inline_text_refresh_layer_id_;
        pending_inline_text_refresh_layer_id_.clear();
        if (active_text_edit_layer_id_ != layer_id)
            return;
        if (auto layer = title_->find_layer(layer_id))
            update_layer_panels(layer, playhead_);
    });

    inline_text_live_publish_timer_ = new QTimer(this);
    inline_text_live_publish_timer_->setSingleShot(true);
    inline_text_live_publish_timer_->setInterval(40);
    connect(inline_text_live_publish_timer_, &QTimer::timeout, this, [this]() {
        if (!shutting_down_ && live_editing_ && title_ &&
            !active_text_edit_layer_id_.empty())
            save_live_edit();
    });

    autosave_timer_ = new QTimer(this);
    autosave_timer_->setSingleShot(false);
    connect(autosave_timer_, &QTimer::timeout, this, &TitleEditor::perform_autosave);
    update_autosave_timer();

    clock_timer_ = new QTimer(this);
    // Dynamic text does not need to wake the editor/OBS render path at 30 Hz.
    // A 10 Hz UI refresh is smooth for clocks while avoiding constant canvas invalidation.
    clock_timer_->setInterval(100);
    connect(clock_timer_, &QTimer::timeout, this, [this]() {
        update_title_bar();
    });
    clock_timer_->start();

    qApp->installEventFilter(this);
    QTimer::singleShot(0, this, &TitleEditor::update_display_refresh_pacing);
}

TitleEditor::~TitleEditor()
{
    begin_shutdown();
    save_sidebar_default_colors();
    save_new_layer_defaults();
    save_editor_layout();
}

void TitleEditor::begin_shutdown()
{
    if (shutting_down_)
        return;
    shutting_down_ = true;
    if (external_data_callback_id_ != 0) {
        ExternalDataManager::instance().remove_change_callback(
            external_data_callback_id_);
        external_data_callback_id_ = 0;
    }
    playing_ = false;
    if (canvas_) canvas_->set_playback_active(false);

    if (qApp)
        qApp->removeEventFilter(this);

    const auto stop_timer = [](QTimer *timer) {
        if (timer)
            timer->stop();
    };
    stop_timer(play_timer_);
    stop_timer(gui_refresh_timer_);
    stop_timer(layout_settle_timer_);
    stop_timer(clock_timer_);
    stop_timer(cache_invalidation_timer_);
    stop_timer(autosave_timer_);
    stop_timer(status_activity_timer_);
    stop_timer(inline_text_panel_refresh_timer_);
    stop_timer(inline_text_live_publish_timer_);
    pending_inline_text_refresh_layer_id_.clear();
    for (QTimer *timer : findChildren<QTimer *>())
        timer->stop();

    /* Preserve QObject::destroyed/title_saved observers owned by TitleDock,
     * but stop every child/timer sender from entering TitleEditor slots during
     * the remainder of teardown. */
    const auto child_objects = findChildren<QObject *>();
    for (QObject *child : child_objects)
        disconnect(child, nullptr, this, nullptr);

    /* Tear down the cross-dock signal path before either endpoint begins Qt
     * child destruction.  This prevents queued/final value notifications from
     * rebuilding effect handles while the native OBS canvas display is being
     * removed. */
    if (effects_panel_ && canvas_) {
        disconnect(effects_panel_, nullptr, canvas_, nullptr);
        disconnect(canvas_, nullptr, effects_panel_, nullptr);
    }
    if (effects_panel_)
        effects_panel_->begin_shutdown();
    if (canvas_)
        canvas_->prepare_for_shutdown();

    CacheManager::instance().setInteractiveBypass(false);
    CacheManager::instance().setEditorPrerenderFocus(QString(), false);
}




QWidget *TitleEditor::create_effects_panel()
{
    effects_panel_ = new EffectsPanel(this);
    connect(effects_panel_, &EffectsPanel::property_changed,
            this, [this](bool push_undo_snapshot) {
                if (updating_layer_panels_)
                    return;
                // Every EffectsPanel notification represents a real visual-model edit.
                // Bypass the selection-only visual hash guard so enable/disable and
                // stack changes always invalidate prerender/frame caches immediately.
                force_next_title_visual_update();
                on_title_modified(push_undo_snapshot);
                if (layers_) layers_->refresh();
            });

    /* Dock panels are constructed after the central canvas.  Connecting only
     * from the canvas-construction branch therefore left these signals entirely
     * disconnected in the normal editor startup order. */
    if (canvas_) {
        connect(effects_panel_, &EffectsPanel::extension_canvas_handles_changed,
                canvas_, &CanvasPreview::set_extension_canvas_handles,
                Qt::UniqueConnection);
        connect(canvas_, &CanvasPreview::extension_canvas_handle_moved,
                effects_panel_, &EffectsPanel::set_extension_canvas_handle_position,
                Qt::UniqueConnection);
        canvas_->set_extension_canvas_handles(effects_panel_->extension_canvas_handles());
    }
    return effects_panel_;
}

