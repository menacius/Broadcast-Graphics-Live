namespace {

using bgs::live_text::exposed_text_layers;
using bgs::live_text::live_cue_layer_value;
using bgs::live_text::normalize_live_text_rows;

constexpr int TemplateCategoryNameRole = Qt::UserRole + 1;
constexpr int TemplateFilterCategoryRole = Qt::UserRole + 20;
constexpr int TemplateFilterSubcategoryRole = Qt::UserRole + 21;
constexpr int TemplateFilterCollectionRole = Qt::UserRole + 22;
constexpr int TemplateFilterDepthRole = Qt::UserRole + 23;
constexpr const char *kDockSettingsGroup = "TitleDock";
constexpr const char *kDockSplitterStateKey = "sectionSplitterState";
constexpr const char *kTemplateIconViewKey = "templateIconView";
constexpr const char *kVisibilityFilterKey = "visibilityFilterActive";
constexpr const char *kLastSelectedTitleIdKey = "lastSelectedTitleId";
constexpr const char *kTitleSourceId = "broadcast_graphics_live_source";
constexpr const char *kPlaylistLoopKey = "playlistLoop";
constexpr const char *kPlaylistReverseKey = "playlistReverse";
constexpr const char *kPlaylistHoldSecondsKey = "playlistHoldSeconds";
constexpr const char *kBackgroundPersistenceKey = "backgroundPersistence";
constexpr const char *kTextPersistenceKey = "textPersistence";
constexpr const char *kLiveTextLinesPerRowKey = "liveTextLinesPerRow";
constexpr const char *kTemplateLibrarySettingsGroup = "TemplateLibrary";
constexpr const char *kTemplateLibraryByCollectionKey = "viewByCollection";
constexpr const char *kTemplateLibraryIconViewKey = "iconView";
constexpr const char *kTemplateLibraryGeometryKey = "geometry";
constexpr const char *kTemplateLibrarySplitterKey = "splitterState";
constexpr int kMinLiveTextLinesPerRow = 1;
constexpr int kMaxLiveTextLinesPerRow = 8;
constexpr int kTitleListIconExtent = 16;
constexpr int kTitleIconViewThumbWidth = 144;
constexpr int kTitleIconViewThumbHeight = 82;
constexpr int kTitleIconViewItemWidth = 172;
constexpr int kTitleIconViewItemHeight = 126;
constexpr int kTitleIconViewTextLines = 2;
constexpr int kTitlePlaylistActiveRole = Qt::UserRole + 20;
constexpr int kTitleCueVisualStateRole = Qt::UserRole + 21;
constexpr int kTitleListThumbnailRole = Qt::UserRole + 22;
constexpr int kTitleShowStatusIconRole = Qt::UserRole + 23;
constexpr int kLiveCueColumn = 0;
constexpr int kLiveCacheColumn = 1;
constexpr int kLiveSelectColumn = 2;
constexpr int kLiveFirstValueColumn = 3;
constexpr int kStaticCueColumn = 0;
constexpr int kStaticCacheColumn = 1;
constexpr int kStaticTitleColumn = 2;

static QColor live_cue_state_color(bool current, bool queued, bool ending);

enum class TitleCueVisualState : int {
    Inactive = 0,
    Queued,
    Cued,
    Ending,
};

static TitleCueVisualState title_cue_visual_state(const Title &title,
                                                  bool waiting_for_prerender = false)
{
    /* Outro/uncue has visual priority over every other cue condition. During
     * a cue-to-cue hand-off both current and pending rows can exist, but the
     * thumbnail must remain yellow until the outgoing row actually finishes. */
    if (title.current_cue_row >= 0 &&
        (title.cue_uncue_requested || title.pending_cue_row >= 0))
        return TitleCueVisualState::Ending;
    if (title.current_cue_row >= 0)
        return TitleCueVisualState::Cued;
    if (title.pending_cue_row >= 0 || waiting_for_prerender)
        return TitleCueVisualState::Queued;
    return TitleCueVisualState::Inactive;
}

static QColor title_cue_visual_color(TitleCueVisualState state)
{
    switch (state) {
    case TitleCueVisualState::Ending:
        return live_cue_state_color(false, false, true);
    case TitleCueVisualState::Cued:
        return live_cue_state_color(true, false, false);
    case TitleCueVisualState::Queued:
        return live_cue_state_color(false, true, false);
    case TitleCueVisualState::Inactive:
    default:
        return QColor();
    }
}

static void append_unique_diagnostic(std::vector<std::string> &items, const QString &value)
{
    const std::string utf8 = value.toStdString();
    if (utf8.empty())
        return;
    if (std::find(items.begin(), items.end(), utf8) == items.end())
        items.push_back(utf8);
}

static bool imported_font_family_available(const QString &family, const QStringList &available_families)
{
    const QString candidate = family.trimmed();
    if (candidate.isEmpty())
        return true;
    return std::any_of(available_families.begin(), available_families.end(),
                       [&candidate](const QString &installed) {
                           return installed.compare(candidate, Qt::CaseInsensitive) == 0;
                       });
}

static void complete_import_diagnostics(const Title &title, TitleImportDiagnostics &diagnostics)
{
    const QStringList available_families = QFontDatabase().families();
    QSet<QString> checked_fonts;
    QSet<QString> checked_effects;
    auto &extension_catalog = BglEffectExtensionCatalog::instance();
    if (extension_catalog.effects().empty())
        extension_catalog.reload();

    auto inspect_font = [&](const std::string &font_family, const std::string &layer_name) {
        const QString family = QString::fromStdString(font_family).trimmed();
        if (family.isEmpty())
            return;
        const QString key = family.toCaseFolded();
        if (checked_fonts.contains(key))
            return;
        checked_fonts.insert(key);
        if (!imported_font_family_available(family, available_families)) {
            append_unique_diagnostic(
                diagnostics.missing_fonts,
                QStringLiteral("%1: %2")
                    .arg(QString::fromStdString(layer_name), family));
        }
    };

    for (const auto &layer : title.layers) {
        if (!layer)
            continue;

        if (layer->type == LayerType::Image && !layer->image_path.empty() &&
            !QFileInfo::exists(QString::fromStdString(layer->image_path))) {
            append_unique_diagnostic(
                diagnostics.missing_images,
                QStringLiteral("%1: %2")
                    .arg(QString::fromStdString(layer->name),
                         QString::fromStdString(layer->image_path)));
        }

        if (layer->type == LayerType::Text || layer->type == LayerType::Clock ||
            layer->type == LayerType::Ticker) {
            inspect_font(layer->font_family, layer->name);
            inspect_font(layer->rich_text.default_format.font_family, layer->name);
            if (layer->rich_text.has_typing_format &&
                (layer->rich_text.typing_format_mask & RichTextCharFontFamily)) {
                inspect_font(layer->rich_text.typing_format.font_family, layer->name);
            }
            for (const auto &range : layer->rich_text.ranges) {
                if (range.mask & RichTextCharFontFamily)
                    inspect_font(range.format.font_family, layer->name);
            }
            if (!layer->rich_text.auto_default_style_preset_id.empty())
                inspect_font(layer->rich_text.auto_default_style_cached_format.font_family, layer->name);
            for (const auto &rule : layer->rich_text.auto_style_rules) {
                if (rule.cached_mask & RichTextCharFontFamily)
                    inspect_font(rule.cached_format.font_family, layer->name);
            }
        }

        for (const LayerEffect &effect : layer->effects) {
            const QString stableId = effect.extension_id.empty()
                ? BglEffectExtensionCatalog::builtInId(effect.type)
                : QString::fromStdString(effect.extension_id);
            const QString diagnosticKey = stableId.isEmpty()
                ? QStringLiteral("legacy:%1").arg(static_cast<int>(effect.type))
                : stableId;
            if (checked_effects.contains(diagnosticKey))
                continue;
            checked_effects.insert(diagnosticKey);

            const auto *extension = stableId.isEmpty()
                ? nullptr : extension_catalog.find(stableId);
            if (!extension || !extension->builtIn) {
                if (!extension || extension->shaderPath.isEmpty() ||
                    !QFileInfo(extension->shaderPath).isReadable()) {
                    append_unique_diagnostic(
                        diagnostics.missing_effects,
                        QStringLiteral("%1: %2")
                            .arg(QString::fromStdString(layer->name), stableId));
                }
                continue;
            }

            const TitleEffectDefinition *definition =
                TitleEffectRegistry::definition(extension->builtInType);
            if (!definition) {
                append_unique_diagnostic(
                    diagnostics.missing_effects,
                    QStringLiteral("%1: effect type %2")
                        .arg(QString::fromStdString(layer->name))
                        .arg(static_cast<int>(effect.type)));
                continue;
            }

            bool available = definition->has_embedded_fallback;
            char *resolved_path = obs_module_file(definition->relative_path);
            if (resolved_path) {
                available = available || QFileInfo(QString::fromUtf8(resolved_path)).isFile();
                bfree(resolved_path);
            }
            if (!available) {
                append_unique_diagnostic(
                    diagnostics.missing_effects,
                    QStringLiteral("%1: %2")
                        .arg(QString::fromStdString(layer->name),
                             QString::fromUtf8(definition->display_name)));
            }
        }
    }
}

static QString import_diagnostics_text(const QString &title_name, const TitleImportDiagnostics &diagnostics)
{
    QStringList sections;
    sections.push_back(bgl_tr("OBSTitles.ImportMissingResourcesTitleFormat").arg(title_name));

    auto append_section = [&sections](const QString &heading, const std::vector<std::string> &items) {
        if (items.empty())
            return;
        sections.push_back(heading);
        for (const std::string &item : items)
            sections.push_back(QStringLiteral("  • %1").arg(QString::fromStdString(item)));
    };
    append_section(bgl_tr("OBSTitles.MissingEffectsHeading"), diagnostics.missing_effects);
    append_section(bgl_tr("OBSTitles.MissingImagesHeading"), diagnostics.missing_images);
    append_section(bgl_tr("OBSTitles.MissingFontsHeading"), diagnostics.missing_fonts);
    return sections.join(QLatin1Char('\n'));
}

static void show_import_diagnostics(QWidget *parent, const QStringList &diagnostic_blocks)
{
    if (diagnostic_blocks.isEmpty())
        return;

    const QString details = diagnostic_blocks.join(QStringLiteral("\n\n"));
    QMessageBox dialog(parent);
    dialog.setIcon(QMessageBox::Warning);
    dialog.setWindowTitle(bgl_tr("OBSTitles.ImportMissingResources"));
    dialog.setText(bgl_tr("OBSTitles.ImportMissingResourcesMessage"));
    dialog.setInformativeText(details.left(1800));
    if (details.size() > 1800)
        dialog.setDetailedText(details);
    dialog.setStandardButtons(QMessageBox::Ok);
    dialog.exec();
}


static bool layer_uses_image_cue_value(const std::shared_ptr<Layer> &layer)
{
    return layer && layer->type == LayerType::Image;
}

static void apply_live_cue_layer_value(const std::shared_ptr<Layer> &target, const std::string &value)
{
    if (!target)
        return;
    target->live_cue_hidden_if_empty = target->exposed_hide_if_empty && value.empty();
    if (target->type == LayerType::Image) {
        bgs::apply_exposed_image_cue_value(*target, value);
        return;
    }

    target->text_content = value;
    if (target->rich_text.empty())
        target->rich_text = rich_text_document_from_layer_defaults(*target);
    RichTextCharFormat insertion_format =
        rich_text_effective_typing_format(target->rich_text);
    rich_text_document_replace_text(target->rich_text, target->text_content, insertion_format,
                                        target->rich_text.has_typing_format
                                            ? target->rich_text.typing_format_mask : 0);
}

static QString live_cue_image_file_name(const QString &path)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty())
        return QString();
    const QFileInfo info(trimmed);
    const QString file_name = info.fileName();
    return file_name.isEmpty() ? trimmed : file_name;
}

static QString current_scene_collection_titles_label()
{
    char *collection_name = obs_frontend_get_current_scene_collection();
    QString name = QString::fromUtf8(collection_name ? collection_name : "").trimmed();
    bfree(collection_name);

    if (name.isEmpty())
        name = bgl_tr("OBSTitles.SceneCollectionFallback");

    return bgl_tr("OBSTitles.SceneCollectionTitlesAndGraphicsFormat").arg(name);
}

static QString live_text_layer_header(const std::shared_ptr<Layer> &layer)
{
    if (!layer) return bgl_tr("OBSTitles.Text");
    QString name = QString::fromStdString(layer->name).trimmed();
    if (!name.isEmpty()) return name;
    if (layer_uses_image_cue_value(layer)) {
        name = live_cue_image_file_name(QString::fromStdString(layer->image_path)).trimmed();
        return name.isEmpty() ? bgl_tr("OBSTitles.Image") : name;
    }
    name = QString::fromStdString(layer->text_content).trimmed();
    return name.isEmpty() ? bgl_tr("OBSTitles.Text") : name;
}

static QString live_cue_cache_text(FrameCacheState state)
{
    switch (state) {
    case FrameCacheState::Queued: return bgl_tr("OBSTitles.Queued");
    case FrameCacheState::Rendering: return bgl_tr("OBSTitles.Caching");
    case FrameCacheState::CachedRam: return bgl_tr("OBSTitles.ReadyRam");
    case FrameCacheState::CachedDisk: return bgl_tr("OBSTitles.ReadyDisk");
    case FrameCacheState::Stale: return bgl_tr("OBSTitles.CacheErrorOrStale");
    case FrameCacheState::Disabled: return bgl_tr("OBSTitles.CachingDisabled");
    case FrameCacheState::NotCached:
    default: return bgl_tr("OBSTitles.NotCached");
    }
}

static QString live_cue_cache_tooltip(FrameCacheState state)
{
    switch (state) {
    case FrameCacheState::Queued:
        return bgl_tr("OBSTitles.LiveCueQueuedTooltip");
    case FrameCacheState::Rendering:
        return bgl_tr("OBSTitles.LiveCueCachingTooltip");
    case FrameCacheState::CachedRam:
        return bgl_tr("OBSTitles.LiveCueCachedRamTooltip");
    case FrameCacheState::CachedDisk:
        return bgl_tr("OBSTitles.LiveCueCachedDiskTooltip");
    case FrameCacheState::Stale:
        return bgl_tr("OBSTitles.LiveCueStaleTooltip");
    case FrameCacheState::Disabled:
        return bgl_tr("OBSTitles.LiveCueCacheDisabledTooltip");
    case FrameCacheState::NotCached:
    default:
        return bgl_tr("OBSTitles.LiveCueNotCachedTooltip");
    }
}

static QColor live_cue_cache_color(FrameCacheState state)
{
    switch (state) {
    /* Queued and Rendering are a single visual progress phase.  Giving them
     * different tints made the same bucket flicker orange/yellow every time
     * ownership moved between the queue and worker.  The four SVG shapes, not
     * the scheduler's transient state, now communicate prerender progress. */
    case FrameCacheState::Queued:
    case FrameCacheState::Rendering: return QColor(255, 202, 74);
    case FrameCacheState::CachedRam: return QColor(39, 186, 103);
    case FrameCacheState::CachedDisk: return QColor(74, 144, 226);
    case FrameCacheState::Stale: return QColor(214, 90, 90);
    case FrameCacheState::Disabled: return QColor(130, 130, 130);
    case FrameCacheState::NotCached:
    default: return QColor(130, 130, 130);
    }
}

static const char *live_cue_cache_progress_icon_name(int progress_percent)
{
    const int progress = std::clamp(progress_percent, 0, 100);
    if (progress >= 100)
        return "cache-ok.svg";
    if (progress >= 75)
        return "cache-progress-75-100.svg";
    if (progress >= 50)
        return "cache-progress-50-75.svg";
    if (progress >= 25)
        return "cache-progress-25-50.svg";
    return "cache-queued-0-25.svg";
}

static const char *live_cue_cache_icon_name(FrameCacheState state, int progress_percent)
{
    switch (state) {
    case FrameCacheState::Queued:
    case FrameCacheState::Rendering:
        return live_cue_cache_progress_icon_name(progress_percent);
    case FrameCacheState::CachedRam:
    case FrameCacheState::CachedDisk:
    case FrameCacheState::Disabled:
        return "cache-ok.svg";
    case FrameCacheState::Stale:
        return "cache-error.svg";
    case FrameCacheState::NotCached:
    default:
        return "cache-queued-0-25.svg";
    }
}

static QIcon obs_icon(const char *file_name, const QColor &color);
static QIcon title_list_status_icon(bool has_exposed_text, bool has_scene_mask);

struct LiveCueAggregateCacheStatus {
    FrameCacheState state = FrameCacheState::NotCached;
    int progress_percent = 0;
    int ready_rows = 0;
    int total_rows = 0;
};

static LiveCueAggregateCacheStatus aggregate_live_cue_cache_status(const std::shared_ptr<Title> &title)
{
    LiveCueAggregateCacheStatus status;
    if (!title || title->live_text_rows.empty()) {
        status.state = CacheManager::instance().cacheEnabled()
            ? FrameCacheState::NotCached
            : FrameCacheState::Disabled;
        return status;
    }

    status.total_rows = static_cast<int>(title->live_text_rows.size());
    if (!CacheManager::instance().cacheEnabled()) {
        status.state = FrameCacheState::Disabled;
        return status;
    }

    for (int row = 0; row < status.total_rows; ++row) {
        const FrameCacheState row_state = CacheManager::instance().liveCueState(title, row);
        if (row_state == FrameCacheState::CachedRam || row_state == FrameCacheState::CachedDisk)
            ++status.ready_rows;
    }

    /* Aggregate progress includes steady row frames and the independent
     * Background Persistence/outro transition-pair states. Row icons remain
     * stable when a new row is added; only the title-level badge reflects the
     * newly required transition work. */
    status.progress_percent = CacheManager::instance().liveCueAggregateProgressPercent(title);
    status.state = CacheManager::instance().liveCueAggregateState(title);
    return status;
}


static QString aggregate_live_cue_cache_tooltip(const LiveCueAggregateCacheStatus &status)
{
    if (status.total_rows <= 0)
        return bgl_tr("OBSTitles.LiveCueNotCachedTooltip");
    return QStringLiteral("%1 (%2%, %3/%4 cues)")
        .arg(live_cue_cache_text(status.state))
        .arg(status.progress_percent)
        .arg(status.ready_rows)
        .arg(status.total_rows);
}

static QIcon title_list_combined_status_icon(bool has_exposed_text, bool has_scene_mask,
                                             const LiveCueAggregateCacheStatus &cache_status,
                                             bool show_cache)
{
    constexpr int gap = 3;
    const int width = show_cache ? kTitleListIconExtent * 2 + gap : kTitleListIconExtent;
    QPixmap pixmap(width, kTitleListIconExtent);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    const QPixmap type_pixmap = title_list_status_icon(has_exposed_text, has_scene_mask).pixmap(kTitleListIconExtent, kTitleListIconExtent);
    const QPixmap cache_pixmap = obs_icon(live_cue_cache_icon_name(cache_status.state, cache_status.progress_percent),
                                          live_cue_cache_color(cache_status.state)).pixmap(kTitleListIconExtent, kTitleListIconExtent);
    painter.drawPixmap(0, 0, type_pixmap);
    if (show_cache)
        painter.drawPixmap(kTitleListIconExtent + gap, 0, cache_pixmap);
    painter.end();
    return QIcon(pixmap);
}

static QToolButton *centered_cell_tool_button(QTableWidget *table, int row, int col, const char *object_name)
{
    if (!table || row < 0 || row >= table->rowCount() || col < 0 || col >= table->columnCount())
        return nullptr;

    if (auto *cell = table->cellWidget(row, col)) {
        if (auto *button = cell->findChild<QToolButton *>(QString::fromUtf8(object_name)))
            return button;
        if (auto *button = qobject_cast<QToolButton *>(cell))
            return button;
    }

    auto *holder = new QWidget(table);
    auto *layout = new QHBoxLayout(holder);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->setAlignment(Qt::AlignCenter);

    auto *button = new QToolButton(holder);
    button->setObjectName(QString::fromUtf8(object_name));
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIconSize(QSize(18, 18));
    button->setFixedSize(28, 24);
    layout->addWidget(button, 0, Qt::AlignCenter);

    table->setCellWidget(row, col, holder);
    return button;
}

static QToolButton *live_cue_cache_button(QTableWidget *table, int row)
{
    if (!table)
        return nullptr;
    const int col = table->columnCount() <= 3 ? kStaticCacheColumn : kLiveCacheColumn;
    return centered_cell_tool_button(table, row, col, "liveCueCacheButton");
}

static QToolButton *live_cue_button(QTableWidget *table, int row, int col)
{
    auto *button = centered_cell_tool_button(table, row, col, "liveCueButton");
    if (button) {
        button->setIconSize(QSize(20, 20));
        /* The cue control owns the complete table cell, while the icon remains
         * centered by QToolButton.  Clicking whitespace in the cue column now
         * triggers the same action as clicking the icon itself. */
        button->setMinimumSize(0, 0);
        button->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        if (auto *layout = button->parentWidget() ? button->parentWidget()->layout() : nullptr) {
            layout->setAlignment(Qt::Alignment{});
            layout->setAlignment(button, Qt::Alignment{});
        }
    }
    return button;
}

static QColor live_cue_state_color(bool current, bool queued, bool ending = false)
{
    /* Ending has priority over current: from the instant uncue is requested
     * until the outro reaches the authored end, the active cue is shown in
     * yellow rather than red. */
    if (ending)
        return QColor(255, 202, 74);
    if (current)
        return QColor(208, 48, 48);
    if (queued)
        return QColor(40, 184, 79);
    return QColor(150, 150, 150);
}

static QColor live_cue_select_cell_color(bool current, bool queued, bool ending = false)
{
    if (ending)
        return QColor(255, 202, 74, 88);
    if (current)
        return QColor(208, 48, 48, 72);
    if (queued)
        return QColor(40, 184, 79, 72);
    return QColor();
}

static QIcon obs_icon(const char *file_name)
{
    return bgl_icon(file_name);
}

static QIcon obs_icon(const char *file_name, const QColor &color)
{
    return bgl_icon(file_name, color);
}

static std::string obs_text_std(const char *key)
{
    return bgl_tr(key).toStdString();
}

static int obs_toolbar_icon_extent(QWidget *widget)
{
    int size = widget ? widget->style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, widget) : 0;
    return size > 0 ? size : 16;
}

static int obs_layout_spacing(QWidget *widget)
{
    int spacing = widget ? widget->style()->pixelMetric(QStyle::PM_LayoutVerticalSpacing, nullptr, widget) : -1;
    return spacing >= 0 ? spacing : 4;
}

static QToolBar *make_obs_dock_toolbar(QWidget *parent)
{
    auto *toolbar = new QToolBar(parent);
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setOrientation(Qt::Horizontal);
    toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    toolbar->setIconSize(QSize(obs_toolbar_icon_extent(parent), obs_toolbar_icon_extent(parent)));
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return toolbar;
}

static QToolButton *make_obs_dock_tool_button(QWidget *parent, const QString &text,
                                              const QIcon &icon, const QString &tooltip)
{
    auto *button = new QToolButton(parent);
    button->setText(text);
    button->setAccessibleName(text);
    button->setToolTip(tooltip);
    button->setIcon(icon);
    button->setIconSize(QSize(obs_toolbar_icon_extent(parent), obs_toolbar_icon_extent(parent)));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    return button;
}

static QString dock_highlight_button_style(bool hide_menu_indicator = false,
                                           bool include_persistence_state = false)
{
    QString style;
    if (include_persistence_state) {
        style += QStringLiteral(
            "QToolButton[persistenceState=\"background\"]{"
            "background:palette(midlight);color:palette(text);"
            "border:1px solid palette(highlight);border-radius:3px;}"
            "QToolButton[persistenceState=\"background\"]:hover{background:palette(mid);}");
    }
    style += QStringLiteral(
        "QToolButton:checked{"
        "background:palette(highlight);color:palette(highlighted-text);"
        "border:1px solid palette(highlight);border-radius:3px;}"
        "QToolButton:checked:hover{background:palette(highlight);color:palette(highlighted-text);}");
    if (hide_menu_indicator)
        style += QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}");
    return style;
}

static LongPressToolButton *make_obs_dock_long_press_tool_button(QWidget *parent, const QString &text,
                                                                 const QIcon &icon, const QString &tooltip)
{
    auto *button = new LongPressToolButton(parent);
    button->setText(text);
    button->setAccessibleName(text);
    button->setToolTip(tooltip);
    button->setIcon(icon);
    button->setIconSize(QSize(obs_toolbar_icon_extent(parent), obs_toolbar_icon_extent(parent)));
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    button->setPopupMode(QToolButton::DelayedPopup);
    return button;
}


static QWidget *toolbar_spacer(QWidget *parent)
{
    auto *spacer = new QWidget(parent);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return spacer;
}

static QIcon globe_status_icon(bool enabled)
{
    const QColor color = enabled ? QColor(38, 184, 79) : QColor(145, 145, 145);
    return obs_icon("globe.svg", color);
}

static QJsonArray live_text_rows_to_json(const std::vector<std::vector<std::string>> &rows)
{
    QJsonArray json_rows;
    for (const auto &row : rows) {
        QJsonArray json_row;
        for (const auto &cell : row)
            json_row.append(QString::fromStdString(cell));
        json_rows.append(json_row);
    }
    return json_rows;
}

static bool live_text_rows_from_json(const QJsonDocument &doc,
                                     std::vector<std::vector<std::string>> &rows,
                                     QString *error)
{
    QJsonArray json_rows;
    if (doc.isArray()) {
        json_rows = doc.array();
    } else if (doc.isObject()) {
        const QJsonObject root = doc.object();
        const QJsonValue rows_value = root.value(QStringLiteral("rows"));
        const QJsonValue live_rows_value = root.value(QStringLiteral("live_text_rows"));
        if (rows_value.isArray())
            json_rows = rows_value.toArray();
        else if (live_rows_value.isArray())
            json_rows = live_rows_value.toArray();
        else {
            if (error) *error = bgl_tr("OBSTitles.LiveCueJsonRowsError");
            return false;
        }
    } else {
        if (error) *error = bgl_tr("OBSTitles.LiveCueJsonInvalidError");
        return false;
    }

    rows.clear();
    rows.reserve(json_rows.size());
    for (const QJsonValue &row_value : json_rows) {
        if (!row_value.isArray()) {
            if (error) *error = bgl_tr("OBSTitles.LiveCueJsonRowArrayError");
            return false;
        }
        std::vector<std::string> row;
        const QJsonArray json_row = row_value.toArray();
        row.reserve(json_row.size());
        for (const QJsonValue &cell_value : json_row)
            row.push_back(cell_value.toVariant().toString().toStdString());
        rows.push_back(std::move(row));
    }
    return true;
}

static void set_bold_label(QLabel *label)
{
    if (!label) return;
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
}


class LiveTextCueField : public QPlainTextEdit {
public:
    explicit LiveTextCueField(const QString &text, QWidget *parent = nullptr)
        : QPlainTextEdit(parent)
    {
        setPlainText(text);
        update_tooltip(text);
        setLineWrapMode(QPlainTextEdit::WidgetWidth);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setTabChangesFocus(true);
        document()->setDocumentMargin(0.0);
        setFrameShape(QFrame::NoFrame);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        connect(this, &QPlainTextEdit::textChanged, this, [this]() {
            update_tooltip(toPlainText());
        });
    }

    void update_tooltip(const QString &text)
    {
        setToolTip(text);
        if (viewport())
            viewport()->setToolTip(text);
    }

    std::function<void(const QString &)> editing_finished;
    std::function<void(bool)> focus_changed;

protected:
    void focusInEvent(QFocusEvent *event) override
    {
        QPlainTextEdit::focusInEvent(event);
        if (focus_changed)
            focus_changed(true);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
            !event->modifiers().testFlag(Qt::ShiftModifier)) {
            if (editing_finished)
                editing_finished(toPlainText());
            focusNextChild();
            event->accept();
            return;
        }

        // A literal newline is intentionally available only through Shift+Enter.
        QPlainTextEdit::keyPressEvent(event);
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        QPlainTextEdit::focusOutEvent(event);
        if (editing_finished)
            editing_finished(toPlainText());
        if (focus_changed) {
            const auto callback = focus_changed;
            QTimer::singleShot(0, qApp, [callback]() { callback(false); });
        }
    }
};

class LiveImageCueField : public QWidget {
public:
    explicit LiveImageCueField(const QString &path, QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 2, 3, 2);
        layout->setSpacing(4);

        label_ = new QLabel(this);
        label_->setTextInteractionFlags(Qt::NoTextInteraction);
        label_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        label_->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);

        button_ = new QToolButton(this);
        button_->setAutoRaise(true);
        button_->setCursor(Qt::ArrowCursor);
        button_->setIcon(obs_icon("import.svg", palette().color(QPalette::ButtonText)));
        button_->setToolTip(bgl_tr("OBSTitles.Browse"));
        button_->setFixedSize(22, 22);

        layout->addWidget(label_, 1);
        layout->addWidget(button_, 0);

        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        label_->installEventFilter(this);
        button_->installEventFilter(this);
        set_path(path);
        connect(button_, &QToolButton::clicked, this, [this]() {
            const QString initial_dir = path_.isEmpty()
                ? QString()
                : QFileInfo(path_).absolutePath();
            const QString selected = QFileDialog::getOpenFileName(
                this, bgl_tr("OBSTitles.ChooseImage"), initial_dir,
                bgl_tr("OBSTitles.ImageFileFilter"));
            if (selected.isEmpty())
                return;
            set_path(selected);
            if (path_changed)
                path_changed(selected);
        });
    }

    QString path() const { return path_; }

    void set_read_only(bool read_only)
    {
        read_only_ = read_only;
        if (button_)
            button_->setVisible(!read_only_);
        setProperty("externalTableReadOnly", read_only_);
    }

    bool is_read_only() const { return read_only_; }

    void set_path(const QString &path)
    {
        path_ = path;
        const QString file_name = live_cue_image_file_name(path_);
        label_->setText(file_name.isEmpty() ? QStringLiteral("-") : file_name);
        preview_tooltip_ = path_.isEmpty() ? QString() : path_.toHtmlEscaped();
        preview_tooltip_dirty_ = true;
        setToolTip(preview_tooltip_);
        label_->setToolTip(preview_tooltip_);
    }

    std::function<void(const QString &)> path_changed;

protected:
    bool event(QEvent *event) override
    {
        if (event && event->type() == QEvent::ToolTip)
            return show_preview_tooltip(static_cast<QHelpEvent *>(event));
        return QWidget::event(event);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if ((watched == label_ || watched == button_) &&
            event && event->type() == QEvent::ToolTip)
            return show_preview_tooltip(static_cast<QHelpEvent *>(event));
        return QWidget::eventFilter(watched, event);
    }

private:
    bool show_preview_tooltip(QHelpEvent *event)
    {
        ensure_preview_tooltip();
        if (!event || preview_tooltip_.isEmpty())
            return false;
        QToolTip::showText(event->globalPos(), preview_tooltip_, this);
        return true;
    }

    void ensure_preview_tooltip()
    {
        if (!preview_tooltip_dirty_)
            return;
        preview_tooltip_dirty_ = false;

        const QFileInfo info(path_);
        if (!info.exists() || !info.isFile()) {
            preview_tooltip_ = path_.isEmpty() ? QString() : path_.toHtmlEscaped();
            setToolTip(preview_tooltip_);
            if (label_) label_->setToolTip(preview_tooltip_);
            return;
        }

        QImage image(path_);
        if (image.isNull()) {
            preview_tooltip_ = path_.toHtmlEscaped();
            setToolTip(preview_tooltip_);
            if (label_) label_->setToolTip(preview_tooltip_);
            return;
        }

