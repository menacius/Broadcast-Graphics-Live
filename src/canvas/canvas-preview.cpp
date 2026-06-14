#include "title-editor-internal.h"

namespace {
constexpr int kCanvasRulerThickness = 24;
constexpr double kCanvasGuideHitTolerancePx = 5.0;
constexpr const char *kEditorRulersVisibleKey = "rulersVisible";
constexpr const char *kEditorGuidesVisibleKey = "guidesVisible";
constexpr const char *kEditorGuidesLockedKey = "guidesLocked";
constexpr const char *kEditorGuideCoordinatesVisibleKey = "guideCoordinatesVisible";
constexpr const char *kEditorVerticalGuidesKey = "verticalGuides";
constexpr const char *kEditorHorizontalGuidesKey = "horizontalGuides";

QStringList guide_values_to_strings(const std::vector<double> &values)
{
    QStringList out;
    for (double v : values)
        if (std::isfinite(v)) out << QString::number(v, 'f', 3);
    return out;
}

std::vector<double> guide_values_from_strings(const QStringList &values)
{
    std::vector<double> out;
    out.reserve((size_t)values.size());
    for (const QString &value : values) {
        bool ok = false;
        const double v = value.toDouble(&ok);
        if (ok && std::isfinite(v)) out.push_back(v);
    }
    return out;
}
}

CanvasPreview::CanvasPreview(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(400, 225);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    load_ruler_guide_settings();

    inline_text_editor_ = new QTextEdit(this);
    inline_text_editor_->hide();
    inline_text_editor_->setAcceptRichText(true);
    inline_text_editor_->setFrameShape(QFrame::NoFrame);
    inline_text_editor_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    inline_text_editor_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    inline_text_editor_->setLineWrapMode(QTextEdit::FixedPixelWidth);
    inline_text_editor_->setCursorWidth(2);
    inline_text_editor_->setContentsMargins(0, 0, 0, 0);
    inline_text_editor_->viewport()->setContentsMargins(0, 0, 0, 0);
    inline_text_editor_->setAttribute(Qt::WA_TranslucentBackground, true);
    inline_text_editor_->viewport()->setAttribute(Qt::WA_TranslucentBackground, true);
    inline_text_editor_->setStyleSheet(
        "QTextEdit{background:transparent;border:0px;padding:0px;"
        "color:rgba(255,255,255,0);selection-background-color:rgba(255,255,255,0);"
        "selection-color:rgba(255,255,255,0);}");
    inline_text_editor_->installEventFilter(this);
    connect(inline_text_editor_->document(), &QTextDocument::contentsChanged, this, [this]() {
        if (updating_inline_text_editor_ || refreshing_inline_text_) return;
        refresh_inline_text_edit(true, true);
    });
    if (auto *layout = inline_text_editor_->document()->documentLayout()) {
        connect(layout, &QAbstractTextDocumentLayout::documentSizeChanged, this, [this](const QSizeF &) {
            if (updating_inline_text_editor_ || refreshing_inline_text_) return;
            refresh_inline_text_edit(true, true);
        });
        connect(layout, &QAbstractTextDocumentLayout::update, this, [this](const QRectF &) {
            if (committing_inline_text_ || updating_inline_text_editor_ || inline_text_layer_id_.empty()) return;
            if (inline_text_editor_)
                inline_text_editor_->viewport()->update();
        });
    }
    auto emit_cursor_changed = [this]() {
        if (committing_inline_text_ || updating_inline_text_editor_ || inline_text_layer_id_.empty()) return;
        const std::string layer_id = inline_text_layer_id_;
        sync_inline_text_layer(false);
        if (inline_text_editor_) {
            inline_text_editor_->viewport()->update();
            update(inline_text_editor_->geometry().adjusted(-4, -4, 4, 4));
        }
        emit text_edit_cursor_changed(layer_id);
    };
    connect(inline_text_editor_, &QTextEdit::cursorPositionChanged, this, emit_cursor_changed);
    connect(inline_text_editor_, &QTextEdit::selectionChanged, this, emit_cursor_changed);
}



void CanvasPreview::begin_text_edit_for_layer(const std::string &layer_id)
{
    if (!title_ || layer_id.empty()) return;
    auto layer = title_->find_layer(layer_id);
    if (!layer || !is_canvas_text_layer(*layer) || layer->type == LayerType::Clock) return;
    selected_layer_ids_ = {layer_id};
    sel_layer_id_ = layer_id;
    begin_text_edit(layer);
}

void CanvasPreview::apply_active_text_char_format(const std::string &layer_id, const RichTextCharFormat &format, uint32_t mask)
{
    if (!inline_text_editor_ || inline_text_layer_id_.empty() || inline_text_layer_id_ != layer_id)
        return;
    auto layer = title_ ? title_->find_layer(layer_id) : nullptr;
    const double visual_scale = layer ? inline_text_visual_scale(*layer) : 1.0;
    QTextCharFormat qfmt;
    if (mask & (RichTextCharFontFamily | RichTextCharFontStyle | RichTextCharFontSize |
                RichTextCharBold | RichTextCharItalic | RichTextCharUnderline | RichTextCharStrikethrough)) {
        QFont font = inline_text_editor_->currentFont();
        if (mask & RichTextCharFontFamily) font.setFamily(QString::fromStdString(format.font_family));
        if (mask & RichTextCharFontStyle) font.setStyleName(QString::fromStdString(format.font_style));
        if (mask & RichTextCharFontSize) font.setPixelSize(std::max(1, (int)std::round(format.font_size * visual_scale)));
        if (mask & RichTextCharBold) font.setBold(format.bold);
        if (mask & RichTextCharItalic) font.setItalic(format.italic);
        if (mask & RichTextCharUnderline) font.setUnderline(format.underline);
        if (mask & RichTextCharStrikethrough) font.setStrikeOut(format.strikethrough);
        qfmt.setFont(font);
    }
    if (mask & RichTextCharUnderline) qfmt.setFontUnderline(format.underline);
    if (mask & RichTextCharStrikethrough) qfmt.setFontStrikeOut(format.strikethrough);
    if (mask & RichTextCharKerning) qfmt.setFontKerning(format.kerning_mode != 2 && format.kerning);
    if (mask & (RichTextCharKerning | RichTextCharTracking)) {
        qfmt.setFontLetterSpacingType(QFont::AbsoluteSpacing);
        qfmt.setFontLetterSpacing(format.tracking +
                                  (format.kerning_mode == 2 ? format.manual_kerning : 0.0f));
    }
    if (mask & RichTextCharScaleX)
        qfmt.setFontStretch(std::clamp((int)std::round(format.scale_x * 100.0f), 1, 4000));
    if (mask & RichTextCharTextStyle) {
        qfmt.setFontCapitalization(format.text_style == 1 ? QFont::AllUppercase
                                  : (format.text_style == 2 ? QFont::SmallCaps : QFont::MixedCase));
        if (format.text_style == 3)
            qfmt.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
        else if (format.text_style == 4)
            qfmt.setVerticalAlignment(QTextCharFormat::AlignSubScript);
        else
            qfmt.setVerticalAlignment(QTextCharFormat::AlignNormal);
    }
    store_editor_rich_text_format_properties_masked(qfmt, format, mask);
    if (mask & RichTextCharFillColor) {
        QColor transparent_color = rich_text_color_from_argb(format.fill.color);
        transparent_color.setAlpha(0);
        qfmt.setForeground(transparent_color);
    }

    QTextCursor cursor = inline_text_editor_->textCursor();
    cursor.mergeCharFormat(qfmt);
    inline_text_editor_->mergeCurrentCharFormat(qfmt);
    inline_text_editor_->setTextCursor(cursor);
    if (layer) {
        layer->rich_text.selection = {(size_t)std::max(0, cursor.anchor()), (size_t)std::max(0, cursor.position())};
        if (!cursor.hasSelection()) {
            layer->rich_text.typing_format = rich_text_format_from_qtext_format(cursor.charFormat(),
                                                                               layer->rich_text.default_format,
                                                                               visual_scale);
            layer->rich_text.has_typing_format = true;
        }
    }
    refresh_inline_text_edit(true, true);
}

void CanvasPreview::set_title(std::shared_ptr<Title> t, bool preserve_view)
{
    commit_text_edit(true);
    title_ = t;
    dirty_ = true;
    if (!preserve_view) {
        pan_offset_ = QPointF(0, 0);
        if (title_) fit_canvas(fit_zoom_up_to_100_);
        else update();
    } else {
        update();
    }
    position_text_editor();
}

CanvasPreview::ViewState CanvasPreview::view_state() const
{
    return ViewState{zoom_percent_, fit_zoom_active_, fit_zoom_up_to_100_, pan_offset_};
}

void CanvasPreview::restore_view_state(const ViewState &state)
{
    zoom_percent_ = std::clamp(state.zoom_percent, 5, 1600);
    fit_zoom_active_ = state.fit_zoom_active;
    fit_zoom_up_to_100_ = state.fit_zoom_up_to_100;
    pan_offset_ = state.pan_offset;
    dirty_ = true;
    emit zoom_percent_changed(zoom_percent_);
    position_text_editor();
    update();
}

void CanvasPreview::set_playhead(double t)
{
    playhead_ = t; dirty_ = true; position_text_editor(); update();
}

void CanvasPreview::set_selected_layer(const std::string &lid)
{
    sel_layer_id_ = lid;
    selected_layer_ids_.clear();
    if (!lid.empty()) selected_layer_ids_.push_back(lid);
    position_text_editor();
    update();
}

void CanvasPreview::set_selected_layers(const std::vector<std::string> &ids)
{
    selected_layer_ids_ = ids;
    sel_layer_id_ = ids.empty() ? std::string() : ids.back();
    position_text_editor();
    update();
}

void CanvasPreview::set_safe_guides_visible(bool visible)
{
    safe_guides_visible_ = visible;
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));
    settings.setValue(QString::fromUtf8(kEditorSafeGuidesVisibleKey), visible);
    settings.endGroup();
    settings.sync();
    update();
}


void CanvasPreview::load_ruler_guide_settings()
{
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));
    rulers_visible_ = settings.value(QString::fromUtf8(kEditorRulersVisibleKey), false).toBool();
    guides_visible_ = settings.value(QString::fromUtf8(kEditorGuidesVisibleKey), true).toBool();
    guides_locked_ = settings.value(QString::fromUtf8(kEditorGuidesLockedKey), false).toBool();
    show_guide_coordinates_ = settings.value(QString::fromUtf8(kEditorGuideCoordinatesVisibleKey), true).toBool();
    vertical_guides_ = guide_values_from_strings(settings.value(QString::fromUtf8(kEditorVerticalGuidesKey)).toStringList());
    horizontal_guides_ = guide_values_from_strings(settings.value(QString::fromUtf8(kEditorHorizontalGuidesKey)).toStringList());
    settings.endGroup();
}

void CanvasPreview::save_ruler_guide_settings() const
{
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));
    settings.setValue(QString::fromUtf8(kEditorRulersVisibleKey), rulers_visible_);
    settings.setValue(QString::fromUtf8(kEditorGuidesVisibleKey), guides_visible_);
    settings.setValue(QString::fromUtf8(kEditorGuidesLockedKey), guides_locked_);
    settings.setValue(QString::fromUtf8(kEditorGuideCoordinatesVisibleKey), show_guide_coordinates_);
    settings.setValue(QString::fromUtf8(kEditorVerticalGuidesKey), guide_values_to_strings(vertical_guides_));
    settings.setValue(QString::fromUtf8(kEditorHorizontalGuidesKey), guide_values_to_strings(horizontal_guides_));
    settings.endGroup();
    settings.sync();
}

void CanvasPreview::set_rulers_visible(bool visible)
{
    if (rulers_visible_ == visible) return;
    rulers_visible_ = visible;
    save_ruler_guide_settings();
    position_text_editor();
    update();
}

void CanvasPreview::set_guides_visible(bool visible)
{
    if (guides_visible_ == visible) return;
    guides_visible_ = visible;
    save_ruler_guide_settings();
    update();
}

void CanvasPreview::set_guides_locked(bool locked)
{
    if (guides_locked_ == locked) return;
    guides_locked_ = locked;
    save_ruler_guide_settings();
    update();
}

void CanvasPreview::set_show_guide_coordinates(bool visible)
{
    if (show_guide_coordinates_ == visible) return;
    show_guide_coordinates_ = visible;
    save_ruler_guide_settings();
    update();
}

void CanvasPreview::clear_user_guides()
{
    if (vertical_guides_.empty() && horizontal_guides_.empty()) return;
    vertical_guides_.clear();
    horizontal_guides_.clear();
    clear_snap_feedback();
    save_ruler_guide_settings();
    update();
}

void CanvasPreview::refresh_preview()
{
    dirty_ = true;
    position_text_editor();
    if (!inline_text_layer_id_.empty())
        render_to_pixmap();
    update();
    if (inline_text_editor_ && inline_text_editor_->isVisible()) {
        inline_text_editor_->viewport()->update();
        repaint(inline_text_editor_->geometry().adjusted(-4, -4, 4, 4));
    }
}


void CanvasPreview::set_snap_enabled(bool enabled)
{
    snap_settings_.enabled = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_snap_to_guides(bool enabled)
{
    snap_settings_.guides = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_snap_to_grid(bool enabled)
{
    snap_settings_.grid = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_snap_to_object_edges(bool enabled)
{
    snap_settings_.object_edges = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_snap_to_object_centers(bool enabled)
{
    snap_settings_.object_centers = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_snap_to_canvas_bounds(bool enabled)
{
    snap_settings_.canvas_bounds = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_snap_to_spacing(bool enabled)
{
    snap_settings_.spacing = enabled;
    if (!enabled) clear_snap_feedback();
    update();
}

void CanvasPreview::set_zoom_percent(int percent)
{
    int clamped = std::clamp(percent, 5, 1600);
    if (zoom_percent_ == clamped && !fit_zoom_active_) return;
    zoom_percent_ = clamped;
    fit_zoom_active_ = false;
    emit zoom_percent_changed(zoom_percent_);
    position_text_editor();
    update();
}

int CanvasPreview::zoom_percent() const
{
    return zoom_percent_;
}

void CanvasPreview::set_checkerboard_pattern(int pattern)
{
    checkerboard_pattern_ = std::clamp(pattern, 0, 5);
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));
    settings.setValue(QString::fromUtf8(kEditorCanvasTransparencyKey), checkerboard_pattern_);
    settings.endGroup();
    settings.sync();
    update();
}

void CanvasPreview::set_selection_tool_active()
{
    commit_text_edit(true);
    active_tool_ = CanvasTool::Selection;
    drawing_shape_ = false;
    color_picker_tooltip_visible_ = false;
    unsetCursor();
    update();
}

void CanvasPreview::set_shape_tool_active(ShapeType shape_type)
{
    active_tool_ = CanvasTool::Shape;
    active_shape_type_ = shape_type;
    drawing_shape_ = false;
    color_picker_tooltip_visible_ = false;
    setCursor(Qt::CrossCursor);
    update();
}


void CanvasPreview::set_text_tool_active(LayerType type)
{
    if (type != LayerType::Text && type != LayerType::Clock && type != LayerType::Ticker)
        type = LayerType::Text;
    active_tool_ = CanvasTool::Text;
    active_text_layer_type_ = type;
    drawing_shape_ = false;
    color_picker_tooltip_visible_ = false;
    setCursor(Qt::IBeamCursor);
    update();
}

void CanvasPreview::set_color_picker_tool_active()
{
    commit_text_edit(true);
    active_tool_ = CanvasTool::ColorPicker;
    drawing_shape_ = false;
    color_picker_tooltip_visible_ = false;
    setCursor(Qt::CrossCursor);
    update();
}

void CanvasPreview::fit_canvas(bool up_to_100)
{
    fit_zoom_active_ = true;
    fit_zoom_up_to_100_ = up_to_100;
    pan_offset_ = QPointF(0, 0);
    double scale = fit_scale();
    if (up_to_100) scale = std::min(scale, 1.0);
    int next_percent = std::clamp((int)std::round(scale * 100.0), 5, 1600);
    if (zoom_percent_ != next_percent) {
        zoom_percent_ = next_percent;
        emit zoom_percent_changed(zoom_percent_);
    }
    update();
}

std::shared_ptr<Layer> CanvasPreview::selected_layer() const
{
    return title_ ? title_->find_layer(sel_layer_id_) : nullptr;
}

std::vector<std::shared_ptr<Layer>> CanvasPreview::selected_layers() const
{
    std::vector<std::shared_ptr<Layer>> layers;
    if (!title_) return layers;
    if (selected_layer_ids_.empty()) {
        if (auto layer = selected_layer()) layers.push_back(layer);
        return layers;
    }
    std::set<std::string> seen;
    for (const auto &id : selected_layer_ids_) {
        if (!seen.insert(id).second) continue;
        auto layer = title_->find_layer(id);
        if (layer) layers.push_back(layer);
    }
    return layers;
}

QRectF CanvasPreview::layer_local_rect(const Layer &layer) const
{
    double lt = playhead_ - layer.in_time;
    double w = eval_box_width(layer, lt);
    double h = eval_box_height(layer, lt);
    double ox = eval_origin_x(layer, lt);
    double oy = eval_origin_y(layer, lt);
    return QRectF(-ox * w, -oy * h, w, h);
}

double CanvasPreview::fit_scale() const
{
    if (!title_ || title_->width <= 0 || title_->height <= 0) return 1.0;
    const double available_width = std::max(1.0, (double)width() - (rulers_visible_ ? kCanvasRulerThickness : 0));
    const double available_height = std::max(1.0, (double)height() - (rulers_visible_ ? kCanvasRulerThickness : 0));
    return std::min(available_width / title_->width,
                    available_height / title_->height);
}

double CanvasPreview::view_scale() const
{
    return std::max(0.05, (double)zoom_percent_ / 100.0);
}

QPointF CanvasPreview::centered_view_origin() const
{
    if (!title_) return QPointF(0, 0);
    double scale = view_scale();
    const double left_reserved = rulers_visible_ ? kCanvasRulerThickness : 0.0;
    const double top_reserved = rulers_visible_ ? kCanvasRulerThickness : 0.0;
    const double available_width = std::max(1.0, (double)width() - left_reserved);
    const double available_height = std::max(1.0, (double)height() - top_reserved);
    return QPointF(left_reserved + (available_width - title_->width * scale) / 2.0,
                   top_reserved + (available_height - title_->height * scale) / 2.0);
}

QPointF CanvasPreview::view_origin() const
{
    return centered_view_origin() + pan_offset_;
}

QPointF CanvasPreview::view_to_canvas(const QPointF &view_pt) const
{
    double scale = view_scale();
    QPointF origin = view_origin();
    return QPointF((view_pt.x() - origin.x()) / scale,
                   (view_pt.y() - origin.y()) / scale);
}

QPointF CanvasPreview::canvas_to_view(const QPointF &canvas_pt) const
{
    double scale = view_scale();
    QPointF origin = view_origin();
    return QPointF(origin.x() + canvas_pt.x() * scale,
                   origin.y() + canvas_pt.y() * scale);
}

static const Layer *editor_find_layer_by_id(const std::shared_ptr<Title> &title, const std::string &id)
{
    if (!title || id.empty()) return nullptr;
    for (const auto &candidate : title->layers) {
        if (candidate && candidate->id == id) return candidate.get();
    }
    return nullptr;
}

static QTransform editor_layer_world_transform(const std::shared_ptr<Title> &title,
                                               const Layer &layer, double playhead, int depth = 0)
{
    QTransform xf;
    if (depth > 64) return xf;
    if (!layer.parent_id.empty()) {
        if (const Layer *parent = editor_find_layer_by_id(title, layer.parent_id))
            xf = editor_layer_world_transform(title, *parent, playhead, depth + 1);
    }
    const double lt = std::max(0.0, playhead - layer.in_time);
    xf.translate(layer.pos_x.evaluate(lt), layer.pos_y.evaluate(lt));
    xf.rotate(layer.rotation.evaluate(lt));
    xf.scale(layer.scale_x.evaluate(lt), layer.scale_y.evaluate(lt));
    return xf;
}

QPointF CanvasPreview::canvas_to_layer(const Layer &layer, const QPointF &canvas_pt) const
{
    return editor_layer_world_transform(title_, layer, playhead_).inverted().map(canvas_pt);
}

QPointF CanvasPreview::layer_to_canvas(const Layer &layer, const QPointF &layer_pt) const
{
    return editor_layer_world_transform(title_, layer, playhead_).map(layer_pt);
}

QRectF CanvasPreview::layer_canvas_bounds(const Layer &layer) const
{
    QRectF r = layer_local_rect(layer);
    const QPointF corners[] = {r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()};

    double min_x = std::numeric_limits<double>::infinity();
    double min_y = std::numeric_limits<double>::infinity();
    double max_x = -std::numeric_limits<double>::infinity();
    double max_y = -std::numeric_limits<double>::infinity();

    for (const QPointF &corner : corners) {
        QPointF canvas = layer_to_canvas(layer, corner);
        min_x = std::min(min_x, canvas.x());
        min_y = std::min(min_y, canvas.y());
        max_x = std::max(max_x, canvas.x());
        max_y = std::max(max_y, canvas.y());
    }

    if (!std::isfinite(min_x) || !std::isfinite(min_y) ||
        !std::isfinite(max_x) || !std::isfinite(max_y))
        return QRectF();

    return QRectF(QPointF(min_x, min_y), QPointF(max_x, max_y)).normalized();
}

QRectF CanvasPreview::selected_canvas_bounds() const
{
    QRectF bounds;
    bool have_bounds = false;
    for (auto &layer : selected_layers()) {
        if (!layer || !layer->visible) continue;
        QRectF layer_bounds = layer_canvas_bounds(*layer);
        if (!layer_bounds.isValid()) continue;
        if (!have_bounds) {
            bounds = layer_bounds;
            have_bounds = true;
        } else {
            bounds = bounds.united(layer_bounds);
        }
    }
    return bounds.normalized();
}

bool CanvasPreview::layer_supports_gradient_handles(const Layer &layer) const
{
    if (layer.locked || !layer.visible || layer.fill_type != 1)
        return false;
    if (playhead_ < layer.in_time || playhead_ > layer.out_time)
        return false;
    return layer.type == LayerType::SolidRect || layer.type == LayerType::Shape ||
           is_canvas_text_layer(layer);
}

CanvasPreview::GradientHandleGeometry CanvasPreview::gradient_handle_geometry(const Layer &layer) const
{
    GradientHandleGeometry g;
    if (!layer_supports_gradient_handles(layer))
        return g;

    const QRectF box = layer_local_rect(layer);
    if (!box.isValid() || box.width() <= 0.0 || box.height() <= 0.0)
        return g;

    g.valid = true;
    g.radial = layer.gradient_type == 1;
    g.local_rect = box;
    g.center = QPointF(box.left() + std::clamp((double)layer.gradient_center_x, 0.0, 1.0) * box.width(),
                       box.top() + std::clamp((double)layer.gradient_center_y, 0.0, 1.0) * box.height());

    const double scale = std::clamp((double)layer.gradient_scale, 0.01, 10.0);
    const double angle = degrees_to_radians(layer.gradient_angle);
    const QPointF axis(std::cos(angle), std::sin(angle));
    if (g.radial) {
        const double radius = std::max(box.width(), box.height()) * 0.5 * scale;
        g.radius = g.center + axis * std::max(1.0, radius);
        g.focal = QPointF(box.left() + std::clamp((double)layer.gradient_focal_x, 0.0, 1.0) * box.width(),
                          box.top() + std::clamp((double)layer.gradient_focal_y, 0.0, 1.0) * box.height());
        g.start = g.center;
        g.end = g.radius;
    } else {
        const double length = std::hypot(box.width(), box.height()) * 0.5 * scale;
        const QPointF delta = axis * std::max(1.0, length);
        g.start = g.center - delta;
        g.end = g.center + delta;
        g.radius = g.end;
        g.focal = g.center;
    }
    return g;
}

CanvasPreview::DragMode CanvasPreview::hit_test_gradient_handles(const Layer &layer, const QPointF &view_pt) const
{
    GradientHandleGeometry g = gradient_handle_geometry(layer);
    if (!g.valid)
        return DragMode::None;

    auto near_local = [&](const QPointF &local, double radius = CANVAS_CONTROL_HIT_RADIUS_PX) {
        const QPointF view = canvas_to_view(layer_to_canvas(layer, local));
        return QLineF(view_pt, view).length() <= radius;
    };

    if (g.radial) {
        if (near_local(g.focal, CANVAS_CONTROL_HIT_RADIUS_PX * 1.2)) return DragMode::GradientFocal;
        if (near_local(g.radius, CANVAS_CONTROL_HIT_RADIUS_PX * 1.2)) return DragMode::GradientRadius;
        if (near_local(g.center, CANVAS_CONTROL_HIT_RADIUS_PX * 1.35)) return DragMode::GradientCenter;
    } else {
        if (near_local(g.start, CANVAS_CONTROL_HIT_RADIUS_PX * 1.2)) return DragMode::GradientStart;
        if (near_local(g.end, CANVAS_CONTROL_HIT_RADIUS_PX * 1.2)) return DragMode::GradientEnd;
        if (near_local(g.center, CANVAS_CONTROL_HIT_RADIUS_PX * 1.35)) return DragMode::GradientCenter;
    }
    return DragMode::None;
}

void CanvasPreview::draw_gradient_handles(QPainter &p, const Layer &layer)
{
    GradientHandleGeometry g = gradient_handle_geometry(layer);
    if (!g.valid)
        return;

    auto to_view = [&](const QPointF &local) {
        return canvas_to_view(layer_to_canvas(layer, local));
    };
    auto draw_handle = [&](const QPointF &view_pt, const QColor &fill, double radius = CANVAS_GRADIENT_HANDLE_RADIUS_PX) {
        p.setPen(QPen(QColor(10, 10, 10, 210), 3.0));
        p.setBrush(fill);
        p.drawEllipse(view_pt, radius, radius);
        p.setPen(QPen(QColor(255, 255, 255, 235), 1.0));
        p.drawEllipse(view_pt, radius, radius);
    };

    const QPointF center = to_view(g.center);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen guide_pen(QColor(255, 255, 255, 215), 1.4, Qt::SolidLine);
    guide_pen.setCosmetic(true);
    p.setPen(guide_pen);
    p.setBrush(Qt::NoBrush);

    if (g.radial) {
        const QPointF radius = to_view(g.radius);
        const QPointF focal = to_view(g.focal);
        p.drawLine(center, radius);
        QPen focal_pen(QColor(255, 215, 80, 210), 1.2, Qt::DashLine);
        focal_pen.setCosmetic(true);
        p.setPen(focal_pen);
        p.drawLine(center, focal);
        draw_handle(center, QColor(0, 145, 255));
        draw_handle(radius, QColor(255, 255, 255));
        draw_handle(focal, QColor(255, 210, 60), CANVAS_GRADIENT_HANDLE_RADIUS_PX - 0.5);
    } else {
        const QPointF start = to_view(g.start);
        const QPointF end = to_view(g.end);
        p.drawLine(start, end);
        draw_handle(start, QColor(255, 255, 255));
        draw_handle(end, QColor(255, 255, 255));
        draw_handle(center, QColor(0, 145, 255));
    }
    p.restore();
}

void CanvasPreview::begin_gradient_drag(const Layer &layer)
{
    GradientHandleGeometry g = gradient_handle_geometry(layer);
    gradient_drag_ = GradientDragState{};
    if (!g.valid)
        return;

    gradient_drag_.active = true;
    gradient_drag_.radial = g.radial;
    gradient_drag_.local_rect = g.local_rect;
    gradient_drag_.center = g.center;
    gradient_drag_.start = g.start;
    gradient_drag_.end = g.end;
    gradient_drag_.radius = g.radius;
    gradient_drag_.focal = g.focal;
    gradient_drag_.center_x = layer.gradient_center_x;
    gradient_drag_.center_y = layer.gradient_center_y;
    gradient_drag_.focal_x = layer.gradient_focal_x;
    gradient_drag_.focal_y = layer.gradient_focal_y;
    gradient_drag_.scale = layer.gradient_scale;
    gradient_drag_.angle = layer.gradient_angle;
}

bool CanvasPreview::apply_gradient_drag(const QPointF &view_pt, Qt::KeyboardModifiers modifiers)
{
    if (!gradient_drag_.active)
        return false;
    if (drag_mode_ != DragMode::GradientStart && drag_mode_ != DragMode::GradientEnd &&
        drag_mode_ != DragMode::GradientCenter && drag_mode_ != DragMode::GradientRadius &&
        drag_mode_ != DragMode::GradientFocal)
        return false;

    auto layer = selected_layer();
    if (!layer || layer->locked)
        return false;

    const QRectF box = gradient_drag_.local_rect;
    if (!box.isValid() || box.width() <= 0.0 || box.height() <= 0.0)
        return false;

    const QPointF local = canvas_to_layer(*layer, view_to_canvas(view_pt));
    auto normalized = [&](const QPointF &pt) {
        return QPointF(std::clamp((pt.x() - box.left()) / box.width(), 0.0, 1.0),
                       std::clamp((pt.y() - box.top()) / box.height(), 0.0, 1.0));
    };
    auto assign_center = [&](const QPointF &pt) {
        const QPointF n = normalized(pt);
        layer->gradient_center_x = (float)n.x();
        layer->gradient_center_y = (float)n.y();
    };
    auto assign_focal = [&](const QPointF &pt) {
        const QPointF n = normalized(pt);
        layer->gradient_focal_x = (float)n.x();
        layer->gradient_focal_y = (float)n.y();
    };
    auto assign_axis = [&](const QPointF &a, const QPointF &b) {
        const QPointF c((a.x() + b.x()) * 0.5, (a.y() + b.y()) * 0.5);
        assign_center(c);
        const QPointF delta = b - a;
        const double distance = std::max(1.0, QLineF(a, b).length());
        double angle = radians_to_degrees(std::atan2(delta.y(), delta.x()));
        if (modifiers.testFlag(Qt::ShiftModifier))
            angle = std::round(angle / CANVAS_ROTATION_SNAP_DEGREES) * CANVAS_ROTATION_SNAP_DEGREES;
        layer->gradient_angle = (float)normalize_degrees(angle);
        const double base = std::max(1.0, std::hypot(box.width(), box.height()));
        layer->gradient_scale = (float)std::clamp(distance / base, 0.01, 10.0);
    };

    clear_snap_feedback();
    if (drag_mode_ == DragMode::GradientCenter) {
        const QPointF delta = local - gradient_drag_.center;
        assign_center(gradient_drag_.center + delta);
        if (gradient_drag_.radial)
            assign_focal(gradient_drag_.focal + delta);
    } else if (drag_mode_ == DragMode::GradientFocal) {
        assign_focal(local);
    } else if (drag_mode_ == DragMode::GradientRadius) {
        assign_center(gradient_drag_.center);
        const QPointF delta = local - gradient_drag_.center;
        const double radius = std::max(1.0, QLineF(gradient_drag_.center, local).length());
        const double base = std::max(1.0, std::max(box.width(), box.height()) * 0.5);
        double angle = radians_to_degrees(std::atan2(delta.y(), delta.x()));
        if (modifiers.testFlag(Qt::ShiftModifier))
            angle = std::round(angle / CANVAS_ROTATION_SNAP_DEGREES) * CANVAS_ROTATION_SNAP_DEGREES;
        layer->gradient_angle = (float)normalize_degrees(angle);
        layer->gradient_scale = (float)std::clamp(radius / base, 0.01, 10.0);
    } else if (drag_mode_ == DragMode::GradientStart) {
        assign_axis(local, gradient_drag_.end);
    } else if (drag_mode_ == DragMode::GradientEnd) {
        assign_axis(gradient_drag_.start, local);
    }

    dirty_ = true;
    drag_changed_ = true;
    drag_current_view_ = view_pt;
    update();
    return true;
}

bool CanvasPreview::layer_supports_corner_radius_handles(const Layer &layer) const
{
    if (layer.locked || !layer.visible || layer.type != LayerType::Shape)
        return false;
    if (playhead_ < layer.in_time || playhead_ > layer.out_time)
        return false;
    return layer.shape_type == ShapeType::Rectangle || layer.shape_type == ShapeType::RoundedRectangle;
}

QPointF CanvasPreview::corner_radius_handle_local_pos(const Layer &layer, const QRectF &box, DragMode mode) const
{
    const double max_radius = std::max(0.0, std::min(box.width(), box.height()) / 2.0);
    auto radius = [max_radius](float value) {
        return std::clamp((double)value, 0.0, max_radius);
    };
    switch (mode) {
    case DragMode::CornerRadiusTL: {
        const double r = radius(layer.corner_radius_tl);
        return QPointF(box.left() + r, box.top() + r);
    }
    case DragMode::CornerRadiusTR: {
        const double r = radius(layer.corner_radius_tr);
        return QPointF(box.right() - r, box.top() + r);
    }
    case DragMode::CornerRadiusBR: {
        const double r = radius(layer.corner_radius_br);
        return QPointF(box.right() - r, box.bottom() - r);
    }
    case DragMode::CornerRadiusBL: {
        const double r = radius(layer.corner_radius_bl);
        return QPointF(box.left() + r, box.bottom() - r);
    }
    default:
        return QPointF();
    }
}

CanvasPreview::DragMode CanvasPreview::hit_test_corner_radius_handles(const Layer &layer, const QPointF &view_pt) const
{
    if (!layer_supports_corner_radius_handles(layer))
        return DragMode::None;
    const QRectF box = layer_local_rect(layer);
    if (!box.isValid() || box.width() <= 0.0 || box.height() <= 0.0)
        return DragMode::None;
    for (DragMode mode : {DragMode::CornerRadiusTL, DragMode::CornerRadiusTR,
                          DragMode::CornerRadiusBR, DragMode::CornerRadiusBL}) {
        const QPointF view = canvas_to_view(layer_to_canvas(layer, corner_radius_handle_local_pos(layer, box, mode)));
        if (QLineF(view_pt, view).length() <= CANVAS_CONTROL_HIT_RADIUS_PX * 1.25)
            return mode;
    }
    return DragMode::None;
}

void CanvasPreview::draw_corner_radius_handles(QPainter &p, const Layer &layer)
{
    if (!layer_supports_corner_radius_handles(layer))
        return;
    const QRectF box = layer_local_rect(layer);
    if (!box.isValid() || box.width() <= 0.0 || box.height() <= 0.0)
        return;

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen guide_pen(QColor(255, 255, 255, 180), 1.0, Qt::DashLine);
    guide_pen.setCosmetic(true);
    p.setPen(guide_pen);
    p.setBrush(Qt::NoBrush);

    auto to_view = [&](const QPointF &local) {
        return canvas_to_view(layer_to_canvas(layer, local));
    };
    auto draw_handle = [&](DragMode mode) {
        const QPointF handle = corner_radius_handle_local_pos(layer, box, mode);
        QPointF corner;
        switch (mode) {
        case DragMode::CornerRadiusTL: corner = box.topLeft(); break;
        case DragMode::CornerRadiusTR: corner = box.topRight(); break;
        case DragMode::CornerRadiusBR: corner = box.bottomRight(); break;
        case DragMode::CornerRadiusBL: corner = box.bottomLeft(); break;
        default: return;
        }
        p.drawLine(to_view(corner), to_view(handle));
        const QPointF view = to_view(handle);
        p.setPen(QPen(QColor(25, 25, 25, 230), 3.0));
        p.setBrush(QColor(255, 255, 255, 245));
        p.drawEllipse(view, 5.0, 5.0);
        p.setPen(QPen(QColor(0, 120, 255, 255), 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(view, 5.0, 5.0);
        p.setPen(guide_pen);
    };
    draw_handle(DragMode::CornerRadiusTL);
    draw_handle(DragMode::CornerRadiusTR);
    draw_handle(DragMode::CornerRadiusBR);
    draw_handle(DragMode::CornerRadiusBL);
    p.restore();
}

void CanvasPreview::begin_corner_radius_drag(const Layer &layer)
{
    corner_radius_drag_ = CornerRadiusDragState{};
    if (!layer_supports_corner_radius_handles(layer))
        return;
    const QRectF box = layer_local_rect(layer);
    if (!box.isValid() || box.width() <= 0.0 || box.height() <= 0.0)
        return;
    corner_radius_drag_.active = true;
    corner_radius_drag_.local_rect = box;
    corner_radius_drag_.top_left = layer.corner_radius_tl;
    corner_radius_drag_.top_right = layer.corner_radius_tr;
    corner_radius_drag_.bottom_right = layer.corner_radius_br;
    corner_radius_drag_.bottom_left = layer.corner_radius_bl;
    corner_radius_drag_.locked = layer.corner_radius_locked;
}

bool CanvasPreview::apply_corner_radius_drag(const QPointF &view_pt, Qt::KeyboardModifiers modifiers)
{
    if (!corner_radius_drag_.active)
        return false;
    if (drag_mode_ != DragMode::CornerRadiusTL && drag_mode_ != DragMode::CornerRadiusTR &&
        drag_mode_ != DragMode::CornerRadiusBR && drag_mode_ != DragMode::CornerRadiusBL)
        return false;
    auto layer = selected_layer();
    if (!layer || layer->locked)
        return false;

    const QRectF box = corner_radius_drag_.local_rect;
    const QPointF local = canvas_to_layer(*layer, view_to_canvas(view_pt));
    double radius = 0.0;
    switch (drag_mode_) {
    case DragMode::CornerRadiusTL:
        radius = std::min(local.x() - box.left(), local.y() - box.top());
        break;
    case DragMode::CornerRadiusTR:
        radius = std::min(box.right() - local.x(), local.y() - box.top());
        break;
    case DragMode::CornerRadiusBR:
        radius = std::min(box.right() - local.x(), box.bottom() - local.y());
        break;
    case DragMode::CornerRadiusBL:
        radius = std::min(local.x() - box.left(), box.bottom() - local.y());
        break;
    default:
        return false;
    }
    const double max_radius = std::max(0.0, std::min(box.width(), box.height()) / 2.0);
    radius = std::clamp(radius, 0.0, max_radius);
    if (modifiers.testFlag(Qt::ShiftModifier))
        radius = std::round(radius);

    if (corner_radius_drag_.locked || layer->corner_radius_locked) {
        set_layer_all_corner_radii(*layer, (float)radius);
        layer->corner_radius_locked = true;
    } else {
        switch (drag_mode_) {
        case DragMode::CornerRadiusTL: layer->corner_radius_tl = (float)radius; break;
        case DragMode::CornerRadiusTR: layer->corner_radius_tr = (float)radius; break;
        case DragMode::CornerRadiusBR: layer->corner_radius_br = (float)radius; break;
        case DragMode::CornerRadiusBL: layer->corner_radius_bl = (float)radius; break;
        default: break;
        }
        set_layer_corner_radii(*layer, layer->corner_radius_tl, layer->corner_radius_tr,
                               layer->corner_radius_br, layer->corner_radius_bl);
        layer->corner_radius_locked = false;
    }

    clear_snap_feedback();
    dirty_ = true;
    drag_changed_ = true;
    drag_current_view_ = view_pt;
    update();
    return true;
}

CanvasPreview::DragMode CanvasPreview::hit_test_selected(const QPointF &view_pt) const
{
    auto layers = selected_layers();
    if (layers.empty()) return DragMode::None;

    if (layers.size() > 1) {
        QRectF r = selected_canvas_bounds();
        if (!r.isValid() || r.isEmpty()) return DragMode::None;
        auto canvas_handle_to_view = [&](const QPointF &p) { return canvas_to_view(p); };
        auto near_pt = [&](const QPointF &p) {
            QPointF view = canvas_handle_to_view(p);
            return std::abs(view_pt.x() - view.x()) <= CANVAS_CONTROL_HIT_RADIUS_PX &&
                   std::abs(view_pt.y() - view.y()) <= CANVAS_CONTROL_HIT_RADIUS_PX;
        };
        QRectF view_bounds(canvas_to_view(r.topLeft()), canvas_to_view(r.bottomRight()));
        view_bounds = view_bounds.normalized();
        auto near_rotation_corner = [&](const QPointF &p) {
            return QLineF(view_pt, canvas_handle_to_view(p)).length() <= CANVAS_ROTATE_HIT_RADIUS_PX &&
                   !view_bounds.adjusted(-CANVAS_CONTROL_SIZE_PX, -CANVAS_CONTROL_SIZE_PX,
                                         CANVAS_CONTROL_SIZE_PX, CANVAS_CONTROL_SIZE_PX).contains(view_pt);
        };
        if (near_pt(r.topLeft())) return DragMode::ResizeNW;
        if (near_pt(QPointF(r.center().x(), r.top()))) return DragMode::ResizeN;
        if (near_pt(r.topRight())) return DragMode::ResizeNE;
        if (near_pt(QPointF(r.right(), r.center().y()))) return DragMode::ResizeE;
        if (near_pt(r.bottomRight())) return DragMode::ResizeSE;
        if (near_pt(QPointF(r.center().x(), r.bottom()))) return DragMode::ResizeS;
        if (near_pt(r.bottomLeft())) return DragMode::ResizeSW;
        if (near_pt(QPointF(r.left(), r.center().y()))) return DragMode::ResizeW;
        if (near_rotation_corner(r.topLeft()) || near_rotation_corner(r.topRight()) ||
            near_rotation_corner(r.bottomRight()) || near_rotation_corner(r.bottomLeft()))
            return DragMode::Rotate;
        QPointF canvas = view_to_canvas(view_pt);
        for (const auto &layer : layers) {
            if (!layer || layer->locked) continue;
            QPointF local = canvas_to_layer(*layer, canvas);
            if (layer_local_rect(*layer).contains(local))
                return DragMode::Move;
        }
        return DragMode::None;
    }

    auto layer = layers.front();
    if (!layer || layer->locked) return DragMode::None;

    DragMode gradient_hit = hit_test_gradient_handles(*layer, view_pt);
    if (gradient_hit != DragMode::None)
        return gradient_hit;
    DragMode corner_radius_hit = hit_test_corner_radius_handles(*layer, view_pt);
    if (corner_radius_hit != DragMode::None)
        return corner_radius_hit;

    QRectF r = layer_local_rect(*layer);
    auto layer_point_to_view = [&](const QPointF &p) {
        return canvas_to_view(layer_to_canvas(*layer, p));
    };
    auto near_pt = [&](const QPointF &p) {
        QPointF view = layer_point_to_view(p);
        return std::abs(view_pt.x() - view.x()) <= CANVAS_CONTROL_HIT_RADIUS_PX &&
               std::abs(view_pt.y() - view.y()) <= CANVAS_CONTROL_HIT_RADIUS_PX;
    };
    QPainterPath layer_path;
    layer_path.moveTo(layer_point_to_view(r.topLeft()));
    layer_path.lineTo(layer_point_to_view(r.topRight()));
    layer_path.lineTo(layer_point_to_view(r.bottomRight()));
    layer_path.lineTo(layer_point_to_view(r.bottomLeft()));
    layer_path.closeSubpath();
    auto near_rotation_corner = [&](const QPointF &p) {
        return QLineF(view_pt, layer_point_to_view(p)).length() <= CANVAS_ROTATE_HIT_RADIUS_PX &&
               !layer_path.contains(view_pt);
    };

    if (near_pt(r.topLeft())) return DragMode::ResizeNW;
    if (near_pt(QPointF(r.center().x(), r.top()))) return DragMode::ResizeN;
    if (near_pt(r.topRight())) return DragMode::ResizeNE;
    if (near_pt(QPointF(r.right(), r.center().y()))) return DragMode::ResizeE;
    if (near_pt(r.bottomRight())) return DragMode::ResizeSE;
    if (near_pt(QPointF(r.center().x(), r.bottom()))) return DragMode::ResizeS;
    if (near_pt(r.bottomLeft())) return DragMode::ResizeSW;
    if (near_pt(QPointF(r.left(), r.center().y()))) return DragMode::ResizeW;
    if (near_rotation_corner(r.topLeft()) || near_rotation_corner(r.topRight()) ||
        near_rotation_corner(r.bottomRight()) || near_rotation_corner(r.bottomLeft()))
        return DragMode::Rotate;
    if (QLineF(view_pt, layer_point_to_view(QPointF(0, 0))).length() <= CANVAS_CONTROL_HIT_RADIUS_PX * 1.25)
        return DragMode::Origin;

    if (layer_path.contains(view_pt)) return DragMode::Move;
    return DragMode::None;
}
void CanvasPreview::begin_marquee(const QPointF &view_pt, Qt::KeyboardModifiers)
{
    drag_mode_ = DragMode::Marquee;
    marquee_active_ = false;
    drag_start_view_ = view_pt;
    drag_current_view_ = view_pt;
    marquee_base_selection_ = selected_layer_ids_;
    drag_changed_ = false;
    clear_snap_feedback();
}

void CanvasPreview::update_marquee(const QPointF &view_pt, Qt::KeyboardModifiers modifiers)
{
    if (!title_) return;
    drag_current_view_ = view_pt;
    if ((drag_current_view_ - drag_start_view_).manhattanLength() < QApplication::startDragDistance()) {
        update();
        return;
    }

    marquee_active_ = true;
    QRectF view_rect(drag_start_view_, drag_current_view_);
    view_rect = view_rect.normalized();
    QRectF canvas_rect(view_to_canvas(view_rect.topLeft()), view_to_canvas(view_rect.bottomRight()));
    canvas_rect = canvas_rect.normalized();
    auto intersects_or_touches = [](const QRectF &a, const QRectF &b) {
        if (!a.isValid() || !b.isValid()) return false;
        return a.left() <= b.right() && a.right() >= b.left() &&
               a.top() <= b.bottom() && a.bottom() >= b.top();
    };

    std::set<std::string> selected;
    if (modifiers & (Qt::ShiftModifier | Qt::ControlModifier))
        selected.insert(marquee_base_selection_.begin(), marquee_base_selection_.end());

    std::vector<std::string> hits;
    for (const auto &layer : title_->layers) {
        if (!layer || !layer->visible || layer->locked) continue;
        if (playhead_ < layer->in_time || playhead_ > layer->out_time) continue;
        QRectF bounds = layer_canvas_bounds(*layer);
        if (intersects_or_touches(canvas_rect, bounds))
            hits.push_back(layer->id);
    }

    if (modifiers & Qt::ControlModifier) {
        for (const auto &id : hits) {
            auto it = selected.find(id);
            if (it == selected.end()) selected.insert(id);
            else selected.erase(it);
        }
    } else {
        selected.insert(hits.begin(), hits.end());
    }

    selected_layer_ids_.clear();
    for (const auto &layer : title_->layers) {
        if (layer && selected.find(layer->id) != selected.end())
            selected_layer_ids_.push_back(layer->id);
    }
    sel_layer_id_ = selected_layer_ids_.empty() ? std::string() : selected_layer_ids_.back();
    emit layers_selected(selected_layer_ids_);
    update();
}

bool CanvasPreview::duplicate_selected_layers_for_drag()
{
    if (!title_ || selected_layer_ids_.empty()) return false;

    std::set<std::string> selected_ids(selected_layer_ids_.begin(), selected_layer_ids_.end());
    std::map<std::string, std::string> cloned_ids_by_original;
    std::map<std::string, std::shared_ptr<Layer>> clones_by_original;
    std::vector<std::shared_ptr<Layer>> clones;

    for (const auto &layer : title_->layers) {
        if (!layer || layer->locked || selected_ids.find(layer->id) == selected_ids.end())
            continue;

        auto clone = std::make_shared<Layer>(*layer);
        clone->id = TitleDataStore::make_uuid();
        clone->name = clone->name.empty() ? editor_text_std("OBSTitles.LayerCopy") :
                                            clone->name + editor_text_std("OBSTitles.CopySuffix");
        cloned_ids_by_original[layer->id] = clone->id;
        clones_by_original[layer->id] = clone;
        clones.push_back(clone);
    }

    if (clones.empty()) return false;

    for (auto &clone : clones) {
        auto parent_clone = cloned_ids_by_original.find(clone->parent_id);
        if (parent_clone != cloned_ids_by_original.end()) {
            clone->parent_id = parent_clone->second;
        } else if (!clone->parent_id.empty() && !title_->find_layer(clone->parent_id)) {
            clone->parent_id.clear();
        }
        auto mask_clone = cloned_ids_by_original.find(clone->mask_source_id);
        if (mask_clone != cloned_ids_by_original.end()) {
            clone->mask_source_id = mask_clone->second;
        } else if (!clone->mask_source_id.empty() && !title_->find_layer(clone->mask_source_id)) {
            clone->mask_source_id.clear();
            clone->mask_mode = MaskMode::None;
        }
    }

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

    selected_layer_ids_.clear();
    drag_layer_states_.clear();
    for (const auto &clone : clones) {
        if (!clone) continue;
        selected_layer_ids_.push_back(clone->id);
        double lt = std::clamp(playhead_ - clone->in_time, 0.0,
                               std::max(0.0, clone->out_time - clone->in_time));
        drag_layer_states_.push_back({clone->id,
                                      clone->pos_x.evaluate(lt),
                                      clone->pos_y.evaluate(lt),
                                      (float)eval_box_width(*clone, lt),
                                      (float)eval_box_height(*clone, lt),
                                      clone->scale_x.evaluate(lt),
                                      clone->scale_y.evaluate(lt),
                                      clone->rotation.evaluate(lt)});
    }
    sel_layer_id_ = selected_layer_ids_.empty() ? std::string() : selected_layer_ids_.back();
    drag_start_selection_bounds_ = selected_canvas_bounds();

    emit layer_structure_changed();
    emit layers_selected(selected_layer_ids_);
    dirty_ = true;
    update();
    return true;
}

bool CanvasPreview::nudge_selected_layers(double dx, double dy)
{
    auto layers = selected_layers();
    if (!title_ || layers.empty()) return false;

    auto selected_contains = [&](const std::string &id) {
        return std::find(selected_layer_ids_.begin(), selected_layer_ids_.end(), id) != selected_layer_ids_.end();
    };
    auto has_selected_parent = [&](const Layer &layer) {
        std::string parent_id = layer.parent_id;
        int guard = 0;
        while (!parent_id.empty() && guard++ < 64) {
            if (selected_contains(parent_id))
                return true;
            const Layer *parent = editor_find_layer_by_id(title_, parent_id);
            if (!parent)
                break;
            parent_id = parent->parent_id;
        }
        return false;
    };

    bool changed = false;
    for (const auto &layer : layers) {
        if (!layer || layer->locked || has_selected_parent(*layer)) continue;
        double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                               std::max(0.0, layer->out_time - layer->in_time));
        set_animated_value(layer->pos_x, lt, layer->pos_x.evaluate(lt) + dx);
        set_animated_value(layer->pos_y, lt, layer->pos_y.evaluate(lt) + dy);
        changed = true;
    }

    if (!changed) return false;

    dirty_ = true;
    update();
    emit layer_geometry_changed();
    return true;
}


void CanvasPreview::clear_snap_feedback()
{
    if (snap_feedback_.empty()) return;
    snap_feedback_.clear();
    update();
}

void CanvasPreview::add_snap_feedback(bool x_axis, double value, const QString &label)
{
    snap_feedback_.push_back({x_axis, value, label});
}

void CanvasPreview::collect_snap_targets(bool x_axis, std::vector<double> &targets, std::vector<QString> &labels) const
{
    if (!title_) return;
    auto add = [&](double value, const QString &label) {
        if (!std::isfinite(value)) return;
        targets.push_back(value);
        labels.push_back(label);
    };

    if (snap_settings_.canvas_bounds) {
        const double size = x_axis ? title_->width : title_->height;
        add(0.0, QStringLiteral("Canvas"));
        add(size * 0.5, QStringLiteral("Canvas center"));
        add(size, QStringLiteral("Canvas"));
    }

    if (snap_settings_.guides) {
        const double size = x_axis ? title_->width : title_->height;
        add(size * OBS_ACTION_SAFE_PERCENT, QStringLiteral("Action safe"));
        add(size * (1.0 - OBS_ACTION_SAFE_PERCENT), QStringLiteral("Action safe"));
        add(size * OBS_GRAPHICS_SAFE_PERCENT, QStringLiteral("Title safe"));
        add(size * (1.0 - OBS_GRAPHICS_SAFE_PERCENT), QStringLiteral("Title safe"));
        const auto &user_guides = x_axis ? vertical_guides_ : horizontal_guides_;
        for (double guide : user_guides)
            add(guide, QStringLiteral("Guide"));
    }

    if (snap_settings_.grid) {
        const double size = x_axis ? title_->width : title_->height;
        constexpr double grid = 10.0;
        for (double v = 0.0; v <= size + 0.01; v += grid)
            add(v, QStringLiteral("Grid"));
    }

    if (!snap_settings_.object_edges && !snap_settings_.object_centers) return;

    std::set<std::string> selected_ids(selected_layer_ids_.begin(), selected_layer_ids_.end());
    if (!sel_layer_id_.empty())
        selected_ids.insert(sel_layer_id_);

    for (const auto &layer : title_->layers) {
        if (!layer || !layer->visible) continue;
        if (playhead_ < layer->in_time || playhead_ > layer->out_time) continue;
        if (selected_ids.find(layer->id) != selected_ids.end())
            continue;
        QRectF bounds = layer_canvas_bounds(*layer);
        if (!bounds.isValid() || bounds.isEmpty()) continue;
        if (snap_settings_.object_edges) {
            add(x_axis ? bounds.left() : bounds.top(), QStringLiteral("Object edge"));
            add(x_axis ? bounds.right() : bounds.bottom(), QStringLiteral("Object edge"));
        }
        if (snap_settings_.object_centers)
            add(x_axis ? bounds.center().x() : bounds.center().y(), QStringLiteral("Object center"));
    }
}

void CanvasPreview::collect_spacing_targets(bool x_axis, std::vector<double> &targets, std::vector<QString> &labels) const
{
    if (!title_ || !snap_settings_.spacing) return;

    struct Span { double start; double end; };
    std::vector<Span> spans;
    std::set<std::string> selected_ids(selected_layer_ids_.begin(), selected_layer_ids_.end());
    if (!sel_layer_id_.empty())
        selected_ids.insert(sel_layer_id_);

    for (const auto &layer : title_->layers) {
        if (!layer || !layer->visible) continue;
        if (playhead_ < layer->in_time || playhead_ > layer->out_time) continue;
        if (selected_ids.find(layer->id) != selected_ids.end())
            continue;
        QRectF bounds = layer_canvas_bounds(*layer);
        if (!bounds.isValid() || bounds.isEmpty()) continue;
        spans.push_back({x_axis ? bounds.left() : bounds.top(), x_axis ? bounds.right() : bounds.bottom()});
    }
    if (spans.empty()) return;
    std::sort(spans.begin(), spans.end(), [](const Span &a, const Span &b) { return a.start < b.start; });

    std::vector<double> gaps;
    for (size_t i = 1; i < spans.size(); ++i) {
        double gap = spans[i].start - spans[i - 1].end;
        if (gap >= 0.0) gaps.push_back(gap);
    }
    if (gaps.empty()) gaps.push_back(0.0);

    auto add = [&](double value) {
        if (!std::isfinite(value)) return;
        targets.push_back(value);
        labels.push_back(QStringLiteral("Spacing"));
    };
    for (const Span &span : spans) {
        for (double gap : gaps) {
            add(span.start - gap);
            add(span.end + gap);
        }
    }
}

QPointF CanvasPreview::snap_delta_for_bounds(const QRectF &start_bounds, const QPointF &delta, bool snap_x, bool snap_y,
                                             bool allow_snap)
{
    if (!allow_snap || !title_ || !snap_settings_.enabled || !start_bounds.isValid()) {
        clear_snap_feedback();
        return delta;
    }

    snap_feedback_.clear();
    QPointF snapped_delta = delta;
    const double tolerance = 6.0 / std::max(0.1, view_scale());

    auto snap_axis = [&](bool x_axis) {
        std::vector<double> targets;
        std::vector<QString> labels;
        collect_snap_targets(x_axis, targets, labels);
        collect_spacing_targets(x_axis, targets, labels);
        if (targets.empty()) return;

        const double offset = x_axis ? snapped_delta.x() : snapped_delta.y();
        const double start_min = x_axis ? start_bounds.left() : start_bounds.top();
        const double start_center = x_axis ? start_bounds.center().x() : start_bounds.center().y();
        const double start_max = x_axis ? start_bounds.right() : start_bounds.bottom();
        const double points[] = {start_min + offset, start_center + offset, start_max + offset};
        double best_adjust = 0.0;
        double best_distance = tolerance + 1.0;
        double best_target = 0.0;
        QString best_label;
        for (size_t i = 0; i < targets.size(); ++i) {
            for (double point : points) {
                double adjust = targets[i] - point;
                double distance = std::abs(adjust);
                if (distance < best_distance) {
                    best_distance = distance;
                    best_adjust = adjust;
                    best_target = targets[i];
                    best_label = labels[i];
                }
            }
        }
        if (best_distance <= tolerance) {
            if (x_axis) snapped_delta.setX(snapped_delta.x() + best_adjust);
            else snapped_delta.setY(snapped_delta.y() + best_adjust);
            add_snap_feedback(x_axis, best_target, best_label);
        }
    };

    if (snap_x) snap_axis(true);
    if (snap_y) snap_axis(false);
    return snapped_delta;
}

QPointF CanvasPreview::snap_canvas_point(const QPointF &canvas_pt, bool snap_x, bool snap_y, bool allow_snap)
{
    QRectF point_bounds(canvas_pt, QSizeF(0.0, 0.0));
    QPointF delta = snap_delta_for_bounds(point_bounds, QPointF(0.0, 0.0), snap_x, snap_y, allow_snap);
    return canvas_pt + delta;
}

static QRectF editor_modifier_rect(const QPointF &anchor, const QPointF &current,
                                   Qt::KeyboardModifiers modifiers, double aspect,
                                   double fixed_width = 0.0, double fixed_height = 0.0);

void CanvasPreview::apply_drag(const QPointF &view_pt, Qt::KeyboardModifiers modifiers)
{
    if (drag_mode_ == DragMode::Marquee) {
        update_marquee(view_pt, modifiers);
        return;
    }

    if (apply_gradient_drag(view_pt, modifiers))
        return;
    if (apply_corner_radius_drag(view_pt, modifiers))
        return;

    if (drag_mode_ == DragMode::Move && alt_duplicate_pending_ && !alt_duplicate_done_) {
        alt_duplicate_done_ = true;
        duplicate_selected_layers_for_drag();
    }

    auto layers = selected_layers();
    if (layers.empty() || drag_mode_ == DragMode::None) return;

    drag_current_view_ = view_pt;
    QPointF canvas = view_to_canvas(view_pt);
    QPointF delta = canvas - drag_start_canvas_;
    const bool allow_snap = !modifiers.testFlag(Qt::ControlModifier);
    const double snap_tolerance = 6.0 / std::max(0.1, view_scale());
    auto snap_coordinate = [&](bool x_axis, double value, double *snapped) {
        if (!allow_snap || !snapped)
            return false;

        std::vector<double> targets;
        std::vector<QString> labels;
        collect_snap_targets(x_axis, targets, labels);
        collect_spacing_targets(x_axis, targets, labels);
        if (targets.empty())
            return false;

        double best_distance = snap_tolerance + 1.0;
        double best_target = value;
        QString best_label;
        for (size_t i = 0; i < targets.size(); ++i) {
            const double distance = std::abs(targets[i] - value);
            if (distance < best_distance) {
                best_distance = distance;
                best_target = targets[i];
                best_label = labels[i];
            }
        }
        if (best_distance > snap_tolerance)
            return false;

        *snapped = best_target;
        add_snap_feedback(x_axis, best_target, best_label);
        return true;
    };

    auto selected_contains = [&](const std::string &id) {
        return std::find(selected_layer_ids_.begin(), selected_layer_ids_.end(), id) != selected_layer_ids_.end();
    };
    auto has_selected_parent = [&](const Layer &layer) {
        std::string parent_id = layer.parent_id;
        int guard = 0;
        while (!parent_id.empty() && guard++ < 64) {
            if (selected_contains(parent_id))
                return true;
            const Layer *parent = editor_find_layer_by_id(title_, parent_id);
            if (!parent)
                break;
            parent_id = parent->parent_id;
        }
        return false;
    };

    if (drag_mode_ == DragMode::Rotate) {
        clear_snap_feedback();
        QPointF pivot_view = canvas_to_view(drag_rotation_pivot_canvas_);
        double current_angle = radians_to_degrees(std::atan2(view_pt.y() - pivot_view.y(),
                                                             view_pt.x() - pivot_view.x()));
        double rotation_delta = normalize_degrees(current_angle - drag_start_rotation_angle_);
        if (modifiers & Qt::ShiftModifier)
            rotation_delta = std::round(rotation_delta / CANVAS_ROTATION_SNAP_DEGREES) * CANVAS_ROTATION_SNAP_DEGREES;
        drag_current_rotation_delta_ = rotation_delta;

        for (const auto &state : drag_layer_states_) {
            auto layer = title_->find_layer(state.id);
            if (!layer || layer->locked) continue;
            double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                   std::max(0.0, layer->out_time - layer->in_time));
            if (layers.size() > 1) {
                QPointF next_pos = rotate_point_around(QPointF(state.x, state.y), drag_rotation_pivot_canvas_, rotation_delta);
                set_animated_value(layer->pos_x, lt, next_pos.x());
                set_animated_value(layer->pos_y, lt, next_pos.y());
            }
            set_animated_value(layer->rotation, lt, state.rotation + rotation_delta);
        }
        dirty_ = true;
        drag_changed_ = true;
        update();
        return;
    }

    if (layers.size() > 1) {
        if (drag_mode_ == DragMode::Move) {
            if (modifiers & Qt::ShiftModifier) {
                if (std::abs(delta.x()) >= std::abs(delta.y())) delta.setY(0.0);
                else delta.setX(0.0);
            }
            delta = snap_delta_for_bounds(drag_start_selection_bounds_, delta, true, true, allow_snap);
            for (const auto &state : drag_layer_states_) {
                auto layer = title_->find_layer(state.id);
                if (!layer || layer->locked || has_selected_parent(*layer)) continue;
                double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                       std::max(0.0, layer->out_time - layer->in_time));
                set_animated_value(layer->pos_x, lt, state.x + delta.x());
                set_animated_value(layer->pos_y, lt, state.y + delta.y());
            }
        } else {
            QRectF start = drag_start_selection_bounds_;
            if (!start.isValid() || start.width() <= 0.0 || start.height() <= 0.0) return;
            if (allow_snap)
                snap_feedback_.clear();
            QRectF next = start;
            bool resize_left = drag_mode_ == DragMode::ResizeNW || drag_mode_ == DragMode::ResizeSW || drag_mode_ == DragMode::ResizeW;
            bool resize_right = drag_mode_ == DragMode::ResizeNE || drag_mode_ == DragMode::ResizeSE || drag_mode_ == DragMode::ResizeE;
            bool resize_top = drag_mode_ == DragMode::ResizeNW || drag_mode_ == DragMode::ResizeNE || drag_mode_ == DragMode::ResizeN;
            bool resize_bottom = drag_mode_ == DragMode::ResizeSW || drag_mode_ == DragMode::ResizeSE || drag_mode_ == DragMode::ResizeS;
            if (resize_left) next.setLeft(std::min(canvas.x(), start.right()));
            if (resize_right) next.setRight(std::max(canvas.x(), start.left()));
            if (resize_top) next.setTop(std::min(canvas.y(), start.bottom()));
            if (resize_bottom) next.setBottom(std::max(canvas.y(), start.top()));
            double sx = next.width() / start.width();
            double sy = next.height() / start.height();
            if (modifiers & Qt::ShiftModifier) {
                double uniform = std::abs(sx) >= std::abs(sy) ? sx : sy;
                sx = sy = uniform;
            }
            next = QRectF(start.topLeft(), QSizeF(start.width() * sx, start.height() * sy)).normalized();
            if (resize_left)
                next.moveRight(start.right());
            else
                next.moveLeft(start.left());
            if (resize_top)
                next.moveBottom(start.bottom());
            else
                next.moveTop(start.top());

            double snapped_value = 0.0;
            if (resize_left && snap_coordinate(true, next.left(), &snapped_value))
                next.setLeft(std::min(snapped_value, next.right()));
            else if (resize_right && snap_coordinate(true, next.right(), &snapped_value))
                next.setRight(std::max(snapped_value, next.left()));
            if (resize_top && snap_coordinate(false, next.top(), &snapped_value))
                next.setTop(std::min(snapped_value, next.bottom()));
            else if (resize_bottom && snap_coordinate(false, next.bottom(), &snapped_value))
                next.setBottom(std::max(snapped_value, next.top()));

            sx = next.width() / start.width();
            sy = next.height() / start.height();
            for (const auto &state : drag_layer_states_) {
                auto layer = title_->find_layer(state.id);
                if (!layer || layer->locked) continue;
                double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                                       std::max(0.0, layer->out_time - layer->in_time));
                double rx = (state.x - start.left()) / start.width();
                double ry = (state.y - start.top()) / start.height();
                set_animated_value(layer->pos_x, lt, next.left() + rx * next.width());
                set_animated_value(layer->pos_y, lt, next.top() + ry * next.height());
                const bool scale_text_object = drag_text_object_scaling_ && is_canvas_text_layer(*layer);
                if (scale_text_object) {
                    set_animated_value(layer->scale_x, lt, state.scale_x * sx);
                    set_animated_value(layer->scale_y, lt, state.scale_y * sy);
                } else {
                    layer->rect_width = std::max(0.0f, (float)(state.w * sx));
                    layer->rect_height = std::max(0.0f, (float)(state.h * sy));
                    set_animated_value(layer->box_width, lt, layer->rect_width);
                    set_animated_value(layer->box_height, lt, layer->rect_height);
                }
            }
        }
        dirty_ = true;
        drag_changed_ = true;
        update();
        return;
    }

    auto layer = layers.front();
    if (!layer) return;
    double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                           std::max(0.0, layer->out_time - layer->in_time));

    if (drag_mode_ == DragMode::Move) {
        if (modifiers & Qt::ShiftModifier) {
            if (std::abs(delta.x()) >= std::abs(delta.y()))
                delta.setY(0.0);
            else
                delta.setX(0.0);
        }
        delta = snap_delta_for_bounds(drag_start_selection_bounds_, delta, true, true, allow_snap);
        set_animated_value(layer->pos_x, lt, drag_start_x_ + delta.x());
        set_animated_value(layer->pos_y, lt, drag_start_y_ + delta.y());
    } else if (drag_mode_ == DragMode::Origin) {
        clear_snap_feedback();
        double w = std::max(1.0f, drag_start_w_);
        double h = std::max(1.0f, drag_start_h_);
        layer->origin_x = (float)std::clamp(drag_start_origin_x_ + delta.x() / w, 0.0, 1.0);
        layer->origin_y = (float)std::clamp(drag_start_origin_y_ + delta.y() / h, 0.0, 1.0);
        set_animated_value(layer->origin_x_prop, lt, layer->origin_x);
        set_animated_value(layer->origin_y_prop, lt, layer->origin_y);
        set_animated_value(layer->pos_x, lt, drag_start_x_ + delta.x());
        set_animated_value(layer->pos_y, lt, drag_start_y_ + delta.y());
    } else {
        bool resize_left = drag_mode_ == DragMode::ResizeNW || drag_mode_ == DragMode::ResizeSW || drag_mode_ == DragMode::ResizeW;
        bool resize_right = drag_mode_ == DragMode::ResizeNE || drag_mode_ == DragMode::ResizeSE || drag_mode_ == DragMode::ResizeE;
        bool resize_top = drag_mode_ == DragMode::ResizeNW || drag_mode_ == DragMode::ResizeNE || drag_mode_ == DragMode::ResizeN;
        bool resize_bottom = drag_mode_ == DragMode::ResizeSW || drag_mode_ == DragMode::ResizeSE || drag_mode_ == DragMode::ResizeS;
        if (allow_snap)
            snap_feedback_.clear();
        const bool scale_text_object = drag_text_object_scaling_ && is_canvas_text_layer(*layer);
        const LayerDragState *start_state = drag_layer_states_.empty() ? nullptr : &drag_layer_states_.front();
        auto non_zero_scale = [](double value) {
            if (std::abs(value) >= 0.0001) return value;
            return value < 0.0 ? -0.0001 : 0.0001;
        };
        auto start_canvas_to_layer = [&](const QPointF &canvas_pt) {
            QTransform start_xf;
            start_xf.translate(start_state->x, start_state->y);
            start_xf.rotate(start_state->rotation);
            start_xf.scale(non_zero_scale(start_state->scale_x),
                           non_zero_scale(start_state->scale_y));
            return start_xf.inverted().map(canvas_pt);
        };
        const bool alt_resize = modifiers.testFlag(Qt::AltModifier);
        const bool lock_aspect_resize = layer->type == LayerType::Image &&
                                        layer->lock_aspect_ratio &&
                                        drag_start_h_ > 0.0f;
        Qt::KeyboardModifiers resize_modifiers = modifiers;
        if (lock_aspect_resize)
            resize_modifiers |= Qt::ShiftModifier;
        QPointF local = start_state ? start_canvas_to_layer(canvas)
                                    : canvas_to_layer(*layer, canvas);
        double left = -drag_start_origin_x_ * drag_start_w_;
        double right = (1.0 - drag_start_origin_x_) * drag_start_w_;
        double top = -drag_start_origin_y_ * drag_start_h_;
        double bottom = (1.0 - drag_start_origin_y_) * drag_start_h_;
        const QRectF start_rect(QPointF(left, top), QPointF(right, bottom));
        const bool corner_resize = (resize_left || resize_right) && (resize_top || resize_bottom);
        const double aspect = drag_start_h_ > 0.0f ? (double)drag_start_w_ / drag_start_h_ : 1.0;

        QPointF anchor = start_rect.center();
        double fixed_w = 0.0;
        double fixed_h = 0.0;
        if (resize_modifiers.testFlag(Qt::AltModifier)) {
            // Alt-resize is center-based. Side handles must stay single-axis:
            // E/W only change width and N/S only change height. The modifier
            // helper doubles dimensions around the anchor for Alt, so pass half
            // the unchanged dimension to preserve the original opposite axis.
            if (!corner_resize) {
                if (resize_left || resize_right)
                    fixed_h = start_rect.height() * 0.5;
                else
                    fixed_w = start_rect.width() * 0.5;
            }
        } else {
            if (corner_resize) {
                anchor = QPointF(resize_left ? start_rect.right() : start_rect.left(),
                                 resize_top ? start_rect.bottom() : start_rect.top());
            } else if (resize_left || resize_right) {
                anchor = QPointF(resize_left ? start_rect.right() : start_rect.left(),
                                 start_rect.center().y());
                fixed_h = start_rect.height();
            } else {
                anchor = QPointF(start_rect.center().x(),
                                 resize_top ? start_rect.bottom() : start_rect.top());
                fixed_w = start_rect.width();
            }
        }

        const QRectF resized_rect = editor_modifier_rect(anchor, local, resize_modifiers, aspect, fixed_w, fixed_h);
        QTransform start_xf;
        if (start_state) {
            start_xf.translate(start_state->x, start_state->y);
            start_xf.rotate(start_state->rotation);
            start_xf.scale(non_zero_scale(start_state->scale_x),
                           non_zero_scale(start_state->scale_y));
        }
        QRectF final_rect = resized_rect;
        if (allow_snap && start_state) {
            QPointF handle_local = local;
            if (resize_left || resize_right)
                handle_local.setX(resize_left ? resized_rect.left() : resized_rect.right());
            if (resize_top || resize_bottom)
                handle_local.setY(resize_top ? resized_rect.top() : resized_rect.bottom());

            QPointF handle_canvas = start_xf.map(handle_local);
            bool snapped_any = false;
            double snapped_value = 0.0;
            QPolygonF proposed_poly;
            proposed_poly << start_xf.map(resized_rect.topLeft())
                          << start_xf.map(resized_rect.topRight())
                          << start_xf.map(resized_rect.bottomRight())
                          << start_xf.map(resized_rect.bottomLeft());
            const QRectF proposed_bounds = proposed_poly.boundingRect();
            if (resize_left && snap_coordinate(true, proposed_bounds.left(), &snapped_value)) {
                handle_canvas.setX(handle_canvas.x() + snapped_value - proposed_bounds.left());
                snapped_any = true;
            } else if (resize_right && snap_coordinate(true, proposed_bounds.right(), &snapped_value)) {
                handle_canvas.setX(handle_canvas.x() + snapped_value - proposed_bounds.right());
                snapped_any = true;
            }
            if (resize_top && snap_coordinate(false, proposed_bounds.top(), &snapped_value)) {
                handle_canvas.setY(handle_canvas.y() + snapped_value - proposed_bounds.top());
                snapped_any = true;
            } else if (resize_bottom && snap_coordinate(false, proposed_bounds.bottom(), &snapped_value)) {
                handle_canvas.setY(handle_canvas.y() + snapped_value - proposed_bounds.bottom());
                snapped_any = true;
            }
            if (snapped_any) {
                const QPointF snapped_local = start_xf.inverted().map(handle_canvas);
                final_rect = editor_modifier_rect(anchor, snapped_local, resize_modifiers, aspect, fixed_w, fixed_h);
            }
        }
        double new_w = std::max(0.0, final_rect.width());
        double new_h = std::max(0.0, final_rect.height());
        const QPointF fixed_center_canvas = start_xf.map(start_rect.center());
        if (scale_text_object) {
            double sx = drag_start_w_ > 0.0f ? new_w / drag_start_w_ : 1.0;
            double sy = drag_start_h_ > 0.0f ? new_h / drag_start_h_ : 1.0;
            double start_scale_x = start_state ? start_state->scale_x : layer->scale_x.evaluate(lt);
            double start_scale_y = start_state ? start_state->scale_y : layer->scale_y.evaluate(lt);
            set_animated_value(layer->scale_x, lt, start_scale_x * sx);
            set_animated_value(layer->scale_y, lt, start_scale_y * sy);
            if (alt_resize && start_state) {
                QTransform next_xf;
                next_xf.translate(start_state->x, start_state->y);
                next_xf.rotate(start_state->rotation);
                next_xf.scale(non_zero_scale(start_scale_x * sx),
                              non_zero_scale(start_scale_y * sy));
                const QPointF current_center_canvas = next_xf.map(start_rect.center());
                const QPointF shift = fixed_center_canvas - current_center_canvas;
                set_animated_value(layer->pos_x, lt, start_state->x + shift.x());
                set_animated_value(layer->pos_y, lt, start_state->y + shift.y());
            }
        } else {
            layer->rect_width = (float)new_w;
            layer->rect_height = (float)new_h;
            set_animated_value(layer->box_width, lt, new_w);
            set_animated_value(layer->box_height, lt, new_h);
            if (start_state) {
                const QRectF actual_rect(-drag_start_origin_x_ * new_w,
                                         -drag_start_origin_y_ * new_h,
                                         new_w, new_h);
                const QPointF local_shift = final_rect.center() - actual_rect.center();
                const QPointF canvas_shift = start_xf.map(local_shift) - start_xf.map(QPointF(0.0, 0.0));
                set_animated_value(layer->pos_x, lt, start_state->x + canvas_shift.x());
                set_animated_value(layer->pos_y, lt, start_state->y + canvas_shift.y());
            }
            if (alt_resize && start_state) {
                const QRectF actual_rect(-drag_start_origin_x_ * new_w,
                                         -drag_start_origin_y_ * new_h,
                                         new_w, new_h);
                QTransform next_xf;
                next_xf.translate(start_state->x, start_state->y);
                next_xf.rotate(start_state->rotation);
                next_xf.scale(non_zero_scale(start_state->scale_x),
                              non_zero_scale(start_state->scale_y));
                const QPointF current_center_canvas = next_xf.map(actual_rect.center());
                const QPointF shift = fixed_center_canvas - current_center_canvas;
                set_animated_value(layer->pos_x, lt, start_state->x + shift.x());
                set_animated_value(layer->pos_y, lt, start_state->y + shift.y());
            }
        }
    }

    dirty_ = true;
    drag_changed_ = true;
    update();
}
void CanvasPreview::render_to_pixmap()
{
    if (!title_) { frame_pixmap_ = QPixmap(); return; }

    frame_pixmap_ = QPixmap::fromImage(render_title_to_image(*title_, playhead_));
    dirty_ = false;
}


QRectF CanvasPreview::canvas_view_rect() const
{
    if (!title_) return QRectF();
    const double scale = view_scale();
    const QPointF origin = view_origin();
    return QRectF(origin.x(), origin.y(), title_->width * scale, title_->height * scale);
}

QRectF CanvasPreview::ruler_top_rect() const
{
    if (!rulers_visible_) return QRectF();
    return QRectF(kCanvasRulerThickness, 0, std::max(0, width() - kCanvasRulerThickness), kCanvasRulerThickness);
}

QRectF CanvasPreview::ruler_left_rect() const
{
    if (!rulers_visible_) return QRectF();
    return QRectF(0, kCanvasRulerThickness, kCanvasRulerThickness, std::max(0, height() - kCanvasRulerThickness));
}

QRectF CanvasPreview::ruler_corner_rect() const
{
    if (!rulers_visible_) return QRectF();
    return QRectF(0, 0, kCanvasRulerThickness, kCanvasRulerThickness);
}

bool CanvasPreview::ruler_hit_test(const QPointF &view_pt, bool &vertical_guide) const
{
    if (!rulers_visible_ || guides_locked_) return false;
    // Match the requested ruler behavior:
    // - vertical (left) ruler creates vertical guides (x-axis guides)
    // - horizontal (top) ruler creates horizontal guides (y-axis guides)
    if (ruler_left_rect().contains(view_pt)) { vertical_guide = true; return true; }
    if (ruler_top_rect().contains(view_pt)) { vertical_guide = false; return true; }
    return false;
}

int CanvasPreview::guide_hit_test(const QPointF &view_pt, bool &x_axis, bool include_locked) const
{
    if (!guides_visible_ || (!include_locked && guides_locked_) || !title_) return -1;
    const QRectF canvas_rect = canvas_view_rect();
    for (size_t i = 0; i < vertical_guides_.size(); ++i) {
        const double x = canvas_to_view(QPointF(vertical_guides_[i], 0.0)).x();
        if (std::abs(view_pt.x() - x) <= kCanvasGuideHitTolerancePx &&
            view_pt.y() >= canvas_rect.top() - 8.0 && view_pt.y() <= canvas_rect.bottom() + 8.0) {
            x_axis = true;
            return (int)i;
        }
    }
    for (size_t i = 0; i < horizontal_guides_.size(); ++i) {
        const double y = canvas_to_view(QPointF(0.0, horizontal_guides_[i])).y();
        if (std::abs(view_pt.y() - y) <= kCanvasGuideHitTolerancePx &&
            view_pt.x() >= canvas_rect.left() - 8.0 && view_pt.x() <= canvas_rect.right() + 8.0) {
            x_axis = false;
            return (int)i;
        }
    }
    return -1;
}



double CanvasPreview::snap_guide_value_to_objects(bool x_axis, double raw_value)
{
    if (!title_ || !snap_settings_.enabled || !snap_settings_.object_edges) {
        clear_snap_feedback();
        return raw_value;
    }

    const double tolerance = 6.0 / std::max(0.1, view_scale());
    double best_value = raw_value;
    double best_distance = tolerance + 1.0;
    QString best_label;

    auto consider = [&](double value, const QString &label) {
        if (!std::isfinite(value)) return;
        const double distance = std::abs(value - raw_value);
        if (distance < best_distance) {
            best_distance = distance;
            best_value = value;
            best_label = label;
        }
    };

    for (const auto &layer : title_->layers) {
        if (!layer || !layer->visible) continue;
        if (playhead_ < layer->in_time || playhead_ > layer->out_time) continue;
        QRectF bounds = layer_canvas_bounds(*layer);
        if (!bounds.isValid() || bounds.isEmpty()) continue;

        consider(x_axis ? bounds.left() : bounds.top(), QStringLiteral("Object edge"));
        consider(x_axis ? bounds.right() : bounds.bottom(), QStringLiteral("Object edge"));
        if (snap_settings_.object_centers)
            consider(x_axis ? bounds.center().x() : bounds.center().y(), QStringLiteral("Object center"));
    }

    snap_feedback_.clear();
    if (best_distance <= tolerance) {
        add_snap_feedback(x_axis, best_value, best_label);
        return best_value;
    }
    return raw_value;
}

void CanvasPreview::draw_rulers(QPainter &p, const QRectF &canvas_rect, double scale, const QPointF &origin)
{
    if (!rulers_visible_ || !title_) return;

    const QPalette pal = palette();
    const QColor bg = pal.color(QPalette::Button);
    const QColor border = pal.color(QPalette::Mid);
    const QColor fg = pal.color(QPalette::ButtonText);
    const QRectF top = ruler_top_rect();
    const QRectF left = ruler_left_rect();
    const QRectF corner = ruler_corner_rect();

    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(top, bg);
    p.fillRect(left, bg);
    p.fillRect(corner, bg.darker(105));
    p.setPen(border);
    p.drawLine(top.bottomLeft(), top.bottomRight());
    p.drawLine(left.topRight(), left.bottomRight());
    p.drawRect(corner.adjusted(0, 0, -1, -1));

    auto nice_step = [](double target_canvas_units) {
        static const double bases[] = {1.0, 2.0, 5.0, 10.0};
        const double exponent = std::pow(10.0, std::floor(std::log10(std::max(1.0, target_canvas_units))));
        for (double base : bases) {
            const double step = base * exponent;
            if (step >= target_canvas_units) return step;
        }
        return 10.0 * exponent;
    };
    const double major_step = nice_step(72.0 / std::max(0.001, scale));
    const double minor_step = major_step / 5.0;
    QFont small = font();
    small.setPointSize(std::max(7, small.pointSize() - 2));
    p.setFont(small);
    p.setPen(fg);

    const double start_x = std::floor(std::max(0.0, view_to_canvas(QPointF(top.left(), 0)).x()) / minor_step) * minor_step;
    const double end_x = std::min((double)title_->width, view_to_canvas(QPointF(top.right(), 0)).x());
    for (double v = start_x; v <= end_x + 0.01; v += minor_step) {
        const bool major = std::fmod(std::abs(v), major_step) < 0.001 || std::abs(std::fmod(std::abs(v), major_step) - major_step) < 0.001;
        const double x = origin.x() + v * scale;
        const double h = major ? 12.0 : 6.0;
        p.drawLine(QPointF(x, top.bottom()), QPointF(x, top.bottom() - h));
        if (major)
            p.drawText(QRectF(x + 3.0, top.top() + 2.0, 80.0, top.height() - 6.0), QString::number((int)std::round(v)));
    }

    const double start_y = std::floor(std::max(0.0, view_to_canvas(QPointF(0, left.top())).y()) / minor_step) * minor_step;
    const double end_y = std::min((double)title_->height, view_to_canvas(QPointF(0, left.bottom())).y());
    for (double v = start_y; v <= end_y + 0.01; v += minor_step) {
        const bool major = std::fmod(std::abs(v), major_step) < 0.001 || std::abs(std::fmod(std::abs(v), major_step) - major_step) < 0.001;
        const double y = origin.y() + v * scale;
        const double w = major ? 12.0 : 6.0;
        p.drawLine(QPointF(left.right(), y), QPointF(left.right() - w, y));
        if (major) {
            p.save();
            p.translate(left.left() + 2.0, y - 3.0);
            p.rotate(-90.0);
            p.drawText(QRectF(0, 0, 80.0, left.width() - 6.0), QString::number((int)std::round(v)));
            p.restore();
        }
    }
    p.restore();
}

void CanvasPreview::draw_guide_coordinate(QPainter &p, const QPointF &view_pt, bool x_axis, double value) const
{
    if (!show_guide_coordinates_) return;
    const QString text = QStringLiteral("%1: %2 px").arg(x_axis ? QStringLiteral("X") : QStringLiteral("Y")).arg(value, 0, 'f', 1);
    const QRectF bubble(view_pt.x() + 12.0, view_pt.y() + 12.0, 92.0, 22.0);
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 185));
    p.drawRoundedRect(bubble, 4, 4);
    p.setPen(Qt::white);
    p.drawText(bubble.adjusted(6, 2, -4, -2), Qt::AlignVCenter | Qt::AlignLeft, text);
    p.restore();
}

void CanvasPreview::draw_user_guides(QPainter &p, const QRectF &canvas_rect)
{
    if (!guides_visible_ || !title_) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    QPen pen(QColor(0, 160, 255, 210), 1.0, Qt::DashLine);
    pen.setDashPattern({4.0, 3.0});
    p.setPen(pen);
    for (double x_value : vertical_guides_) {
        const double x = canvas_to_view(QPointF(x_value, 0.0)).x();
        p.drawLine(QPointF(x, canvas_rect.top()), QPointF(x, canvas_rect.bottom()));
    }
    for (double y_value : horizontal_guides_) {
        const double y = canvas_to_view(QPointF(0.0, y_value)).y();
        p.drawLine(QPointF(canvas_rect.left(), y), QPointF(canvas_rect.right(), y));
    }
    if (dragging_new_guide_ || dragging_guide_index_ >= 0) {
        QPen active_pen(QColor(255, 220, 0, 240), 1.0, Qt::DashLine);
        active_pen.setDashPattern({6.0, 3.0});
        p.setPen(active_pen);
        if (dragging_guide_x_axis_) {
            const double x = canvas_to_view(QPointF(dragging_guide_value_, 0.0)).x();
            p.drawLine(QPointF(x, canvas_rect.top()), QPointF(x, canvas_rect.bottom()));
            draw_guide_coordinate(p, QPointF(x, canvas_rect.top()), true, dragging_guide_value_);
        } else {
            const double y = canvas_to_view(QPointF(0.0, dragging_guide_value_)).y();
            p.drawLine(QPointF(canvas_rect.left(), y), QPointF(canvas_rect.right(), y));
            draw_guide_coordinate(p, QPointF(canvas_rect.left(), y), false, dragging_guide_value_);
        }
    }
    p.restore();
}

void CanvasPreview::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QPalette pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Window));

    if (!title_) return;

    if (!drawing_shape_ && title_has_dynamic_text_layer(title_)) dirty_ = true;
    if (dirty_) render_to_pixmap();
    if (frame_pixmap_.isNull()) return;

    double scale = view_scale();
    QPointF origin = view_origin();
    int dw = (int)(title_->width * scale);
    int dh = (int)(title_->height * scale);
    int ox = (int)origin.x();
    int oy = (int)origin.y();

    auto checkerboard_colors = [this]() {
        if (checkerboard_pattern_ == 3)
            return std::pair<QColor, QColor>(Qt::white, Qt::white);
        if (checkerboard_pattern_ == 4)
            return std::pair<QColor, QColor>(Qt::black, Qt::black);
        if (checkerboard_pattern_ == 5)
            return std::pair<QColor, QColor>(QColor(0x80, 0x80, 0x80), QColor(0x80, 0x80, 0x80));
        if (checkerboard_pattern_ == 0)
            return std::pair<QColor, QColor>(QColor(0xee, 0xee, 0xee), QColor(0xc8, 0xc8, 0xc8));
        if (checkerboard_pattern_ == 2)
            return std::pair<QColor, QColor>(QColor(0x1f, 0x1f, 0x1f), QColor(0x36, 0x36, 0x36));
        return std::pair<QColor, QColor>(QColor(0x33, 0x33, 0x33), QColor(0x4a, 0x4a, 0x4a));
    };
    const auto [checker_a, checker_b] = checkerboard_colors();
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(checker_a));
    p.drawRect(ox, oy, dw, dh);
    if (checker_a != checker_b) {
        p.setBrush(QBrush(checker_b));
        for (int cy = oy; cy < oy + dh; cy += 12)
            for (int cx = ox; cx < ox + dw; cx += 12)
                if ((((cx - ox) / 12) + ((cy - oy) / 12)) % 2 == 0)
                    p.drawRect(cx, cy, 12, 12);
    }

    p.drawPixmap(ox, oy, dw, dh, frame_pixmap_);

    p.save();
    p.setClipRect(QRectF(ox, oy, dw, dh));
    p.translate(origin);
    p.scale(scale, scale);
    for (const auto &layer : title_->layers) {
        if (!layer || !layer->visible || !layer->use_as_scene_mask)
            continue;

        const QRectF local_rect = layer_local_rect(*layer);
        if (!local_rect.isValid() || local_rect.isEmpty())
            continue;

        const double local_time = std::clamp(playhead_ - layer->in_time, 0.0,
                                             std::max(0.0, layer->out_time - layer->in_time));
        QPainterPath path = editor_scene_mask_layer_path(*layer, local_rect, local_time);
        QPen border(TitlePreferences::scene_mask_color(), layer->type == LayerType::Shape && layer->shape_type == ShapeType::Line ? 3.0 : 2.0);
        border.setCosmetic(true);

        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setTransform(editor_layer_world_transform(title_, *layer, playhead_), true);
        p.setPen(border);
        p.setBrush(layer->type == LayerType::Shape && layer->shape_type == ShapeType::Line
                       ? Qt::NoBrush
                       : scene_mask_hatch_brush());
        p.drawPath(path);
        p.restore();
    }
    p.restore();

    if (inline_text_editor_ && inline_text_editor_->isVisible()) {
        const QPoint viewport_origin = inline_text_editor_->viewport()->mapTo(this, QPoint(0, 0));
        const std::vector<QRectF> selection_rects = text_edit_selection_viewport_rects(inline_text_editor_);
        if (!selection_rects.empty()) {
            p.save();
            p.setClipRect(inline_text_editor_->viewport()->rect().translated(viewport_origin));
            p.setCompositionMode(QPainter::CompositionMode_Difference);
            p.setPen(Qt::NoPen);
            p.setBrush(Qt::white);
            for (QRectF selection_rect : selection_rects) {
                selection_rect.translate(viewport_origin);
                p.drawRect(selection_rect);
            }
            p.restore();
        }
    }

    if (safe_guides_visible_) {
        auto draw_guide = [&](double inset, const QColor &color) {
            QRectF r(ox + dw * inset, oy + dh * inset, dw * (1.0 - 2.0 * inset), dh * (1.0 - 2.0 * inset));
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(color, 1.0, Qt::DashLine));
            p.drawRect(r);
        };
        draw_guide(OBS_ACTION_SAFE_PERCENT, QColor(0, 200, 255, 190));
        draw_guide(OBS_GRAPHICS_SAFE_PERCENT, QColor(255, 220, 0, 190));
    }

    draw_user_guides(p, QRectF(ox, oy, dw, dh));

    if (!snap_feedback_.empty()) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        QPen guide_pen(QColor(0, 220, 255, 235), 1.0, Qt::DashLine);
        guide_pen.setDashPattern({5.0, 3.0});
        p.setPen(guide_pen);
        p.setBrush(QColor(0, 20, 30, 180));
        for (const auto &feedback : snap_feedback_) {
            if (feedback.x_axis) {
                double x = canvas_to_view(QPointF(feedback.value, 0.0)).x();
                p.drawLine(QPointF(x, oy), QPointF(x, oy + dh));
                if (!feedback.label.isEmpty())
                    p.drawText(QRectF(x + 5.0, oy + 5.0, 120.0, 18.0), feedback.label);
            } else {
                double y = canvas_to_view(QPointF(0.0, feedback.value)).y();
                p.drawLine(QPointF(ox, y), QPointF(ox + dw, y));
                if (!feedback.label.isEmpty())
                    p.drawText(QRectF(ox + 5.0, y + 5.0, 120.0, 18.0), feedback.label);
            }
        }
        p.restore();
    }

    auto layers = selected_layers();

    auto draw_layer_box = [&](const Layer &layer, bool handles) {
        QRectF box = layer_local_rect(layer);
        auto layer_point_to_view = [&](const QPointF &pt) {
            return canvas_to_view(layer_to_canvas(layer, pt));
        };
        const QPointF corners[] = {
            layer_point_to_view(box.topLeft()),
            layer_point_to_view(box.topRight()),
            layer_point_to_view(box.bottomRight()),
            layer_point_to_view(box.bottomLeft())
        };

        const bool editing_text_layer = !inline_text_layer_id_.empty() &&
            inline_text_layer_id_ == layer.id && is_canvas_text_layer(layer);
        const QColor outline_color = editing_text_layer
            ? QColor(255, 220, 0, handles ? 255 : 210)
            : QColor(0, 120, 255, handles ? 230 : 150);
        const QColor handle_stroke_color = editing_text_layer
            ? QColor(255, 220, 0, 255)
            : QColor(0, 120, 255, 255);

        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(outline_color, editing_text_layer ? 2.0 : 1.5, Qt::DashLine));
        QPolygonF outline;
        for (const QPointF &corner : corners)
            outline << corner;
        p.drawPolygon(outline);
        if (handles) {
            p.setPen(QPen(handle_stroke_color, 1.0));
            p.setBrush(QColor(255, 255, 255));
            const QPointF handle_points[] = {
                corners[0], layer_point_to_view(QPointF(box.center().x(), box.top())), corners[1],
                layer_point_to_view(QPointF(box.right(), box.center().y())), corners[2],
                layer_point_to_view(QPointF(box.center().x(), box.bottom())), corners[3],
                layer_point_to_view(QPointF(box.left(), box.center().y()))
            };
            const double half_handle = CANVAS_CONTROL_SIZE_PX / 2.0;
            for (const QPointF &pt : handle_points)
                p.drawRect(QRectF(pt.x() - half_handle, pt.y() - half_handle,
                                  CANVAS_CONTROL_SIZE_PX, CANVAS_CONTROL_SIZE_PX));

            QPointF origin = layer_point_to_view(QPointF(0, 0));
            p.setPen(QPen(QColor(255, 160, 0), 1.5));
            p.setBrush(QColor(255, 220, 80));
            p.drawEllipse(origin, CANVAS_ORIGIN_RADIUS_PX, CANVAS_ORIGIN_RADIUS_PX);
            p.drawLine(QPointF(origin.x() - CANVAS_CONTROL_SIZE_PX, origin.y()),
                       QPointF(origin.x() + CANVAS_CONTROL_SIZE_PX, origin.y()));
            p.drawLine(QPointF(origin.x(), origin.y() - CANVAS_CONTROL_SIZE_PX),
                       QPointF(origin.x(), origin.y() + CANVAS_CONTROL_SIZE_PX));

            draw_gradient_handles(p, layer);
            draw_corner_radius_handles(p, layer);
        }
        p.restore();
    };

    if (layers.size() == 1) {
        draw_layer_box(*layers.front(), true);
    } else if (layers.size() > 1) {
        for (const auto &layer : layers)
            if (layer) draw_layer_box(*layer, false);

        QRectF bounds = selected_canvas_bounds();
        if (bounds.isValid() && !bounds.isEmpty()) {
            QRectF view_bounds(canvas_to_view(bounds.topLeft()), canvas_to_view(bounds.bottomRight()));
            view_bounds = view_bounds.normalized();
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 160, 255, 240), 1.5, Qt::SolidLine));
            p.drawRect(view_bounds);
            p.setPen(QPen(QColor(0, 120, 255, 255), 1.0));
            p.setBrush(QColor(255, 255, 255));
            const QPointF points[] = {
                view_bounds.topLeft(), QPointF(view_bounds.center().x(), view_bounds.top()), view_bounds.topRight(),
                QPointF(view_bounds.right(), view_bounds.center().y()), view_bounds.bottomRight(),
                QPointF(view_bounds.center().x(), view_bounds.bottom()), view_bounds.bottomLeft(),
                QPointF(view_bounds.left(), view_bounds.center().y())
            };
            for (const QPointF &pt : points)
                p.drawRect(QRectF(pt.x() - CANVAS_CONTROL_SIZE_PX / 2.0,
                                  pt.y() - CANVAS_CONTROL_SIZE_PX / 2.0,
                                  CANVAS_CONTROL_SIZE_PX, CANVAS_CONTROL_SIZE_PX));
        }
    }

    draw_toolbar_preview(p);

    if (drag_mode_ == DragMode::Rotate) {
        QPointF pivot = canvas_to_view(drag_rotation_pivot_canvas_);
        double start_angle = std::atan2(drag_start_view_.y() - pivot.y(), drag_start_view_.x() - pivot.x());
        double radius = std::max(24.0, QLineF(pivot, drag_current_view_).length());
        QRectF arc_rect(pivot.x() - radius, pivot.y() - radius, radius * 2.0, radius * 2.0);
        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(255, 190, 40, 235), 1.5, Qt::DashLine));
        p.drawLine(pivot, drag_current_view_);
        p.setPen(QPen(QColor(255, 190, 40, 235), 2.0));
        p.drawEllipse(pivot, CANVAS_ORIGIN_RADIUS_PX + 2.0, CANVAS_ORIGIN_RADIUS_PX + 2.0);
        p.drawArc(arc_rect, (int)std::round(-radians_to_degrees(start_angle) * 16.0),
                  (int)std::round(-drag_current_rotation_delta_ * 16.0));
        p.restore();
    }

    if (drag_mode_ == DragMode::Marquee && marquee_active_) {
        QRectF marquee(drag_start_view_, drag_current_view_);
        marquee = marquee.normalized();
        p.setBrush(QColor(0, 120, 255, 45));
        p.setPen(QPen(QColor(0, 160, 255, 220), 1.0, Qt::DashLine));
        p.drawRect(marquee);
    }

    draw_canvas_drag_tooltip(p);
    draw_color_picker_tooltip(p);
    draw_rulers(p, QRectF(ox, oy, dw, dh), scale, origin);
}
double CanvasPreview::toolbar_draw_aspect_ratio() const
{
    if (active_tool_ == CanvasTool::Text) {
        const double default_width = title_ ? std::max(1.0, title_->width * 0.5) : 960.0;
        return default_width / 160.0;
    }
    return 1.0;
}

static QRectF editor_modifier_rect(const QPointF &anchor, const QPointF &current,
                                   Qt::KeyboardModifiers modifiers, double aspect,
                                   double fixed_width, double fixed_height)
{
    QPointF delta = current - anchor;
    double width = fixed_width > 0.0 ? fixed_width : std::abs(delta.x());
    double height = fixed_height > 0.0 ? fixed_height : std::abs(delta.y());

    if (modifiers.testFlag(Qt::ShiftModifier)) {
        aspect = std::max(0.001, aspect);
        if (fixed_width > 0.0 && fixed_height <= 0.0)
            width = height * aspect;
        else if (fixed_height > 0.0 && fixed_width <= 0.0)
            height = width / aspect;
        else if (width / std::max(1.0, height) > aspect)
            height = width / aspect;
        else
            width = height * aspect;
    }

    const double sign_x = delta.x() < 0.0 ? -1.0 : 1.0;
    const double sign_y = delta.y() < 0.0 ? -1.0 : 1.0;
    QRectF rect;
    if (modifiers.testFlag(Qt::AltModifier)) {
        rect = QRectF(anchor.x() - width,
                      anchor.y() - height,
                      width * 2.0, height * 2.0);
    } else {
        const bool center_fixed_width = fixed_width > 0.0;
        const bool center_fixed_height = fixed_height > 0.0;
        const double left = center_fixed_width ? anchor.x() - width * 0.5 : std::min(anchor.x(), anchor.x() + sign_x * width);
        const double right = center_fixed_width ? anchor.x() + width * 0.5 : std::max(anchor.x(), anchor.x() + sign_x * width);
        const double top = center_fixed_height ? anchor.y() - height * 0.5 : std::min(anchor.y(), anchor.y() + sign_y * height);
        const double bottom = center_fixed_height ? anchor.y() + height * 0.5 : std::max(anchor.y(), anchor.y() + sign_y * height);
        rect = QRectF(QPointF(left, top), QPointF(right, bottom));
    }

    rect = rect.normalized();
    if (rect.width() < 1.0) rect.setWidth(1.0);
    if (rect.height() < 1.0) rect.setHeight(1.0);
    return rect;
}

QRectF CanvasPreview::toolbar_draw_rect(const QPointF &canvas_pt, Qt::KeyboardModifiers modifiers) const
{
    return editor_modifier_rect(shape_draw_start_canvas_, canvas_pt, modifiers, toolbar_draw_aspect_ratio());
}

QRectF CanvasPreview::snapped_toolbar_draw_rect(const QRectF &raw_rect, bool allow_snap)
{
    QRectF rect = raw_rect.normalized();
    if (!allow_snap || !rect.isValid()) {
        clear_snap_feedback();
        return rect;
    }

    return rect;
}

QRect CanvasPreview::toolbar_preview_update_rect() const
{
    if (!title_ || !drawing_shape_)
        return QRect();

    const QRectF canvas_rect = shape_draw_current_rect_.isValid()
                                  ? shape_draw_current_rect_
                                  : toolbar_draw_rect(shape_draw_current_canvas_, shape_draw_modifiers_);
    QRectF view_rect(canvas_to_view(canvas_rect.topLeft()), canvas_to_view(canvas_rect.bottomRight()));
    view_rect = view_rect.normalized();
    return view_rect.adjusted(-24.0, -24.0, 24.0, 24.0).toAlignedRect();
}

void CanvasPreview::draw_toolbar_preview(QPainter &p)
{
    if (!title_ || !drawing_shape_)
        return;

    const QRectF canvas_rect = shape_draw_current_rect_.isValid()
                                  ? shape_draw_current_rect_
                                  : toolbar_draw_rect(shape_draw_current_canvas_, shape_draw_modifiers_);
    QRectF view_rect(canvas_to_view(canvas_rect.topLeft()), canvas_to_view(canvas_rect.bottomRight()));
    view_rect = view_rect.normalized();

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);

    const bool text_preview = active_tool_ == CanvasTool::Text;
    const QColor accent = text_preview ? layer_type_color(active_text_layer_type_) : layer_type_color(LayerType::Shape);
    QColor fill = accent;
    fill.setAlpha(text_preview ? 38 : (active_shape_type_ == ShapeType::Line ? 0 : 42));
    QColor stroke = accent.lighter(135);
    stroke.setAlpha(235);

    QPen pen(stroke, active_shape_type_ == ShapeType::Line && !text_preview ? 2.0 : 1.6, Qt::DashLine);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(text_preview ? QColor(20, 20, 20, 90) : fill);

    if (text_preview) {
        p.drawRoundedRect(view_rect, 4.0, 4.0);
        QString label = text_tool_display_name(active_text_layer_type_);
        p.setPen(stroke);
        QFont label_font = p.font();
        label_font.setBold(true);
        p.setFont(label_font);
        p.drawText(view_rect.adjusted(8.0, 6.0, -8.0, -6.0), Qt::AlignLeft | Qt::AlignTop, label);
    } else {
        p.drawPath(tool_shape_path(active_shape_type_, view_rect));
    }

    p.restore();
}

bool CanvasPreview::sample_color_at_view(const QPointF &view_pt, QColor &color)
{
    if (!title_) return false;
    if (dirty_ || frame_pixmap_.isNull())
        render_to_pixmap();
    if (frame_pixmap_.isNull()) return false;

    const QPointF canvas = view_to_canvas(view_pt);
    const QImage image = frame_pixmap_.toImage();
    if (image.isNull()) return false;

    const int x = (int)std::floor(canvas.x());
    const int y = (int)std::floor(canvas.y());
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height())
        return false;

    color = image.pixelColor(x, y);
    return color.isValid();
}

void CanvasPreview::update_color_picker_tooltip(const QPointF &view_pt)
{
    color_picker_tooltip_pos_ = view_pt;
    QColor color;
    color_picker_tooltip_visible_ = sample_color_at_view(view_pt, color);
    if (color_picker_tooltip_visible_)
        color_picker_tooltip_color_ = color;
    update();
}

static QString editor_color_hex(const QColor &color)
{
    if (color.alpha() < 255) {
        return QStringLiteral("#%1%2%3%4")
            .arg(color.red(), 2, 16, QLatin1Char('0'))
            .arg(color.green(), 2, 16, QLatin1Char('0'))
            .arg(color.blue(), 2, 16, QLatin1Char('0'))
            .arg(color.alpha(), 2, 16, QLatin1Char('0'))
            .toUpper();
    }
    return QStringLiteral("#%1%2%3")
        .arg(color.red(), 2, 16, QLatin1Char('0'))
        .arg(color.green(), 2, 16, QLatin1Char('0'))
        .arg(color.blue(), 2, 16, QLatin1Char('0'))
        .toUpper();
}

void CanvasPreview::draw_color_picker_tooltip(QPainter &p)
{
    if (active_tool_ != CanvasTool::ColorPicker || !color_picker_tooltip_visible_)
        return;

    const QString hex = editor_color_hex(color_picker_tooltip_color_);
    const QFontMetrics fm(font());
    const int swatch = 18;
    const int pad = 8;
    const int gap = 7;
    const int width = pad * 2 + swatch + gap + fm.horizontalAdvance(hex);
    const int height = std::max(30, pad * 2 + swatch);
    QPointF pos = color_picker_tooltip_pos_ + QPointF(16.0, 18.0);
    if (pos.x() + width + 4 > rect().right())
        pos.setX(color_picker_tooltip_pos_.x() - width - 16.0);
    if (pos.y() + height + 4 > rect().bottom())
        pos.setY(color_picker_tooltip_pos_.y() - height - 16.0);
    pos.setX(std::max(4.0, pos.x()));
    pos.setY(std::max(4.0, pos.y()));

    QRectF box(pos, QSizeF(width, height));
    QRectF swatch_rect(box.left() + pad, box.top() + (box.height() - swatch) / 2.0, swatch, swatch);
    QRectF text_rect(swatch_rect.right() + gap, box.top(), width - pad - swatch - gap, height);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(18, 18, 18), 1.0));
    p.setBrush(QColor(48, 48, 48, 235));
    p.drawRoundedRect(box, 4.0, 4.0);

    const int cell = 5;
    p.setPen(Qt::NoPen);
    for (int y = (int)swatch_rect.top(); y < (int)swatch_rect.bottom(); y += cell) {
        for (int x = (int)swatch_rect.left(); x < (int)swatch_rect.right(); x += cell) {
            const bool dark = ((x / cell) + (y / cell)) % 2;
            p.fillRect(QRect(x, y, cell, cell).intersected(swatch_rect.toAlignedRect()),
                       dark ? QColor(95, 95, 95) : QColor(165, 165, 165));
        }
    }
    p.fillRect(swatch_rect, color_picker_tooltip_color_);
    p.setPen(QPen(QColor(16, 16, 16), 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(swatch_rect.adjusted(0.0, 0.0, -1.0, -1.0));

    p.setPen(QColor(235, 235, 235));
    p.drawText(text_rect, Qt::AlignVCenter | Qt::AlignLeft, hex);
    p.restore();
}

QString CanvasPreview::canvas_drag_tooltip_text() const
{
    if (drag_mode_ == DragMode::None || drag_mode_ == DragMode::Marquee || !drag_changed_)
        return QString();

    auto is_resize_drag = [](DragMode mode) {
        return mode == DragMode::ResizeNW || mode == DragMode::ResizeN || mode == DragMode::ResizeNE ||
               mode == DragMode::ResizeE || mode == DragMode::ResizeSE || mode == DragMode::ResizeS ||
               mode == DragMode::ResizeSW || mode == DragMode::ResizeW;
    };
    auto is_corner_radius_drag = [](DragMode mode) {
        return mode == DragMode::CornerRadiusTL || mode == DragMode::CornerRadiusTR ||
               mode == DragMode::CornerRadiusBR || mode == DragMode::CornerRadiusBL;
    };

    if (drag_mode_ == DragMode::Move) {
        const QPointF canvas = view_to_canvas(drag_current_view_);
        return QStringLiteral("X: %1 px  Y: %2 px")
            .arg(std::round(canvas.x()), 0, 'f', 0)
            .arg(std::round(canvas.y()), 0, 'f', 0);
    }

    if (is_resize_drag(drag_mode_)) {
        const auto layers = selected_layers();
        double w = 0.0;
        double h = 0.0;
        if (layers.size() > 1) {
            const QRectF bounds = selected_canvas_bounds();
            w = bounds.width();
            h = bounds.height();
        } else if (!layers.empty() && layers.front()) {
            const double lt = std::clamp(playhead_ - layers.front()->in_time, 0.0,
                                         std::max(0.0, layers.front()->out_time - layers.front()->in_time));
            w = eval_box_width(*layers.front(), lt);
            h = eval_box_height(*layers.front(), lt);
        }
        return QStringLiteral("W: %1 px  H: %2 px")
            .arg(std::round(w), 0, 'f', 0)
            .arg(std::round(h), 0, 'f', 0);
    }

    if (drag_mode_ == DragMode::Rotate) {
        const double radians = degrees_to_radians(drag_current_rotation_delta_);
        return QStringLiteral("Rad: %1").arg(radians, 0, 'f', 3);
    }

    if (is_corner_radius_drag(drag_mode_)) {
        auto layer = selected_layer();
        if (!layer)
            return QString();
        double radius = 0.0;
        switch (drag_mode_) {
        case DragMode::CornerRadiusTL: radius = layer->corner_radius_tl; break;
        case DragMode::CornerRadiusTR: radius = layer->corner_radius_tr; break;
        case DragMode::CornerRadiusBR: radius = layer->corner_radius_br; break;
        case DragMode::CornerRadiusBL: radius = layer->corner_radius_bl; break;
        default: break;
        }
        return QStringLiteral("R: %1 px").arg(std::round(radius), 0, 'f', 0);
    }

    return QString();
}

void CanvasPreview::draw_canvas_drag_tooltip(QPainter &p)
{
    const QString text = canvas_drag_tooltip_text();
    if (text.isEmpty())
        return;

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QFontMetrics fm(font());
    const int width = fm.horizontalAdvance(text) + 18;
    const int height = std::max(24, fm.height() + 8);
    QPointF pos = drag_current_view_ + QPointF(14.0, 22.0);
    if (pos.x() + width + 6 > rect().right())
        pos.setX(drag_current_view_.x() - width - 14.0);
    if (pos.y() + height + 6 > rect().bottom())
        pos.setY(drag_current_view_.y() - height - 14.0);
    pos.setX(std::max(6.0, pos.x()));
    pos.setY(std::max(6.0, pos.y()));

    const QRectF box(pos, QSizeF(width, height));
    p.setPen(QPen(QColor(18, 18, 18), 1.0));
    p.setBrush(QColor(48, 48, 48, 235));
    p.drawRoundedRect(box, 4.0, 4.0);
    p.setPen(QColor(245, 245, 245));
    p.drawText(box.adjusted(9.0, 0.0, -9.0, 0.0), Qt::AlignVCenter | Qt::AlignLeft, text);
    p.restore();
}

void CanvasPreview::update_shape_drawing(const QPointF &view_pt, Qt::KeyboardModifiers modifiers)
{
    if (!drawing_shape_) return;

    const QRect old_update_rect = last_toolbar_preview_update_rect_;
    shape_draw_current_canvas_ = view_to_canvas(view_pt);
    shape_draw_modifiers_ = modifiers;
    const bool allow_snap = !modifiers.testFlag(Qt::ControlModifier);
    if (allow_snap)
        snap_feedback_.clear();

    const QPointF raw_current = shape_draw_current_canvas_;
    QRectF raw_rect = toolbar_draw_rect(raw_current, shape_draw_modifiers_);
    if (allow_snap && raw_rect.isValid()) {
        const QPointF delta = raw_current - shape_draw_start_canvas_;
        const bool alt = modifiers.testFlag(Qt::AltModifier);
        const bool active_left = alt ? delta.x() < 0.0 : raw_rect.left() < shape_draw_start_canvas_.x();
        const bool active_right = alt ? delta.x() >= 0.0 : raw_rect.right() > shape_draw_start_canvas_.x();
        const bool active_top = alt ? delta.y() < 0.0 : raw_rect.top() < shape_draw_start_canvas_.y();
        const bool active_bottom = alt ? delta.y() >= 0.0 : raw_rect.bottom() > shape_draw_start_canvas_.y();

        auto snap_value = [&](bool x_axis, double value, double *snapped) {
            if (!snapped)
                return false;
            std::vector<double> targets;
            std::vector<QString> labels;
            collect_snap_targets(x_axis, targets, labels);
            collect_spacing_targets(x_axis, targets, labels);
            const double tolerance = 6.0 / std::max(0.1, view_scale());
            double best_distance = tolerance + 1.0;
            double best_target = value;
            QString best_label;
            for (size_t i = 0; i < targets.size(); ++i) {
                const double distance = std::abs(targets[i] - value);
                if (distance < best_distance) {
                    best_distance = distance;
                    best_target = targets[i];
                    best_label = labels[i];
                }
            }
            if (best_distance > tolerance)
                return false;
            *snapped = best_target;
            add_snap_feedback(x_axis, best_target, best_label);
            return true;
        };

        QPointF snapped_current = raw_current;
        double snapped = 0.0;
        if (active_left && snap_value(true, raw_rect.left(), &snapped))
            snapped_current.setX(raw_current.x() + snapped - raw_rect.left());
        else if (active_right && snap_value(true, raw_rect.right(), &snapped))
            snapped_current.setX(raw_current.x() + snapped - raw_rect.right());
        if (active_top && snap_value(false, raw_rect.top(), &snapped))
            snapped_current.setY(raw_current.y() + snapped - raw_rect.top());
        else if (active_bottom && snap_value(false, raw_rect.bottom(), &snapped))
            snapped_current.setY(raw_current.y() + snapped - raw_rect.bottom());

        shape_draw_current_canvas_ = snapped_current;
        raw_rect = toolbar_draw_rect(shape_draw_current_canvas_, shape_draw_modifiers_);
    }
    shape_draw_current_rect_ = raw_rect.normalized();
    drawing_shape_changed_ = shape_draw_current_rect_.width() >= 2.0 || shape_draw_current_rect_.height() >= 2.0;

    last_toolbar_preview_update_rect_ = toolbar_preview_update_rect();
    QRect repaint_rect = old_update_rect.united(last_toolbar_preview_update_rect_);
    if (repaint_rect.isEmpty())
        repaint_rect = this->rect();
    update(repaint_rect);
}


std::shared_ptr<Layer> CanvasPreview::text_layer_at_view_pos(const QPointF &view_pt) const
{
    if (!title_) return nullptr;
    QPointF canvas = view_to_canvas(view_pt);
    for (auto it = title_->layers.rbegin(); it != title_->layers.rend(); ++it) {
        const auto &layer = *it;
        if (!layer || !is_canvas_text_layer(*layer) || layer->type == LayerType::Clock) continue;
        if (!layer->visible || layer->locked) continue;
        if (playhead_ < layer->in_time || playhead_ > layer->out_time) continue;
        if (layer_local_rect(*layer).contains(canvas_to_layer(*layer, canvas)))
            return layer;
    }
    return nullptr;
}


static QString scale_rich_text_font_sizes(const QString &html, double scale)
{
    if (html.isEmpty() || std::abs(scale - 1.0) < 0.0001)
        return html;

    QString scaled = html;
    QRegularExpression re(
        QStringLiteral("((?:font-size|margin-left|margin-right|margin-top|margin-bottom|text-indent)\\s*:\\s*)(-?[0-9]+(?:\\.[0-9]+)?)(px|pt)"),
        QRegularExpression::CaseInsensitiveOption);
    qsizetype offset = 0;
    QRegularExpressionMatch match;
    while ((match = re.match(scaled, offset)).hasMatch()) {
        const QString property = match.captured(1);
        const double value = match.captured(2).toDouble();
        const QString unit = match.captured(3);
        const double scaled_value = property.trimmed().startsWith(QStringLiteral("font-size"), Qt::CaseInsensitive)
                                      ? std::max(1.0, value * scale)
                                      : value * scale;
        const QString replacement = QStringLiteral("%1%2%3")
                                        .arg(property)
                                        .arg(scaled_value, 0, 'f', 3)
                                        .arg(unit);
        scaled.replace(match.capturedStart(0), match.capturedLength(0), replacement);
        offset = match.capturedStart(0) + replacement.size();
    }
    return scaled;
}


static bool inline_document_has_style_overrides(const QTextDocument *doc, const Layer &layer, double t, double visual_scale)
{
    if (!doc) return false;

    QFont expected_font = font_for_layer(layer, t);
    if (expected_font.pixelSize() > 0)
        expected_font.setPixelSize(std::max(1, (int)std::round(expected_font.pixelSize() * visual_scale)));
    const QColor expected_color = color_from_argb(eval_text_color(layer, t));
    const int expected_weight = expected_font.weight();
    const bool expected_italic = expected_font.italic();
    const bool expected_underline = layer.text_underline;
    const bool expected_strike = layer.text_strikethrough;

    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || fragment.text().isEmpty()) continue;
            const QTextCharFormat fmt = fragment.charFormat();
            if (fmt.fontWeight() != expected_weight) return true;
            if (fmt.fontItalic() != expected_italic) return true;
            if (fmt.fontUnderline() != expected_underline) return true;
            if (fmt.fontStrikeOut() != expected_strike) return true;
            if (fmt.hasProperty(QTextFormat::FontFamily) && fmt.fontFamily() != expected_font.family()) return true;
            if (fmt.hasProperty(QTextFormat::FontPixelSize) && std::abs(fmt.font().pixelSize() - expected_font.pixelSize()) > 1) return true;
            if (fmt.hasProperty(QTextFormat::FontPointSize) && expected_font.pointSizeF() > 0.0 &&
                std::abs(fmt.fontPointSize() - expected_font.pointSizeF()) > 0.5) return true;
            if (fmt.foreground().style() != Qt::NoBrush) {
                const QColor color = fmt.foreground().color();
                if (layer.fill_type == 1 && color.isValid() && color.alpha() == 0)
                    continue;
                if (color.isValid() && color != expected_color) return true;
            }
        }
    }
    return false;
}

double CanvasPreview::inline_text_visual_scale(const Layer &layer) const
{
    const double lt = std::max(0.0, playhead_ - layer.in_time);
    const double sx = std::abs(layer.scale_x.evaluate(lt));
    const double sy = std::abs(layer.scale_y.evaluate(lt));
    return std::clamp(view_scale() * std::sqrt(std::max(0.0001, sx * sy)), 0.05, 16.0);
}

void CanvasPreview::configure_inline_text_editor(const Layer &layer)
{
    if (!inline_text_editor_) return;

    QSignalBlocker blocker(inline_text_editor_);
    QTextCursor saved_cursor = inline_text_editor_->textCursor();

    const double local_time = std::max(0.0, playhead_ - layer.in_time);
    const double visual_scale = inline_text_visual_scale(layer);
    QFont font = font_for_layer(layer, local_time);
    if (font.pixelSize() > 0)
        font.setPixelSize(std::max(1, (int)std::round(font.pixelSize() * visual_scale)));
    inline_text_editor_->setFont(font);
    QColor transparent_text_color = color_from_argb(eval_text_color(layer, local_time));
    transparent_text_color.setAlpha(0);
    inline_text_editor_->setTextColor(transparent_text_color);

    QTextDocument *doc = inline_text_editor_->document();
    doc->setDocumentMargin(0.0);
    doc->setDefaultFont(font);

    QTextOption option = doc->defaultTextOption();
    option.setUseDesignMetrics(true);
    option.setWrapMode(layer.text_overflow_mode == 0
                           ? (layer.paragraph_hyphenate ? QTextOption::WrapAnywhere
                                                         : QTextOption::WrapAtWordBoundaryOrAnywhere)
                           : QTextOption::NoWrap);
    Qt::Alignment align = Qt::AlignLeft;
    if (layer.align_h == 1 || layer.align_h == 4) align = Qt::AlignHCenter;
    else if (layer.align_h == 2 || layer.align_h == 5) align = Qt::AlignRight;
    else if (layer.align_h >= 3) align = Qt::AlignJustify;
    option.setAlignment(align);
    doc->setDefaultTextOption(option);

    const QRectF local = layer_local_rect(layer);
    const QRectF text_rect = text_rect_for_style(local, layer);
    const int wrap_width_px = std::max(1, (int)std::ceil(text_rect.width() * visual_scale));
    doc->setTextWidth(layer.text_overflow_mode == 2 ? -1.0 : (qreal)wrap_width_px);
    doc->setPageSize(layer.text_overflow_mode == 2
                         ? QSizeF(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX)
                         : QSizeF(wrap_width_px, QWIDGETSIZE_MAX));
    inline_text_editor_->setLineWrapMode(layer.text_overflow_mode == 0 ? QTextEdit::FixedPixelWidth : QTextEdit::NoWrap);
    inline_text_editor_->setLineWrapColumnOrWidth(wrap_width_px);
    inline_text_editor_->setWordWrapMode(option.wrapMode());

    QTextBlockFormat block_format;
    block_format.setAlignment(align);
    block_format.setLeftMargin(std::max(0.0, eval_paragraph_indent_left(layer, local_time)) * visual_scale);
    block_format.setRightMargin(std::max(0.0, eval_paragraph_indent_right(layer, local_time)) * visual_scale);
    block_format.setTextIndent(eval_paragraph_indent_first_line(layer, local_time) * visual_scale);
    block_format.setTopMargin(std::max(0.0, eval_paragraph_space_before(layer, local_time)) * visual_scale);
    block_format.setBottomMargin(std::max(0.0, eval_paragraph_space_after(layer, local_time)) * visual_scale);

    const bool has_structured_rich_text = !layer.rich_text.plain_text.empty() || !layer.rich_text.ranges.empty();
    QTextCharFormat char_format;
    char_format.setFont(font);
    RichTextCharFormat layer_format = layer_char_format_for_editor(layer);
    store_rich_text_format_properties(char_format, layer_format);
    QColor editor_text_color = color_from_argb(eval_text_color(layer, local_time));
    editor_text_color.setAlpha(0);
    char_format.setForeground(editor_text_color);
    char_format.setFontUnderline(layer.text_underline);
    char_format.setFontStrikeOut(layer.text_strikethrough);

    if (layer.rich_text_html.empty()) {
        QTextCursor format_cursor(doc);
        format_cursor.select(QTextCursor::Document);
        format_cursor.mergeBlockFormat(block_format);
        if (!has_structured_rich_text)
            format_cursor.mergeCharFormat(char_format);
    } else {
        QTextCursor format_cursor(doc);
        format_cursor.select(QTextCursor::Document);
        format_cursor.mergeCharFormat(char_format);
    }
    /*
     * Do not call mergeCurrentCharFormat() while the saved cursor owns a
     * selection. QTextEdit applies that merge to the selected document text,
     * so re-positioning the inline editor after begin_text_edit() could repaint
     * every character with the current/layer style and hide mixed per-character
     * sizes or colors until edit mode was committed.
     */
    if (!saved_cursor.hasSelection())
        inline_text_editor_->mergeCurrentCharFormat(char_format);
    if (auto *layout = doc->documentLayout())
        layout->documentSize();
    inline_text_editor_->setTextCursor(saved_cursor);
}

bool CanvasPreview::sync_inline_text_layer(bool mark_dirty)
{
    if (!inline_text_editor_ || inline_text_layer_id_.empty() || !title_) return false;
    auto layer = title_->find_layer(inline_text_layer_id_);
    if (!layer) return false;

    QTextDocument *editor_doc = inline_text_editor_->document();
    const std::string plain = inline_text_editor_->toPlainText().toStdString();
    if (plain == layer->text_content && editor_doc && !editor_doc->isModified()) {
        const QTextCursor cursor = inline_text_editor_->textCursor();
        RichTextSelection selection{(size_t)std::max(0, cursor.anchor()),
                                    (size_t)std::max(0, cursor.position())};
        const size_t text_len = layer->rich_text.plain_text.size();
        selection.anchor = std::min(selection.anchor, text_len);
        selection.head = std::min(selection.head, text_len);
        const bool selection_changed = layer->rich_text.selection.anchor != selection.anchor ||
                                       layer->rich_text.selection.head != selection.head;
        if (selection_changed)
            layer->rich_text.selection = selection;
        if (!cursor.hasSelection()) {
            const double visual_scale = inline_text_visual_scale(*layer);
            layer->rich_text.typing_format = rich_text_format_from_qtext_format(cursor.charFormat(),
                                                                               layer->rich_text.default_format,
                                                                               visual_scale);
            layer->rich_text.has_typing_format = true;
            rich_text_document_sync_layer_mirrors(*layer);
        } else {
            layer->rich_text.has_typing_format = false;
        }
        return selection_changed;
    }

    const double visual_scale = inline_text_visual_scale(*layer);
    RichTextDocument next_model = rich_text_document_from_qtext_document(editor_doc, *layer, visual_scale, inline_text_editor_->textCursor());
    const bool selection_changed = layer->rich_text.selection.anchor != next_model.selection.anchor ||
                                   layer->rich_text.selection.head != next_model.selection.head;
    const bool changed = layer->text_content != plain || layer->rich_text.plain_text != next_model.plain_text ||
                         !rich_text_char_formats_equal(layer->rich_text.default_format, next_model.default_format) ||
                         layer->rich_text.default_paragraph_format.align_h != next_model.default_paragraph_format.align_h ||
                         layer->rich_text.default_paragraph_format.align_v != next_model.default_paragraph_format.align_v ||
                         std::abs(layer->rich_text.default_paragraph_format.indent_left - next_model.default_paragraph_format.indent_left) >= 0.0001f ||
                         std::abs(layer->rich_text.default_paragraph_format.indent_right - next_model.default_paragraph_format.indent_right) >= 0.0001f ||
                         std::abs(layer->rich_text.default_paragraph_format.indent_first_line - next_model.default_paragraph_format.indent_first_line) >= 0.0001f ||
                         std::abs(layer->rich_text.default_paragraph_format.line_spacing - next_model.default_paragraph_format.line_spacing) >= 0.0001f ||
                         std::abs(layer->rich_text.default_paragraph_format.space_before - next_model.default_paragraph_format.space_before) >= 0.0001f ||
                         std::abs(layer->rich_text.default_paragraph_format.space_after - next_model.default_paragraph_format.space_after) >= 0.0001f ||
                         layer->rich_text.default_paragraph_format.hyphenate != next_model.default_paragraph_format.hyphenate ||
                         layer->rich_text.has_typing_format != next_model.has_typing_format ||
                         (layer->rich_text.has_typing_format && !rich_text_char_formats_equal(layer->rich_text.typing_format, next_model.typing_format)) ||
                         !rich_text_ranges_equal(layer->rich_text.ranges, next_model.ranges);
    if (!changed) {
        if (selection_changed)
            layer->rich_text.selection = next_model.selection;
        return false;
    }

    layer->text_content = plain;
    layer->rich_text = std::move(next_model);
    layer->rich_text_html.clear();
    rich_text_document_sync_layer_mirrors(*layer);
    if (editor_doc)
        editor_doc->setModified(false);
    if (mark_dirty) dirty_ = true;
    return true;
}

void CanvasPreview::refresh_inline_text_edit(bool mark_dirty, bool emit_changed)
{
    if (committing_inline_text_ || updating_inline_text_editor_ || refreshing_inline_text_ ||
        !inline_text_editor_ || inline_text_layer_id_.empty())
        return;

    refreshing_inline_text_ = true;
    const std::string layer_id = inline_text_layer_id_;
    const bool model_changed = sync_inline_text_layer(mark_dirty);

    if (auto layer = title_ ? title_->find_layer(layer_id) : nullptr) {
        if ((layer->text_box_width_to_text || layer->text_box_height_to_text) && inline_text_editor_) {
            QTextDocument *doc = inline_text_editor_->document();
            const double visual_scale = std::max(0.0001, inline_text_visual_scale(*layer));
            QSizeF doc_size;
            if (doc) {
                if (auto *layout = doc->documentLayout())
                    doc_size = layout->documentSize();
                if (!doc_size.isValid() || doc_size.isEmpty())
                    doc_size = doc->size();
            }
            QFontMetricsF metrics(inline_text_editor_->currentFont());
            const double min_w = std::max(24.0, metrics.horizontalAdvance(QStringLiteral("M")) / visual_scale);
            const double min_h = std::max(12.0, metrics.lineSpacing() / visual_scale);
            if (layer->text_box_width_to_text) {
                const double ideal = doc ? std::max(doc->idealWidth(), doc_size.width()) : 0.0;
                const double next_w = std::clamp(ideal / visual_scale + 2.0, min_w, (double)std::max(1.0f, layer->max_text_box_width));
                if (std::abs(layer->rect_width - next_w) > 0.5) {
                    layer->rect_width = (float)next_w;
                    layer->box_width.static_value = next_w;
                    dirty_ = true;
                }
            }
            if (layer->text_box_height_to_text) {
                const double next_h = std::clamp(doc_size.height() / visual_scale + 2.0, min_h, (double)std::max(1.0f, layer->max_text_box_height));
                if (std::abs(layer->rect_height - next_h) > 0.5) {
                    layer->rect_height = (float)next_h;
                    layer->box_height.static_value = next_h;
                    dirty_ = true;
                }
            }
        }
    }

    if (mark_dirty || model_changed)
        dirty_ = true;

    position_text_editor();

    if (dirty_)
        render_to_pixmap();

    if (inline_text_editor_) {
        const QRect editor_rect = inline_text_editor_->geometry().adjusted(-4, -4, 4, 4);
        update(editor_rect);
        inline_text_editor_->update();
        inline_text_editor_->viewport()->update();
        repaint(editor_rect);
    } else {
        update();
    }

    refreshing_inline_text_ = false;

    if (emit_changed && (mark_dirty || model_changed))
        emit text_edit_changed(layer_id);
}

QRectF CanvasPreview::inline_text_document_local_rect(const Layer &layer) const
{
    const QRectF local = layer_local_rect(layer);
    const QRectF text_rect = text_rect_for_style(local, layer);

    const double visual_scale = inline_text_visual_scale(layer);
    const double local_time = std::max(0.0, playhead_ - layer.in_time);
    QFont font = font_for_layer(layer, local_time);
    if (font.pixelSize() > 0)
        font.setPixelSize(std::max(1, (int)std::round(font.pixelSize() * visual_scale)));

    QTextDocument measure_doc;
    measure_doc.setDocumentMargin(0.0);
    measure_doc.setDefaultFont(font);

    QTextOption option = measure_doc.defaultTextOption();
    option.setUseDesignMetrics(true);
    option.setWrapMode(layer.text_overflow_mode == 0
                           ? (layer.paragraph_hyphenate ? QTextOption::WrapAnywhere
                                                         : QTextOption::WrapAtWordBoundaryOrAnywhere)
                           : QTextOption::NoWrap);
    Qt::Alignment align = Qt::AlignLeft;
    if (layer.align_h == 1 || layer.align_h == 4) align = Qt::AlignHCenter;
    else if (layer.align_h == 2 || layer.align_h == 5) align = Qt::AlignRight;
    else if (layer.align_h >= 3) align = Qt::AlignJustify;
    option.setAlignment(align);
    measure_doc.setDefaultTextOption(option);

    const int wrap_width_px = std::max(1, (int)std::ceil(text_rect.width() * visual_scale));
    measure_doc.setTextWidth(layer.text_overflow_mode == 2 ? -1.0 : (qreal)wrap_width_px);
    measure_doc.setPageSize(layer.text_overflow_mode == 2
                                ? QSizeF(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX)
                                : QSizeF(wrap_width_px, QWIDGETSIZE_MAX));

    if (!layer.rich_text.plain_text.empty() || !layer.rich_text.ranges.empty()) {
        populate_qtext_document_from_rich_text(&measure_doc, layer.rich_text, visual_scale);
    } else if (!layer.rich_text_html.empty()) {
        measure_doc.setHtml(scale_rich_text_font_sizes(QString::fromStdString(layer.rich_text_html), visual_scale));
    } else {
        populate_qtext_document_from_plain_layer_text(&measure_doc, layer.text_content,
                                                      layer_char_format_for_editor(layer), visual_scale);
    }

    QTextBlockFormat block_format;
    block_format.setAlignment(align);
    block_format.setLeftMargin(std::max(0.0, eval_paragraph_indent_left(layer, local_time)) * visual_scale);
    block_format.setRightMargin(std::max(0.0, eval_paragraph_indent_right(layer, local_time)) * visual_scale);
    block_format.setTextIndent(eval_paragraph_indent_first_line(layer, local_time) * visual_scale);
    block_format.setTopMargin(std::max(0.0, eval_paragraph_space_before(layer, local_time)) * visual_scale);
    block_format.setBottomMargin(std::max(0.0, eval_paragraph_space_after(layer, local_time)) * visual_scale);
    QTextCursor format_cursor(&measure_doc);
    format_cursor.select(QTextCursor::Document);
    format_cursor.mergeBlockFormat(block_format);

    QSizeF doc_size;
    if (auto *layout = measure_doc.documentLayout()) {
        doc_size = layout->documentSize();
        if (!doc_size.isValid() || doc_size.isEmpty())
            doc_size = measure_doc.size();
    }

    const double doc_width = layer.text_overflow_mode == 2 && doc_size.width() > 0.0
                                 ? doc_size.width() / std::max(0.0001, visual_scale)
                                 : text_rect.width();
    const double doc_height = doc_size.height() > 0.0
                                  ? doc_size.height() / std::max(0.0001, visual_scale)
                                  : text_rect.height();

    double y = text_rect.top();
    if (layer.align_v == 1)
        y = text_rect.top() + (text_rect.height() - doc_height) / 2.0;
    else if (layer.align_v == 2)
        y = text_rect.bottom() - doc_height;
    y -= eval_baseline_shift(layer, local_time);

    double x = text_rect.left();
    if (layer.text_overflow_mode == 2 && doc_width < text_rect.width()) {
        if (layer.align_h == 1 || layer.align_h == 4)
            x = text_rect.left() + (text_rect.width() - doc_width) / 2.0;
        else if (layer.align_h == 2 || layer.align_h == 5)
            x = text_rect.right() - doc_width;
    }

    return QRectF(x, y, std::max(1.0, doc_width), std::max(1.0, doc_height));
}

void CanvasPreview::position_text_editor()
{
    if (!inline_text_editor_ || inline_text_layer_id_.empty() || !title_) return;
    auto layer = title_->find_layer(inline_text_layer_id_);
    if (!layer) {
        inline_text_editor_->hide();
        return;
    }

    const double visual_scale = inline_text_visual_scale(*layer);
    const bool was_updating_inline_text_editor = updating_inline_text_editor_;
    updating_inline_text_editor_ = true;
    configure_inline_text_editor(*layer);
    {
        QSignalBlocker blocker(inline_text_editor_);
        const QTextCursor saved_cursor = inline_text_editor_->textCursor();
        const int anchor = saved_cursor.anchor();
        const int position = saved_cursor.position();
        const QString layer_plain = !layer->rich_text.empty()
                                        ? QString::fromStdString(layer->rich_text.plain_text)
                                        : QString::fromStdString(layer->text_content);
        const bool scale_changed = std::abs(inline_text_last_visual_scale_ - visual_scale) > 0.001;
        const bool text_changed_externally = inline_text_editor_->toPlainText() != layer_plain;
        if (scale_changed || text_changed_externally) {
            if (!layer->rich_text.plain_text.empty() || !layer->rich_text.ranges.empty()) {
                populate_qtext_document_from_rich_text(inline_text_editor_->document(), layer->rich_text, visual_scale);
            } else if (!layer->rich_text_html.empty()) {
                inline_text_editor_->setHtml(scale_rich_text_font_sizes(QString::fromStdString(layer->rich_text_html), visual_scale));
            } else {
                populate_qtext_document_from_plain_layer_text(inline_text_editor_->document(), layer->text_content,
                                                              layer_char_format_for_editor(*layer), visual_scale);
            }
            inline_text_last_visual_scale_ = visual_scale;
            QTextCursor restored(inline_text_editor_->document());
            const int text_len = inline_text_editor_->toPlainText().size();
            restored.setPosition(std::clamp(anchor, 0, text_len));
            restored.setPosition(std::clamp(position, 0, text_len), QTextCursor::KeepAnchor);
            inline_text_editor_->setTextCursor(restored);
            if (auto *doc = inline_text_editor_->document())
                doc->setModified(false);
        }
        if (auto *doc = inline_text_editor_->document())
            if (auto *layout = doc->documentLayout())
                layout->documentSize();
    }
    updating_inline_text_editor_ = was_updating_inline_text_editor;

    const QRectF document_rect = inline_text_document_local_rect(*layer);
    QPolygonF poly;
    poly << canvas_to_view(layer_to_canvas(*layer, document_rect.topLeft()))
         << canvas_to_view(layer_to_canvas(*layer, document_rect.topRight()))
         << canvas_to_view(layer_to_canvas(*layer, document_rect.bottomRight()))
         << canvas_to_view(layer_to_canvas(*layer, document_rect.bottomLeft()));
    QRectF bounds = poly.boundingRect();
    const int left = (int)std::floor(bounds.left());
    const int top = (int)std::floor(bounds.top());
    const int right = (int)std::ceil(bounds.right());
    const int bottom = (int)std::ceil(bounds.bottom());
    inline_text_editor_->setGeometry(QRect(left, top, std::max(1, right - left), std::max(1, bottom - top)));
}

void CanvasPreview::begin_text_edit(const std::shared_ptr<Layer> &layer)
{
    if (!layer || !inline_text_editor_) return;
    if (!inline_text_layer_id_.empty() && inline_text_layer_id_ != layer->id)
        commit_text_edit(true);

    inline_text_layer_id_ = layer->id;
    if (layer->rich_text.empty())
        layer->rich_text = rich_text_document_from_layer_defaults(*layer);
    rich_text_document_sync_layer_mirrors(*layer);
    updating_inline_text_editor_ = true;
    QSignalBlocker blocker(inline_text_editor_);
    configure_inline_text_editor(*layer);
    const double visual_scale = inline_text_visual_scale(*layer);
    if (!layer->rich_text.plain_text.empty() || !layer->rich_text.ranges.empty()) {
        populate_qtext_document_from_rich_text(inline_text_editor_->document(), layer->rich_text, visual_scale);
    } else if (!layer->rich_text_html.empty()) {
        inline_text_editor_->setHtml(scale_rich_text_font_sizes(QString::fromStdString(layer->rich_text_html), visual_scale));
    } else {
        populate_qtext_document_from_plain_layer_text(inline_text_editor_->document(), layer->text_content,
                                                      layer_char_format_for_editor(*layer), visual_scale);
    }
    if (inline_text_editor_->toPlainText().isEmpty())
        inline_text_editor_->setCurrentCharFormat(qtext_format_from_rich_text_format(layer_char_format_for_editor(*layer), visual_scale));
    inline_text_last_visual_scale_ = visual_scale;

    QTextCursor cursor = inline_text_editor_->textCursor();
    cursor.select(QTextCursor::Document);
    inline_text_editor_->setTextCursor(cursor);
    if (!layer->rich_text.empty()) {
        layer->rich_text.selection.anchor = 0;
        layer->rich_text.selection.head = layer->rich_text.plain_text.size();
    }
    position_text_editor();
    updating_inline_text_editor_ = false;
    inline_text_editor_->show();
    inline_text_editor_->raise();
    inline_text_editor_->setFocus(Qt::MouseFocusReason);
    if (auto *doc = inline_text_editor_->document())
        doc->setModified(false);
    emit text_edit_cursor_changed(layer->id);
    dirty_ = true;
    update();
}

void CanvasPreview::commit_text_edit(bool accept_changes)
{
    if (committing_inline_text_ || !inline_text_editor_ || inline_text_layer_id_.empty()) return;
    committing_inline_text_ = true;
    const std::string layer_id = inline_text_layer_id_;

    if (accept_changes)
        sync_inline_text_layer(true);

    inline_text_layer_id_.clear();
    inline_text_last_visual_scale_ = 0.0;
    inline_text_editor_->hide();
    {
        updating_inline_text_editor_ = true;
        QSignalBlocker blocker(inline_text_editor_);
        inline_text_editor_->clear();
        inline_text_editor_->setCurrentCharFormat(QTextCharFormat());
        inline_text_editor_->mergeCurrentCharFormat(QTextCharFormat());
        updating_inline_text_editor_ = false;
    }
    committing_inline_text_ = false;
    dirty_ = true;
    update();
    emit text_edit_committed(layer_id);
}

bool CanvasPreview::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == inline_text_editor_) {
        if (event->type() == QEvent::FocusOut) {
            return false;
        }
        if (event->type() == QEvent::KeyPress) {
            auto *key_event = static_cast<QKeyEvent *>(event);
            auto merge_char_format = [this](const QTextCharFormat &format) {
                QTextCursor cursor = inline_text_editor_->textCursor();
                cursor.mergeCharFormat(format);
                inline_text_editor_->mergeCurrentCharFormat(format);
                inline_text_editor_->setTextCursor(cursor);
                refresh_inline_text_edit(true, true);
            };
            if (key_event->key() == Qt::Key_B && key_event->modifiers().testFlag(Qt::ControlModifier)) {
                QTextCharFormat format;
                format.setFontWeight(inline_text_editor_->fontWeight() == QFont::Bold ? QFont::Normal : QFont::Bold);
                merge_char_format(format);
                key_event->accept();
                return true;
            }
            if (key_event->key() == Qt::Key_I && key_event->modifiers().testFlag(Qt::ControlModifier)) {
                QTextCharFormat format;
                format.setFontItalic(!inline_text_editor_->fontItalic());
                merge_char_format(format);
                key_event->accept();
                return true;
            }
            if (key_event->key() == Qt::Key_U && key_event->modifiers().testFlag(Qt::ControlModifier)) {
                QTextCharFormat format;
                format.setFontUnderline(!inline_text_editor_->fontUnderline());
                merge_char_format(format);
                key_event->accept();
                return true;
            }
            if (key_event->key() == Qt::Key_Escape) {
                commit_text_edit(true);
                key_event->accept();
                return true;
            }
            if (key_event->key() == Qt::Key_Return && key_event->modifiers().testFlag(Qt::ControlModifier)) {
                commit_text_edit(true);
                key_event->accept();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void CanvasPreview::mouseDoubleClickEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::LeftButton) {
        auto layer = text_layer_at_view_pos(ev->pos());
        if (layer) {
            emit layer_clicked(layer->id);
            begin_text_edit(layer);
            ev->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(ev);
}

void CanvasPreview::mousePressEvent(QMouseEvent *ev)
{
    if (!title_) return;

    if (!inline_text_layer_id_.empty()) {
        commit_text_edit(true);
        ev->accept();
        return;
    }

    setFocus(Qt::MouseFocusReason);

    if (ev->button() == Qt::LeftButton) {
        bool vertical_guide = true;
        if (ruler_hit_test(ev->pos(), vertical_guide)) {
            dragging_new_guide_ = true;
            dragging_guide_x_axis_ = vertical_guide;
            dragging_guide_index_ = -1;
            const QPointF canvas_pt = view_to_canvas(ev->pos());
            dragging_guide_value_ = vertical_guide ? canvas_pt.x() : canvas_pt.y();
            drag_mode_ = vertical_guide ? DragMode::GuideX : DragMode::GuideY;
            setCursor(vertical_guide ? Qt::SplitHCursor : Qt::SplitVCursor);
            update();
            ev->accept();
            return;
        }
        bool x_axis = true;
        const int guide_index = guide_hit_test(ev->pos(), x_axis);
        if (guide_index >= 0) {
            dragging_new_guide_ = false;
            dragging_guide_x_axis_ = x_axis;
            dragging_guide_index_ = guide_index;
            dragging_guide_value_ = x_axis ? vertical_guides_[(size_t)guide_index] : horizontal_guides_[(size_t)guide_index];
            drag_mode_ = x_axis ? DragMode::GuideX : DragMode::GuideY;
            setCursor(x_axis ? Qt::SplitHCursor : Qt::SplitVCursor);
            update();
            ev->accept();
            return;
        }
    }

    if (ev->button() == Qt::MiddleButton) {
        panning_ = true;
        pan_start_view_ = QPointF(ev->pos());
        pan_start_offset_ = pan_offset_;
        setCursor(Qt::ClosedHandCursor);
        ev->accept();
        return;
    }

    if (ev->button() != Qt::LeftButton) return;

    if (active_tool_ == CanvasTool::ColorPicker) {
        update_color_picker_tooltip(ev->pos());
        if (color_picker_tooltip_visible_)
            emit color_picked(color_picker_tooltip_color_);
        ev->accept();
        return;
    }

    if (active_tool_ == CanvasTool::Text) {
        if (auto layer = text_layer_at_view_pos(ev->pos())) {
            emit layer_clicked(layer->id);
            begin_text_edit(layer);
            ev->accept();
            return;
        }
    }

    if (active_tool_ == CanvasTool::Shape || active_tool_ == CanvasTool::Text) {
        drawing_shape_ = true;
        drawing_shape_changed_ = false;
        drag_mode_ = DragMode::None;
        selected_layer_ids_.clear();
        sel_layer_id_.clear();
        shape_draw_start_canvas_ = snap_canvas_point(view_to_canvas(ev->pos()), true, true,
                                                     !ev->modifiers().testFlag(Qt::ControlModifier));
        shape_draw_current_canvas_ = shape_draw_start_canvas_;
        shape_draw_modifiers_ = ev->modifiers();
        shape_draw_current_rect_ = toolbar_draw_rect(shape_draw_current_canvas_, shape_draw_modifiers_);
        last_toolbar_preview_update_rect_ = toolbar_preview_update_rect();
        update(last_toolbar_preview_update_rect_.isEmpty() ? rect() : last_toolbar_preview_update_rect_);
        ev->accept();
        return;
    }

    drag_mode_ = hit_test_selected(ev->pos());
    if (drag_mode_ == DragMode::None) {
        QPointF canvas = view_to_canvas(ev->pos());
        for (auto it = title_->layers.rbegin(); it != title_->layers.rend(); ++it) {
            auto &l = *it;
            if (!l || !l->visible || l->locked) continue;
            if (playhead_ < l->in_time || playhead_ > l->out_time) continue;
            QPointF local = canvas_to_layer(*l, canvas);
            if (layer_local_rect(*l).contains(local)) {
                std::vector<std::string> next_ids;
                if (ev->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))
                    next_ids = selected_layer_ids_;
                auto existing = std::find(next_ids.begin(), next_ids.end(), l->id);
                if (ev->modifiers() & Qt::ControlModifier) {
                    if (existing == next_ids.end()) next_ids.push_back(l->id);
                    else next_ids.erase(existing);
                } else if (existing == next_ids.end()) {
                    if (!(ev->modifiers() & Qt::ShiftModifier)) next_ids.clear();
                    next_ids.push_back(l->id);
                }
                emit layers_selected(next_ids);
                selected_layer_ids_ = next_ids;
                sel_layer_id_ = selected_layer_ids_.empty() ? std::string() : selected_layer_ids_.back();
                drag_mode_ = DragMode::Move;
                break;
            }
        }
    }

    if (drag_mode_ == DragMode::None) {
        begin_marquee(ev->pos(), ev->modifiers());
        ev->accept();
        return;
    }

    drag_changed_ = false;
    alt_duplicate_pending_ = (drag_mode_ == DragMode::Move) && ev->modifiers().testFlag(Qt::AltModifier);
    alt_duplicate_done_ = false;
    drag_start_view_ = ev->pos();
    drag_current_view_ = ev->pos();
    drag_start_canvas_ = view_to_canvas(ev->pos());
    drag_layer_states_.clear();
    gradient_drag_ = GradientDragState{};
    corner_radius_drag_ = CornerRadiusDragState{};
    drag_start_selection_bounds_ = selected_canvas_bounds();

    auto layers = selected_layers();
    drag_text_object_scaling_ = false;
    auto layer = selected_layer();
    if (!layer && !layers.empty()) layer = layers.front();
    if (!layer) return;

    auto is_resize_drag = [](DragMode mode) {
        return mode == DragMode::ResizeNW || mode == DragMode::ResizeN || mode == DragMode::ResizeNE ||
               mode == DragMode::ResizeE || mode == DragMode::ResizeSE || mode == DragMode::ResizeS ||
               mode == DragMode::ResizeSW || mode == DragMode::ResizeW;
    };
    drag_text_object_scaling_ = is_resize_drag(drag_mode_) && ev->modifiers().testFlag(Qt::AltModifier) &&
        std::any_of(layers.begin(), layers.end(), [](const std::shared_ptr<Layer> &selected) {
            return selected && is_canvas_text_layer(*selected);
        });

    for (const auto &selected : layers) {
        if (!selected || selected->locked) continue;
        double lt = std::clamp(playhead_ - selected->in_time, 0.0,
                               std::max(0.0, selected->out_time - selected->in_time));
        drag_layer_states_.push_back({selected->id,
                                      selected->pos_x.evaluate(lt),
                                      selected->pos_y.evaluate(lt),
                                      (float)eval_box_width(*selected, lt),
                                      (float)eval_box_height(*selected, lt),
                                      selected->scale_x.evaluate(lt),
                                      selected->scale_y.evaluate(lt),
                                      selected->rotation.evaluate(lt)});
    }

    double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                           std::max(0.0, layer->out_time - layer->in_time));
    drag_start_x_ = layer->pos_x.evaluate(lt);
    drag_start_y_ = layer->pos_y.evaluate(lt);
    drag_start_w_ = (float)eval_box_width(*layer, lt);
    drag_start_h_ = (float)eval_box_height(*layer, lt);
    drag_start_origin_x_ = layer->origin_x;
    drag_start_origin_y_ = layer->origin_y;
    begin_gradient_drag(*layer);
    begin_corner_radius_drag(*layer);
    drag_rotation_pivot_canvas_ = layers.size() > 1
        ? drag_start_selection_bounds_.center()
        : layer_to_canvas(*layer, QPointF(0, 0));
    QPointF pivot_view = canvas_to_view(drag_rotation_pivot_canvas_);
    drag_start_rotation_angle_ = radians_to_degrees(std::atan2(drag_start_view_.y() - pivot_view.y(),
                                                               drag_start_view_.x() - pivot_view.x()));
    drag_current_rotation_delta_ = 0.0;
    auto set_cursor_for_mode = [this](DragMode mode) {
        if (mode == DragMode::Move) setCursor(Qt::ClosedHandCursor);
        else if (mode == DragMode::Origin) setCursor(Qt::CrossCursor);
        else if (mode == DragMode::Rotate) setCursor(canvas_rotation_cursor());
        else if (mode == DragMode::GradientCenter || mode == DragMode::GradientFocal) setCursor(Qt::CrossCursor);
        else if (mode == DragMode::GradientStart || mode == DragMode::GradientEnd ||
                 mode == DragMode::GradientRadius) setCursor(Qt::SizeAllCursor);
        else if (mode == DragMode::CornerRadiusTL || mode == DragMode::CornerRadiusTR ||
                 mode == DragMode::CornerRadiusBR || mode == DragMode::CornerRadiusBL) setCursor(Qt::SizeAllCursor);
        else if (mode == DragMode::ResizeN || mode == DragMode::ResizeS) setCursor(Qt::SizeVerCursor);
        else if (mode == DragMode::ResizeE || mode == DragMode::ResizeW) setCursor(Qt::SizeHorCursor);
        else if (mode == DragMode::ResizeNE || mode == DragMode::ResizeSW) setCursor(Qt::SizeBDiagCursor);
        else setCursor(Qt::SizeFDiagCursor);
    };
    set_cursor_for_mode(drag_mode_);
    ev->accept();
}

void CanvasPreview::mouseMoveEvent(QMouseEvent *ev)
{
    if (panning_ && (ev->buttons() & Qt::MiddleButton)) {
        pan_offset_ = pan_start_offset_ + (QPointF(ev->pos()) - pan_start_view_);
        fit_zoom_active_ = false;
        position_text_editor();
        update();
        ev->accept();
        return;
    }

    if (drawing_shape_ && (ev->buttons() & Qt::LeftButton)) {
        update_shape_drawing(ev->pos(), ev->modifiers());
        ev->accept();
        return;
    }

    if ((drag_mode_ == DragMode::GuideX || drag_mode_ == DragMode::GuideY) && (ev->buttons() & Qt::LeftButton)) {
        const QPointF canvas_pt = view_to_canvas(ev->pos());
        dragging_guide_value_ = drag_mode_ == DragMode::GuideX ? canvas_pt.x() : canvas_pt.y();
        if (title_) {
            if (drag_mode_ == DragMode::GuideX)
                dragging_guide_value_ = std::clamp(dragging_guide_value_, -10000.0, title_->width + 10000.0);
            else
                dragging_guide_value_ = std::clamp(dragging_guide_value_, -10000.0, title_->height + 10000.0);
        }
        dragging_guide_value_ = snap_guide_value_to_objects(drag_mode_ == DragMode::GuideX, dragging_guide_value_);
        update();
        ev->accept();
        return;
    }

    if (drag_mode_ != DragMode::None && (ev->buttons() & Qt::LeftButton)) {
        apply_drag(ev->pos(), ev->modifiers());
        ev->accept();
        return;
    }

    if (active_tool_ == CanvasTool::Shape) {
        setCursor(Qt::CrossCursor);
        return;
    }
    if (active_tool_ == CanvasTool::Text) {
        setCursor(Qt::IBeamCursor);
        return;
    }
    if (active_tool_ == CanvasTool::ColorPicker) {
        update_color_picker_tooltip(ev->pos());
        setCursor(Qt::CrossCursor);
        return;
    }

    bool hover_x_axis = true;
    if (guide_hit_test(ev->pos(), hover_x_axis) >= 0) {
        setCursor(hover_x_axis ? Qt::SplitHCursor : Qt::SplitVCursor);
        return;
    }
    bool hover_vertical_guide = true;
    if (ruler_hit_test(ev->pos(), hover_vertical_guide)) {
        setCursor(hover_vertical_guide ? Qt::SplitHCursor : Qt::SplitVCursor);
        return;
    }

    DragMode mode = hit_test_selected(ev->pos());
    if (mode == DragMode::Move) setCursor(Qt::OpenHandCursor);
    else if (mode == DragMode::Origin) setCursor(Qt::CrossCursor);
    else if (mode == DragMode::Rotate) setCursor(canvas_rotation_cursor());
    else if (mode == DragMode::GradientCenter || mode == DragMode::GradientFocal) setCursor(Qt::CrossCursor);
    else if (mode == DragMode::GradientStart || mode == DragMode::GradientEnd ||
             mode == DragMode::GradientRadius) setCursor(Qt::SizeAllCursor);
    else if (mode == DragMode::CornerRadiusTL || mode == DragMode::CornerRadiusTR ||
             mode == DragMode::CornerRadiusBR || mode == DragMode::CornerRadiusBL) setCursor(Qt::SizeAllCursor);
    else if (mode == DragMode::ResizeN || mode == DragMode::ResizeS) setCursor(Qt::SizeVerCursor);
    else if (mode == DragMode::ResizeE || mode == DragMode::ResizeW) setCursor(Qt::SizeHorCursor);
    else if (mode == DragMode::ResizeNE || mode == DragMode::ResizeSW) setCursor(Qt::SizeBDiagCursor);
    else if (mode != DragMode::None) setCursor(Qt::SizeFDiagCursor);
    else unsetCursor();
}

void CanvasPreview::leaveEvent(QEvent *ev)
{
    if (color_picker_tooltip_visible_) {
        color_picker_tooltip_visible_ = false;
        update();
    }
    QWidget::leaveEvent(ev);
}

void CanvasPreview::keyPressEvent(QKeyEvent *ev)
{
    if (!inline_text_layer_id_.empty() && ev->key() == Qt::Key_Escape) {
        commit_text_edit(true);
        ev->accept();
        return;
    }

    double dx = 0.0;
    double dy = 0.0;
    const double amount = ev->modifiers().testFlag(Qt::ShiftModifier) ? 10.0 : 1.0;

    switch (ev->key()) {
    case Qt::Key_Left:
        dx = -amount;
        break;
    case Qt::Key_Right:
        dx = amount;
        break;
    case Qt::Key_Up:
        dy = -amount;
        break;
    case Qt::Key_Down:
        dy = amount;
        break;
    default:
        QWidget::keyPressEvent(ev);
        return;
    }

    if (nudge_selected_layers(dx, dy))
        ev->accept();
    else
        QWidget::keyPressEvent(ev);
}

void CanvasPreview::mouseReleaseEvent(QMouseEvent *ev)
{
    if (ev->button() == Qt::MiddleButton && panning_) {
        panning_ = false;
        unsetCursor();
        ev->accept();
        return;
    }

    if (ev->button() == Qt::LeftButton && drawing_shape_) {
        const QRect old_update_rect = last_toolbar_preview_update_rect_;
        const QPointF release_canvas = view_to_canvas(ev->pos());
        const bool dragged_far_enough = QLineF(shape_draw_start_canvas_, release_canvas).length() >= 2.0;
        if (drawing_shape_changed_ || dragged_far_enough)
            update_shape_drawing(ev->pos(), ev->modifiers());

        const QRect repaint_rect = old_update_rect.united(last_toolbar_preview_update_rect_);
        const QRectF final_rect = shape_draw_current_rect_.isValid()
                                    ? shape_draw_current_rect_.normalized()
                                    : toolbar_draw_rect(shape_draw_current_canvas_, shape_draw_modifiers_);
        const bool has_drawn_size = drawing_shape_changed_ || dragged_far_enough;
        const bool was_text_tool = active_tool_ == CanvasTool::Text;
        const ShapeType final_shape_type = active_shape_type_;
        const LayerType final_text_type = active_text_layer_type_;
        const QPointF start_canvas = shape_draw_start_canvas_;

        drawing_shape_ = false;
        drawing_shape_changed_ = false;
        shape_draw_current_rect_ = QRectF();
        last_toolbar_preview_update_rect_ = QRect();
        clear_snap_feedback();
        update(repaint_rect.isEmpty() ? rect() : repaint_rect);

        if (was_text_tool)
            emit text_drawing_started(final_text_type, start_canvas);
        else
            emit shape_drawing_started(final_shape_type, start_canvas);
        if (has_drawn_size)
            emit shape_drawing_changed(final_rect);
        emit shape_drawing_finished(true);

        setCursor(active_tool_ == CanvasTool::Shape ? Qt::CrossCursor :
                  (active_tool_ == CanvasTool::Text ? Qt::IBeamCursor :
                   (active_tool_ == CanvasTool::ColorPicker ? Qt::CrossCursor : Qt::ArrowCursor)));
        ev->accept();
        return;
    }

    if (ev->button() != Qt::LeftButton || drag_mode_ == DragMode::None) return;

    if (drag_mode_ == DragMode::GuideX || drag_mode_ == DragMode::GuideY) {
        const bool x_axis = drag_mode_ == DragMode::GuideX;
        const QRectF canvas_rect = canvas_view_rect();
        const bool inside_canvas_band = x_axis
            ? (ev->pos().x() >= canvas_rect.left() - 12.0 && ev->pos().x() <= canvas_rect.right() + 12.0)
            : (ev->pos().y() >= canvas_rect.top() - 12.0 && ev->pos().y() <= canvas_rect.bottom() + 12.0);
        auto &guides = x_axis ? vertical_guides_ : horizontal_guides_;
        if (inside_canvas_band) {
            if (dragging_guide_index_ >= 0 && (size_t)dragging_guide_index_ < guides.size())
                guides[(size_t)dragging_guide_index_] = dragging_guide_value_;
            else
                guides.push_back(dragging_guide_value_);
            std::sort(guides.begin(), guides.end());
        } else if (dragging_guide_index_ >= 0 && (size_t)dragging_guide_index_ < guides.size()) {
            guides.erase(guides.begin() + dragging_guide_index_);
        }
        dragging_new_guide_ = false;
        dragging_guide_index_ = -1;
        drag_mode_ = DragMode::None;
        clear_snap_feedback();
        save_ruler_guide_settings();
        unsetCursor();
        update();
        ev->accept();
        return;
    }

    if (drag_mode_ == DragMode::Marquee) {
        update_marquee(ev->pos(), ev->modifiers());
        if (!marquee_active_)
            emit layers_selected(std::vector<std::string>{});
        drag_mode_ = DragMode::None;
        marquee_active_ = false;
        drag_changed_ = false;
        alt_duplicate_pending_ = false;
        alt_duplicate_done_ = false;
        drag_text_object_scaling_ = false;
        gradient_drag_ = GradientDragState{};
        corner_radius_drag_ = CornerRadiusDragState{};
        clear_snap_feedback();
        unsetCursor();
        update();
        ev->accept();
        return;
    }

    bool changed = drag_changed_;
    drag_mode_ = DragMode::None;
    drag_changed_ = false;
    alt_duplicate_pending_ = false;
    alt_duplicate_done_ = false;
    drag_text_object_scaling_ = false;
    gradient_drag_ = GradientDragState{};
    corner_radius_drag_ = CornerRadiusDragState{};
    drag_layer_states_.clear();
    clear_snap_feedback();
    unsetCursor();
    if (changed)
        emit layer_geometry_changed();
    ev->accept();
}



void CanvasPreview::contextMenuEvent(QContextMenuEvent *ev)
{
    bool x_axis = true;
    const int guide_index = guide_hit_test(ev->pos(), x_axis, true);
    if (guide_index < 0) {
        QWidget::contextMenuEvent(ev);
        return;
    }

    QMenu menu(this);
    QAction *delete_guide = menu.addAction(QStringLiteral("Delete guide"));
    delete_guide->setEnabled(!guides_locked_);
    QAction *chosen = menu.exec(ev->globalPos());
    if (chosen == delete_guide && !guides_locked_) {
        auto &guides = x_axis ? vertical_guides_ : horizontal_guides_;
        if ((size_t)guide_index < guides.size()) {
            guides.erase(guides.begin() + guide_index);
            clear_snap_feedback();
            save_ruler_guide_settings();
            update();
        }
    }
    ev->accept();
}

void CanvasPreview::wheelEvent(QWheelEvent *ev)
{
    if (!title_) return;
    QPointF anchor_canvas = view_to_canvas(ev->position());
    int next = zoom_percent_;
    if (ev->angleDelta().y() > 0)
        next = (int)std::round(next * 1.1);
    else
        next = (int)std::round(next / 1.1);
    zoom_percent_ = std::clamp(next, 5, 1600);
    fit_zoom_active_ = false;
    QPointF origin_without_pan = centered_view_origin();
    double scale = view_scale();
    pan_offset_ = ev->position() - origin_without_pan - QPointF(anchor_canvas.x() * scale, anchor_canvas.y() * scale);
    emit zoom_percent_changed(zoom_percent_);
    position_text_editor();
    update();
    ev->accept();
}

void CanvasPreview::resizeEvent(QResizeEvent *)
{
    dirty_ = true;
    if (fit_zoom_active_) fit_canvas(fit_zoom_up_to_100_);
    position_text_editor();
}

/* ══════════════════════════════════════════════════════════════════
 *  LayerStack
 * ══════════════════════════════════════════════════════════════════ */

