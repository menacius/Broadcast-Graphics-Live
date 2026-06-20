#include "title-editor-internal.h"
#include "title-localization.h"
#include "cache-manager.h"
#include "title-source.h"

#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>

namespace {
constexpr int kCanvasRulerThickness = 24;
constexpr double kCanvasGuideHitTolerancePx = 5.0;
constexpr const char *kEditorRulersVisibleKey = "rulersVisible";
constexpr const char *kEditorGuidesVisibleKey = "guidesVisible";
constexpr const char *kEditorGuidesLockedKey = "guidesLocked";
constexpr const char *kEditorGuideCoordinatesVisibleKey = "guideCoordinatesVisible";
constexpr const char *kEditorCanvasBorderVisibleKey = "canvasBorderVisible";
constexpr const char *kEditorVerticalGuidesKey = "verticalGuides";
constexpr const char *kEditorHorizontalGuidesKey = "horizontalGuides";
constexpr const char *kEditorAdaptiveRenderingKey = "adaptiveRendering";
constexpr const char *kEditorAdaptiveQualityKey = "adaptiveRenderingQuality";

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

QColor editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole role)
{
    return TitlePreferences::canvas_helper_color(role);
}

TitlePreferences::CanvasHelperColorRole snap_feedback_role_from_label(const QString &label)
{
    if (label == obsgs_tr("OBSTitles.ObjectEdge") ||
        label == obsgs_tr("OBSTitles.ObjectCenter") ||
        label == obsgs_tr("OBSTitles.Spacing")) {
        return TitlePreferences::CanvasHelperColorRole::ObjectSnapLines;
    }
    return TitlePreferences::CanvasHelperColorRole::CanvasSnapLines;
}

bool layer_is_shape_sized(const Layer &layer)
{
    return layer.type == LayerType::Shape || layer.type == LayerType::SolidRect;
}

double shape_resize_metric_factor(double old_w, double old_h, double new_w, double new_h)
{
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
}

void apply_shape_resize_metrics_from_state(Layer &layer, double old_w, double old_h,
                                           float start_stroke_width,
                                           float start_corner_radius_tl,
                                           float start_corner_radius_tr,
                                           float start_corner_radius_br,
                                           float start_corner_radius_bl,
                                           double new_w, double new_h)
{
    if (!layer_is_shape_sized(layer))
        return;
    const double factor = shape_resize_metric_factor(old_w, old_h, new_w, new_h);
    if (!std::isfinite(factor) || factor <= 0.0)
        return;
    if (layer.scale_stroke_with_shape &&
        layer.outline_enabled &&
        layer.stroke_fill_type != 0 &&
        start_stroke_width > 0.0f) {
        layer.stroke_width = (float)std::clamp((double)start_stroke_width * factor, 0.0, 512.0);
    }
    if (layer.scale_corners_with_shape) {
        set_layer_corner_radii(layer,
                               (float)std::clamp((double)start_corner_radius_tl * factor, 0.0, 9999.0),
                               (float)std::clamp((double)start_corner_radius_tr * factor, 0.0, 9999.0),
                               (float)std::clamp((double)start_corner_radius_br * factor, 0.0, 9999.0),
                               (float)std::clamp((double)start_corner_radius_bl * factor, 0.0, 9999.0));
    }
}

std::string unique_canvas_layer_name(const Title &title, const std::string &base_name,
                                     const std::set<std::string> &exclude_ids,
                                     std::set<std::string> *reserved_names)
{
    std::string base = QString::fromStdString(base_name).trimmed().toStdString();
    if (base.empty())
        base = editor_text_std("OBSTitles.Layer");

    std::set<std::string> used;
    for (const auto &layer : title.layers) {
        if (!layer || exclude_ids.find(layer->id) != exclude_ids.end()) continue;
        used.insert(layer->name);
    }
    if (reserved_names)
        used.insert(reserved_names->begin(), reserved_names->end());

    if (used.find(base) == used.end()) {
        if (reserved_names) reserved_names->insert(base);
        return base;
    }

    for (int suffix = 2; suffix < 10000; ++suffix) {
        const std::string candidate = QStringLiteral("%1 %2")
            .arg(QString::fromStdString(base))
            .arg(suffix, 2, 10, QChar('0'))
            .toStdString();
        if (used.find(candidate) != used.end()) continue;
        if (reserved_names) reserved_names->insert(candidate);
        return candidate;
    }

    const std::string fallback = base + " " + TitleDataStore::make_uuid().substr(0, 8);
    if (reserved_names) reserved_names->insert(fallback);
    return fallback;
}
}

CanvasPreview::CanvasPreview(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(400, 225);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAcceptDrops(true);
    load_ruler_guide_settings();

    // Coalesce expensive full-title renders. Mouse/tablet events may arrive far
    // faster than the renderer can finish when a title contains many layers.
    // Keeping at most one scheduled render prevents an ever-growing GUI event
    // backlog and lets OBS keep servicing its own video thread.
    render_coalesce_timer_ = new QTimer(this);
    render_coalesce_timer_->setSingleShot(true);
    render_coalesce_timer_->setTimerType(Qt::PreciseTimer);
    connect(render_coalesce_timer_, &QTimer::timeout, this, [this]() {
        if (dirty_ && isVisible())
            update();
    });
    last_render_clock_.start();

    QSettings adaptive_settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    adaptive_settings.beginGroup(QStringLiteral("titleEditor"));
    adaptive_rendering_enabled_ = adaptive_settings.value(
        QString::fromUtf8(kEditorAdaptiveRenderingKey), true).toBool();
    const int saved_quality = adaptive_settings.value(
        QString::fromUtf8(kEditorAdaptiveQualityKey), 0).toInt();
    adaptive_quality_mode_ = static_cast<AdaptiveQualityMode>(std::clamp(saved_quality, 0, 5));
    adaptive_settings.endGroup();

    adaptive_full_quality_timer_ = new QTimer(this);
    adaptive_full_quality_timer_->setSingleShot(true);
    adaptive_full_quality_timer_->setInterval(140);
    connect(adaptive_full_quality_timer_, &QTimer::timeout, this, [this]() {
        if (!adaptive_interaction_active_)
            return;
        adaptive_interaction_active_ = false;
        // Auto refines to full quality after interaction. Fixed modes keep their
        // selected raster scale, but still perform one clean post-interaction
        // render so the editor-local cache is populated from the settled model.
        if (frame_pixmap_preview_scale_ < 0.999) {
            force_live_full_quality_render_ =
                adaptive_quality_mode_ == AdaptiveQualityMode::Auto;
            dirty_ = true;
            update();
        }
    });

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
    invalidate_canvas_overlay_caches();
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
    qfmt.setProperty(RichTextPropAutoGenerated, false);
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
        const QString editor_text = inline_text_editor_->toPlainText();
        layer->rich_text.selection = {rich_byte_offset_from_qtext_position(editor_text, std::max(0, cursor.anchor())),
                                     rich_byte_offset_from_qtext_position(editor_text, std::max(0, cursor.position()))};
        if (!cursor.hasSelection()) {
            layer->rich_text.typing_format = rich_text_format_from_qtext_format(cursor.charFormat(),
                                                                               layer->rich_text.default_format,
                                                                               visual_scale);
            layer->rich_text.has_typing_format = true;
        }
    }
    refresh_inline_text_edit(true, true);
}


QPointF CanvasPreview::view_center_canvas_point() const
{
    return view_to_canvas(QPointF(width() * 0.5, height() * 0.5));
}

void CanvasPreview::set_title(std::shared_ptr<Title> t, bool preserve_view)
{
    commit_text_edit(true);
    title_ = t;
    editor_quality_cache_.clear();
    invalidate_canvas_overlay_caches();
    dirty_ = true;
    adaptive_interaction_active_ = false;
    force_live_full_quality_render_ = false;
    frame_pixmap_preview_scale_ = 1.0;
    last_full_quality_render_cost_ms_ = 0;
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
    invalidate_canvas_overlay_caches();
    dirty_ = true;
    emit zoom_percent_changed(zoom_percent_);
    position_text_editor();
    update();
}

void CanvasPreview::set_playhead(double t)
{
    if (std::abs(playhead_ - t) < 1e-9)
        return;
    const double old_playhead = playhead_;
    playhead_ = t;
    if (canvas_overlay_changes_with_playhead(old_playhead, playhead_))
        invalidate_canvas_overlay_caches();
    dirty_ = true;
    position_text_editor();
    update();
}

void CanvasPreview::set_selected_layer(const std::string &lid)
{
    sel_layer_id_ = lid;
    selected_layer_ids_.clear();
    if (!lid.empty()) selected_layer_ids_.push_back(lid);
    invalidate_canvas_overlay_caches();
    position_text_editor();
    update();
}

void CanvasPreview::set_selected_layers(const std::vector<std::string> &ids)
{
    selected_layer_ids_ = ids;
    sel_layer_id_ = ids.empty() ? std::string() : ids.back();
    invalidate_canvas_overlay_caches();
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
    canvas_border_visible_ = settings.value(QString::fromUtf8(kEditorCanvasBorderVisibleKey), true).toBool();
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
    settings.setValue(QString::fromUtf8(kEditorCanvasBorderVisibleKey), canvas_border_visible_);
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

void CanvasPreview::set_canvas_border_visible(bool visible)
{
    if (canvas_border_visible_ == visible) return;
    canvas_border_visible_ = visible;
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
    // Model edits invalidate only the editor-local reduced-quality cache. The
    // full-quality OBS/prerender cache retains its own independent lifecycle.
    editor_quality_cache_.clear();
    invalidate_canvas_overlay_caches();
    dirty_ = true;
    position_text_editor();
    if (!inline_text_layer_id_.empty())
        render_to_pixmap();
    update();
    if (inline_text_editor_ && inline_text_editor_->isVisible()) {
        inline_text_editor_->viewport()->update();
        // Queue the affected region instead of forcing an immediate synchronous
        // paint, which can recursively stall mouse interaction and the OBS UI.
        update(inline_text_editor_->geometry().adjusted(-4, -4, 4, 4));
    }
}

void CanvasPreview::clear_rendered_frame()
{
    frame_pixmap_ = QPixmap();
    frame_pixmap_canvas_offset_ = QPoint();
    frame_pixmap_canvas_size_ = title_ ? QSize(title_->width, title_->height) : QSize();
    frame_pixmap_preview_scale_ = 1.0;
    invalidate_canvas_overlay_caches();
    dirty_ = false;
    update();
}

QImage CanvasPreview::current_rendered_frame() const
{
    if (frame_pixmap_.isNull())
        return QImage();

    const QSize canvas_size = frame_pixmap_canvas_size_.isValid()
        ? frame_pixmap_canvas_size_
        : frame_pixmap_.size();
    QImage image(canvas_size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    const QSize logical_payload_size(
        std::max(1, (int)std::round(frame_pixmap_.width() / std::max(0.125, frame_pixmap_preview_scale_))),
        std::max(1, (int)std::round(frame_pixmap_.height() / std::max(0.125, frame_pixmap_preview_scale_))));
    painter.drawPixmap(QRect(frame_pixmap_canvas_offset_, logical_payload_size), frame_pixmap_);
    return image;
}


void CanvasPreview::set_adaptive_rendering_enabled(bool enabled)
{
    if (adaptive_rendering_enabled_ == enabled)
        return;
    adaptive_rendering_enabled_ = enabled;
    adaptive_interaction_active_ = false;
    editor_quality_cache_.clear();
    if (adaptive_full_quality_timer_)
        adaptive_full_quality_timer_->stop();
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QStringLiteral("titleEditor"));
    settings.setValue(QString::fromUtf8(kEditorAdaptiveRenderingKey), enabled);
    settings.endGroup();
    dirty_ = true;
    update();
}

void CanvasPreview::set_adaptive_quality_mode(AdaptiveQualityMode mode)
{
    if (adaptive_quality_mode_ == mode)
        return;
    adaptive_quality_mode_ = mode;
    adaptive_interaction_active_ = false;
    force_live_full_quality_render_ = false;
    if (adaptive_full_quality_timer_)
        adaptive_full_quality_timer_->stop();
    QSettings settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    settings.beginGroup(QStringLiteral("titleEditor"));
    settings.setValue(QString::fromUtf8(kEditorAdaptiveQualityKey), static_cast<int>(mode));
    settings.endGroup();
    // Changing quality invalidates the editor-local reduced-quality frames. It
    // does not invalidate or overwrite the full-quality OBS/prerender cache.
    editor_quality_cache_.clear();
    clear_rendered_frame();
    dirty_ = true;
    update();
}

QString CanvasPreview::adaptive_quality_label() const
{
    switch (adaptive_quality_mode_) {
    case AdaptiveQualityMode::Full: return QStringLiteral("Full");
    case AdaptiveQualityMode::Percent75: return QStringLiteral("75%");
    case AdaptiveQualityMode::Percent50: return QStringLiteral("50%");
    case AdaptiveQualityMode::Percent37_5: return QStringLiteral("37,5%");
    case AdaptiveQualityMode::Percent25: return QStringLiteral("25%");
    case AdaptiveQualityMode::Auto:
    default: return QStringLiteral("Auto");
    }
}

void CanvasPreview::begin_adaptive_interaction()
{
    if (!adaptive_rendering_enabled_)
        return;
    adaptive_interaction_active_ = true;
    if (adaptive_full_quality_timer_)
        adaptive_full_quality_timer_->start();
}

double CanvasPreview::adaptive_preview_scale() const
{
    if (!adaptive_rendering_enabled_)
        return 1.0;
    switch (adaptive_quality_mode_) {
    case AdaptiveQualityMode::Full: return 1.0;
    case AdaptiveQualityMode::Percent75: return 0.75;
    case AdaptiveQualityMode::Percent50: return 0.5;
    case AdaptiveQualityMode::Percent37_5: return 0.375;
    case AdaptiveQualityMode::Percent25: return 0.25;
    case AdaptiveQualityMode::Auto:
    default: break;
    }
    if (!adaptive_interaction_active_)
        return 1.0;
    // Stay at full quality for titles already capable of interactive rendering.
    if (last_full_quality_render_cost_ms_ <= 18) return 1.0;
    if (last_full_quality_render_cost_ms_ <= 28) return 0.75;
    if (last_full_quality_render_cost_ms_ <= 45) return 0.5;
    if (last_full_quality_render_cost_ms_ <= 75) return 0.375;
    return 0.25;
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
    invalidate_canvas_overlay_caches();
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
    invalidate_checkerboard_cache();
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
    invalidate_canvas_overlay_caches();
    unsetCursor();
    update();
}

void CanvasPreview::set_shape_tool_active(ShapeType shape_type)
{
    active_tool_ = CanvasTool::Shape;
    active_shape_type_ = shape_type;
    drawing_shape_ = false;
    color_picker_tooltip_visible_ = false;
    invalidate_canvas_overlay_caches();
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
    invalidate_canvas_overlay_caches();
    setCursor(Qt::IBeamCursor);
    update();
}

void CanvasPreview::set_color_picker_tool_active()
{
    commit_text_edit(true);
    active_tool_ = CanvasTool::ColorPicker;
    drawing_shape_ = false;
    color_picker_tooltip_visible_ = false;
    invalidate_canvas_overlay_caches();
    setCursor(Qt::CrossCursor);
    update();
}

void CanvasPreview::set_gradient_tool_active()
{
    commit_text_edit(true);
    active_tool_ = CanvasTool::Gradient;
    drawing_shape_ = false;
    color_picker_tooltip_visible_ = false;
    gradient_tool_dragging_ = false;
    invalidate_canvas_overlay_caches();
    setCursor(Qt::CrossCursor);
    update();
}

void CanvasPreview::set_gradient_editor_active(bool active)
{
    if (gradient_editor_active_ == active) return;
    gradient_editor_active_ = active;
    invalidate_canvas_overlay_caches();
    if (!active && active_tool_ != CanvasTool::Gradient) {
        gradient_drag_ = GradientDragState{};
        if (drag_mode_ == DragMode::GradientStart || drag_mode_ == DragMode::GradientEnd ||
            drag_mode_ == DragMode::GradientCenter || drag_mode_ == DragMode::GradientRadius ||
            drag_mode_ == DragMode::GradientFocal)
            drag_mode_ = DragMode::None;
    }
    update();
}

void CanvasPreview::fit_canvas(bool up_to_100)
{
    fit_zoom_active_ = true;
    fit_zoom_up_to_100_ = up_to_100;
    pan_offset_ = QPointF(0, 0);
    invalidate_canvas_overlay_caches();
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
    xf.translate(layer.position.evaluate(lt).x, layer.position.evaluate(lt).y);
    xf.rotate(layer.rotation.evaluate(lt));
    xf.scale(layer.scale.evaluate(lt).x, layer.scale.evaluate(lt).y);
    return xf;
}

static bool editor_layer_has_ancestor(const std::shared_ptr<Title> &title,
                                      const Layer &layer,
                                      const std::string &ancestor_id)
{
    std::string parent_id = layer.parent_id;
    int guard = 0;
    while (!parent_id.empty() && guard++ < 64) {
        if (parent_id == ancestor_id)
            return true;
        const Layer *parent = editor_find_layer_by_id(title, parent_id);
        if (!parent)
            break;
        parent_id = parent->parent_id;
    }
    return false;
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

void CanvasPreview::invalidate_selection_overlay_cache() const
{
    selection_overlay_cache_valid_ = false;
    selection_overlay_cache_ = SelectionOverlayGeometry{};
}

void CanvasPreview::invalidate_hover_overlay_cache() const
{
    hover_overlay_cache_valid_ = false;
    hover_overlay_cache_ = HoverOverlayGeometry{};
}

void CanvasPreview::invalidate_canvas_overlay_caches() const
{
    invalidate_selection_overlay_cache();
    invalidate_hover_overlay_cache();
}

bool CanvasPreview::layer_overlay_changes_with_playhead(const Layer &layer) const
{
    if (layer.position.is_animated() || layer.scale.is_animated() || layer.rotation.is_animated() ||
        layer.size.is_animated() || layer.origin_prop.is_animated())
        return true;

    std::string parent_id = layer.parent_id;
    int guard = 0;
    while (!parent_id.empty() && guard++ < 64) {
        const Layer *parent = editor_find_layer_by_id(title_, parent_id);
        if (!parent)
            break;
        if (parent->position.is_animated() || parent->scale.is_animated() || parent->rotation.is_animated())
            return true;
        parent_id = parent->parent_id;
    }

    return false;
}

bool CanvasPreview::overlay_visibility_crosses_playhead_boundary(const Layer &layer,
                                                                 double old_playhead,
                                                                 double new_playhead) const
{
    const bool was_active = old_playhead >= layer.in_time && old_playhead <= layer.out_time;
    const bool is_active = new_playhead >= layer.in_time && new_playhead <= layer.out_time;
    return was_active != is_active;
}

bool CanvasPreview::canvas_overlay_changes_with_playhead(double old_playhead, double new_playhead) const
{
    if (!title_)
        return false;

    for (const auto &layer : selected_layers()) {
        if (!layer)
            continue;
        if (overlay_visibility_crosses_playhead_boundary(*layer, old_playhead, new_playhead) ||
            layer_overlay_changes_with_playhead(*layer))
            return true;
    }

    if (!hovered_layer_id_.empty()) {
        if (const Layer *hovered = editor_find_layer_by_id(title_, hovered_layer_id_)) {
            if (overlay_visibility_crosses_playhead_boundary(*hovered, old_playhead, new_playhead) ||
                layer_overlay_changes_with_playhead(*hovered))
                return true;
        } else {
            return true;
        }
    }

    return false;
}

const CanvasPreview::SelectionOverlayGeometry &CanvasPreview::selection_overlay_geometry() const
{
    if (selection_overlay_cache_valid_)
        return selection_overlay_cache_;

    SelectionOverlayGeometry geometry;
    const auto layers = selected_layers();
    geometry.valid = true;
    geometry.layers.reserve(layers.size());

    for (const auto &layer : layers) {
        if (!layer)
            continue;

        const QRectF box = layer_local_rect(*layer);
        SelectionOverlayLayerGeometry layer_geometry;
        layer_geometry.layer = layer.get();
        layer_geometry.editing_text_layer = !inline_text_layer_id_.empty() &&
            inline_text_layer_id_ == layer->id && is_canvas_text_layer(*layer);

        auto layer_point_to_view = [&](const QPointF &pt) {
            return canvas_to_view(layer_to_canvas(*layer, pt));
        };
        const QPointF corners[] = {
            layer_point_to_view(box.topLeft()),
            layer_point_to_view(box.topRight()),
            layer_point_to_view(box.bottomRight()),
            layer_point_to_view(box.bottomLeft())
        };
        for (const QPointF &corner : corners)
            layer_geometry.outline << corner;

        layer_geometry.handles[0] = corners[0];
        layer_geometry.handles[1] = layer_point_to_view(QPointF(box.center().x(), box.top()));
        layer_geometry.handles[2] = corners[1];
        layer_geometry.handles[3] = layer_point_to_view(QPointF(box.right(), box.center().y()));
        layer_geometry.handles[4] = corners[2];
        layer_geometry.handles[5] = layer_point_to_view(QPointF(box.center().x(), box.bottom()));
        layer_geometry.handles[6] = corners[3];
        layer_geometry.handles[7] = layer_point_to_view(QPointF(box.left(), box.center().y()));
        layer_geometry.origin = layer_point_to_view(QPointF(0, 0));
        geometry.layers.push_back(layer_geometry);
    }

    if (geometry.layers.size() > 1) {
        const QRectF bounds = selected_canvas_bounds();
        if (bounds.isValid() && !bounds.isEmpty()) {
            geometry.multi_bounds_view = QRectF(canvas_to_view(bounds.topLeft()),
                                                canvas_to_view(bounds.bottomRight())).normalized();
            const QRectF &view_bounds = geometry.multi_bounds_view;
            geometry.multi_handles[0] = view_bounds.topLeft();
            geometry.multi_handles[1] = QPointF(view_bounds.center().x(), view_bounds.top());
            geometry.multi_handles[2] = view_bounds.topRight();
            geometry.multi_handles[3] = QPointF(view_bounds.right(), view_bounds.center().y());
            geometry.multi_handles[4] = view_bounds.bottomRight();
            geometry.multi_handles[5] = QPointF(view_bounds.center().x(), view_bounds.bottom());
            geometry.multi_handles[6] = view_bounds.bottomLeft();
            geometry.multi_handles[7] = QPointF(view_bounds.left(), view_bounds.center().y());
        }
    }

    selection_overlay_cache_ = geometry;
    selection_overlay_cache_valid_ = true;
    return selection_overlay_cache_;
}

const CanvasPreview::HoverOverlayGeometry &CanvasPreview::hover_overlay_geometry() const
{
    if (hover_overlay_cache_valid_)
        return hover_overlay_cache_;

    HoverOverlayGeometry geometry;
    if (!title_ || hovered_layer_id_.empty() || drag_mode_ != DragMode::None || drawing_shape_) {
        hover_overlay_cache_ = geometry;
        hover_overlay_cache_valid_ = true;
        return hover_overlay_cache_;
    }

    auto it = std::find_if(title_->layers.begin(), title_->layers.end(), [this](const std::shared_ptr<Layer> &layer) {
        return layer && layer->id == hovered_layer_id_;
    });
    if (it != title_->layers.end() && *it) {
        const Layer &layer = **it;
        if (layer.visible && !layer.locked && playhead_ >= layer.in_time && playhead_ <= layer.out_time) {
            const QRectF box = layer_local_rect(layer);
            if (box.isValid() && !box.isEmpty()) {
                auto layer_point_to_view = [&](const QPointF &pt) {
                    return canvas_to_view(layer_to_canvas(layer, pt));
                };
                geometry.layer = &layer;
                geometry.hovered_is_selected =
                    std::find(selected_layer_ids_.begin(), selected_layer_ids_.end(), hovered_layer_id_) != selected_layer_ids_.end() ||
                    sel_layer_id_ == hovered_layer_id_;
                geometry.outline << layer_point_to_view(box.topLeft())
                                 << layer_point_to_view(box.topRight())
                                 << layer_point_to_view(box.bottomRight())
                                 << layer_point_to_view(box.bottomLeft());
            }
        }
    }

    hover_overlay_cache_ = geometry;
    hover_overlay_cache_valid_ = true;
    return hover_overlay_cache_;
}

bool CanvasPreview::gradient_handles_visible() const
{
    return active_tool_ == CanvasTool::Gradient || gradient_editor_active_;
}

bool CanvasPreview::layer_supports_gradient_handles(const Layer &layer) const
{
    if (!gradient_handles_visible() || layer.locked || !layer.visible || layer.fill_type != 1)
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
    g.radial = layer.gradient_type == 1 || layer.gradient_type == 4;
    g.local_rect = box;
    g.center = QPointF(box.left() + (double)layer.gradient_center_x * box.width(),
                       box.top() + (double)layer.gradient_center_y * box.height());

    const double scale = std::clamp((double)layer.gradient_scale, 0.01, 100.0);
    const double angle = degrees_to_radians(layer.gradient_angle);
    const QPointF axis(std::cos(angle), std::sin(angle));
    if (g.radial) {
        const double radius = std::max(box.width(), box.height()) * 0.5 * scale;
        g.radius = g.center + axis * std::max(1.0, radius);
        g.focal = QPointF(box.left() + (double)layer.gradient_focal_x * box.width(),
                          box.top() + (double)layer.gradient_focal_y * box.height());
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
        return QPointF((pt.x() - box.left()) / box.width(),
                       (pt.y() - box.top()) / box.height());
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
        layer->gradient_scale = (float)std::clamp(distance / base, 0.01, 100.0);
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
        layer->gradient_scale = (float)std::clamp(radius / base, 0.01, 100.0);
    } else if (drag_mode_ == DragMode::GradientStart) {
        assign_axis(local, gradient_drag_.end);
    } else if (drag_mode_ == DragMode::GradientEnd) {
        assign_axis(gradient_drag_.start, local);
    }

    dirty_ = true;
    drag_changed_ = true;
    drag_current_view_ = view_pt;
    invalidate_canvas_overlay_caches();
    update();
    return true;
}


bool CanvasPreview::begin_gradient_tool_drag(const QPointF &view_pt, Qt::KeyboardModifiers modifiers)
{
    auto layer = selected_layer();
    if (!layer || layer->locked || !layer->visible)
        return false;
    if (!(layer->type == LayerType::SolidRect || layer->type == LayerType::Shape || is_canvas_text_layer(*layer)))
        return false;

    const QRectF box = layer_local_rect(*layer);
    if (!box.isValid() || box.width() <= 0.0 || box.height() <= 0.0)
        return false;

    layer->fill_type = 1;
    if (layer->gradient_type < 0 || layer->gradient_type > 4)
        layer->gradient_type = 0;
    const QPointF local = canvas_to_layer(*layer, view_to_canvas(view_pt));
    gradient_tool_dragging_ = true;
    gradient_tool_start_local_ = local;
    drag_mode_ = (layer->gradient_type == 1 || layer->gradient_type == 4) ? DragMode::GradientRadius : DragMode::GradientEnd;

    auto normalized = [&](const QPointF &pt) {
        return QPointF((pt.x() - box.left()) / box.width(),
                       (pt.y() - box.top()) / box.height());
    };
    const QPointF n = normalized(local);
    layer->gradient_center_x = (float)n.x();
    layer->gradient_center_y = (float)n.y();
    layer->gradient_focal_x = (float)n.x();
    layer->gradient_focal_y = (float)n.y();
    layer->gradient_scale = 0.01f;
    layer->gradient_angle = modifiers.testFlag(Qt::ShiftModifier) ? 0.0f : layer->gradient_angle;

    begin_gradient_drag(*layer);
    dirty_ = true;
    drag_changed_ = true;
    invalidate_canvas_overlay_caches();
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

QPointF CanvasPreview::corner_radius_visual_offset_view(const Layer &layer, const QRectF &box, DragMode mode) const
{
    constexpr double kCornerRadiusHandleOffsetPx = 20.0;
    const double max_radius = std::max(0.0, std::min(box.width(), box.height()) / 2.0);
    auto radius = [max_radius](float value) {
        return std::clamp((double)value, 0.0, max_radius);
    };
    QPointF corner;
    QPointF inward;
    double current_radius = 0.0;
    switch (mode) {
    case DragMode::CornerRadiusTL:
        corner = box.topLeft();
        inward = corner + QPointF(1.0, 1.0);
        current_radius = radius(layer.corner_radius_tl);
        break;
    case DragMode::CornerRadiusTR:
        corner = box.topRight();
        inward = corner + QPointF(-1.0, 1.0);
        current_radius = radius(layer.corner_radius_tr);
        break;
    case DragMode::CornerRadiusBR:
        corner = box.bottomRight();
        inward = corner + QPointF(-1.0, -1.0);
        current_radius = radius(layer.corner_radius_br);
        break;
    case DragMode::CornerRadiusBL:
        corner = box.bottomLeft();
        inward = corner + QPointF(1.0, -1.0);
        current_radius = radius(layer.corner_radius_bl);
        break;
    default:
        return QPointF();
    }

    const QPointF corner_view = canvas_to_view(layer_to_canvas(layer, corner));
    const QPointF inward_view = canvas_to_view(layer_to_canvas(layer, inward));
    const QLineF direction(corner_view, inward_view);
    if (direction.length() <= 0.001)
        return QPointF();
    const double t = max_radius > 0.0 ? std::clamp(current_radius / max_radius, 0.0, 1.0) : 0.0;
    const double offset_px = kCornerRadiusHandleOffsetPx * (1.0 - 2.0 * t);
    return (inward_view - corner_view) / direction.length() * offset_px;
}

QPointF CanvasPreview::corner_radius_handle_view_pos(const Layer &layer, const QRectF &box, DragMode mode) const
{
    return canvas_to_view(layer_to_canvas(layer, corner_radius_handle_local_pos(layer, box, mode))) +
           corner_radius_visual_offset_view(layer, box, mode);
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
        const QPointF view = corner_radius_handle_view_pos(layer, box, mode);
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
        QPointF corner;
        switch (mode) {
        case DragMode::CornerRadiusTL: corner = box.topLeft(); break;
        case DragMode::CornerRadiusTR: corner = box.topRight(); break;
        case DragMode::CornerRadiusBR: corner = box.bottomRight(); break;
        case DragMode::CornerRadiusBL: corner = box.bottomLeft(); break;
            default: return;
        }
        const QPointF view = corner_radius_handle_view_pos(layer, box, mode);
        p.drawLine(to_view(corner), view);
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
    const double max_radius = std::max(0.0, std::min(box.width(), box.height()) / 2.0);
    auto radius_from_local = [&](const QPointF &local) {
        switch (drag_mode_) {
        case DragMode::CornerRadiusTL:
            return std::min(local.x() - box.left(), local.y() - box.top());
        case DragMode::CornerRadiusTR:
            return std::min(box.right() - local.x(), local.y() - box.top());
        case DragMode::CornerRadiusBR:
            return std::min(box.right() - local.x(), box.bottom() - local.y());
        case DragMode::CornerRadiusBL:
            return std::min(local.x() - box.left(), box.bottom() - local.y());
        default:
            return 0.0;
        }
    };
    auto set_preview_radius = [&](double value) {
        switch (drag_mode_) {
        case DragMode::CornerRadiusTL: layer->corner_radius_tl = (float)value; break;
        case DragMode::CornerRadiusTR: layer->corner_radius_tr = (float)value; break;
        case DragMode::CornerRadiusBR: layer->corner_radius_br = (float)value; break;
        case DragMode::CornerRadiusBL: layer->corner_radius_bl = (float)value; break;
        default: break;
        }
    };

    double radius = 0.0;
    for (int i = 0; i < 4; ++i) {
        set_preview_radius(radius);
        const QPointF local = canvas_to_layer(*layer, view_to_canvas(
            view_pt - corner_radius_visual_offset_view(*layer, box, drag_mode_)));
        radius = std::clamp(radius_from_local(local), 0.0, max_radius);
    }
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
    invalidate_canvas_overlay_caches();
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

bool CanvasPreview::active_draw_tool_can_manipulate_selected() const
{
    if (active_tool_ != CanvasTool::Shape && active_tool_ != CanvasTool::Text)
        return false;
    const auto layers = selected_layers();
    if (layers.empty())
        return false;
    for (const auto &layer : layers) {
        if (!layer || layer->locked || !layer->visible)
            return false;
        if (playhead_ < layer->in_time || playhead_ > layer->out_time)
            return false;
        if (active_tool_ == CanvasTool::Shape) {
            if (!(layer->type == LayerType::Shape || layer->type == LayerType::SolidRect))
                return false;
        } else if (active_tool_ == CanvasTool::Text) {
            if (!is_canvas_text_layer(*layer))
                return false;
        }
    }
    return true;
}

void CanvasPreview::set_cursor_for_drag_mode(DragMode mode, bool dragging)
{
    if (mode == DragMode::Move) setCursor(dragging ? Qt::ClosedHandCursor : Qt::OpenHandCursor);
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
    invalidate_canvas_overlay_caches();
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
    std::set<std::string> reserved_names;

    for (const auto &layer : title_->layers) {
        if (!layer || layer->locked || selected_ids.find(layer->id) == selected_ids.end())
            continue;

        auto clone = std::make_shared<Layer>(*layer);
        clone->id = TitleDataStore::make_uuid();
        clone->name = clone->name.empty() ? editor_text_std("OBSTitles.LayerCopy") :
                                            clone->name + editor_text_std("OBSTitles.CopySuffix");
        clone->name = unique_canvas_layer_name(*title_, clone->name, {}, &reserved_names);
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
                                      {},
                                      clone->position.evaluate(lt).x,
                                      clone->position.evaluate(lt).y,
                                      (float)eval_box_width(*clone, lt),
                                      (float)eval_box_height(*clone, lt),
                                      clone->scale.evaluate(lt).x,
                                      clone->scale.evaluate(lt).y,
                                      clone->rotation.evaluate(lt),
                                      clone->stroke_width,
                                      clone->corner_radius_tl,
                                      clone->corner_radius_tr,
                                      clone->corner_radius_br,
                                      clone->corner_radius_bl});
    }
    sel_layer_id_ = selected_layer_ids_.empty() ? std::string() : selected_layer_ids_.back();
    invalidate_canvas_overlay_caches();
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
        set_animated_x(layer->position, lt, layer->position.evaluate(lt).x + dx);
        set_animated_y(layer->position, lt, layer->position.evaluate(lt).y + dy);
        changed = true;
    }

    if (!changed) return false;

    begin_adaptive_interaction();
    dirty_ = true;
    invalidate_canvas_overlay_caches();
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
        add(0.0, obsgs_tr("OBSTitles.Canvas"));
        add(size * 0.5, obsgs_tr("OBSTitles.CanvasCenter"));
        add(size, obsgs_tr("OBSTitles.Canvas"));
    }

    if (snap_settings_.guides) {
        const double size = x_axis ? title_->width : title_->height;
        add(size * OBS_ACTION_SAFE_PERCENT, obsgs_tr("OBSTitles.ActionSafe"));
        add(size * (1.0 - OBS_ACTION_SAFE_PERCENT), obsgs_tr("OBSTitles.ActionSafe"));
        add(size * OBS_GRAPHICS_SAFE_PERCENT, obsgs_tr("OBSTitles.TitleSafe"));
        add(size * (1.0 - OBS_GRAPHICS_SAFE_PERCENT), obsgs_tr("OBSTitles.TitleSafe"));
        const auto &user_guides = x_axis ? vertical_guides_ : horizontal_guides_;
        for (double guide : user_guides)
            add(guide, obsgs_tr("OBSTitles.Guide"));
    }

    if (snap_settings_.grid) {
        const double size = x_axis ? title_->width : title_->height;
        constexpr double grid = 10.0;
        for (double v = 0.0; v <= size + 0.01; v += grid)
            add(v, obsgs_tr("OBSTitles.Grid"));
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
            add(x_axis ? bounds.left() : bounds.top(), obsgs_tr("OBSTitles.ObjectEdge"));
            add(x_axis ? bounds.right() : bounds.bottom(), obsgs_tr("OBSTitles.ObjectEdge"));
        }
        if (snap_settings_.object_centers)
            add(x_axis ? bounds.center().x() : bounds.center().y(), obsgs_tr("OBSTitles.ObjectCenter"));
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
        labels.push_back(obsgs_tr("OBSTitles.Spacing"));
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
    // Point snapping is used by draw-tool hover/start feedback.  Do not route it
    // through snap_delta_for_bounds(): a zero-size QRectF is invalid in Qt, so
    // that path clears the feedback and returns the raw point.  Resolve each
    // axis directly so hover can show the Adobe-style snap dot + helper lines
    // before the first mouse press.
    if (!allow_snap || !title_ || !snap_settings_.enabled) {
        clear_snap_feedback();
        return canvas_pt;
    }

    snap_feedback_.clear();
    QPointF snapped = canvas_pt;
    const double tolerance = 6.0 / std::max(0.1, view_scale());

    auto snap_axis = [&](bool x_axis) {
        std::vector<double> targets;
        std::vector<QString> labels;
        collect_snap_targets(x_axis, targets, labels);
        collect_spacing_targets(x_axis, targets, labels);
        if (targets.empty())
            return;

        const double value = x_axis ? canvas_pt.x() : canvas_pt.y();
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

        if (best_distance <= tolerance) {
            if (x_axis)
                snapped.setX(best_target);
            else
                snapped.setY(best_target);
            add_snap_feedback(x_axis, best_target, best_label);
        }
    };

    if (snap_x)
        snap_axis(true);
    if (snap_y)
        snap_axis(false);
    return snapped;
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
                set_animated_x(layer->position, lt, next_pos.x());
                set_animated_y(layer->position, lt, next_pos.y());
            }
            set_animated_value(layer->rotation, lt, state.rotation + rotation_delta);
        }
        dirty_ = true;
        drag_changed_ = true;
        invalidate_canvas_overlay_caches();
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
                set_animated_x(layer->position, lt, state.x + delta.x());
                set_animated_y(layer->position, lt, state.y + delta.y());
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
                set_animated_x(layer->position, lt, next.left() + rx * next.width());
                set_animated_y(layer->position, lt, next.top() + ry * next.height());
                const bool scale_text_object = drag_text_object_scaling_ && is_canvas_text_layer(*layer);
                if (scale_text_object) {
                    set_animated_x(layer->scale, lt, state.scale_x * sx);
                    set_animated_y(layer->scale, lt, state.scale_y * sy);
                } else {
                    layer->rect_width = std::max(0.0f, (float)(state.w * sx));
                    layer->rect_height = std::max(0.0f, (float)(state.h * sy));
                    set_animated_x(layer->size, lt, layer->rect_width);
                    set_animated_y(layer->size, lt, layer->rect_height);
                    apply_shape_resize_metrics_from_state(*layer, state.w, state.h,
                                                          state.stroke_width,
                                                          state.corner_radius_tl,
                                                          state.corner_radius_tr,
                                                          state.corner_radius_br,
                                                          state.corner_radius_bl,
                                                          layer->rect_width,
                                                          layer->rect_height);
                }
            }
        }
        dirty_ = true;
        drag_changed_ = true;
        invalidate_canvas_overlay_caches();
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
        set_animated_x(layer->position, lt, drag_start_x_ + delta.x());
        set_animated_y(layer->position, lt, drag_start_y_ + delta.y());
    } else if (drag_mode_ == DragMode::Origin) {
        clear_snap_feedback();
        double w = std::max(1.0f, drag_start_w_);
        double h = std::max(1.0f, drag_start_h_);
        layer->origin_x = (float)std::clamp(drag_start_origin_x_ + delta.x() / w, 0.0, 1.0);
        layer->origin_y = (float)std::clamp(drag_start_origin_y_ + delta.y() / h, 0.0, 1.0);
        set_animated_x(layer->origin_prop, lt, layer->origin_x);
        set_animated_y(layer->origin_prop, lt, layer->origin_y);
        set_animated_x(layer->position, lt, drag_start_x_ + delta.x());
        set_animated_y(layer->position, lt, drag_start_y_ + delta.y());
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
        const bool lock_aspect_resize =
            ((layer->type == LayerType::Image && layer->image_box_lock_aspect_ratio) ||
             (layer_is_shape_sized(*layer) && layer->lock_aspect_ratio)) &&
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
            double start_scale_x = start_state ? start_state->scale_x : layer->scale.evaluate(lt).x;
            double start_scale_y = start_state ? start_state->scale_y : layer->scale.evaluate(lt).y;
            set_animated_x(layer->scale, lt, start_scale_x * sx);
            set_animated_y(layer->scale, lt, start_scale_y * sy);
            if (alt_resize && start_state) {
                QTransform next_xf;
                next_xf.translate(start_state->x, start_state->y);
                next_xf.rotate(start_state->rotation);
                next_xf.scale(non_zero_scale(start_scale_x * sx),
                              non_zero_scale(start_scale_y * sy));
                const QPointF current_center_canvas = next_xf.map(start_rect.center());
                const QPointF shift = fixed_center_canvas - current_center_canvas;
                set_animated_x(layer->position, lt, start_state->x + shift.x());
                set_animated_y(layer->position, lt, start_state->y + shift.y());
            }
        } else {
            layer->rect_width = (float)new_w;
            layer->rect_height = (float)new_h;
            set_animated_x(layer->size, lt, new_w);
            set_animated_y(layer->size, lt, new_h);
            if (start_state) {
                apply_shape_resize_metrics_from_state(*layer, start_state->w, start_state->h,
                                                      start_state->stroke_width,
                                                      start_state->corner_radius_tl,
                                                      start_state->corner_radius_tr,
                                                      start_state->corner_radius_br,
                                                      start_state->corner_radius_bl,
                                                      new_w,
                                                      new_h);
            }
            if (start_state) {
                const QRectF actual_rect(-drag_start_origin_x_ * new_w,
                                         -drag_start_origin_y_ * new_h,
                                         new_w, new_h);
                const QPointF local_shift = final_rect.center() - actual_rect.center();
                const QPointF canvas_shift = start_xf.map(local_shift) - start_xf.map(QPointF(0.0, 0.0));
                set_animated_x(layer->position, lt, start_state->x + canvas_shift.x());
                set_animated_y(layer->position, lt, start_state->y + canvas_shift.y());
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
                set_animated_x(layer->position, lt, start_state->x + shift.x());
                set_animated_y(layer->position, lt, start_state->y + shift.y());
            }
        }
    }

    begin_adaptive_interaction();
    dirty_ = true;
    drag_changed_ = true;
    invalidate_canvas_overlay_caches();
    update();
}
void CanvasPreview::render_to_pixmap()
{
    if (render_in_progress_)
        return;
    render_in_progress_ = true;
    QElapsedTimer render_cost;
    render_cost.start();

    if (!title_) {
        frame_pixmap_ = QPixmap();
        frame_pixmap_canvas_offset_ = QPoint();
        frame_pixmap_canvas_size_ = QSize();
        frame_pixmap_preview_scale_ = 1.0;
        render_in_progress_ = false;
        return;
    }

    /* An empty title must clear the previous cached pixmap immediately.  A
     * cache miss intentionally keeps the last valid image for normal playback,
     * but after deleting the final layer that behaviour leaves a stale picture
     * on the editor canvas. */
    if (title_->layers.empty()) {
        frame_pixmap_ = QPixmap();
        frame_pixmap_canvas_offset_ = QPoint();
        frame_pixmap_canvas_size_ = QSize(title_->width, title_->height);
        frame_pixmap_preview_scale_ = 1.0;
        dirty_ = false;
        render_in_progress_ = false;
        update();
        return;
    }

    const CachePlaybackSettings settings = CacheManager::instance().playbackSettings();
    QImage image;
    /* While editing text, always render the preview from the live model. The
     * frame cache is intentionally bypassed here because rich-text auto styling
     * changes can happen without a text-content change, and the inline editor is
     * visually transparent over the canvas-rendered text. */
    const bool live_corner_radius_drag =
        corner_radius_drag_.active &&
        (drag_mode_ == DragMode::CornerRadiusTL || drag_mode_ == DragMode::CornerRadiusTR ||
         drag_mode_ == DragMode::CornerRadiusBR || drag_mode_ == DragMode::CornerRadiusBL);
    const bool live_geometry_drag =
        drag_mode_ != DragMode::None && drag_mode_ != DragMode::Marquee &&
        drag_mode_ != DragMode::GuideX && drag_mode_ != DragMode::GuideY;
    /* Every model-changing canvas gesture must preview the current live title
     * immediately.  Previously only corner-radius manipulation bypassed the
     * cache, so move/scale/rotate/gradient/origin drags kept displaying the
     * previous cached frame until the invalidation timer completed. */
    const bool dynamic_text_title = title_has_dynamic_text_layer(title_);
    /* Clock and ticker layers are intentionally excluded from prerender/cache,
     * so their editor preview must always use the live uncached renderer.
     * Sending them through requestFrame() returns no cached image and leaves
     * the clock/ticker content invisible even though the canvas keeps ticking. */
    const bool model_is_interactive = !inline_text_layer_id_.empty() || live_corner_radius_drag ||
        live_geometry_drag || adaptive_interaction_active_ || force_live_full_quality_render_;
    const double preview_scale = adaptive_preview_scale();
    if (adaptive_rendering_enabled_ && preview_scale < 0.999) {
        // Editor-only reduced-resolution rasterization. Fixed quality modes also
        // use a private editor cache, deliberately separate from CacheManager,
        // so reduced frames can never leak into OBS output or global prerender.
        const bool fixed_quality = adaptive_quality_mode_ != AdaptiveQualityMode::Auto;
        // Never read or populate the settled-frame cache while the model is
        // changing. A fixed-quality mode must use the same live interactive path
        // as Auto; otherwise repeated nudges can display a stale frame and pay
        // cache/hash overhead on every key event.
        const bool allow_editor_cache = fixed_quality && !adaptive_interaction_active_;
        const QString editor_cache_key = QStringLiteral("%1:%2:%3")
            .arg(QString::fromStdString(title_->id))
            .arg(qRound64(playhead_ * 1000.0))
            .arg(qRound(preview_scale * 1000.0));
        if (allow_editor_cache)
            image = editor_quality_cache_.value(editor_cache_key);
        if (image.isNull()) {
            // Reduced editor previews use a draft render while interacting.
            // Expensive temporal/blur effects are temporarily bypassed because
            // they dominate render time but do not improve transform feedback.
            image = render_title_to_image_scaled(*title_, playhead_, preview_scale,
                                                 adaptive_interaction_active_);
            if (allow_editor_cache && !image.isNull()) {
                if (editor_quality_cache_.size() >= 240)
                    editor_quality_cache_.clear();
                editor_quality_cache_.insert(editor_cache_key, image);
            }
        }
    } else if (dynamic_text_title || model_is_interactive) {
        image = CacheManager::instance().renderUncachedFrame(title_, playhead_);
    } else {
        image = CacheManager::instance().requestFrame(title_, playhead_, settings.cached_frames_only);
    }
    if (!image.isNull()) {
        bool ok_x = false, ok_y = false, ok_w = false, ok_h = false;
        const int crop_x = image.text(QStringLiteral("obs_gsp_canvas_x")).toInt(&ok_x);
        const int crop_y = image.text(QStringLiteral("obs_gsp_canvas_y")).toInt(&ok_y);
        const int canvas_w = image.text(QStringLiteral("obs_gsp_canvas_width")).toInt(&ok_w);
        const int canvas_h = image.text(QStringLiteral("obs_gsp_canvas_height")).toInt(&ok_h);
        const bool sparse = ok_x && ok_y && ok_w && ok_h && crop_x >= 0 && crop_y >= 0 &&
            canvas_w > 0 && canvas_h > 0 &&
            crop_x + image.width() <= canvas_w && crop_y + image.height() <= canvas_h;
        bool ok_preview_scale = false;
        const double preview_scale = image.text(QStringLiteral("obs_gsp_preview_scale")).toDouble(&ok_preview_scale);
        frame_pixmap_preview_scale_ = ok_preview_scale ? std::clamp(preview_scale, 0.125, 1.0) : 1.0;
        frame_pixmap_ = QPixmap::fromImage(image);
        frame_pixmap_canvas_offset_ = sparse ? QPoint(crop_x, crop_y) : QPoint();
        frame_pixmap_canvas_size_ = sparse ? QSize(canvas_w, canvas_h) :
            (frame_pixmap_preview_scale_ < 0.999 && title_ ? QSize(title_->width, title_->height) : image.size());
    }
    dirty_ = false;
    force_live_full_quality_render_ = false;
    const int cost_ms = std::max(1, (int)render_cost.elapsed());
    if (frame_pixmap_preview_scale_ >= 0.999)
        last_full_quality_render_cost_ms_ = cost_ms;
    // Adapt the presentation cadence to actual render cost. Fast titles remain
    // 60 fps; complex titles are capped so input and OBS never starve.
    // During direct manipulation prioritize input latency over reproducing each
    // intermediate frame. Coalescing keeps at most one pending render, while a
    // fixed 60 Hz presentation target avoids the sluggish cost-derived cadence.
    render_interval_ms_ = adaptive_interaction_active_ ? 16
                                                       : std::clamp(cost_ms + 2, 16, 50);
    last_render_clock_.restart();
    render_in_progress_ = false;
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

        consider(x_axis ? bounds.left() : bounds.top(), obsgs_tr("OBSTitles.ObjectEdge"));
        consider(x_axis ? bounds.right() : bounds.bottom(), obsgs_tr("OBSTitles.ObjectEdge"));
        if (snap_settings_.object_centers)
            consider(x_axis ? bounds.center().x() : bounds.center().y(), obsgs_tr("OBSTitles.ObjectCenter"));
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
    draw_ruler_mouse_indicators(p, canvas_rect);
    p.restore();
}

void CanvasPreview::draw_guide_coordinate(QPainter &p, const QPointF &view_pt, bool x_axis, double value) const
{
    if (!show_guide_coordinates_) return;
    const QString text = obsgs_tr("OBSTitles.AxisPixelReadout").arg(x_axis ? obsgs_tr("OBSTitles.X") : obsgs_tr("OBSTitles.Y")).arg(value, 0, 'f', 1);
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
    QPen pen(editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::Guides), 1.0, Qt::DashLine);
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
        QPen active_pen(editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::ActiveGuide), 1.0, Qt::DashLine);
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


void CanvasPreview::invalidate_checkerboard_cache()
{
    checkerboard_tile_ = QPixmap();
    checkerboard_tile_pattern_ = -1;
}

void CanvasPreview::draw_static_checkerboard(QPainter &p, const QRect &canvas_rect_px)
{
    if (canvas_rect_px.isEmpty())
        return;

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

    constexpr int checker_size = 12;
    constexpr int tile_size = checker_size * 2;
    if (checkerboard_tile_.isNull() || checkerboard_tile_pattern_ != checkerboard_pattern_) {
        const auto [checker_a, checker_b] = checkerboard_colors();
        checkerboard_tile_ = QPixmap(tile_size, tile_size);
        checkerboard_tile_.fill(checker_a);
        if (checker_a != checker_b) {
            QPainter tile_painter(&checkerboard_tile_);
            tile_painter.setPen(Qt::NoPen);
            tile_painter.setBrush(checker_b);
            tile_painter.drawRect(0, 0, checker_size, checker_size);
            tile_painter.drawRect(checker_size, checker_size, checker_size, checker_size);
        }
        checkerboard_tile_pattern_ = checkerboard_pattern_;
    }

    p.save();
    p.setClipRect(canvas_rect_px);
    p.drawTiledPixmap(canvas_rect_px, checkerboard_tile_, QPoint(0, 0));
    p.restore();
}

void CanvasPreview::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QPalette pal = palette();
    p.fillRect(rect(), pal.color(QPalette::Window));

    if (!title_) return;

    if (!drawing_shape_ && title_has_dynamic_text_layer(title_)) dirty_ = true;
    if (dirty_) {
        const qint64 elapsed = last_render_clock_.isValid() ? last_render_clock_.elapsed() : render_interval_ms_;
        if (!render_in_progress_ && (frame_pixmap_.isNull() || elapsed >= render_interval_ms_)) {
            render_to_pixmap();
        } else if (render_coalesce_timer_ && !render_coalesce_timer_->isActive()) {
            render_coalesce_timer_->start(std::max(1, render_interval_ms_ - (int)elapsed));
        }
    }

    /* A null frame means that the title is visually empty, not that the canvas
     * itself should disappear. Draw the checkerboard, rulers, guides and other
     * editor chrome regardless, and only skip the artwork pixmap below. */
    double scale = view_scale();
    QPointF origin = view_origin();
    int dw = (int)(title_->width * scale);
    int dh = (int)(title_->height * scale);
    int ox = (int)origin.x();
    int oy = (int)origin.y();

    const QRectF canvas_rect_f(ox, oy, dw, dh);
    const QRect canvas_rect_px = canvas_rect_f.toAlignedRect();
    draw_static_checkerboard(p, canvas_rect_px);

    if (!frame_pixmap_.isNull()) {
        const int pix_x = ox + static_cast<int>(std::round(frame_pixmap_canvas_offset_.x() * scale));
        const int pix_y = oy + static_cast<int>(std::round(frame_pixmap_canvas_offset_.y() * scale));
        const double logical_pix_w = frame_pixmap_preview_scale_ > 0.0
            ? frame_pixmap_.width() / frame_pixmap_preview_scale_ : frame_pixmap_.width();
        const double logical_pix_h = frame_pixmap_preview_scale_ > 0.0
            ? frame_pixmap_.height() / frame_pixmap_preview_scale_ : frame_pixmap_.height();
        const int pix_w = static_cast<int>(std::round(logical_pix_w * scale));
        const int pix_h = static_cast<int>(std::round(logical_pix_h * scale));
        p.drawPixmap(pix_x, pix_y, pix_w, pix_h, frame_pixmap_);
    }

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
        draw_guide(OBS_ACTION_SAFE_PERCENT, editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::ActionSafe));
        draw_guide(OBS_GRAPHICS_SAFE_PERCENT, editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::GraphicsSafe));
    }

    draw_user_guides(p, QRectF(ox, oy, dw, dh));
    draw_hover_layer_box(p);

    if (!snap_feedback_.empty()) {
        p.save();
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setBrush(QColor(0, 20, 30, 180));
        for (const auto &feedback : snap_feedback_) {
            QPen guide_pen(editor_canvas_helper_color(snap_feedback_role_from_label(feedback.label)), 1.0, Qt::DashLine);
            guide_pen.setDashPattern({5.0, 3.0});
            guide_pen.setCosmetic(true);
            p.setPen(guide_pen);
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

    const SelectionOverlayGeometry &selection_overlay = selection_overlay_geometry();

    auto draw_layer_box = [&](const SelectionOverlayLayerGeometry &layer_geometry, bool handles) {
        if (!layer_geometry.layer)
            return;
        const Layer &layer = *layer_geometry.layer;
        const bool editing_text_layer = layer_geometry.editing_text_layer;
        QColor text_box_color = editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::TextBoundingBox);
        if (!handles && text_box_color.alpha() > 210)
            text_box_color.setAlpha(210);
        QColor selection_box_color = editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox);
        if (!handles && selection_box_color.alpha() > 150)
            selection_box_color.setAlpha(150);
        const QColor outline_color = editing_text_layer ? text_box_color : selection_box_color;
        QColor handle_stroke_color = editing_text_layer
            ? editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::TextBoundingBox)
            : editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox);
        if (handle_stroke_color.alpha() < 180)
            handle_stroke_color.setAlpha(180);

        p.save();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(outline_color, editing_text_layer ? 2.0 : 1.5, Qt::DashLine));
        p.drawPolygon(layer_geometry.outline);
        if (handles) {
            p.setPen(QPen(handle_stroke_color, 1.0));
            p.setBrush(QColor(255, 255, 255));
            const double half_handle = CANVAS_CONTROL_SIZE_PX / 2.0;
            for (const QPointF &pt : layer_geometry.handles)
                p.drawRect(QRectF(pt.x() - half_handle, pt.y() - half_handle,
                                  CANVAS_CONTROL_SIZE_PX, CANVAS_CONTROL_SIZE_PX));

            QPointF origin = layer_geometry.origin;
            p.setPen(QPen(QColor(255, 160, 0), 1.5));
            p.setBrush(QColor(255, 220, 80));
            p.drawEllipse(origin, CANVAS_ORIGIN_RADIUS_PX, CANVAS_ORIGIN_RADIUS_PX);
            p.drawLine(QPointF(origin.x() - CANVAS_CONTROL_SIZE_PX, origin.y()),
                       QPointF(origin.x() + CANVAS_CONTROL_SIZE_PX, origin.y()));
            p.drawLine(QPointF(origin.x(), origin.y() - CANVAS_CONTROL_SIZE_PX),
                       QPointF(origin.x(), origin.y() + CANVAS_CONTROL_SIZE_PX));

            if (gradient_handles_visible())
                draw_gradient_handles(p, layer);
        }
        p.restore();
    };

    if (selection_overlay.layers.size() == 1) {
        draw_layer_box(selection_overlay.layers.front(), true);
    } else if (selection_overlay.layers.size() > 1) {
        for (const auto &layer_geometry : selection_overlay.layers)
            draw_layer_box(layer_geometry, false);

        if (selection_overlay.multi_bounds_view.isValid() && !selection_overlay.multi_bounds_view.isEmpty()) {
            const QRectF &view_bounds = selection_overlay.multi_bounds_view;
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox), 1.5, Qt::SolidLine));
            p.drawRect(view_bounds);
            p.setPen(QPen(editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox), 1.0));
            p.setBrush(QColor(255, 255, 255));
            for (const QPointF &pt : selection_overlay.multi_handles)
                p.drawRect(QRectF(pt.x() - CANVAS_CONTROL_SIZE_PX / 2.0,
                                  pt.y() - CANVAS_CONTROL_SIZE_PX / 2.0,
                                  CANVAS_CONTROL_SIZE_PX, CANVAS_CONTROL_SIZE_PX));
        }
    }

    draw_toolbar_preview(p);
    draw_snap_cursor_indicator(p);

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
        QColor marquee_fill = editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox);
        marquee_fill.setAlpha(std::min(marquee_fill.alpha(), 45));
        p.setBrush(marquee_fill);
        p.setPen(QPen(editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox), 1.0, Qt::DashLine));
        p.drawRect(marquee);
    }

    draw_canvas_drag_tooltip(p);
    draw_color_picker_tooltip(p);
    draw_canvas_border(p, QRectF(ox, oy, dw, dh));
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

    const int x = (int)std::floor(canvas.x()) - frame_pixmap_canvas_offset_.x();
    const int y = (int)std::floor(canvas.y()) - frame_pixmap_canvas_offset_.y();
    if (x < 0 || y < 0 || x >= image.width() || y >= image.height()) {
        color = QColor(Qt::transparent);
        return true;
    }

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
    if (drawing_shape_) {
        const QRectF rect = shape_draw_current_rect_.isValid()
                                ? shape_draw_current_rect_.normalized()
                                : toolbar_draw_rect(shape_draw_current_canvas_, shape_draw_modifiers_).normalized();
        if (drawing_shape_changed_ && rect.isValid() && !rect.isEmpty()) {
            return obsgs_tr("OBSTitles.SizePixelReadout")
                .arg(std::round(rect.width()), 0, 'f', 0)
                .arg(std::round(rect.height()), 0, 'f', 0);
        }
    }

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
        return obsgs_tr("OBSTitles.PositionPixelReadout")
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
        return obsgs_tr("OBSTitles.SizePixelReadout")
            .arg(std::round(w), 0, 'f', 0)
            .arg(std::round(h), 0, 'f', 0);
    }

    if (drag_mode_ == DragMode::Rotate) {
        const double radians = degrees_to_radians(drag_current_rotation_delta_);
        return obsgs_tr("OBSTitles.RadiansReadout").arg(radians, 0, 'f', 3);
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
        return obsgs_tr("OBSTitles.RadiusPixelReadout").arg(std::round(radius), 0, 'f', 0);
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


QRect CanvasPreview::snap_cursor_update_rect() const
{
    if (!snap_cursor_visible_)
        return QRect();
    const QPointF view_pt = canvas_to_view(snap_cursor_canvas_);
    return QRectF(view_pt.x() - 12.0, view_pt.y() - 12.0, 24.0, 24.0).toAlignedRect();
}

QRect CanvasPreview::final_snap_cursor_update_rect() const
{
    if (!final_snap_cursor_visible_)
        return QRect();
    const QPointF view_pt = canvas_to_view(final_snap_cursor_canvas_);
    return QRectF(view_pt.x() - 12.0, view_pt.y() - 12.0, 24.0, 24.0).toAlignedRect();
}

QRect CanvasPreview::canvas_drag_tooltip_update_rect() const
{
    const QString text = canvas_drag_tooltip_text();
    if (text.isEmpty())
        return QRect();

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
    return QRectF(pos, QSizeF(width, height)).toAlignedRect().adjusted(-3, -3, 3, 3);
}

void CanvasPreview::clear_draw_tool_snap_cursor()
{
    const bool had_cursor = snap_cursor_visible_;
    const bool had_final_cursor = final_snap_cursor_visible_;
    const bool had_feedback = !snap_feedback_.empty();
    const QRect old_rect = snap_cursor_update_rect().united(final_snap_cursor_update_rect());
    snap_cursor_visible_ = false;
    final_snap_cursor_visible_ = false;
    snap_feedback_.clear();
    if (had_cursor || had_final_cursor)
        update(old_rect.adjusted(-2, -2, 2, 2));
    if (had_feedback)
        update();
}

void CanvasPreview::update_draw_tool_snap_cursor(const QPointF &view_pt, Qt::KeyboardModifiers modifiers)
{
    if (!title_ || drawing_shape_ ||
        (active_tool_ != CanvasTool::Shape && active_tool_ != CanvasTool::Text)) {
        clear_draw_tool_snap_cursor();
        return;
    }

    const bool had_feedback = !snap_feedback_.empty();
    const QRect old_rect = snap_cursor_update_rect();
    const bool allow_snap = !modifiers.testFlag(Qt::ControlModifier);
    const QPointF snapped = snap_canvas_point(view_to_canvas(view_pt), true, true, allow_snap);
    const bool show = allow_snap && snap_settings_.enabled && !snap_feedback_.empty();
    snap_cursor_visible_ = show;
    snap_cursor_canvas_ = snapped;

    const QRect new_rect = snap_cursor_update_rect();
    QRect dirty = old_rect.united(new_rect);
    if (!dirty.isEmpty())
        update(dirty.adjusted(-4, -4, 4, 4));
    // Snap helpers can span the whole canvas/ruler area, so repaint the full
    // preview when hover feedback appears, disappears, or changes.  This keeps
    // the Adobe-style snapped dot and helper lines visible before mouse press.
    if (had_feedback || !snap_feedback_.empty())
        update();
}

void CanvasPreview::draw_snap_cursor_indicator(QPainter &p)
{
    if (active_tool_ != CanvasTool::Shape && active_tool_ != CanvasTool::Text && !drawing_shape_)
        return;
    if (!snap_cursor_visible_ && !final_snap_cursor_visible_)
        return;

    auto role = TitlePreferences::CanvasHelperColorRole::CanvasSnapLines;
    for (const auto &feedback : snap_feedback_) {
        if (snap_feedback_role_from_label(feedback.label) == TitlePreferences::CanvasHelperColorRole::ObjectSnapLines) {
            role = TitlePreferences::CanvasHelperColorRole::ObjectSnapLines;
            break;
        }
    }
    QColor color = editor_canvas_helper_color(role);
    if (!color.isValid())
        color = QColor(0, 210, 110, 220);
    QColor fill = color;
    fill.setAlpha(std::min(80, std::max(32, color.alpha() / 3)));

    auto draw_circle = [&](const QPointF &canvas_pt, double radius, bool filled) {
        const QPointF pt = canvas_to_view(canvas_pt);
        QPen pen(color, 1.5, Qt::SolidLine);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.setBrush(filled ? fill : Qt::NoBrush);
        p.drawEllipse(pt, radius, radius);
    };

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    if (snap_cursor_visible_)
        draw_circle(snap_cursor_canvas_, 5.0, true);
    if (final_snap_cursor_visible_)
        draw_circle(final_snap_cursor_canvas_, 5.0, true);
    p.restore();
}

void CanvasPreview::update_shape_drawing(const QPointF &view_pt, Qt::KeyboardModifiers modifiers)
{
    if (!drawing_shape_) return;

    const QRect old_update_rect = last_toolbar_preview_update_rect_
                                      .united(snap_cursor_update_rect())
                                      .united(final_snap_cursor_update_rect())
                                      .united(canvas_drag_tooltip_update_rect());
    drag_current_view_ = view_pt;
    shape_draw_current_canvas_ = view_to_canvas(view_pt);
    shape_draw_modifiers_ = modifiers;
    const bool allow_snap = !modifiers.testFlag(Qt::ControlModifier);
    final_snap_cursor_visible_ = false;
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
        final_snap_cursor_visible_ = !snap_feedback_.empty();
        final_snap_cursor_canvas_ = shape_draw_current_canvas_;
        raw_rect = toolbar_draw_rect(shape_draw_current_canvas_, shape_draw_modifiers_);
    }
    shape_draw_current_rect_ = raw_rect.normalized();
    drawing_shape_changed_ = shape_draw_current_rect_.width() >= 2.0 || shape_draw_current_rect_.height() >= 2.0;

    last_toolbar_preview_update_rect_ = toolbar_preview_update_rect()
                                           .united(snap_cursor_update_rect())
                                           .united(final_snap_cursor_update_rect())
                                           .united(canvas_drag_tooltip_update_rect());
    QRect repaint_rect = old_update_rect.united(last_toolbar_preview_update_rect_);
    if (repaint_rect.isEmpty())
        repaint_rect = this->rect();
    update(repaint_rect.adjusted(-4, -4, 4, 4));
}


std::shared_ptr<Layer> CanvasPreview::layer_at_view_pos(const QPointF &view_pt) const
{
    if (!title_) return nullptr;
    const QPointF canvas = view_to_canvas(view_pt);
    for (auto it = title_->layers.rbegin(); it != title_->layers.rend(); ++it) {
        const auto &layer = *it;
        if (!layer || !layer->visible || layer->locked) continue;
        if (playhead_ < layer->in_time || playhead_ > layer->out_time) continue;
        const QRectF local_rect = layer_local_rect(*layer);
        if (!local_rect.isValid() || local_rect.isEmpty()) continue;
        if (local_rect.contains(canvas_to_layer(*layer, canvas)))
            return layer;
    }
    return nullptr;
}

void CanvasPreview::update_hover_layer(const QPointF &view_pt)
{
    last_mouse_view_pos_ = view_pt;
    mouse_inside_canvas_ = title_ && canvas_view_rect().contains(view_pt);

    std::string next_hover;
    if (mouse_inside_canvas_ && drag_mode_ == DragMode::None && !drawing_shape_ &&
        inline_text_layer_id_.empty()) {
        const bool selection_hover = active_tool_ == CanvasTool::Selection;
        const bool draw_tool_hover = active_tool_ == CanvasTool::Shape || active_tool_ == CanvasTool::Text;
        if (selection_hover || draw_tool_hover) {
            if (auto layer = layer_at_view_pos(view_pt)) {
                if (selection_hover ||
                    (active_tool_ == CanvasTool::Shape &&
                     (layer->type == LayerType::Shape || layer->type == LayerType::SolidRect)) ||
                    (active_tool_ == CanvasTool::Text && is_canvas_text_layer(*layer))) {
                    next_hover = layer->id;
                }
            }
        }
    }

    const bool hover_changed = hovered_layer_id_ != next_hover;
    if (hover_changed) {
        hovered_layer_id_ = next_hover;
        invalidate_hover_overlay_cache();
        update();
    }

    if (rulers_visible_) {
        QRect ruler_dirty = ruler_top_rect().toAlignedRect().united(ruler_left_rect().toAlignedRect());
        if (!ruler_dirty.isEmpty()) {
            // Keep cursor indicators visually locked to the mouse.  update() is
            // intentionally asynchronous and can lag a few milliseconds behind
            // high-frequency mouse move events, so force an immediate repaint of
            // only the lightweight ruler strip.
            repaint(ruler_dirty.adjusted(-2, -2, 2, 2));
        } else if (!hover_changed) {
            update();
        }
    } else if (!hover_changed) {
        // No repaint is needed for ordinary mouse motion when neither the hover
        // outline nor ruler indicators changed.
    }
}

void CanvasPreview::draw_hover_layer_box(QPainter &p)
{
    const HoverOverlayGeometry &hover_overlay = hover_overlay_geometry();
    if (!hover_overlay.layer)
        return;
    const Layer &layer = *hover_overlay.layer;

    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    if (!hover_overlay.hovered_is_selected) {
        QPen pen(editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::HoverBoundingBox), 1.0, Qt::SolidLine);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(hover_overlay.outline);
    }
    if (layer_supports_corner_radius_handles(layer))
        draw_corner_radius_handles(p, layer);
    p.restore();
}

void CanvasPreview::draw_canvas_border(QPainter &p, const QRectF &canvas_rect)
{
    if (!canvas_border_visible_ || !canvas_rect.isValid() || canvas_rect.isEmpty()) return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    QPen pen(editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::CanvasBorder), 1.0, Qt::SolidLine);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(canvas_rect.adjusted(0.5, 0.5, -0.5, -0.5));
    p.restore();
}

void CanvasPreview::draw_ruler_mouse_indicators(QPainter &p, const QRectF &canvas_rect)
{
    if (!rulers_visible_ || !title_ || !mouse_inside_canvas_) return;
    if (!canvas_rect.contains(last_mouse_view_pos_)) return;

    const double x = std::clamp(last_mouse_view_pos_.x(), canvas_rect.left(), canvas_rect.right());
    const double y = std::clamp(last_mouse_view_pos_.y(), canvas_rect.top(), canvas_rect.bottom());
    const QRectF top = ruler_top_rect();
    const QRectF left = ruler_left_rect();
    if (!top.isValid() || !left.isValid()) return;

    QColor accent = editor_canvas_helper_color(TitlePreferences::CanvasHelperColorRole::RulerMouseIndicator);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    QPen pen(accent, 1.0, Qt::SolidLine);
    pen.setCosmetic(true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    // Illustrator-style cursor indicators: simple guide lines only, without
    // numeric labels.  The ruler repaint is forced from mouseMoveEvent so these
    // lines track the pointer immediately instead of waiting for a deferred
    // widget update.
    p.drawLine(QPointF(x, top.top()), QPointF(x, top.bottom()));
    p.drawLine(QPointF(left.left(), y), QPointF(left.right(), y));
    p.restore();
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

    /*
     * Gradient handles are an editor overlay, not normal layer geometry.
     * When the Gradient tab is open (or the Gradient tool is active), the
     * handles are visible and must get first chance at the mouse press.
     *
     * This is intentionally before setFocus(), guide hit-testing, selection,
     * marquee, and normal transform handles. Otherwise a click on a gradient
     * handle can be interpreted as a dock/panel focus change or a normal canvas
     * selection operation, which makes the Gradient tab lose the interaction
     * and feels like the user is dragging the window instead of the handle.
     */
    if (ev->button() == Qt::LeftButton && gradient_handles_visible()) {
        if (auto layer = selected_layer()) {
            DragMode gradient_hit = hit_test_gradient_handles(*layer, ev->pos());
            if (gradient_hit != DragMode::None) {
                drag_mode_ = gradient_hit;
                drag_changed_ = false;
                drag_start_view_ = ev->pos();
                drag_current_view_ = ev->pos();
                drag_start_canvas_ = view_to_canvas(ev->pos());
                drag_layer_states_.clear();
                gradient_drag_ = GradientDragState{};
                corner_radius_drag_ = CornerRadiusDragState{};
                begin_gradient_drag(*layer);
                if (drag_mode_ == DragMode::GradientCenter || drag_mode_ == DragMode::GradientFocal)
                    setCursor(Qt::CrossCursor);
                else
                    setCursor(Qt::SizeAllCursor);
                ev->accept();
                return;
            }
        }
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
        clear_draw_tool_snap_cursor();
        update_color_picker_tooltip(ev->pos());
        if (color_picker_tooltip_visible_)
            emit color_picked(color_picker_tooltip_color_);
        ev->accept();
        return;
    }

    if (active_tool_ == CanvasTool::Gradient) {
        if (begin_gradient_tool_drag(ev->pos(), ev->modifiers())) {
            ev->accept();
            return;
        }
    }

    bool begin_draw_tool_layer_manipulation = false;
    if ((active_tool_ == CanvasTool::Shape || active_tool_ == CanvasTool::Text) &&
        active_draw_tool_can_manipulate_selected()) {
        drag_mode_ = hit_test_selected(ev->pos());

        // With the Shape tool active, dragging the body of an existing shape should
        // not move it.  Body drag remains reserved for drawing a new shape, while
        // explicit handles (resize/rotate/origin/corner radius) still manipulate
        // the selected shape.  Text keeps its existing body-move behavior.
        if (active_tool_ == CanvasTool::Shape && drag_mode_ == DragMode::Move)
            drag_mode_ = DragMode::None;

        if (drag_mode_ != DragMode::None) {
            begin_draw_tool_layer_manipulation = true;
            clear_draw_tool_snap_cursor();
        }
    }

    if (!begin_draw_tool_layer_manipulation && active_tool_ == CanvasTool::Text) {
        if (auto layer = text_layer_at_view_pos(ev->pos())) {
            emit layer_clicked(layer->id);
            begin_text_edit(layer);
            ev->accept();
            return;
        }
    }

    if (!begin_draw_tool_layer_manipulation &&
        (active_tool_ == CanvasTool::Shape || active_tool_ == CanvasTool::Text)) {
        drawing_shape_ = true;
        drawing_shape_changed_ = false;
        drag_mode_ = DragMode::None;
        selected_layer_ids_.clear();
        sel_layer_id_.clear();
        invalidate_canvas_overlay_caches();
        drag_current_view_ = ev->pos();
        shape_draw_start_canvas_ = snap_canvas_point(view_to_canvas(ev->pos()), true, true,
                                                     !ev->modifiers().testFlag(Qt::ControlModifier));
        shape_draw_current_canvas_ = shape_draw_start_canvas_;
        snap_cursor_visible_ = !snap_feedback_.empty();
        snap_cursor_canvas_ = shape_draw_start_canvas_;
        final_snap_cursor_visible_ = false;
        shape_draw_modifiers_ = ev->modifiers();
        shape_draw_current_rect_ = toolbar_draw_rect(shape_draw_current_canvas_, shape_draw_modifiers_);
        last_toolbar_preview_update_rect_ = toolbar_preview_update_rect();
        update(last_toolbar_preview_update_rect_.isEmpty() ? rect() : last_toolbar_preview_update_rect_);
        ev->accept();
        return;
    }

    if (!begin_draw_tool_layer_manipulation)
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
                invalidate_canvas_overlay_caches();
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
    drag_child_layer_states_.clear();
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
                                      {},
                                      selected->position.evaluate(lt).x,
                                      selected->position.evaluate(lt).y,
                                      (float)eval_box_width(*selected, lt),
                                      (float)eval_box_height(*selected, lt),
                                      selected->scale.evaluate(lt).x,
                                      selected->scale.evaluate(lt).y,
                                      selected->rotation.evaluate(lt),
                                      selected->stroke_width,
                                      selected->corner_radius_tl,
                                      selected->corner_radius_tr,
                                      selected->corner_radius_br,
                                      selected->corner_radius_bl});
    }

    auto selected_contains = [&](const std::string &id) {
        return std::find(selected_layer_ids_.begin(), selected_layer_ids_.end(), id) != selected_layer_ids_.end();
    };
    if (is_resize_drag(drag_mode_)) {
        for (const auto &candidate : title_->layers) {
            if (!candidate || candidate->locked || selected_contains(candidate->id))
                continue;
            std::string resize_root_id;
            for (const auto &selected : layers) {
                if (selected && editor_layer_has_ancestor(title_, *candidate, selected->id)) {
                    resize_root_id = selected->id;
                    break;
                }
            }
            if (resize_root_id.empty())
                continue;
            double child_lt = std::clamp(playhead_ - candidate->in_time, 0.0,
                                         std::max(0.0, candidate->out_time - candidate->in_time));
            drag_child_layer_states_.push_back({candidate->id,
                                                resize_root_id,
                                                candidate->position.evaluate(child_lt).x,
                                                candidate->position.evaluate(child_lt).y,
                                                (float)eval_box_width(*candidate, child_lt),
                                                (float)eval_box_height(*candidate, child_lt),
                                                candidate->scale.evaluate(child_lt).x,
                                                candidate->scale.evaluate(child_lt).y,
                                                candidate->rotation.evaluate(child_lt),
                                                candidate->stroke_width,
                                                candidate->corner_radius_tl,
                                                candidate->corner_radius_tr,
                                                candidate->corner_radius_br,
                                                candidate->corner_radius_bl});
        }
    }

    double lt = std::clamp(playhead_ - layer->in_time, 0.0,
                           std::max(0.0, layer->out_time - layer->in_time));
    drag_start_x_ = layer->position.evaluate(lt).x;
    drag_start_y_ = layer->position.evaluate(lt).y;
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
    update_hover_layer(ev->pos());

    if (panning_ && (ev->buttons() & Qt::MiddleButton)) {
        pan_offset_ = pan_start_offset_ + (QPointF(ev->pos()) - pan_start_view_);
        fit_zoom_active_ = false;
        invalidate_canvas_overlay_caches();
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

    if (active_tool_ == CanvasTool::Shape || active_tool_ == CanvasTool::Text) {
        if (active_draw_tool_can_manipulate_selected()) {
            DragMode mode = hit_test_selected(ev->pos());

            // Do not advertise body-move for existing shape layers while the Shape
            // tool is active; only explicit handles should switch away from the
            // draw cursor.
            if (active_tool_ == CanvasTool::Shape && mode == DragMode::Move)
                mode = DragMode::None;

            if (mode != DragMode::None) {
                clear_draw_tool_snap_cursor();
                set_cursor_for_drag_mode(mode, false);
                return;
            }
        }
        update_draw_tool_snap_cursor(ev->pos(), ev->modifiers());
        setCursor(active_tool_ == CanvasTool::Shape ? Qt::CrossCursor : Qt::IBeamCursor);
        return;
    }
    if (active_tool_ == CanvasTool::ColorPicker) {
        clear_draw_tool_snap_cursor();
        update_color_picker_tooltip(ev->pos());
        setCursor(Qt::CrossCursor);
        return;
    }
    if (active_tool_ == CanvasTool::Gradient || gradient_handles_visible()) {
        clear_draw_tool_snap_cursor();
        if (auto layer = selected_layer()) {
            DragMode mode = hit_test_gradient_handles(*layer, ev->pos());
            if (mode == DragMode::GradientCenter || mode == DragMode::GradientFocal) setCursor(Qt::CrossCursor);
            else if (mode == DragMode::GradientStart || mode == DragMode::GradientEnd ||
                     mode == DragMode::GradientRadius) setCursor(Qt::SizeAllCursor);
            else setCursor(active_tool_ == CanvasTool::Gradient ? Qt::CrossCursor : Qt::ArrowCursor);
        } else {
            setCursor(active_tool_ == CanvasTool::Gradient ? Qt::CrossCursor : Qt::ArrowCursor);
        }
        return;
    }

    clear_draw_tool_snap_cursor();

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



bool CanvasPreview::mime_has_external_canvas_content(const QMimeData *mime) const
{
    if (!mime)
        return false;
    if (mime->hasImage())
        return true;
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            if (!url.isLocalFile())
                continue;
            const QString path = url.toLocalFile();
            QImageReader reader(path);
            if (reader.canRead())
                return true;
        }
    }
    return mime->hasText() && !mime->text().trimmed().isEmpty();
}

bool CanvasPreview::handle_external_canvas_mime(const QMimeData *mime, const QPointF &canvas_pt)
{
    if (!mime || !title_)
        return false;

    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls()) {
            if (!url.isLocalFile())
                continue;
            const QString path = url.toLocalFile();
            QImageReader reader(path);
            if (!reader.canRead())
                continue;
            emit external_image_layer_requested(path, canvas_pt);
            return true;
        }
    }

    if (mime->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mime->imageData());
        if (!image.isNull()) {
            QString base_dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
            if (base_dir.isEmpty())
                base_dir = QDir::tempPath();
            QDir dir(base_dir + QStringLiteral("/obs-gsp-pasted-assets"));
            if (!dir.exists())
                dir.mkpath(QStringLiteral("."));
            const QString path = dir.filePath(QStringLiteral("pasted-image-%1.png")
                .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz"))));
            if (image.save(path, "PNG")) {
                emit external_image_layer_requested(path, canvas_pt);
                return true;
            }
        }
    }

    if (mime->hasText()) {
        const QString text = mime->text().trimmed();
        if (!text.isEmpty()) {
            emit external_text_layer_requested(text, canvas_pt);
            return true;
        }
    }

    return false;
}

void CanvasPreview::dragEnterEvent(QDragEnterEvent *ev)
{
    if (mime_has_external_canvas_content(ev ? ev->mimeData() : nullptr)) {
        ev->acceptProposedAction();
        return;
    }
    QWidget::dragEnterEvent(ev);
}

void CanvasPreview::dragMoveEvent(QDragMoveEvent *ev)
{
    if (mime_has_external_canvas_content(ev ? ev->mimeData() : nullptr)) {
        ev->acceptProposedAction();
        return;
    }
    QWidget::dragMoveEvent(ev);
}

void CanvasPreview::dropEvent(QDropEvent *ev)
{
    if (!ev) {
        QWidget::dropEvent(ev);
        return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF view_pt = ev->position();
#else
    const QPointF view_pt = ev->pos();
#endif
    if (handle_external_canvas_mime(ev->mimeData(), view_to_canvas(view_pt))) {
        ev->acceptProposedAction();
        setFocus(Qt::MouseFocusReason);
        return;
    }
    QWidget::dropEvent(ev);
}

void CanvasPreview::leaveEvent(QEvent *ev)
{
    bool needs_update = false;
    if (color_picker_tooltip_visible_) {
        color_picker_tooltip_visible_ = false;
        needs_update = true;
    }
    if (mouse_inside_canvas_ || !hovered_layer_id_.empty()) {
        mouse_inside_canvas_ = false;
        hovered_layer_id_.clear();
        invalidate_hover_overlay_cache();
        needs_update = true;
    }
    if (snap_cursor_visible_ || final_snap_cursor_visible_) {
        snap_cursor_visible_ = false;
        final_snap_cursor_visible_ = false;
        needs_update = true;
    }
    if (needs_update)
        update();
    QWidget::leaveEvent(ev);
}

void CanvasPreview::keyPressEvent(QKeyEvent *ev)
{
    if (ev && inline_text_layer_id_.empty() && ev->matches(QKeySequence::Paste)) {
        const QPointF paste_pt = view_to_canvas(QPointF(width() * 0.5, height() * 0.5));
        if (handle_external_canvas_mime(QApplication::clipboard() ? QApplication::clipboard()->mimeData() : nullptr, paste_pt)) {
            ev->accept();
            return;
        }
    }

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
        snap_cursor_visible_ = false;
        final_snap_cursor_visible_ = false;
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
                   (active_tool_ == CanvasTool::ColorPicker || active_tool_ == CanvasTool::Gradient ? Qt::CrossCursor : Qt::ArrowCursor)));
        ev->accept();
        return;
    }

    if (ev->button() != Qt::LeftButton || drag_mode_ == DragMode::None) return;

    if (gradient_tool_dragging_) {
        apply_gradient_drag(ev->pos(), ev->modifiers());
        gradient_tool_dragging_ = false;
        gradient_drag_ = GradientDragState{};
        drag_mode_ = DragMode::None;
        emit layer_geometry_changed();
        ev->accept();
        return;
    }

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
    QAction *delete_guide = menu.addAction(obsgs_tr("OBSTitles.DeleteGuide"));
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
    if (!title_) {
        ev->ignore();
        return;
    }

    QPointF wheel_delta = ev->pixelDelta();
    if (wheel_delta.isNull()) {
        constexpr double kWheelPanStepPx = 80.0;
        wheel_delta = QPointF(ev->angleDelta()) / 120.0 * kWheelPanStepPx;
    }

    const Qt::KeyboardModifiers mods = ev->modifiers();
    if (mods.testFlag(Qt::AltModifier)) {
        const double zoom_delta = !qFuzzyIsNull(wheel_delta.y()) ? wheel_delta.y() : wheel_delta.x();
        if (qFuzzyIsNull(zoom_delta)) {
            ev->accept();
            return;
        }

        const QPointF anchor_canvas = view_to_canvas(ev->position());
        const double factor = std::pow(1.1, zoom_delta / 80.0);
        const int next = (int)std::round((double)zoom_percent_ * factor);
        zoom_percent_ = std::clamp(next, 5, 1600);
        fit_zoom_active_ = false;
        const QPointF origin_without_pan = centered_view_origin();
        const double scale = view_scale();
        pan_offset_ = ev->position() - origin_without_pan -
                      QPointF(anchor_canvas.x() * scale, anchor_canvas.y() * scale);
        invalidate_canvas_overlay_caches();
        emit zoom_percent_changed(zoom_percent_);
    } else if (mods.testFlag(Qt::ControlModifier)) {
        const double horizontal_delta = !qFuzzyIsNull(wheel_delta.y()) ? wheel_delta.y() : wheel_delta.x();
        pan_offset_.rx() += horizontal_delta;
        fit_zoom_active_ = false;
        invalidate_canvas_overlay_caches();
    } else {
        const double vertical_delta = !qFuzzyIsNull(wheel_delta.y()) ? wheel_delta.y() : wheel_delta.x();
        pan_offset_.ry() += vertical_delta;
        fit_zoom_active_ = false;
        invalidate_canvas_overlay_caches();
    }

    position_text_editor();
    update();
    ev->accept();
}

void CanvasPreview::resizeEvent(QResizeEvent *)
{
    dirty_ = true;
    invalidate_canvas_overlay_caches();
    if (fit_zoom_active_) fit_canvas(fit_zoom_up_to_100_);
    position_text_editor();
}

/* ══════════════════════════════════════════════════════════════════
 *  LayerStack
 * ══════════════════════════════════════════════════════════════════ */
