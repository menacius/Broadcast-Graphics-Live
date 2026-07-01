/* ══════════════════════════════════════════════════════════════════
 *  Constructor
 * ══════════════════════════════════════════════════════════════════ */
TitleDock::TitleDock(QWidget *parent)
    : QDockWidget(bgl_tr("OBSTitles.DockName"), parent)
{
    bgl_apply_brand_icon(this);
    setFeatures(QDockWidget::DockWidgetMovable |
                QDockWidget::DockWidgetFloatable);
    build_ui();

    // Accept external template file drops anywhere over the dock/template list.
    // QListWidget uses a viewport child for DnD, so installing the filter on
    // every child avoids lost drops when the cursor is over labels, toolbars,
    // empty list areas, or the viewport.
    setAcceptDrops(true);
    installEventFilter(this);
    const auto dock_drop_widgets = findChildren<QWidget *>();
    for (QWidget *w : dock_drop_widgets) {
        if (!w)
            continue;
        w->setAcceptDrops(true);
        w->installEventFilter(this);
    }

    /* React to external data changes.  Always marshal back to the dock's
     * Qt thread so background/source playback changes cannot touch widgets.
     */
    change_callback_id_ = TitleDataStore::instance().on_change([this]() {
        QTimer::singleShot(0, this, [this]() {
            if (!updating_exposed_text_)
                refresh();
        });
    });
    const QPointer<TitleDock> external_table_guard(this);
    external_data_callback_id_ = ExternalDataManager::instance().on_change(
        [external_table_guard](const ExternalDataUpdate &update) {
            if (!external_table_guard || !update.table_changed)
                return;
            ExternalDataLog::write_lazy(
                ExternalDataLogLevel::Debug, "Dock", [&]() {
                    std::ostringstream stream;
                    stream << "table update received source="
                           << ExternalDataLog::sanitize(update.source_id, 160)
                           << " table=" << ExternalDataLog::sanitize(update.table_path, 320)
                           << " sequence=" << update.sequence;
                    return stream.str();
                });
            QTimer::singleShot(0, external_table_guard,
                [external_table_guard, update]() {
                    if (!external_table_guard)
                        return;
                    bool selected_changed = false;
                    for (const auto &title : TitleDataStore::instance().titles()) {
                        if (!title || !title_uses_external_update(*title, update))
                            continue;
                        if (!synchronize_live_text_table_bindings(*title, update.source_id))
                            continue;
                        TitleDataStore::instance().touch_runtime_change();
                        CacheManager::instance().refreshLiveCueStructureAsync(title);
                        if (title->id == external_table_guard->selected_id())
                            selected_changed = true;
                    }
                    ExternalDataLog::write_lazy(
                        ExternalDataLogLevel::Debug, "Dock", [&]() {
                            return std::string("table update applied source=") +
                                   ExternalDataLog::sanitize(update.source_id, 160) +
                                   " selectedChanged=" + (selected_changed ? "1" : "0");
                        });
                    if (selected_changed &&
                        !external_table_guard->updating_exposed_text_)
                        external_table_guard->populate_exposed_text();
                });
        });

    install_obs_state_callbacks();
    populate_list();
    seen_store_revision_ = TitleDataStore::instance().revision();
    BGL_LOG_INFO("Dock", QStringLiteral(
        "Dock created titles=%1 revision=%2")
        .arg(static_cast<int>(TitleDataStore::instance().titles().size()))
        .arg(static_cast<qulonglong>(seen_store_revision_)));
    live_refresh_timer_ = new QTimer(this);
    live_refresh_timer_->setInterval(100);
    connect(live_refresh_timer_, &QTimer::timeout, this, [this]() {
        update_live_cue_timer_label();
        uint64_t revision = TitleDataStore::instance().revision();
        if (revision == seen_store_revision_ || updating_exposed_text_) return;
        seen_store_revision_ = revision;
        sync_playlist_runtime_state();
        /* Source-side cue completion is runtime-only. Refresh the lightweight
         * thumbnail cue role on every runtime revision so an Ending (yellow)
         * thumbnail returns to Inactive as soon as the outro clears
         * current_cue_row/cue_uncue_requested. Do not wait for a cache signal
         * or rebuild the preview pixmap. */
        refresh_title_list_cue_visual_states();

        bool playlist_active = false;
        for (const auto &candidate : TitleDataStore::instance().titles()) {
            if (candidate && candidate->playlist_active) {
                playlist_active = true;
                break;
            }
        }
        if (playlist_active) {
            /* Playlist cue changes are runtime-only revisions. Rebuilding the
             * complete table here destroys every cell editor and steals focus on
             * each step. Update only cue/cache decoration while any playlist runs. */
            auto selected_title = TitleDataStore::instance().get_title(selected_id());
            if (selected_title)
                update_live_text_runtime_status(selected_title);
            update_playlist_countdown_label();
            return;
        }
        populate_exposed_text();
    });
    live_refresh_timer_->start();
    connect(&CacheManager::instance(), &CacheManager::liveCueStateChanged, this,
            [this](const QString &title_id, int row) {
                if (title_id.isEmpty() && row < 0) {
                    QTimer::singleShot(0, this, [this]() {
                        populate_list();
                        populate_exposed_text();
                    });
                    return;
                }
                if (title_id != QString::fromStdString(selected_id()) || updating_exposed_text_) {
                    schedule_title_list_cache_icon_update(title_id);
                    return;
                }
                auto title = TitleDataStore::instance().get_title(selected_id());
                if (!title || !text_table_ || row < 0 || row >= text_table_->rowCount()) {
                    populate_exposed_text();
                    return;
                }
                update_live_text_cache_cell(title, row);
                schedule_title_list_cache_icon_update(title_id);
                if (cache_waiting_title_id_ == title_id && cache_waiting_cue_row_ == row &&
                    CacheManager::instance().prepareLiveCueForPlayback(title, row)) {
                    BGL_LOG_INFO("LiveCueUI", QStringLiteral("Armed cue row prerender complete; cueing title=%1 row=%2")
                                                .arg(title_id).arg(row));
                    cache_waiting_cue_row_ = -1;
                    cache_waiting_title_id_.clear();
                    cue_live_text_row_for_title(title, row, false, true);
                }
            });
    connect(&CacheManager::instance(), &CacheManager::cacheEnabledChanged, this,
            [this](bool) {
                if (!updating_exposed_text_) {
                    populate_list();
                    populate_exposed_text();
                }
            });
}


bool TitleDock::eventFilter(QObject *watched, QEvent *event)
{
    Q_UNUSED(watched);

    if (!event)
        return QObject::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::DragEnter: {
        auto *drag = static_cast<QDragEnterEvent *>(event);
        if (mime_has_title_templates(drag->mimeData())) {
            drag->setDropAction(Qt::CopyAction);
            drag->accept();
            return true;
        }
        break;
    }
    case QEvent::DragMove: {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        if (mime_has_title_templates(drag->mimeData())) {
            drag->setDropAction(Qt::CopyAction);
            drag->accept();
            return true;
        }
        break;
    }
    case QEvent::Drop: {
        auto *drop = static_cast<QDropEvent *>(event);
        const QStringList paths = title_template_paths_from_mime(drop->mimeData());
        if (!paths.isEmpty()) {
            drop->setDropAction(Qt::CopyAction);
            drop->accept();
            import_title_paths(paths);
            return true;
        }
        break;
    }
    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}

void TitleDock::update_live_text_cache_cell(const std::shared_ptr<Title> &title, int row)
{
    if (!title || !text_table_ || row < 0 || row >= text_table_->rowCount())
        return;

    update_live_text_select_cell_status(row);

    const FrameCacheState state = CacheManager::instance().liveCueState(title, row);
    auto *button = live_cue_cache_button(text_table_, row);
    if (!button)
        return;

    const int progress_percent = CacheManager::instance().liveCueProgressPercent(title, row);
    const QColor color = live_cue_cache_color(state);
    const char *icon_name = live_cue_cache_icon_name(state, progress_percent);
    const QString visual_key = QStringLiteral("%1:%2")
                                   .arg(QString::fromUtf8(icon_name))
                                   .arg(color.rgba());
    if (button->property("bglCacheVisualKey").toString() != visual_key) {
        BGL_LOG_DEBUG("LiveCueUI", QStringLiteral("Update cache cell title=%1 row=%2 state=%3 progress=%4 icon=%5")
                                         .arg(QString::fromStdString(title->id))
                                         .arg(row)
                                         .arg((int)state)
                                         .arg(progress_percent)
                                         .arg(QString::fromUtf8(icon_name)));
        button->setIcon(obs_icon(icon_name, color));
        button->setProperty("bglCacheVisualKey", visual_key);
    }
    const QString tooltip = QStringLiteral("%1\n%2")
                                .arg(state == FrameCacheState::Queued || state == FrameCacheState::Rendering
                                         ? QStringLiteral("%1 (%2%)").arg(live_cue_cache_text(state)).arg(progress_percent)
                                         : live_cue_cache_text(state),
                                     live_cue_cache_tooltip(state));
    if (button->toolTip() != tooltip)
        button->setToolTip(tooltip);
    button->setEnabled(true);
    button->setCursor(state == FrameCacheState::Disabled ? Qt::ArrowCursor : Qt::PointingHandCursor);
    button->setStyleSheet(QStringLiteral(
        "QToolButton{background:transparent;border:none;padding:0;}"
        "QToolButton:hover{background:transparent;border:none;}"));
    button->disconnect();
    connect(button, &QToolButton::clicked, this, [this, row]() {
        auto current_title = TitleDataStore::instance().get_title(selected_id());
        if (!current_title || row < 0 || row >= (int)current_title->live_text_rows.size())
            return;
        if (!CacheManager::instance().cacheEnabled())
            return;
        CacheManager::instance().cacheLiveCueNow(current_title, row);
        update_live_text_cache_cell(current_title, row);
    });
}

void TitleDock::update_live_text_cache_cells()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_)
        return;

    for (int row = 0; row < text_table_->rowCount(); ++row) {
        update_live_text_cache_cell(title, row);
        update_live_text_select_cell_status(row);
    }
}


void TitleDock::update_live_text_runtime_status_fast(const std::shared_ptr<Title> &title, int primary_row, int previous_row)
{
    if (!title || !text_table_)
        return;

    const QString title_id = QString::fromStdString(title->id);
    const int cue_col = kLiveCueColumn;

    auto update_row = [this, title, title_id, cue_col](int row) {
        if (row < 0 || !text_table_ || row >= text_table_->rowCount())
            return;
        update_live_text_select_cell_status(row);
        update_live_text_cache_cell(title, row);
        if (auto *cue = live_cue_button(text_table_, row, cue_col)) {
            const bool waiting_for_prerender = cache_waiting_title_id_ == title_id &&
                cache_waiting_cue_row_ == row;
            const bool ending = row == title->current_cue_row && title->cue_uncue_requested;
            cue->setIcon(obs_icon("cue.svg", live_cue_state_color(
                row == title->current_cue_row,
                row == title->pending_cue_row || waiting_for_prerender,
                ending)));
        }
    };

    update_row(primary_row);
    update_row(previous_row);
    if (title->pending_cue_row >= 0 && title->pending_cue_row != primary_row && title->pending_cue_row != previous_row)
        update_row(title->pending_cue_row);

    if (auto *cue_table = dynamic_cast<LiveTextCueTable *>(text_table_))
        cue_table->viewport()->update();
    text_table_->viewport()->update();

    update_live_cue_timer_label();
    update_titles_compact_rail();

    // The aggregate cache badge can be comparatively expensive on titles with many cue rows
    // because it queries every cue.  Defer it so row/program feedback stays immediate.
    schedule_title_list_cache_icon_update(title_id);
}


void TitleDock::update_live_text_runtime_status(const std::shared_ptr<Title> &title)
{
    if (!title || !text_table_)
        return;

    const QString title_id = QString::fromStdString(title->id);
    const int cue_col = kLiveCueColumn;

    for (int row = 0; row < text_table_->rowCount(); ++row) {
        update_live_text_select_cell_status(row);
        update_live_text_cache_cell(title, row);

        auto *cue = live_cue_button(text_table_, row, cue_col);
        if (!cue)
            continue;
        const bool waiting_for_prerender = cache_waiting_title_id_ == title_id &&
            cache_waiting_cue_row_ == row;
        const bool ending = row == title->current_cue_row && title->cue_uncue_requested;
        cue->setIcon(obs_icon("cue.svg", live_cue_state_color(
            row == title->current_cue_row,
            row == title->pending_cue_row || waiting_for_prerender,
            ending)));
    }

    schedule_title_list_cache_icon_update(title_id);

    if (auto *cue_table = dynamic_cast<LiveTextCueTable *>(text_table_))
        cue_table->viewport()->update();
    text_table_->viewport()->update();
    update_live_cue_timer_label();
    update_titles_compact_rail();
}

void TitleDock::refresh_title_list_cue_visual_states()
{
    if (!list_)
        return;

    bool visual_state_changed = false;
    for (int i = 0; i < list_->count(); ++i) {
        auto *item = list_->item(i);
        if (!item)
            continue;
        const QString title_id = item->data(Qt::UserRole).toString();
        auto title = TitleDataStore::instance().get_title(title_id.toStdString());
        if (!title)
            continue;
        const bool waiting_for_prerender = cache_waiting_title_id_ == title_id &&
            cache_waiting_cue_row_ >= 0;
        const int state = static_cast<int>(
            title_cue_visual_state(*title, waiting_for_prerender));
        if (item->data(kTitleCueVisualStateRole).toInt() != state) {
            item->setData(kTitleCueVisualStateRole, state);
            visual_state_changed = true;
        }
    }

    // Repaint once after all item roles have been updated.  This is both
    // cheaper than issuing one paint request per title and avoids relying on
    // the per-item rectangle lookup that caused an MSVC/Qt6 build
    // regression in this include path.
    if (visual_state_changed && list_->viewport())
        list_->viewport()->update();
    update_titles_compact_rail();
}

void TitleDock::schedule_title_list_cache_icon_update(const QString &title_id)
{
    if (!list_ || title_id.isEmpty())
        return;

    pending_title_cache_icon_updates_.insert(title_id);
    if (title_cache_icon_update_scheduled_)
        return;

    title_cache_icon_update_scheduled_ = true;
    QTimer::singleShot(25, this, [this]() { flush_title_list_cache_icon_updates(); });
}

void TitleDock::flush_title_list_cache_icon_updates()
{
    title_cache_icon_update_scheduled_ = false;
    const QSet<QString> pending = pending_title_cache_icon_updates_;
    pending_title_cache_icon_updates_.clear();
    for (const QString &title_id : pending)
        update_title_list_cache_icon(title_id);
}

void TitleDock::update_title_list_cache_icon(const QString &title_id)
{
    if (!list_ || title_id.isEmpty())
        return;

    auto title = TitleDataStore::instance().get_title(title_id.toStdString());
    if (!title)
        return;

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);
    const bool has_exposed_text = !exposed.empty();
    const bool has_scene_mask = title_has_scene_mask(title);
    const LiveCueAggregateCacheStatus cache_status = aggregate_live_cue_cache_status(title);
    for (int i = 0; i < list_->count(); ++i) {
        auto *item = list_->item(i);
        if (!item || item->data(Qt::UserRole).toString() != title_id)
            continue;
        if (template_icon_view_) {
            item->setIcon(title_icon_view_icon(*title, has_exposed_text, has_scene_mask, cache_status, title->playlist_active, CacheManager::instance().cacheEnabled()));
            item->setSizeHint(QSize(kTitleIconViewItemWidth, kTitleIconViewItemHeight));
        } else {
            item->setIcon(title_list_combined_status_icon(has_exposed_text, has_scene_mask, cache_status, CacheManager::instance().cacheEnabled()));
        }
        item->setData(kTitleListThumbnailRole, title_preview_pixmap(*title, QSize(96, 54)));
        item->setData(kTitleShowStatusIconRole, true);
        item->setData(kTitlePlaylistActiveRole, title->playlist_active);
        const bool waiting_for_prerender = cache_waiting_title_id_ == title_id &&
            cache_waiting_cue_row_ >= 0;
        item->setData(kTitleCueVisualStateRole,
                      static_cast<int>(title_cue_visual_state(*title, waiting_for_prerender)));
        item->setToolTip(QStringLiteral("%1\n%2")
                             .arg(bgl_tr("OBSTitles.LayerCountTooltipFormat").arg(title->layers.size()).arg(title->duration),
                                  aggregate_live_cue_cache_tooltip(cache_status)));
        break;
    }
    if (title_id == QString::fromStdString(selected_id()))
        update_titles_compact_rail();
}

void TitleDock::update_live_text_select_cell_status(int row)
{
    if (!text_table_ || row < 0 || row >= text_table_->rowCount())
        return;

    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title)
        return;

    const QString title_id = QString::fromStdString(title->id);
    const bool waiting_for_prerender = cache_waiting_title_id_ == title_id &&
        cache_waiting_cue_row_ == row;
    const bool current = row == title->current_cue_row;
    const bool queued = row == title->pending_cue_row || waiting_for_prerender;
    const bool ending = current && title->cue_uncue_requested;
    const QColor status_color = live_cue_select_cell_color(current, queued, ending);
    const QString background_style = status_color.isValid()
        ? QStringLiteral("background-color: rgba(%1, %2, %3, %4);")
              .arg(status_color.red()).arg(status_color.green()).arg(status_color.blue()).arg(status_color.alpha())
        : QString();
    const QString editor_style = status_color.isValid()
        ? QStringLiteral("QPlainTextEdit{padding:3px;%1}").arg(background_style)
        : QStringLiteral("QPlainTextEdit{padding:3px;}");

    for (int col = 0; col < text_table_->columnCount(); ++col) {
        if (auto *cell_item = text_table_->item(row, col)) {
            if (status_color.isValid())
                cell_item->setBackground(QBrush(status_color));
            else
                cell_item->setBackground(QBrush());
        }

        auto *widget = text_table_->cellWidget(row, col);
        if (!widget)
            continue;
        widget->setStyleSheet(background_style);
        if (auto *field = dynamic_cast<LiveTextCueField *>(widget))
            field->setStyleSheet(editor_style);
        else if (auto *image = dynamic_cast<LiveImageCueField *>(widget))
            image->setStyleSheet(background_style);
    }

    if (text_table_->viewport())
        text_table_->viewport()->update();
}

void TitleDock::adjust_live_text_table_columns(bool fill_to_viewport)
{
    if (!text_table_ || text_table_->columnCount() <= 0)
        return;

    auto *header = text_table_->horizontalHeader();
    if (!header)
        return;

    const int last_col = text_table_->columnCount() - 1;
    const int cache_col = text_table_->columnCount() > 2 ? kLiveCacheColumn : -1;
    const int first_text_col = text_table_->columnCount() > 3 ? kLiveFirstValueColumn : -1;
    const int last_text_col = text_table_->columnCount() > 3 ? last_col : -1;

    QSignalBlocker header_block(header);
    auto resize_to_contents = [&](int col) {
        if (col >= 0 && col < text_table_->columnCount()) {
            header->setSectionResizeMode(col, QHeaderView::ResizeToContents);
            text_table_->resizeColumnToContents(col);
        }
    };

    if (text_table_->columnCount() <= 3) {
        resize_to_contents(kStaticCueColumn);
        if (!text_table_->isColumnHidden(kStaticCacheColumn))
            resize_to_contents(kStaticCacheColumn);
        header->setSectionResizeMode(kStaticTitleColumn, QHeaderView::Interactive);
        const int viewport_width = text_table_->viewport() ? text_table_->viewport()->width() : text_table_->width();
        const int cache_width = text_table_->isColumnHidden(kStaticCacheColumn) ? 0 : header->sectionSize(kStaticCacheColumn);
        const int remaining = viewport_width - header->sectionSize(kStaticCueColumn) - cache_width;
        if (remaining > 0)
            header->resizeSection(kStaticTitleColumn, remaining);
        return;
    }

    resize_to_contents(kLiveCueColumn);
    resize_to_contents(kLiveSelectColumn);
    if (cache_col >= 0 && !text_table_->isColumnHidden(cache_col))
        resize_to_contents(cache_col);

    if (first_text_col >= 0) {
        for (int col = first_text_col; col <= last_text_col; ++col)
            header->setSectionResizeMode(col, QHeaderView::Interactive);
    }

    int total_width = 0;
    for (int col = 0; col < text_table_->columnCount(); ++col)
        total_width += header->sectionSize(col);

    const int viewport_width = text_table_->viewport() ? text_table_->viewport()->width() : text_table_->width();
    if (fill_to_viewport && first_text_col >= 0 && viewport_width > 0 && total_width < viewport_width) {
        const int missing = viewport_width - total_width;
        const int text_cols = std::max(1, last_text_col - first_text_col + 1);
        const int per_col = missing / text_cols;
        int remainder = missing % text_cols;
        for (int col = first_text_col; col <= last_text_col; ++col) {
            const int add = per_col + (remainder-- > 0 ? 1 : 0);
            header->resizeSection(col, header->sectionSize(col) + add);
        }
    }
}

TitleDock::~TitleDock()
{
    save_dock_settings();
    BGL_LOG_INFO("Dock", QStringLiteral("Dock destroyed"));
    remove_obs_state_callbacks();

    TitleDataStore::instance().remove_change_callback(change_callback_id_);
    change_callback_id_ = 0;
    ExternalDataManager::instance().remove_change_callback(external_data_callback_id_);
    external_data_callback_id_ = 0;

    stop_playlist();
    if (live_refresh_timer_)
        live_refresh_timer_->stop();

    if (editor_) {
        TitleEditor *editor = editor_;
        editor_ = nullptr;
        disconnect(editor, nullptr, this, nullptr);
        delete editor;
    }
}


void TitleDock::load_dock_settings()
{
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kDockSettingsGroup));

    template_icon_view_ = settings.value(QString::fromUtf8(kTemplateIconViewKey), template_icon_view_).toBool();
    visibility_filter_active_ = settings.value(QString::fromUtf8(kVisibilityFilterKey), visibility_filter_active_).toBool();
    last_selected_title_id_ = settings.value(QString::fromUtf8(kLastSelectedTitleIdKey)).toString();
    playlist_loop_ = settings.value(QString::fromUtf8(kPlaylistLoopKey), playlist_loop_).toBool();
    playlist_reverse_ = settings.value(QString::fromUtf8(kPlaylistReverseKey), playlist_reverse_).toBool();
    playlist_hold_seconds_ = std::clamp(settings.value(QString::fromUtf8(kPlaylistHoldSecondsKey),
                                                       playlist_hold_seconds_).toDouble(),
                                        0.0, 3600.0);
    background_persistence_ = settings.value(QString::fromUtf8(kBackgroundPersistenceKey),
                                             background_persistence_).toBool();
    text_persistence_ = settings.value(QString::fromUtf8(kTextPersistenceKey),
                                       text_persistence_).toBool();
    live_text_lines_per_row_ = std::clamp(settings.value(QString::fromUtf8(kLiveTextLinesPerRowKey),
                                                         live_text_lines_per_row_).toInt(),
                                          kMinLiveTextLinesPerRow,
                                          kMaxLiveTextLinesPerRow);

    titles_collapsed_ = settings.value(QString::fromUtf8(kTitlesCollapsedKey),
                                        titles_collapsed_).toBool();
    titles_expanded_width_ = std::max(kTitlesMinimumExpandedWidth,
        settings.value(QString::fromUtf8(kTitlesExpandedWidthKey), titles_expanded_width_).toInt());
    const bool has_saved_expanded_splitter =
        settings.contains(QString::fromUtf8(kTitlesExpandedSplitterSizeKey));
    titles_expanded_splitter_size_ = std::max(kTitlesCompactRailHeight + 8,
        settings.value(QString::fromUtf8(kTitlesExpandedSplitterSizeKey),
                       titles_expanded_splitter_size_).toInt());
    const int stored_area = settings.value(QString::fromUtf8(kStoredDockAreaKey),
                                           static_cast<int>(stored_dock_area_)).toInt();
    if (stored_area == static_cast<int>(Qt::LeftDockWidgetArea) ||
        stored_area == static_cast<int>(Qt::RightDockWidgetArea) ||
        stored_area == static_cast<int>(Qt::TopDockWidgetArea) ||
        stored_area == static_cast<int>(Qt::BottomDockWidgetArea)) {
        stored_dock_area_ = static_cast<Qt::DockWidgetArea>(stored_area);
    }

    const QByteArray splitter_state = settings.value(QString::fromUtf8(kDockSplitterStateKey)).toByteArray();
    if (!splitter_state.isEmpty() && sections_)
        sections_->restoreState(splitter_state);
    if (!has_saved_expanded_splitter && sections_) {
        const QList<int> sizes = sections_->sizes();
        if (!sizes.isEmpty() && sizes.front() > kTitlesCompactRailHeight + 8)
            titles_expanded_splitter_size_ = sizes.front();
    }

    settings.endGroup();
}

void TitleDock::save_dock_settings() const
{
    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
    settings.beginGroup(QString::fromUtf8(kDockSettingsGroup));

    if (sections_)
        settings.setValue(QString::fromUtf8(kDockSplitterStateKey), sections_->saveState());
    settings.setValue(QString::fromUtf8(kTemplateIconViewKey), template_icon_view_);
    settings.setValue(QString::fromUtf8(kVisibilityFilterKey), visibility_filter_active_);
    settings.setValue(QString::fromUtf8(kLastSelectedTitleIdKey), last_selected_title_id_);
    settings.setValue(QString::fromUtf8(kPlaylistLoopKey), playlist_loop_);
    settings.setValue(QString::fromUtf8(kPlaylistReverseKey), playlist_reverse_);
    settings.setValue(QString::fromUtf8(kPlaylistHoldSecondsKey), playlist_hold_seconds_);
    settings.setValue(QString::fromUtf8(kBackgroundPersistenceKey), background_persistence_);
    settings.setValue(QString::fromUtf8(kTextPersistenceKey), text_persistence_);
    settings.setValue(QString::fromUtf8(kLiveTextLinesPerRowKey), live_text_lines_per_row_);
    settings.setValue(QString::fromUtf8(kTitlesCollapsedKey), titles_collapsed_);
    settings.setValue(QString::fromUtf8(kTitlesExpandedWidthKey), titles_expanded_width_);
    settings.setValue(QString::fromUtf8(kTitlesExpandedSplitterSizeKey),
                      titles_expanded_splitter_size_);
    settings.setValue(QString::fromUtf8(kStoredDockAreaKey),
                      static_cast<int>(stored_dock_area_));

    settings.endGroup();
}

void TitleDock::update_scene_collection_title()
{
    if (template_lbl_)
        template_lbl_->setText(current_scene_collection_titles_label());
}

void TitleDock::install_obs_state_callbacks()
{
    if (obs_state_callbacks_installed_) return;

    signal_handler_t *handler = obs_get_signal_handler();
    if (!handler) return;

    signal_handler_connect(handler, "source_activate", &TitleDock::on_obs_source_state_signal, this);
    signal_handler_connect(handler, "source_deactivate", &TitleDock::on_obs_source_state_signal, this);
    signal_handler_connect(handler, "source_show", &TitleDock::on_obs_source_state_signal, this);
    signal_handler_connect(handler, "source_hide", &TitleDock::on_obs_source_state_signal, this);
    signal_handler_connect(handler, "source_create", &TitleDock::on_obs_source_state_signal, this);
    signal_handler_connect(handler, "source_destroy", &TitleDock::on_obs_source_state_signal, this);
    obs_frontend_add_event_callback(&TitleDock::on_obs_frontend_event, this);
    obs_state_callbacks_installed_ = true;
}

void TitleDock::remove_obs_state_callbacks()
{
    if (!obs_state_callbacks_installed_) return;

    signal_handler_t *handler = obs_get_signal_handler();
    if (handler) {
        signal_handler_disconnect(handler, "source_activate", &TitleDock::on_obs_source_state_signal, this);
        signal_handler_disconnect(handler, "source_deactivate", &TitleDock::on_obs_source_state_signal, this);
        signal_handler_disconnect(handler, "source_show", &TitleDock::on_obs_source_state_signal, this);
        signal_handler_disconnect(handler, "source_hide", &TitleDock::on_obs_source_state_signal, this);
        signal_handler_disconnect(handler, "source_create", &TitleDock::on_obs_source_state_signal, this);
        signal_handler_disconnect(handler, "source_destroy", &TitleDock::on_obs_source_state_signal, this);
    }
    obs_frontend_remove_event_callback(&TitleDock::on_obs_frontend_event, this);
    obs_state_callbacks_installed_ = false;
}

void TitleDock::refresh_for_obs_source_state_change()
{
    update_titles_compact_rail();
    if (visibility_filter_active_)
        populate_list();
}

void TitleDock::on_obs_source_state_signal(void *priv, calldata_t *)
{
    auto *dock = static_cast<TitleDock *>(priv);
    if (!dock) return;

    QTimer::singleShot(0, dock, [dock]() { dock->refresh_for_obs_source_state_change(); });
}

void TitleDock::on_obs_frontend_event(obs_frontend_event event, void *priv)
{
    auto *dock = static_cast<TitleDock *>(priv);
    if (!dock) return;

    switch (event) {
    case OBS_FRONTEND_EVENT_SCENE_CHANGED:
    case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
        QTimer::singleShot(0, dock, [dock]() { dock->refresh_for_obs_source_state_change(); });
        break;
    default:
        break;
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  UI construction
 * ══════════════════════════════════════════════════════════════════ */
void TitleDock::build_ui()
{
    container_ = new QWidget(this);
    auto *root = new QVBoxLayout(container_);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    sections_ = new QSplitter(Qt::Vertical, container_);
    sections_->setChildrenCollapsible(false);
    root->addWidget(sections_, 1);

    titles_section_ = new QWidget(sections_);
    auto *titles_section_layout = new QVBoxLayout(titles_section_);
    titles_section_layout->setContentsMargins(0, 0, 0, 0);
    titles_section_layout->setSpacing(0);

    titles_expanded_content_ = new QWidget(titles_section_);
    auto *template_layout = new QVBoxLayout(titles_expanded_content_);
    template_layout->setContentsMargins(0, 0, 0, 0);
    template_layout->setSpacing(obs_layout_spacing(titles_expanded_content_));
    titles_section_layout->addWidget(titles_expanded_content_, 1);

    /* ── header toolbar ── */
    auto *template_toolbar = make_obs_dock_toolbar(titles_expanded_content_);

    btn_add_ = make_obs_dock_tool_button(template_toolbar, bgl_tr("OBSTitles.Add"), obs_icon("add.svg"),
                                         bgl_tr("OBSTitles.AddTooltip"));
    btn_dup_ = make_obs_dock_tool_button(template_toolbar, bgl_tr("OBSTitles.Duplicate"), obs_icon("duplicate.svg"),
                                         bgl_tr("OBSTitles.Duplicate"));
    btn_del_ = make_obs_dock_tool_button(template_toolbar, bgl_tr("OBSTitles.Delete"), obs_icon("delete.svg"),
                                         bgl_tr("OBSTitles.Delete"));
    btn_rename_ = make_obs_dock_tool_button(template_toolbar, bgl_tr("OBSTitles.EditDetails"), obs_icon("rename.svg"),
                                            bgl_tr("OBSTitles.EditDetailsTooltip"));
    btn_export_ = make_obs_dock_tool_button(template_toolbar, bgl_tr("OBSTitles.Export"), obs_icon("export.svg"),
                                            bgl_tr("OBSTitles.ExportTooltip"));
    btn_edit_ = make_obs_dock_tool_button(template_toolbar, bgl_tr("OBSTitles.Edit"), obs_icon("edit.svg"),
                                          bgl_tr("OBSTitles.EditTooltip"));
    btn_scene_ = make_obs_dock_tool_button(template_toolbar, bgl_tr("OBSTitles.AddToScene"), obs_icon("add-to-scene.svg"),
                                           bgl_tr("OBSTitles.AddToSceneTooltip"));
    btn_view_ = make_obs_dock_tool_button(template_toolbar, bgl_tr("OBSTitles.IconView"), obs_icon("icon-view.svg"),
                                          bgl_tr("OBSTitles.IconViewTooltip"));
    btn_visibility_filter_ = make_obs_dock_tool_button(template_toolbar, bgl_tr("OBSTitles.VisibleSourceFilter"),
                                                       obs_icon("layer-visible.svg"),
                                                       bgl_tr("OBSTitles.VisibleSourceFilterTooltip"));
    btn_visibility_filter_->setCheckable(true);
    btn_visibility_filter_->setStyleSheet(dock_highlight_button_style());

    template_toolbar->addWidget(btn_add_);
    template_toolbar->addSeparator();
    template_toolbar->addWidget(btn_dup_);
    template_toolbar->addWidget(btn_del_);
    template_toolbar->addWidget(toolbar_spacer(template_toolbar));
    template_toolbar->addWidget(btn_view_);
    template_toolbar->addWidget(btn_visibility_filter_);
    template_toolbar->addSeparator();
    template_toolbar->addWidget(btn_rename_);
    template_toolbar->addWidget(btn_export_);
    template_toolbar->addWidget(btn_edit_);
    template_toolbar->addWidget(btn_scene_);

    /* ── template/title section ── */
    titles_header_layout_ = new QHBoxLayout();
    titles_header_layout_->setContentsMargins(0, 0, 0, 0);
    titles_header_layout_->setSpacing(0);

    btn_titles_collapse_ = new BglCaretButton(titles_expanded_content_);
    btn_titles_collapse_->setAutoRaise(true);
    btn_titles_collapse_->setFocusPolicy(Qt::NoFocus);
    btn_titles_collapse_->setAccessibleName(bgl_tr("OBSTitles.CollapseTitlesAndGraphics"));
    btn_titles_collapse_->setToolTip(bgl_tr("OBSTitles.CollapseTitlesAndGraphics"));
    titles_header_layout_->addWidget(btn_titles_collapse_);

    template_lbl_ = new QLabel(current_scene_collection_titles_label(), titles_expanded_content_);
    set_bold_label(template_lbl_);
    titles_header_layout_->addWidget(template_lbl_);
    titles_header_layout_->addStretch();
    auto *build_label = new QLabel(QStringLiteral(BGL_BUILD_DISPLAY), titles_expanded_content_);
    build_label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    build_label->setToolTip(QStringLiteral("Delivery identifier for this package"));
    QFont build_font = build_label->font();
    build_font.setPointSize(std::max(1, build_font.pointSize() - 1));
    build_label->setFont(build_font);
    build_label->setProperty("bglSecondaryText", true);
    titles_header_layout_->addWidget(build_label);
    template_layout->addLayout(titles_header_layout_);
    template_layout->addWidget(template_toolbar);

    auto *title_list = new TitleListWidget(titles_expanded_content_);
    title_list->delete_requested = [this]() { on_delete(); };
    title_list->files_dropped = [this](const QStringList &paths) { import_title_paths(paths); };
    list_ = title_list;
    list_->setAlternatingRowColors(true);
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setMinimumHeight(120);
    list_->setItemDelegate(new TitleIconViewDelegate(list_));
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        auto *clicked = list_->itemAt(pos);
        if (clicked && !clicked->isSelected()) {
            list_->clearSelection();
            clicked->setSelected(true);
            list_->setCurrentItem(clicked);
        }
        if (!list_->currentItem())
            return;
        QMenu menu(list_);
        menu.addAction(obs_icon("edit.svg"), bgl_tr("OBSTitles.Edit"), this, &TitleDock::on_edit);
        menu.addAction(obs_icon("add-to-scene.svg"), bgl_tr("OBSTitles.AddToScene"), this, &TitleDock::on_add_to_scene);
        menu.addSeparator();
        menu.addAction(obs_icon("duplicate.svg"), bgl_tr("OBSTitles.Duplicate"), this, &TitleDock::on_duplicate);
        menu.addAction(obs_icon("rename.svg"), bgl_tr("OBSTitles.EditDetails"), this, &TitleDock::on_rename);
        menu.addAction(obs_icon("export.svg"), bgl_tr("OBSTitles.Export"), this, &TitleDock::on_export);
        menu.addSeparator();
        menu.addAction(obs_icon("delete.svg"), bgl_tr("OBSTitles.Delete"), this, &TitleDock::on_delete);
        menu.exec(list_->viewport()->mapToGlobal(pos));
    });
    template_layout->addWidget(list_, 1);

    titles_compact_rail_ = new QWidget(titles_section_);
    titles_compact_rail_->setObjectName(QStringLiteral("BglTitlesCompactRail"));
    titles_compact_rail_->setFixedHeight(kTitlesCompactRailHeight);
    titles_compact_layout_ = new QHBoxLayout(titles_compact_rail_);
    titles_compact_layout_->setContentsMargins(4, 2, 4, 2);
    titles_compact_layout_->setSpacing(4);

    btn_titles_expand_ = new BglCaretButton(titles_compact_rail_);
    btn_titles_expand_->setAutoRaise(true);
    btn_titles_expand_->setFocusPolicy(Qt::NoFocus);
    btn_titles_expand_->setAccessibleName(bgl_tr("OBSTitles.ExpandTitlesAndGraphics"));
    btn_titles_expand_->setToolTip(bgl_tr("OBSTitles.ExpandTitlesAndGraphics"));
    titles_compact_layout_->addWidget(btn_titles_expand_);

    titles_compact_dock_icon_ = new QLabel(titles_compact_rail_);
    titles_compact_dock_icon_->setPixmap(bgl_brand_icon().pixmap(20, 20));
    titles_compact_dock_icon_->setToolTip(bgl_tr("OBSTitles.DockName"));
    titles_compact_layout_->addWidget(titles_compact_dock_icon_);

    titles_compact_active_title_ = new QToolButton(titles_compact_rail_);
    titles_compact_active_title_->setAutoRaise(true);
    titles_compact_active_title_->setFocusPolicy(Qt::NoFocus);
    titles_compact_active_title_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    titles_compact_active_title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    titles_compact_active_title_->setMinimumWidth(0);
    titles_compact_layout_->addWidget(titles_compact_active_title_, 1);

    titles_compact_cue_state_ = new QToolButton(titles_compact_rail_);
    titles_compact_cue_state_->setAutoRaise(true);
    titles_compact_cue_state_->setFocusPolicy(Qt::NoFocus);
    titles_compact_cue_state_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    titles_compact_layout_->addWidget(titles_compact_cue_state_);

    titles_compact_cache_state_ = new QToolButton(titles_compact_rail_);
    titles_compact_cache_state_->setAutoRaise(true);
    titles_compact_cache_state_->setFocusPolicy(Qt::NoFocus);
    titles_compact_cache_state_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    titles_compact_layout_->addWidget(titles_compact_cache_state_);
    titles_section_layout->addWidget(titles_compact_rail_);
    titles_compact_rail_->hide();

    auto *live_section = new QWidget(sections_);
    auto *live_layout = new QVBoxLayout(live_section);
    live_layout->setContentsMargins(0, 0, 0, 0);
    live_layout->setSpacing(obs_layout_spacing(live_section));

    auto *live_header = new QHBoxLayout();
    live_header->setContentsMargins(0, 0, 0, 0);
    live_header->setSpacing(0);

    /* ── exposed text section ── */
    text_editor_lbl_ = new QLabel(bgl_tr("OBSTitles.LiveText"), live_section);
    set_bold_label(text_editor_lbl_);

    live_cue_timer_lbl_ = new QLabel(live_section);
    QFont live_cue_timer_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    live_cue_timer_font.setBold(true);
    live_cue_timer_lbl_->setFont(live_cue_timer_font);
    live_cue_timer_lbl_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    live_cue_timer_lbl_->setMinimumWidth(58);
    live_cue_timer_lbl_->setVisible(false);

    auto *live_toolbar = make_obs_dock_toolbar(live_section);
    btn_add_text_row_ = make_obs_dock_tool_button(live_toolbar, bgl_tr("OBSTitles.AddRow"), obs_icon("add.svg"),
                                                  bgl_tr("OBSTitles.AddCueRowTooltip"));
    btn_delete_text_row_ = make_obs_dock_tool_button(live_toolbar, bgl_tr("OBSTitles.Delete"), obs_icon("delete.svg"),
                                                     bgl_tr("OBSTitles.DeleteCueTooltip"));
    btn_row_up_ = make_obs_dock_tool_button(live_toolbar, bgl_tr("OBSTitles.MoveUp"), obs_icon("move-up.svg"),
                                            bgl_tr("OBSTitles.MoveCueUpTooltip"));
    btn_row_down_ = make_obs_dock_tool_button(live_toolbar, bgl_tr("OBSTitles.MoveDown"), obs_icon("move-down.svg"),
                                              bgl_tr("OBSTitles.MoveCueDownTooltip"));
    btn_data_sources_ = make_obs_dock_tool_button(live_toolbar, bgl_tr("OBSTitles.DataSources"),
                                                  obs_icon("import.svg"),
                                                  bgl_tr("OBSTitles.DataSourcesTooltip"));
    auto *data_sources_menu = new QMenu(btn_data_sources_);
    data_sources_menu->addAction(obs_icon("import.svg"), bgl_tr("OBSTitles.ImportData"),
                                 this, &TitleDock::on_import_live_text_data);
    data_sources_menu->addAction(obs_icon("import.svg"), bgl_tr("OBSTitles.ImportAppendData"),
                                 this, &TitleDock::on_import_append_live_text_data);
    data_sources_menu->addAction(obs_icon("export.svg"), bgl_tr("OBSTitles.ExportData"),
                                 this, &TitleDock::on_export_live_text_data);
    data_sources_menu->addSeparator();
    data_sources_menu->addAction(obs_icon("import.svg"),
                                 QStringLiteral("Populate from external table…"),
                                 this, &TitleDock::on_map_external_table);
    btn_data_sources_->setMenu(data_sources_menu);
    btn_data_sources_->setPopupMode(QToolButton::InstantPopup);
    btn_data_sources_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}"));
    btn_live_text_settings_ = make_obs_dock_tool_button(live_toolbar, bgl_tr("OBSTitles.Settings"),
                                                         obs_icon("settings.svg"),
                                                         bgl_tr("OBSTitles.LiveTextSettingsTooltip"));
    btn_live_text_settings_->setText(bgl_tr("OBSTitles.Settings"));
    btn_live_text_settings_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    btn_external_refresh_ = make_obs_dock_long_press_tool_button(live_toolbar, bgl_tr("OBSTitles.RefreshExternalData"),
                                                                 globe_status_icon(false),
                                                                 bgl_tr("OBSTitles.RefreshExternalDataTooltip"));
    btn_external_refresh_->setCheckable(true);
    btn_external_refresh_->setMinimumWidth(obs_toolbar_icon_extent(live_toolbar) + 10);
    btn_external_refresh_->setStyleSheet(dock_highlight_button_style(true));
    btn_playlist_ = make_obs_dock_long_press_tool_button(live_toolbar, bgl_tr("OBSTitles.Playlist"), obs_icon("play.svg"),
                                                         bgl_tr("OBSTitles.PlaylistTooltip"));
    btn_playlist_->setCheckable(true);
    btn_playlist_->setStyleSheet(dock_highlight_button_style(true));
    playlist_countdown_lbl_ = new QLabel(QStringLiteral("--"), live_toolbar);
    playlist_countdown_lbl_->setToolTip(bgl_tr("OBSTitles.PlaylistNextCueTooltip"));
    playlist_countdown_lbl_->setMinimumWidth(44);
    playlist_countdown_lbl_->setAlignment(Qt::AlignCenter);
    btn_persistence_settings_ = make_obs_dock_long_press_tool_button(live_toolbar, bgl_tr("OBSTitles.Persistence"),
                                                                     obs_icon("persistence.svg"),
                                                                     bgl_tr("OBSTitles.PersistenceTooltip"));
    btn_persistence_settings_->setToolButtonStyle(Qt::ToolButtonIconOnly);
    btn_persistence_settings_->setMinimumWidth(obs_toolbar_icon_extent(live_toolbar) + 10);
    btn_persistence_settings_->setCheckable(true);
    btn_persistence_settings_->setStyleSheet(dock_highlight_button_style(true, true));
    live_toolbar->addWidget(btn_add_text_row_);
    live_toolbar->addWidget(btn_delete_text_row_);
    live_toolbar->addWidget(btn_data_sources_);
    live_toolbar->addWidget(btn_row_up_);
    live_toolbar->addWidget(btn_row_down_);
    live_toolbar->addSeparator();
    live_toolbar->addWidget(btn_playlist_);
    live_toolbar->addWidget(playlist_countdown_lbl_);
    live_toolbar->addWidget(btn_persistence_settings_);
    live_toolbar->addWidget(toolbar_spacer(live_toolbar));
    live_toolbar->addWidget(btn_live_text_settings_);
    live_toolbar->addWidget(btn_external_refresh_);

    live_header->addWidget(text_editor_lbl_);
    live_header->addStretch();
    live_header->addWidget(live_cue_timer_lbl_);
    live_layout->addLayout(live_header);

    text_table_ = new LiveTextCueTable(live_section);
    text_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(text_table_, &QTableWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (!text_table_)
            return;
        const QModelIndex index = text_table_->indexAt(pos);
        if (index.isValid() && !text_table_->selectionModel()->isRowSelected(index.row(), QModelIndex()))
            text_table_->selectRow(index.row());
        auto title = TitleDataStore::instance().get_title(selected_id());
        if (!title)
            return;
        QMenu menu(text_table_);
        if (index.isValid()) {
            const int row = index.row();
            menu.addAction(obs_icon("cue.svg"), bgl_tr("OBSTitles.PlayCue"), this,
                           [this, row]() { cue_live_text_row(row, true); });
            menu.addSeparator();
        }
        menu.addAction(obs_icon("add.svg"), bgl_tr("OBSTitles.AddRow"), this, &TitleDock::on_add_live_text_row);
        QAction *delete_rows = menu.addAction(obs_icon("delete.svg"), bgl_tr("OBSTitles.Delete"), this,
                                               &TitleDock::on_delete_live_text_rows);
        delete_rows->setEnabled(!selected_live_text_rows().empty());
        menu.addSeparator();
        QAction *clear_all = menu.addAction(bgl_tr("OBSTitles.ClearAllLiveTextCues"));
        clear_all->setEnabled(!title->live_text_rows.empty());
        connect(clear_all, &QAction::triggered, this, [this, title]() {
            const auto answer = QMessageBox::question(
                this, bgl_tr("OBSTitles.ClearAllLiveTextCues"),
                bgl_tr("OBSTitles.ClearAllLiveTextCuesConfirm"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer != QMessageBox::Yes)
                return;
            bool changed = false;
            updating_exposed_text_ = true;
            for (auto &row : title->live_text_rows) {
                for (auto &value : row) {
                    if (!value.empty()) {
                        value.clear();
                        changed = true;
                    }
                }
            }
            if (changed) {
                TitleDataStore::instance().save();
                TitleDataStore::instance().touch_runtime_change();
                CacheManager::instance().refreshLiveCueStructureAsync(title);
                seen_store_revision_ = TitleDataStore::instance().revision();
            }
            updating_exposed_text_ = false;
            populate_exposed_text();
        });
        menu.exec(text_table_->viewport()->mapToGlobal(pos));
    });
    if (auto *cue_table = dynamic_cast<LiveTextCueTable *>(text_table_))
        cue_table->layout_requested = [this]() { adjust_live_text_table_columns(); };
    auto *live_text_header = new LiveTextCueHeader(text_table_);
    live_text_header->select_all_toggled = [this](bool checked) { set_all_live_text_rows_checked(checked); };
    text_table_->setHorizontalHeader(live_text_header);
    text_table_->setMinimumHeight(96);
    text_table_->setAlternatingRowColors(false);
    text_table_->verticalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    text_table_->verticalHeader()->setDefaultSectionSize(30);
    text_table_->horizontalHeader()->setStretchLastSection(false);
    text_table_->horizontalHeader()->setSectionsMovable(true);
    text_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    text_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    text_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    text_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    live_layout->addWidget(text_table_, 1);
    live_layout->addWidget(live_toolbar);

    sections_->addWidget(titles_section_);
    sections_->addWidget(live_section);
    sections_->setStretchFactor(0, 2);
    sections_->setStretchFactor(1, 1);

    /* ── status ── */
    status_lbl_ = new QLabel(bgl_tr("OBSTitles.NoTitleSelected"), container_);
    status_lbl_->setAlignment(Qt::AlignCenter);
    QFont sf = status_lbl_->font();
    sf.setPointSize(std::max(1, sf.pointSize() - 1));
    status_lbl_->setFont(sf);
    template_layout->addWidget(status_lbl_);
    template_layout->addWidget(template_toolbar);
    update_template_view_mode();

    setWidget(container_);

    /* ── connections ── */
    auto *add_menu = new QMenu(btn_add_);
    add_menu->addAction(bgl_tr("OBSTitles.AddBlankTitle"), this, &TitleDock::on_add);
    add_menu->addAction(bgl_tr("OBSTitles.AddFromTemplatesLibrary"), this, &TitleDock::on_add_from_templates_library);
    add_menu->addAction(bgl_tr("OBSTitles.Import"), this, &TitleDock::on_import);
    btn_add_->setMenu(add_menu);
    btn_add_->setPopupMode(QToolButton::InstantPopup);
    btn_add_->setStyleSheet(QStringLiteral("QToolButton::menu-indicator{image:none;width:0px;}"));

    connect(btn_titles_collapse_, &QToolButton::clicked, this,
            [this]() { set_titles_collapsed(true); });
    connect(btn_titles_expand_, &QToolButton::clicked, this,
            [this]() { set_titles_collapsed(false); });
    connect(this, &QDockWidget::dockLocationChanged, this,
            [this](Qt::DockWidgetArea area) { update_stored_dock_area(area); });
    connect(this, &QDockWidget::topLevelChanged, this, [this](bool floating) {
        update_titles_collapse_direction();
        if (floating && !titles_collapsed_ && titles_expanded_width_ >= kTitlesMinimumExpandedWidth)
            resize(titles_expanded_width_, height());
        save_dock_settings();
    });

    connect(btn_dup_,   &QToolButton::clicked, this, &TitleDock::on_duplicate);
    connect(btn_rename_, &QToolButton::clicked, this, &TitleDock::on_rename);
    connect(btn_del_,   &QToolButton::clicked, this, &TitleDock::on_delete);
    connect(btn_export_, &QToolButton::clicked, this, &TitleDock::on_export);
    connect(btn_edit_,  &QToolButton::clicked, this, &TitleDock::on_edit);
    connect(btn_scene_, &QToolButton::clicked, this, &TitleDock::on_add_to_scene);
    connect(btn_view_, &QToolButton::clicked, this, &TitleDock::on_toggle_template_view);
    connect(btn_visibility_filter_, &QToolButton::toggled, this, &TitleDock::on_toggle_visibility_filter);
    connect(btn_add_text_row_, &QToolButton::clicked, this, &TitleDock::on_add_live_text_row);
    connect(btn_delete_text_row_, &QToolButton::clicked, this, &TitleDock::on_delete_live_text_rows);
    connect(btn_row_up_, &QToolButton::clicked, this, &TitleDock::on_move_live_text_row_up);
    auto *persistence_menu = new QMenu(btn_persistence_settings_);
    act_background_persistence_ = persistence_menu->addAction(bgl_tr("OBSTitles.BackgroundPersistence"));
    act_background_persistence_->setCheckable(true);
    act_text_persistence_ = persistence_menu->addAction(bgl_tr("OBSTitles.TextPersistence"));
    act_text_persistence_->setCheckable(true);
    btn_persistence_settings_->setMenu(persistence_menu);

