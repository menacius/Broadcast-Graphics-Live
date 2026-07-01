namespace {
constexpr int kCanvasRulerThickness = 24;
constexpr double kCanvasGuideHitTolerancePx = 5.0;
constexpr double kPenBezierActivationDistancePx = 5.0;
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
    if (label == bgl_tr("OBSTitles.ObjectEdge") ||
        label == bgl_tr("OBSTitles.ObjectCenter") ||
        label == bgl_tr("OBSTitles.Spacing")) {
        return TitlePreferences::CanvasHelperColorRole::ObjectSnapLines;
    }
    return TitlePreferences::CanvasHelperColorRole::CanvasSnapLines;
}

bool layer_is_shape_sized(const Layer &layer)
{
    return layer.type == LayerType::Shape || layer.type == LayerType::SolidRect;
}

bool layer_has_fixed_canvas_geometry(const Layer &)
{
    // Adjustment and color layers start at canvas size, but remain ordinary
    // transformable layers. Their initial geometry is a default, not a lock.
    return false;
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

std::vector<double> capture_path_corner_radii(const Layer &layer)
{
    std::vector<double> radii;
    radii.reserve(layer.path_points.size());
    for (const auto &point : layer.path_points)
        radii.push_back(point.corner_radius);
    return radii;
}

void apply_shape_resize_metrics_from_state(Layer &layer, double old_w, double old_h,
                                           float start_stroke_width,
                                           float start_corner_radius_tl,
                                           float start_corner_radius_tr,
                                           float start_corner_radius_br,
                                           float start_corner_radius_bl,
                                           float start_shape_roundness,
                                           float start_shape_inner_roundness,
                                           const std::vector<double> &start_path_corner_radii,
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
        layer.shape_roundness = (float)std::clamp((double)start_shape_roundness * factor, 0.0, 9999.0);
        layer.shape_inner_roundness = (float)std::clamp((double)start_shape_inner_roundness * factor, 0.0, 9999.0);
        if (start_path_corner_radii.size() == layer.path_points.size()) {
            for (size_t i = 0; i < layer.path_points.size(); ++i)
                layer.path_points[i].corner_radius = std::clamp(start_path_corner_radii[i] * factor, 0.0, 9999.0);
        }
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

struct CanvasPreview::GpuDisplayState {
    gs_swapchain_t *swapchain = nullptr;
    gs_texture_t *underlay = nullptr;
    gs_texture_t *overlay = nullptr;
    gs_texture_t *selection_mask = nullptr;
    uint32_t underlay_w = 0;
    uint32_t underlay_h = 0;
    uint32_t overlay_w = 0;
    uint32_t overlay_h = 0;
    uint32_t selection_mask_w = 0;
    uint32_t selection_mask_h = 0;
    uint32_t requested_w = 0;
    uint32_t requested_h = 0;
    uint32_t swapchain_w = 0;
    uint32_t swapchain_h = 0;
    QRect artwork_rect_px;
    bool enabled = true;
    bool update_color_space = false;
    bool destroying = false;
    std::mutex mutex;
};

static bool canvas_qt_to_gs_window(QWindow *window, gs_window &gswindow)
{
    if (!window)
        return false;
#ifdef _WIN32
    gswindow.hwnd = reinterpret_cast<HWND>(window->winId());
    return gswindow.hwnd != nullptr;
#elif defined(__APPLE__)
    gswindow.view = (id)window->winId();
    return gswindow.view != nullptr;
#else
    switch (obs_get_nix_platform()) {
    case OBS_NIX_PLATFORM_X11_EGL:
        gswindow.id = window->winId();
        gswindow.display = obs_get_nix_platform_display();
        return gswindow.display != nullptr;
#ifdef ENABLE_WAYLAND
    case OBS_NIX_PLATFORM_WAYLAND:
#if QT_VERSION < QT_VERSION_CHECK(6, 9, 0)
#ifdef OBS_BGS_HAS_QPA_NATIVE_INTERFACE
        if (QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface())
            gswindow.display = native->nativeResourceForWindow("surface", window);
#else
        return false;
#endif
#else
        gswindow.display = reinterpret_cast<void *>(window->winId());
#endif
        return gswindow.display != nullptr;
#endif
    default:
        return false;
    }
#endif
}

static QImage gpu_upload_image(const QImage &source)
{
    if (source.isNull())
        return QImage();
    QImage image = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (!image.isNull() && image.bytesPerLine() != image.width() * 4)
        image = image.copy();
    return image;
}

CanvasPreview::CanvasPreview(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(400, 225);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_StaticContents, true);
    setAttribute(Qt::WA_DontCreateNativeAncestors, true);
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    gpu_display_ = std::make_unique<GpuDisplayState>();
    gpu_render_session_ = title_gpu_render_session_create();
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
        if (!shutting_down_ && dirty_ && isVisible())
            update();
    });

    /* obs_display_t is tied to OBS' project-rate main render loop. The editor
     * needs an independently paced presentation path, so it owns a public
     * libobs graphics swap chain and presents it from the Qt/monitor scheduler.
     * Playback still only queues presents from project-rate transport ticks. */
    present_coalesce_timer_ = new QTimer(this);
    present_coalesce_timer_->setSingleShot(true);
    present_coalesce_timer_->setTimerType(Qt::PreciseTimer);
    connect(present_coalesce_timer_, &QTimer::timeout, this, [this]() {
        if (!shutting_down_ && isVisible())
            update();
    });
    last_render_clock_.start();

    QSettings adaptive_settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
    adaptive_settings.beginGroup(QStringLiteral("titleEditor"));
    adaptive_rendering_enabled_ = adaptive_settings.value(
        QString::fromUtf8(kEditorAdaptiveRenderingKey), false).toBool();
    const int saved_quality = adaptive_settings.value(
        QString::fromUtf8(kEditorAdaptiveQualityKey), 0).toInt();
    adaptive_quality_mode_ = static_cast<AdaptiveQualityMode>(std::clamp(saved_quality, 0, 5));
    adaptive_settings.endGroup();

    adaptive_full_quality_timer_ = new QTimer(this);
    adaptive_full_quality_timer_->setSingleShot(true);
    adaptive_full_quality_timer_->setInterval(140);
    connect(adaptive_full_quality_timer_, &QTimer::timeout, this, [this]() {
        if (shutting_down_)
            return;
        if (!adaptive_interaction_active_)
            return;
        /* Never refine while a canvas manipulation is still active. The old
         * timer expired 140 ms after mouse-down, which silently returned long
         * drags to full resolution and made Adaptive Rendering appear broken. */
        if (drag_mode_ != DragMode::None) {
            adaptive_full_quality_timer_->start();
            return;
        }
        const bool was_reduced_quality = frame_image_preview_scale_ < 0.999;
        end_adaptive_interaction();
        if (was_reduced_quality) {
            dirty_ = true;
            update();
        }
    });

    inline_text_editor_ = new QTextEdit(this);
    inline_text_editor_->hide();
    inline_text_editor_->setAcceptRichText(true);
    inline_text_editor_->setTextInteractionFlags(Qt::TextEditorInteraction);
    inline_text_editor_->setFrameShape(QFrame::NoFrame);
    inline_text_editor_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    inline_text_editor_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    inline_text_editor_->setLineWrapMode(QTextEdit::FixedPixelWidth);
    inline_text_editor_->setCursorWidth(0);
    inline_text_editor_->setContentsMargins(0, 0, 0, 0);
    inline_text_editor_->viewport()->setContentsMargins(0, 0, 0, 0);
    inline_text_editor_->setAttribute(Qt::WA_TranslucentBackground, true);
    inline_text_editor_->viewport()->setAttribute(Qt::WA_TranslucentBackground, true);
    QPalette inline_palette = inline_text_editor_->palette();
    /* The editor itself stays transparent so the canvas remains the visual
     * source of truth. Selection inversion is applied later in display space;
     * clear every palette group so an inactive/focus-changing QTextEdit cannot
     * reintroduce a native white selection rectangle on some Qt styles. */
    const QColor transparent_selection(0, 0, 0, 0);
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive,
                                        QPalette::Disabled}) {
        inline_palette.setColor(group, QPalette::Highlight, transparent_selection);
        inline_palette.setColor(group, QPalette::HighlightedText, transparent_selection);
    }
    inline_text_editor_->setPalette(inline_palette);
    inline_text_editor_->setStyleSheet(
        "QTextEdit{background:transparent;border:0px;padding:0px;"
        "color:rgba(255,255,255,0);"
        "selection-background-color:rgba(0,0,0,0);"
        "selection-color:rgba(0,0,0,0);}");
    inline_text_editor_->installEventFilter(this);
    inline_text_refresh_timer_ = new QTimer(this);
    inline_text_refresh_timer_->setSingleShot(true);
    inline_text_refresh_timer_->setInterval(0);
    connect(inline_text_refresh_timer_, &QTimer::timeout, this, [this]() {
        if (shutting_down_)
            return;
        const bool mark_dirty = inline_text_refresh_mark_dirty_;
        const bool emit_changed = inline_text_refresh_emit_changed_;
        inline_text_refresh_mark_dirty_ = false;
        inline_text_refresh_emit_changed_ = false;
        refresh_inline_text_edit(mark_dirty, emit_changed);
    });
    connect(inline_text_editor_->document(), &QTextDocument::contentsChange,
            this, [this](int position, int chars_removed, int chars_added) {
        if (updating_inline_text_editor_ || refreshing_inline_text_) return;
        if (inline_text_change_count_ == 0) {
            inline_text_change_position_ = position;
            inline_text_change_removed_ = chars_removed;
            inline_text_change_added_ = chars_added;
        }
        ++inline_text_change_count_;
        begin_adaptive_interaction();
        schedule_inline_text_refresh(true, true);
    });
    if (auto *layout = inline_text_editor_->document()->documentLayout()) {
        connect(layout, &QAbstractTextDocumentLayout::documentSizeChanged, this, [this](const QSizeF &) {
            if (updating_inline_text_editor_ || refreshing_inline_text_) return;
            auto layer = title_ && !inline_text_layer_id_.empty()
                ? title_->find_layer(inline_text_layer_id_) : nullptr;
            if (!layer || (!layer->text_box_width_to_text &&
                           !layer->text_box_height_to_text))
                return;
            /* The final unconstrained size may arrive after the text signal.
             * Coalesce it with the same edit instead of rendering twice. */
            schedule_inline_text_refresh(true, true);
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
        /* contentsChange already queued one canonical model update. Avoid
         * processing the same keystroke again from cursorPositionChanged, and
         * do not rebuild every properties panel before the coalesced text
         * transaction has even been applied.  A true cursor/selection move is
         * still published immediately. */
        const bool content_change_pending = inline_text_change_count_ != 0;
        if (!content_change_pending)
            sync_inline_text_layer(false);
        invalidate_canvas_overlay_caches();
        if (inline_text_editor_) {
            inline_text_editor_->viewport()->update();
            update(inline_text_editor_->geometry().adjusted(-4, -4, 4, 4));
        }
        if (!content_change_pending)
            emit text_edit_cursor_changed(layer_id);
    };
    connect(inline_text_editor_, &QTextEdit::cursorPositionChanged, this, emit_cursor_changed);
    connect(inline_text_editor_, &QTextEdit::selectionChanged, this, emit_cursor_changed);
}

CanvasPreview::~CanvasPreview()
{
    prepare_for_shutdown();
}

void CanvasPreview::prepare_for_shutdown()
{
    if (shutting_down_)
        return;
    shutting_down_ = true;

    setUpdatesEnabled(false);
    if (render_coalesce_timer_)
        render_coalesce_timer_->stop();
    if (present_coalesce_timer_)
        present_coalesce_timer_->stop();
    if (adaptive_full_quality_timer_)
        adaptive_full_quality_timer_->stop();
    if (inline_text_refresh_timer_)
        inline_text_refresh_timer_->stop();
    blockSignals(true);

    drag_mode_ = DragMode::None;
    active_extension_handle_ = -1;
    extension_canvas_handles_ = QJsonArray();
    playback_frame_pending_ = false;
    playback_present_pending_ = false;
    editing_present_pending_ = false;
    transport_playback_active_ = false;
    force_present_pending_ = false;
    dirty_ = false;

    if (title_)
        bgs::ticker_runtime::set_adaptive_pause(title_->id, false);
    destroy_gpu_display();
    if (gpu_render_session_) {
        title_gpu_render_session_destroy(gpu_render_session_);
        gpu_render_session_ = nullptr;
    }
    title_.reset();
    frame_image_ = QImage();
    prefetched_cache_frame_ = QImage();
    gpu_overlay_cache_ = QImage();
    editor_quality_cache_.clear();
}

QPaintEngine *CanvasPreview::paintEngine() const
{
    if (gpu_display_ && gpu_display_->swapchain)
        return nullptr;
    return QWidget::paintEngine();
}

bool CanvasPreview::ensure_gpu_display()
{
    if (shutting_down_)
        return false;
    if (!gpu_display_)
        gpu_display_ = std::make_unique<GpuDisplayState>();
    if (gpu_display_->swapchain)
        return true;
    if (gpu_display_->destroying || !isVisible())
        return false;

    (void)winId();
    QWindow *window = windowHandle();
    if (!window || !window->isExposed())
        return false;

    const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());
    gs_init_data info = {};
    info.cx = static_cast<uint32_t>(std::max(1, qRound(width() * dpr)));
    info.cy = static_cast<uint32_t>(std::max(1, qRound(height() * dpr)));
    info.format = GS_BGRA;
    info.zsformat = GS_ZS_NONE;
    if (!canvas_qt_to_gs_window(window, info.window))
        return false;

    obs_enter_graphics();
    gpu_display_->swapchain = gs_swapchain_create(&info);
    obs_leave_graphics();
    if (!gpu_display_->swapchain)
        return false;
    gpu_display_->requested_w = info.cx;
    gpu_display_->requested_h = info.cy;
    gpu_display_->swapchain_w = info.cx;
    gpu_display_->swapchain_h = info.cy;
    gpu_display_->enabled = true;
    gpu_display_->update_color_space = true;
    force_present_pending_ = true;
    return true;
}

void CanvasPreview::destroy_gpu_display()
{
    if (!gpu_display_)
        return;
    gpu_display_->destroying = true;
    if (present_coalesce_timer_)
        present_coalesce_timer_->stop();

    const bool has_graphics_resources = gpu_display_->swapchain ||
        gpu_display_->underlay || gpu_display_->overlay ||
        gpu_display_->selection_mask;
    if (has_graphics_resources) {
        obs_enter_graphics();
        {
            std::lock_guard<std::mutex> lock(gpu_display_->mutex);
            if (gpu_display_->underlay)
                gs_texture_destroy(gpu_display_->underlay);
            if (gpu_display_->overlay)
                gs_texture_destroy(gpu_display_->overlay);
            if (gpu_display_->selection_mask)
                gs_texture_destroy(gpu_display_->selection_mask);
            gpu_display_->underlay = nullptr;
            gpu_display_->overlay = nullptr;
            gpu_display_->selection_mask = nullptr;
            if (gpu_display_->swapchain)
                gs_swapchain_destroy(gpu_display_->swapchain);
            gpu_display_->swapchain = nullptr;
        }
        obs_leave_graphics();
    }
}

void CanvasPreview::update_gpu_display_textures(const QImage *underlay_source,
                                                const QImage *overlay_source,
                                                const QImage *selection_mask_source)
{
    if (shutting_down_ || !gpu_display_ || !gpu_display_->swapchain)
        return;

    const QImage underlay = underlay_source ? gpu_upload_image(*underlay_source) : QImage();
    const QImage overlay = overlay_source ? gpu_upload_image(*overlay_source) : QImage();
    const QImage selection_mask = selection_mask_source
        ? gpu_upload_image(*selection_mask_source)
        : QImage();
    const qreal dpr = std::max<qreal>(1.0, devicePixelRatioF());

    QRect artwork_rect;
    if (title_) {
        const double scale = view_scale();
        const QPointF origin = view_origin();
        artwork_rect = QRect(qRound(origin.x() * dpr), qRound(origin.y() * dpr),
                             qRound(title_->width * scale * dpr),
                             qRound(title_->height * scale * dpr));
    }

    auto upload_texture = [](gs_texture_t *&texture, uint32_t &stored_w,
                             uint32_t &stored_h, const QImage &image) {
        if (image.isNull()) {
            if (texture)
                gs_texture_destroy(texture);
            texture = nullptr;
            stored_w = stored_h = 0;
            return;
        }
        const uint32_t w = static_cast<uint32_t>(image.width());
        const uint32_t h = static_cast<uint32_t>(image.height());
        if (!texture || stored_w != w || stored_h != h) {
            if (texture)
                gs_texture_destroy(texture);
            const uint8_t *data[1] = {image.constBits()};
            texture = gs_texture_create(w, h, GS_BGRA, 1, data, GS_DYNAMIC);
            stored_w = texture ? w : 0;
            stored_h = texture ? h : 0;
        } else {
            gs_texture_set_image(texture, image.constBits(),
                                 static_cast<uint32_t>(image.bytesPerLine()), false);
        }
    };

    obs_enter_graphics();
    {
        std::lock_guard<std::mutex> lock(gpu_display_->mutex);
        if (underlay_source) {
            upload_texture(gpu_display_->underlay, gpu_display_->underlay_w,
                           gpu_display_->underlay_h, underlay);
        }
        if (overlay_source) {
            upload_texture(gpu_display_->overlay, gpu_display_->overlay_w,
                           gpu_display_->overlay_h, overlay);
        }
        if (selection_mask_source) {
            upload_texture(gpu_display_->selection_mask,
                           gpu_display_->selection_mask_w,
                           gpu_display_->selection_mask_h, selection_mask);
        }
        gpu_display_->artwork_rect_px = artwork_rect;
    }
    obs_leave_graphics();
}

bool CanvasPreview::present_gpu_display_if_due()
{
    if (shutting_down_ || !gpu_display_ || !gpu_display_->swapchain ||
        !gpu_display_->enabled || !isVisible())
        return false;

    /* While transport is active, generic repaints may not create extra
     * frames. A project tick or an explicit scrub/edit request must authorize
     * the present; the latter remains monitor-capped. */
    if (transport_playback_active_ && !playback_present_pending_ &&
        !editing_present_pending_)
        return false;

    /* An explicit scrub/edit request remains monitor-paced even if transport
     * was still armed when the pointer interaction began. Only a real
     * transport tick receives the project-rate bypass. */
    const bool project_rate_present = transport_playback_active_ &&
        playback_present_pending_ && !editing_present_pending_;
    const int cadence_ms = project_rate_present ? 0 : display_refresh_interval_ms_;
    const qint64 elapsed_ms = last_present_clock_.isValid()
        ? last_present_clock_.elapsed() : cadence_ms;
    if (!force_present_pending_ && elapsed_ms < cadence_ms) {
        if (present_coalesce_timer_ && !present_coalesce_timer_->isActive())
            present_coalesce_timer_->start(
                std::max(1, cadence_ms - static_cast<int>(elapsed_ms)));
        return false;
    }

    const uint32_t cx = std::max<uint32_t>(1, gpu_display_->requested_w);
    const uint32_t cy = std::max<uint32_t>(1, gpu_display_->requested_h);
    bool presented = false;

    /* Measure presentation cadence from the start of the present attempt.
     * Some graphics backends may block briefly inside gs_present(); counting
     * that time prevents an accidental second monitor-interval delay. */
    last_present_clock_.restart();
    obs_enter_graphics();
    gs_load_swapchain(gpu_display_->swapchain);
    if (gpu_display_->swapchain_w != cx || gpu_display_->swapchain_h != cy) {
        gs_resize(cx, cy);
        gpu_display_->swapchain_w = cx;
        gpu_display_->swapchain_h = cy;
    }
    if (gpu_display_->update_color_space)
        gs_update_color_space();
    gpu_display_->update_color_space = false;

    if (gs_is_present_ready()) {
        gs_begin_scene();
        struct vec4 clear_color;
        vec4_set(&clear_color, 0.0f, 0.0f, 0.0f, 1.0f);
        gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH | GS_CLEAR_STENCIL,
                 &clear_color, 1.0f, 0);
        gs_enable_depth_test(false);
        gs_set_cull_mode(GS_NEITHER);
        gs_ortho(0.0f, static_cast<float>(cx), 0.0f,
                 static_cast<float>(cy), -100.0f, 100.0f);
        gs_set_viewport(0, 0, cx, cy);
        gpu_display_draw(this, cx, cy);
        gs_end_scene();
        gs_present();
        presented = true;
    }
    obs_leave_graphics();

    if (presented) {
        playback_present_pending_ = false;
        editing_present_pending_ = false;
        force_present_pending_ = false;
    }
    return presented;
}

void CanvasPreview::gpu_display_draw(void *data, uint32_t cx, uint32_t cy)
{
    auto *canvas = static_cast<CanvasPreview *>(data);
    if (!canvas || canvas->shutting_down_ || !canvas->gpu_display_)
        return;
    auto &state = *canvas->gpu_display_;
    std::lock_guard<std::mutex> lock(state.mutex);

    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    if (!effect)
        return;
    gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
    if (!image)
        return;

    auto draw_texture = [&](gs_texture_t *texture, const QRect &rect) {
        if (!texture || rect.isEmpty())
            return;
        gs_effect_set_texture(image, texture);
        gs_matrix_push();
        gs_matrix_identity();
        gs_matrix_translate3f(static_cast<float>(rect.x()),
                              static_cast<float>(rect.y()), 0.0f);
        while (gs_effect_loop(effect, "Draw"))
            gs_draw_sprite(texture, 0, static_cast<uint32_t>(rect.width()),
                           static_cast<uint32_t>(rect.height()));
        gs_matrix_pop();
    };

    gs_viewport_push();
    gs_projection_push();
    gs_blend_state_push();
    gs_ortho(0.0f, static_cast<float>(cx), 0.0f, static_cast<float>(cy),
             -100.0f, 100.0f);
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    draw_texture(state.underlay, QRect(0, 0, static_cast<int>(cx), static_cast<int>(cy)));
    if (canvas->gpu_render_session_ && !state.artwork_rect_px.isEmpty()) {
        gs_matrix_push();
        gs_matrix_identity();
        gs_matrix_translate3f(static_cast<float>(state.artwork_rect_px.x()),
                              static_cast<float>(state.artwork_rect_px.y()), 0.0f);
        const bool destination_composite = canvas->title_ &&
            title_requires_destination_compositing(*canvas->title_);
        const bool artwork_drawn = destination_composite && state.underlay
            ? title_gpu_render_session_draw_over_background_rect(
                  canvas->gpu_render_session_, state.underlay,
                  state.underlay_w, state.underlay_h,
                  static_cast<float>(state.artwork_rect_px.x()),
                  static_cast<float>(state.artwork_rect_px.y()),
                  static_cast<float>(state.artwork_rect_px.width()),
                  static_cast<float>(state.artwork_rect_px.height()),
                  static_cast<uint32_t>(state.artwork_rect_px.width()),
                  static_cast<uint32_t>(state.artwork_rect_px.height()),
                  false)
            : title_gpu_render_session_draw(
                  canvas->gpu_render_session_,
                  static_cast<uint32_t>(state.artwork_rect_px.width()),
                  static_cast<uint32_t>(state.artwork_rect_px.height()));
        const std::string gpu_error = artwork_drawn
            ? std::string()
            : title_gpu_render_session_last_error(canvas->gpu_render_session_);
        if (!artwork_drawn) {
            BGL_LOG_DEBUG("Canvas", QStringLiteral(
                "GPU canvas draw deferred title=%1 output=%2x%3 error=%4")
                .arg(canvas->title_
                         ? QString::fromStdString(canvas->title_->id)
                         : QStringLiteral("<none>"))
                .arg(state.artwork_rect_px.width())
                .arg(state.artwork_rect_px.height())
                .arg(gpu_error.empty() ? QStringLiteral("unknown")
                                       : QString::fromStdString(gpu_error)));
            /* A permanent GPU-text backend failure changes the next model
             * preparation path to the compatibility raster. The GPU presentation path
             * itself must not mutate the Qt/editor model, so queue exactly one
             * recovery snapshot on the widget thread. This also retries transient
             * first-frame atlas failures without requiring a user edit. */
            bool expected = false;
            if (canvas->gpu_recovery_queued_.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                QMetaObject::invokeMethod(canvas, [canvas]() {
                    canvas->gpu_recovery_queued_.store(
                        false, std::memory_order_release);
                    if (!canvas->title_ || !canvas->gpu_render_session_)
                        return;
                    canvas->gpu_model_dirty_ = true;
                    canvas->dirty_ = true;
                    canvas->update();
                }, Qt::QueuedConnection);
            }
        }
        if (TitlePreferences::cache_playback_logging_enabled()) {
            BGL_LOG_INFO("CachePlayback", QStringLiteral(
                "consumer=editor-canvas stage=draw result=%1 output=%2x%3 rect=%4,%5,%6x%7 error=%8")
                .arg(artwork_drawn ? QStringLiteral("ok") : QStringLiteral("failed"))
                .arg(state.artwork_rect_px.width()).arg(state.artwork_rect_px.height())
                .arg(state.artwork_rect_px.x()).arg(state.artwork_rect_px.y())
                .arg(state.artwork_rect_px.width()).arg(state.artwork_rect_px.height())
                .arg(gpu_error.empty() ? QStringLiteral("none")
                                       : QString::fromStdString(gpu_error)));
        }
        gs_matrix_pop();
    }
    if (state.selection_mask) {
        /* A white texel with inverse-destination/inverse-source blending
         * produces 1 - destination; a transparent black texel leaves the
         * destination untouched. Keep alpha unchanged so this is a true
         * display-space inversion rather than a white alpha overlay. */
        gs_blend_function_separate(GS_BLEND_INVDSTCOLOR, GS_BLEND_INVSRCCOLOR,
                                   GS_BLEND_ZERO, GS_BLEND_ONE);
        draw_texture(state.selection_mask,
                     QRect(0, 0, static_cast<int>(cx), static_cast<int>(cy)));
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
    }
    draw_texture(state.overlay, QRect(0, 0, static_cast<int>(cx), static_cast<int>(cy)));

    gs_blend_state_pop();
    gs_projection_pop();
    gs_viewport_pop();
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
    (void)format;
    (void)mask;
    if (!inline_text_editor_ || inline_text_layer_id_.empty() || inline_text_layer_id_ != layer_id)
        return;
    auto layer = title_ ? title_->find_layer(layer_id) : nullptr;
    if (!layer)
        return;

    /* The Properties panel has already updated the canonical RichTextDocument.
     * Rebuild the transparent Qt editing adapter from that model instead of
     * incrementally merging QTextCharFormat fragments. This preserves sparse
     * masks, mixed styles/colors and independent H/V scale. */
    const QTextCursor saved_cursor = inline_text_editor_->textCursor();
    const int anchor = saved_cursor.anchor();
    const int position = saved_cursor.position();
    const double visual_scale = inline_text_visual_scale(*layer);
    const bool previous_updating = updating_inline_text_editor_;
    updating_inline_text_editor_ = true;
    {
        QSignalBlocker blocker(inline_text_editor_);
        rich_text_document_ensure_canonical(*layer);
        const double local_time = std::max(0.0, playhead_ - layer->in_time);
        const RichTextDocument editor_model =
            rich_text_document_for_editor_time(*layer, local_time);
        populate_qtext_document_from_rich_text(inline_text_editor_->document(),
                                               editor_model, visual_scale);
        QTextCursor restored(inline_text_editor_->document());
        const int text_len = inline_text_editor_->toPlainText().size();
        restored.setPosition(std::clamp(anchor, 0, text_len));
        restored.setPosition(std::clamp(position, 0, text_len), QTextCursor::KeepAnchor);
        inline_text_editor_->setTextCursor(restored);
        if (!restored.hasSelection()) {
            const RichTextCharFormat effective_typing =
                rich_text_effective_typing_format(layer->rich_text);
            QTextCharFormat typing = qtext_format_from_rich_text_format(
                effective_typing, visual_scale);
            apply_editor_rich_text_baseline_delta(
                typing, effective_typing, layer->rich_text.default_format.baseline_shift,
                visual_scale);
            typing.setProperty(RichTextPropManualMask,
                               (uint)(layer->rich_text.typing_format_mask & RichTextCharAll));
            typing.setProperty(RichTextPropAutoGenerated, false);
            inline_text_editor_->setCurrentCharFormat(typing);
        }
        inline_text_editor_->document()->setModified(false);
    }
    inline_text_last_visual_scale_ = visual_scale;
    updating_inline_text_editor_ = previous_updating;
    invalidate_canvas_overlay_caches();
    dirty_ = true;
    gpu_model_dirty_ = true;
    position_text_editor();
    update();
    inline_text_editor_->viewport()->update();
    update(inline_text_editor_->geometry().adjusted(-4, -4, 4, 4));
}


QPointF CanvasPreview::view_center_canvas_point() const
{
    return view_to_canvas(QPointF(width() * 0.5, height() * 0.5));
}

void CanvasPreview::set_title(std::shared_ptr<Title> t, bool preserve_view)
{
    if (shutting_down_)
        return;
    commit_text_edit(true);
    if (title_)
        bgs::ticker_runtime::set_adaptive_pause(title_->id, false);
    title_ = t;
    selected_position_keyframe_index_ = -1;
    motion_path_hover_type_ = MotionPathHoverType::None;
    motion_path_hover_keyframe_index_ = -1;
    spatial_tangent_drag_ = SpatialTangentDragState{};
    spatial_vertex_drag_ = SpatialVertexDragState{};
    BGL_LOG_INFO("Canvas", QStringLiteral(
        "Set canvas title=%1 preserveView=%2")
        .arg(title_ ? QString::fromStdString(title_->id) : QStringLiteral("<none>"))
        .arg(preserve_view ? 1 : 0));
    gpu_model_dirty_ = true;
    playback_frame_pending_ = false;
    prefetched_cache_frame_ = QImage();
    prefetched_cache_time_ = -1.0;
    last_runtime_clock_second_ = std::numeric_limits<qint64>::min();
    editor_quality_cache_.clear();
    invalidate_canvas_overlay_caches();
    dirty_ = true;
    adaptive_interaction_active_ = false;
    force_live_full_quality_render_ = false;
    frame_image_preview_scale_ = 1.0;
    last_full_quality_render_cost_ms_ = 0;

    /* Prime the immutable GPU model immediately when a title is opened. The
     * GPU swap chain can request its first presentation before QWidget's
     * deferred paint scheduler reaches render_to_frame(). Session update does
     * no GPU draw/upload here; it only prepares pending layer resources.
     *
     * Do not mark the model clean after this early snapshot. open_title() still
     * reflects the selected layer into the properties/effects panels after
     * set_title() returns, and that setup can finalize rich-text state without
     * constituting a user edit. A queued refresh below runs after that editor
     * initialization cycle and guarantees that the first normal presentation tick
     * receives the final text model even when caching is disabled. */
    const std::shared_ptr<Title> primed_title = title_;
    if (primed_title && gpu_render_session_) {
        ++gpu_model_revision_;
        title_gpu_render_session_update(gpu_render_session_, *primed_title,
                                        playhead_, gpu_model_revision_);
    }
    QTimer::singleShot(0, this, [this, primed_title]() {
        if (shutting_down_ || !primed_title || title_ != primed_title)
            return;
        gpu_model_dirty_ = true;
        dirty_ = true;
        update();
    });
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

void CanvasPreview::set_playhead(double t, bool playback_frame)
{
    if (shutting_down_)
        return;
    if (std::abs(playhead_ - t) < 1e-9) {
        /* A scrub may land on the currently displayed frame while a queued
         * transport repaint is still pending. Reclassify it as editing even
         * though the numeric playhead value did not change. */
        if (!playback_frame) {
            playback_frame_pending_ = false;
            playback_present_pending_ = false;
            editing_present_pending_ = true;
            update();
        } else {
            editing_present_pending_ = false;
            playback_present_pending_ = true;
            update();
        }
        return;
    }
    const double old_playhead = playhead_;
    playhead_ = t;
    if (canvas_overlay_changes_with_playhead(old_playhead, playhead_))
        invalidate_canvas_overlay_caches();
    /* Only real transport ticks bypass editor refresh pacing. Timeline
     * scrubbing is always editing and therefore remains capped by the monitor
     * refresh rate, independent of the surrounding transport state. */
    playback_frame_pending_ = playback_frame;
    playback_present_pending_ = playback_frame;
    editing_present_pending_ = !playback_frame;
    dirty_ = true;
    position_text_editor();
    update();
}

void CanvasPreview::set_display_refresh_rate(double hz)
{
    if (!std::isfinite(hz) || hz < 1.0)
        hz = 60.0;
    /* Round upward so the integer timer interval can never schedule editing
     * frames faster than the monitor's reported refresh rate. */
    display_refresh_interval_ms_ = std::max(
        1, static_cast<int>(std::ceil(1000.0 / std::clamp(hz, 1.0, 1000.0))));
    if (present_coalesce_timer_ && present_coalesce_timer_->isActive()) {
        present_coalesce_timer_->stop();
        update();
    }
}

void CanvasPreview::set_playback_active(bool active)
{
    if (transport_playback_active_ == active)
        return;
    transport_playback_active_ = active;
    if (active) {
        playback_frame_pending_ = false;
        playback_present_pending_ = false;
        editing_present_pending_ = false;
        if (present_coalesce_timer_)
            present_coalesce_timer_->stop();
    } else {
        playback_frame_pending_ = false;
        playback_present_pending_ = false;
        editing_present_pending_ = true;
        force_present_pending_ = true;
        update();
    }
}

void CanvasPreview::refresh_runtime_dynamic_content()
{
    if (!title_)
        return;

    bool has_ticker = false;
    bool has_clock = false;
    bool has_independent_asset = false;
    for (const auto &layer : title_->layers) {
        if (!layer)
            continue;
        has_ticker = has_ticker || layer->type == LayerType::Ticker;
        has_clock = has_clock || layer->type == LayerType::Clock;
        has_independent_asset = has_independent_asset ||
            (layer->type == LayerType::Asset && layer->asset_animated &&
             layer->asset_playback_mode == 1);
    }
    if (!has_ticker && !has_clock && !has_independent_asset)
        return;

    const qint64 clock_second = QDateTime::currentSecsSinceEpoch();
    if (!has_ticker && !has_independent_asset &&
        clock_second == last_runtime_clock_second_)
        return;
    last_runtime_clock_second_ = clock_second;
    dirty_ = true;
    update();
}

void CanvasPreview::set_selected_layer(const std::string &lid)
{
    const bool changed = sel_layer_id_ != lid;
    sel_layer_id_ = lid;
    selected_layer_ids_.clear();
    if (!lid.empty()) selected_layer_ids_.push_back(lid);
    if (changed) {
        clear_path_point_selection();
        clear_corner_selection();
        selected_position_keyframe_index_ = -1;
        motion_path_hover_type_ = MotionPathHoverType::None;
        motion_path_hover_keyframe_index_ = -1;
    }
    invalidate_canvas_overlay_caches();
    position_text_editor();
    update();
}

void CanvasPreview::set_selected_layers(const std::vector<std::string> &ids)
{
    const std::string next = ids.empty() ? std::string() : ids.back();
    const bool changed = sel_layer_id_ != next || selected_layer_ids_ != ids;
    selected_layer_ids_ = ids;
    sel_layer_id_ = next;
    if (changed) {
        clear_path_point_selection();
        clear_corner_selection();
        selected_position_keyframe_index_ = -1;
        motion_path_hover_type_ = MotionPathHoverType::None;
        motion_path_hover_keyframe_index_ = -1;
    }
    invalidate_canvas_overlay_caches();
    position_text_editor();
    update();
}

void CanvasPreview::set_safe_guides_visible(bool visible)
{
    safe_guides_visible_ = visible;
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kEditorLayoutSettingsGroup));
    settings.setValue(QString::fromUtf8(kEditorSafeGuidesVisibleKey), visible);
    settings.endGroup();
    settings.sync();
    invalidate_canvas_overlay_caches();
    update();
}


void CanvasPreview::load_ruler_guide_settings()
{
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
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
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
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
    invalidate_canvas_overlay_caches();
    update();
}

void CanvasPreview::set_guides_visible(bool visible)
{
    if (guides_visible_ == visible) return;
    guides_visible_ = visible;
    save_ruler_guide_settings();
    invalidate_canvas_overlay_caches();
    update();
}

void CanvasPreview::set_guides_locked(bool locked)
{
    if (guides_locked_ == locked) return;
    guides_locked_ = locked;
    save_ruler_guide_settings();
    invalidate_canvas_overlay_caches();
    update();
}

void CanvasPreview::set_show_guide_coordinates(bool visible)
{
    if (show_guide_coordinates_ == visible) return;
    show_guide_coordinates_ = visible;
    save_ruler_guide_settings();
    invalidate_canvas_overlay_caches();
    update();
}

void CanvasPreview::set_canvas_border_visible(bool visible)
{
    if (canvas_border_visible_ == visible) return;
    canvas_border_visible_ = visible;
    save_ruler_guide_settings();
    invalidate_canvas_overlay_caches();
    update();
}

void CanvasPreview::clear_user_guides()
{
    if (vertical_guides_.empty() && horizontal_guides_.empty()) return;
    vertical_guides_.clear();
    horizontal_guides_.clear();
    clear_snap_feedback();
    save_ruler_guide_settings();
    invalidate_canvas_overlay_caches();
    update();
}

bool CanvasPreview::prepare_cached_playback_frame(double time)
{
    prefetched_cache_frame_ = QImage();
    prefetched_cache_time_ = -1.0;
    if (!title_)
        return false;

    CacheManager &cache = CacheManager::instance();
    const TitleCacheability cacheability = cache.titleCacheability(title_);
    if (adaptive_interaction_active_ || !cache.cacheEnabled() ||
        cacheability == TitleCacheability::NonCacheable)
        return true;

    const int cue_row = title_->current_cue_row;
    const bool has_active_cue = cue_row >= 0 &&
        cue_row < static_cast<int>(title_->live_text_rows.size());

    /* Phase 14 prerenders normally live only in the GPU RAM tier. The old
     * readiness gate checked QImage RAM/disk exclusively, so “Play after
     * rendering” could wait forever while the exact frame was already ready
     * and directly presentable on the GPU. */
    const QString gpu_token = has_active_cue
        ? cache.requestLiveCueFrameGpuToken(title_, cue_row, time, true)
        : cache.requestFrameGpuToken(title_, time, true);
    if (!gpu_token.isEmpty())
        return true;

    QImage cached = has_active_cue
        ? cache.requestLiveCueFrame(title_, cue_row, time, true)
        : cache.requestFrame(title_, time, true);
    if (cached.isNull())
        return false;

    prefetched_cache_frame_ = cached;
    prefetched_cache_time_ = time;
    return true;
}

void CanvasPreview::refresh_preview()
{
    gpu_model_dirty_ = true;
    prefetched_cache_frame_ = QImage();
    prefetched_cache_time_ = -1.0;
    // Model edits invalidate only the editor-local reduced-quality cache. The
    // full-quality OBS/prerender cache retains its own independent lifecycle.
    editor_quality_cache_.clear();
    invalidate_canvas_overlay_caches();
    dirty_ = true;
    position_text_editor();
    /* Inline edits and canvas manipulation are presented by the display-rate
     * scheduler; never force a synchronous artwork render from an input event. */
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
    frame_image_ = QImage();
    frame_image_canvas_offset_ = QPoint();
    frame_image_canvas_size_ = title_ ? QSize(title_->width, title_->height) : QSize();
    frame_image_preview_scale_ = 1.0;
    if (title_ && gpu_render_session_) {
        ++gpu_model_revision_;
        title_gpu_render_session_update(gpu_render_session_, *title_, playhead_,
                                        gpu_model_revision_);
        gpu_model_dirty_ = false;
    }
    invalidate_canvas_overlay_caches();
    dirty_ = false;
    playback_frame_pending_ = false;
    update();
}

QImage CanvasPreview::current_rendered_frame() const
{
    /* Explicit inspection/export path only. Normal editor presentation never
     * maps the GPU frame back to system memory. */
    return gpu_render_session_
        ? title_gpu_render_session_readback(gpu_render_session_)
        : QImage();
}


void CanvasPreview::set_adaptive_rendering_enabled(bool enabled)
{
    if (adaptive_rendering_enabled_ == enabled)
        return;
    if (!enabled && title_)
        bgs::ticker_runtime::set_adaptive_pause(title_->id, false);
    adaptive_rendering_enabled_ = enabled;
    adaptive_interaction_active_ = false;
    editor_quality_cache_.clear();
    if (adaptive_full_quality_timer_)
        adaptive_full_quality_timer_->stop();
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
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
    if (title_)
        bgs::ticker_runtime::set_adaptive_pause(title_->id, false);
    adaptive_quality_mode_ = mode;
    adaptive_interaction_active_ = false;
    force_live_full_quality_render_ = false;
    if (adaptive_full_quality_timer_)
        adaptive_full_quality_timer_->stop();
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
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

