int TitleDock::playlist_step_delay_ms(const std::shared_ptr<Title> &title) const
{
    if (!title) return 1000;

    double seconds = title->playlist_hold_seconds;
    if (title->playback_mode == 1)
        seconds += std::clamp(title->loop_end, title->loop_start, title->duration);
    else if (title->playback_mode == 2)
        seconds += std::clamp(title->pause_time, 0.0, title->duration);
    else
        seconds += title->duration;

    return std::max(1, (int)std::round(seconds * 1000.0));
}

int TitleDock::playlist_hold_delay_ms(const std::shared_ptr<Title> &title) const
{
    const double seconds = title ? title->playlist_hold_seconds : playlist_hold_seconds_;
    return std::max(1, (int)std::round(seconds * 1000.0));
}

bool TitleDock::playlist_row_is_terminal(const std::shared_ptr<Title> &title, int row, int row_count) const
{
    if (row < 0 || row_count <= 0) return false;
    const bool reverse = title ? title->playlist_reverse : playlist_reverse_;
    return reverse ? row == 0 : row == row_count - 1;
}

void TitleDock::play_playlist_outro(const std::shared_ptr<Title> &title)
{
    if (!title) return;

    const int row = title->pending_cue_row >= 0 ? title->pending_cue_row : title->current_cue_row;
    if (row >= 0)
        cue_live_text_row_for_title(title, row, true);
    else if (live_text_playlist_row_count(title) == 1)
        cue_live_text_row_for_title(title, 0, true);
}

void TitleDock::stop_playlist()
{
    stop_playlist_for_title(TitleDataStore::instance().get_title(selected_id()));
}

void TitleDock::stop_playlist_for_title(const std::shared_ptr<Title> &title)
{
    if (!title)
        return;
    title->playlist_active = false;
    title->playlist_next_due_ms = 0;
    BGL_LOG_INFO("Playlist", QStringLiteral("Stopped playlist title=%1")
        .arg(QString::fromStdString(title->id)));
    title->playlist_stop_after_due = false;
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();

    bool any_active = false;
    for (const auto &candidate : TitleDataStore::instance().titles()) {
        if (candidate && candidate->playlist_active) {
            any_active = true;
            break;
        }
    }
    if (!any_active && playlist_timer_)
        playlist_timer_->stop();

    if (QString::fromStdString(title->id) == QString::fromStdString(selected_id())) {
        if (btn_playlist_ && btn_playlist_->isChecked()) {
            QSignalBlocker block(btn_playlist_);
            btn_playlist_->setChecked(false);
        }
        update_playlist_countdown_label();
    }
    update_title_list_cache_icon(QString::fromStdString(title->id));
}

void TitleDock::sync_playlist_runtime_state()
{
    bool any_active = false;
    for (const auto &title : TitleDataStore::instance().titles()) {
        if (title && title->playlist_active) {
            any_active = true;
            break;
        }
    }

    if (any_active) {
        if (playlist_timer_ && !playlist_timer_->isActive())
            playlist_timer_->start();
    } else if (playlist_timer_ && playlist_timer_->isActive()) {
        playlist_timer_->stop();
    }

    if (list_) {
        for (int i = 0; i < list_->count(); ++i) {
            auto *item = list_->item(i);
            if (!item)
                continue;
            const QString title_id = item->data(Qt::UserRole).toString();
            auto title = TitleDataStore::instance().get_title(title_id.toStdString());
            const bool active = title && title->playlist_active;
            if (item->data(kTitlePlaylistActiveRole).toBool() != active)
                schedule_title_list_cache_icon_update(title_id);
        }
    }

    update_playlist_controls();
}

namespace {
QString format_live_cue_elapsed(double seconds)
{
    const qint64 whole_seconds = static_cast<qint64>(
        std::floor(std::max(0.0, seconds)));
    const qint64 minutes = whole_seconds / 60;
    const int remaining_seconds = static_cast<int>(whole_seconds % 60);
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QChar('0'))
        .arg(remaining_seconds, 2, 10, QChar('0'));
}

QString format_live_cue_countdown(double seconds)
{
    const qint64 total_tenths = static_cast<qint64>(
        std::ceil(std::max(0.0, seconds) * 10.0 - 0.000001));
    const qint64 minutes = total_tenths / 600;
    const int remaining_seconds = static_cast<int>((total_tenths / 10) % 60);
    const int tenths = static_cast<int>(total_tenths % 10);
    if (minutes > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(minutes)
            .arg(remaining_seconds, 2, 10, QChar('0'))
            .arg(tenths);
    }
    return QStringLiteral("%1:%2")
        .arg(remaining_seconds, 2, 10, QChar('0'))
        .arg(tenths);
}
} // namespace

void TitleDock::update_live_cue_timer_label()
{
    if (!live_cue_timer_lbl_)
        return;

    auto title = TitleDataStore::instance().get_title(selected_id());
    const LiveCueRuntimeSnapshot runtime = title
        ? live_cue_runtime_state(title->id)
        : LiveCueRuntimeSnapshot{};
    const qint64 now_ms = QDateTime::currentMSecsSinceEpoch();
    const bool fresh_runtime = title && runtime.active &&
        title->current_cue_row >= 0 &&
        runtime.row == title->current_cue_row &&
        runtime.source_token != 0 &&
        runtime.updated_ms > 0 &&
        now_ms - runtime.updated_ms <= 2000;

    if (!fresh_runtime) {
        live_cue_timer_lbl_->clear();
        live_cue_timer_lbl_->setVisible(false);
        return;
    }

    const bool ending = title->cue_uncue_requested ||
        title->pending_cue_row >= 0;
    const bool play_once = title->playback_mode == 0;
    if (ending || play_once) {
        const double remaining = std::max(
            0.0, title->duration - runtime.playhead);
        live_cue_timer_lbl_->setText(format_live_cue_countdown(remaining));
        live_cue_timer_lbl_->setToolTip(bgl_tr(
            ending ? "OBSTitles.LiveCueOutroRemainingTooltip"
                   : "OBSTitles.LiveCuePlayOnceRemainingTooltip"));
    } else {
        live_cue_timer_lbl_->setText(format_live_cue_elapsed(
            runtime.elapsed_seconds));
        live_cue_timer_lbl_->setToolTip(
            bgl_tr("OBSTitles.LiveCueElapsedTooltip"));
    }
    live_cue_timer_lbl_->setVisible(true);
}

void TitleDock::update_playlist_countdown_label()
{
    if (!playlist_countdown_lbl_) return;

    auto title = TitleDataStore::instance().get_title(selected_id());
    const bool active = title && title->playlist_active && title->playlist_next_due_ms > 0;
    if (!active) {
        playlist_countdown_lbl_->setText(QStringLiteral("--"));
        playlist_countdown_lbl_->setVisible(false);
        return;
    }

    const qint64 remaining_ms = std::max<qint64>(0, title->playlist_next_due_ms - QDateTime::currentMSecsSinceEpoch());
    const double remaining_seconds = remaining_ms / 1000.0;
    playlist_countdown_lbl_->setText(QStringLiteral("%1s").arg(remaining_seconds, 0, 'f', remaining_seconds < 10.0 ? 1 : 0));
    playlist_countdown_lbl_->setVisible(true);
}

void TitleDock::start_playlist_step()
{
    start_playlist_step_for_title(TitleDataStore::instance().get_title(selected_id()));
}

void TitleDock::start_playlist_step_for_title(const std::shared_ptr<Title> &title)
{
    const int row_count = live_text_playlist_row_count(title);
    if (!title || row_count <= 0) {
        stop_playlist_for_title(title);
        return;
    }

    if (title->playlist_next_row < 0 || title->playlist_next_row >= row_count)
        title->playlist_next_row = title->playlist_reverse ? row_count - 1 : 0;

    const int row = title->playlist_next_row;
    BGL_LOG_DEBUG("Playlist", QStringLiteral(
        "Advancing playlist title=%1 row=%2 rows=%3 reverse=%4 loop=%5")
        .arg(QString::fromStdString(title->id))
        .arg(row).arg(row_count)
        .arg(title->playlist_reverse ? 1 : 0)
        .arg(title->playlist_loop ? 1 : 0));
    const bool already_active = title->pending_cue_row < 0 && title->current_cue_row == row;
    if (!already_active)
        cue_live_text_row_for_title(title, row, false);
    title->playlist_next_row = next_playlist_row(title, row, row_count);
    title->playlist_stop_after_due = !title->playlist_loop && playlist_row_is_terminal(title, row, row_count);

    title->playlist_next_due_ms = QDateTime::currentMSecsSinceEpoch() +
        (already_active ? playlist_hold_delay_ms(title) : playlist_step_delay_ms(title));
    if (playlist_timer_ && !playlist_timer_->isActive())
        playlist_timer_->start();
    if (QString::fromStdString(title->id) == QString::fromStdString(selected_id()))
        update_playlist_countdown_label();
    update_title_list_cache_icon(QString::fromStdString(title->id));
}

void TitleDock::on_playlist_tick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool any_active = false;
    for (const auto &title : TitleDataStore::instance().titles()) {
        if (!title || !title->playlist_active)
            continue;
        any_active = true;
        if (title->playlist_next_due_ms <= 0 || now < title->playlist_next_due_ms)
            continue;

        if (title->playlist_stop_after_due) {
            play_playlist_outro(title);
            stop_playlist_for_title(title);
        } else {
            start_playlist_step_for_title(title);
        }
    }

    if (!any_active && playlist_timer_)
        playlist_timer_->stop();
    update_playlist_countdown_label();
}

void TitleDock::on_toggle_playlist(bool enabled)
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!enabled) {
        stop_playlist_for_title(title);
        return;
    }

    const int row_count = live_text_playlist_row_count(title);
    if (!title || row_count <= 0) {
        if (btn_playlist_ && btn_playlist_->isChecked()) {
            QSignalBlocker block(btn_playlist_);
            btn_playlist_->setChecked(false);
        }
        update_playlist_countdown_label();
        return;
    }

    title->playlist_active = true;
    title->playlist_loop = playlist_loop_;
    title->playlist_reverse = playlist_reverse_;
    title->playlist_hold_seconds = playlist_hold_seconds_;
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();
    int base = title->pending_cue_row >= 0 ? title->pending_cue_row : title->current_cue_row;
    if (title->pending_cue_row < 0 && base >= 0 && base < row_count) {
        title->playlist_next_row = next_playlist_row(title, base, row_count);
        title->playlist_stop_after_due = !title->playlist_loop && playlist_row_is_terminal(title, base, row_count);
        title->playlist_next_due_ms = QDateTime::currentMSecsSinceEpoch() + playlist_hold_delay_ms(title);
        if (playlist_timer_ && !playlist_timer_->isActive())
            playlist_timer_->start();
        update_playlist_countdown_label();
        update_title_list_cache_icon(QString::fromStdString(title->id));
        return;
    }

    if (base >= 0 && base < row_count)
        title->playlist_next_row = base;
    else
        title->playlist_next_row = title->playlist_reverse ? row_count - 1 : 0;

    start_playlist_step_for_title(title);
}

void TitleDock::update_playlist_controls()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    const bool enabled = title && live_text_playlist_row_count(title) > 0;
    if (title) {
        playlist_loop_ = title->playlist_loop;
        playlist_reverse_ = title->playlist_reverse;
        playlist_hold_seconds_ = title->playlist_hold_seconds;
    }
    if (btn_playlist_) {
        QSignalBlocker block(btn_playlist_);
        btn_playlist_->setEnabled(enabled);
        btn_playlist_->setChecked(enabled && title->playlist_active);
    }
    if (act_playlist_loop_) {
        act_playlist_loop_->setEnabled(enabled);
        QSignalBlocker block(act_playlist_loop_);
        act_playlist_loop_->setChecked(title ? title->playlist_loop : playlist_loop_);
    }
    if (act_playlist_reverse_) {
        act_playlist_reverse_->setEnabled(enabled);
        QSignalBlocker block(act_playlist_reverse_);
        act_playlist_reverse_->setChecked(title ? title->playlist_reverse : playlist_reverse_);
    }
    if (act_playlist_restart_on_active_) {
        act_playlist_restart_on_active_->setEnabled(enabled);
        QSignalBlocker block(act_playlist_restart_on_active_);
        act_playlist_restart_on_active_->setChecked(title && title->playlist_restart_on_source_active);
    }
    if (act_playlist_stop_on_inactive_) {
        act_playlist_stop_on_inactive_->setEnabled(enabled);
        QSignalBlocker block(act_playlist_stop_on_inactive_);
        act_playlist_stop_on_inactive_->setChecked(title && title->playlist_stop_on_source_inactive);
    }
    if (act_playlist_hold_)
        act_playlist_hold_->setEnabled(enabled);
    if (auto *hold_action = qobject_cast<QWidgetAction *>(act_playlist_hold_)) {
        if (auto *widget = hold_action->defaultWidget()) {
            if (auto *spin = widget->findChild<QDoubleSpinBox *>()) {
                QSignalBlocker block(spin);
                spin->setValue(title ? title->playlist_hold_seconds : playlist_hold_seconds_);
            }
        }
    }
    update_playlist_countdown_label();
}

void TitleDock::populate_exposed_text()
{
    if (!text_table_) return;
    QSignalBlocker block(text_table_);
    QSignalBlocker header_block(text_table_->horizontalHeader());
    text_table_->clear();
    text_table_->setRowCount(0);
    text_table_->setColumnCount(0);

    auto *header = live_text_cue_header(text_table_);

    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) {
        if (header) header->set_select_all_visible(false);
        text_editor_lbl_->setText(bgl_tr("OBSTitles.LiveTextSelectTitle"));
        text_table_->setEnabled(false);
        if (btn_add_text_row_) btn_add_text_row_->setEnabled(false);
        if (btn_delete_text_row_) btn_delete_text_row_->setEnabled(false);
        if (btn_row_up_) btn_row_up_->setEnabled(false);
        if (btn_row_down_) btn_row_down_->setEnabled(false);
        update_playlist_controls();
        update_persistence_controls();
        update_external_data_controls();
        update_live_text_select_all_state();
        return;
    }

    const QString current_width_title_id = QString::fromStdString(title->id);
    const bool first_width_fit_for_title = live_text_width_initialized_title_id_ != current_width_title_id;
    if (first_width_fit_for_title)
        live_text_width_initialized_title_id_ = current_width_title_id;

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);

    if (auto *cue_table = dynamic_cast<LiveTextCueTable *>(text_table_)) {
        const QString border_title_id = QString::fromStdString(title->id);
        cue_table->cue_row_border_color = [this, border_title_id](int row) -> QColor {
            auto current_title = TitleDataStore::instance().get_title(selected_id());
            if (!current_title || QString::fromStdString(current_title->id) != border_title_id)
                return QColor();
            const bool waiting_for_prerender = cache_waiting_title_id_ == border_title_id &&
                cache_waiting_cue_row_ == row;
            if (row == current_title->current_cue_row)
                return live_cue_state_color(true, false, current_title->cue_uncue_requested);
            if (row == current_title->pending_cue_row || waiting_for_prerender)
                return live_cue_state_color(false, true);
            return QColor();
        };
    }

    const bool has_exposed = !exposed.empty();
    text_table_->setEnabled(true);
    if (btn_add_text_row_) btn_add_text_row_->setEnabled(has_exposed);
    if (btn_delete_text_row_) btn_delete_text_row_->setEnabled(has_exposed);
    if (btn_row_up_) btn_row_up_->setEnabled(has_exposed);
    if (btn_row_down_) btn_row_down_->setEnabled(has_exposed);
    text_editor_lbl_->setText(bgl_tr("OBSTitles.LiveTextCues"));
    if (header) header->set_select_all_visible(has_exposed);
    if (!has_exposed) {
        text_table_->setRowCount(1);
        text_table_->setColumnCount(3);
        text_table_->setHorizontalHeaderLabels(QStringList()
                                               << QString()
                                               << QString()
                                               << bgl_tr("OBSTitles.Title"));
        text_table_->setColumnHidden(kStaticCacheColumn, !CacheManager::instance().cacheEnabled());
        text_table_->horizontalHeader()->setSectionsMovable(false);
        text_table_->horizontalHeader()->setSectionResizeMode(kStaticCueColumn, QHeaderView::ResizeToContents);
        text_table_->horizontalHeader()->setSectionResizeMode(kStaticTitleColumn, QHeaderView::Stretch);
        text_table_->horizontalHeader()->setSectionResizeMode(kStaticCacheColumn, QHeaderView::ResizeToContents);
        text_table_->setVerticalHeaderItem(0, new QTableWidgetItem(QStringLiteral("1")));

        auto *title_item = new QTableWidgetItem(QString::fromStdString(title->name));
        title_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        title_item->setToolTip(title_item->text());
        text_table_->setItem(0, kStaticTitleColumn, title_item);

        auto *cue = live_cue_button(text_table_, 0, kStaticCueColumn);
        if (cue) {
            const bool current = title->current_cue_row == 0;
            const bool queued = title->pending_cue_row == 0 ||
                (cache_waiting_title_id_ == QString::fromStdString(title->id) && cache_waiting_cue_row_ == 0);
            cue->setIcon(obs_icon("cue.svg", live_cue_state_color(
                current, queued, current && title->cue_uncue_requested)));
            cue->setToolTip(bgl_tr("OBSTitles.PlayCueTooltip"));
            cue->setStyleSheet(QStringLiteral(
                "QToolButton{background:transparent;border:none;padding:0;}"
                "QToolButton:hover{background:transparent;border:none;}"));
            connect(cue, &QToolButton::clicked, this, [this]() { cue_live_text_row(0, true); });
        }
        update_live_text_cache_cell(title, 0);
        update_live_text_select_cell_status(0);
        apply_live_text_row_heights();
        adjust_live_text_table_columns(first_width_fit_for_title);
        update_playlist_controls();
        update_persistence_controls();
        update_external_data_controls();
        update_live_text_select_all_state();
        return;
    }

    /* Reconcile stable row IDs/cache states before creating row widgets. This
     * prevents a freshly rebuilt table from briefly binding old numeric-row
     * states to the new row order after delete/re-add/reorder operations. */
    CacheManager::instance().refreshLiveCueStructureAsync(title);

    text_table_->setRowCount((int)title->live_text_rows.size());
    text_table_->setColumnCount((int)exposed.size() + 3);

    QStringList headers;
    headers << QString() << QString() << "";
    for (const auto &layer : exposed)
        headers << live_text_layer_header(layer);
    text_table_->setHorizontalHeaderLabels(headers);
    text_table_->setColumnHidden(kLiveCacheColumn, !CacheManager::instance().cacheEnabled());
    for (int col = 0; col < (int)exposed.size(); ++col) {
        if (auto *item = text_table_->horizontalHeaderItem(col + kLiveFirstValueColumn))
            item->setToolTip(live_text_layer_header(exposed[col]));
    }
    text_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    text_table_->horizontalHeader()->setSectionsMovable(true);
    restore_live_text_header_state();
    auto *table_header = text_table_->horizontalHeader();
    if (table_header) {
        const int cue_visual = table_header->visualIndex(kLiveCueColumn);
        if (cue_visual > 0)
            table_header->moveSection(cue_visual, 0);
        const int cache_visual = table_header->visualIndex(kLiveCacheColumn);
        if (cache_visual != 1)
            table_header->moveSection(cache_visual, 1);
        const int select_visual = table_header->visualIndex(kLiveSelectColumn);
        if (select_visual != 2)
            table_header->moveSection(select_visual, 2);
    }
    adjust_live_text_table_columns(first_width_fit_for_title);

    for (int row = 0; row < (int)title->live_text_rows.size(); ++row) {
        text_table_->setVerticalHeaderItem(row, new QTableWidgetItem(QString::number(row + 1)));
        auto *select_item = new QTableWidgetItem();
        select_item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        select_item->setCheckState(Qt::Unchecked);
        select_item->setTextAlignment(Qt::AlignCenter);
        text_table_->setItem(row, kLiveSelectColumn, select_item);
        update_live_text_cache_cell(title, row);
        for (int col = 0; col < (int)exposed.size(); ++col) {
            if (exposed[col] && exposed[col]->exposed_single_value && row > 0) {
                auto *single_item = new QTableWidgetItem(QString());
                single_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                single_item->setToolTip(bgl_tr("OBSTitles.ExposedSingleValue"));
                text_table_->setItem(row, col + kLiveFirstValueColumn, single_item);
                continue;
            }
            if (layer_uses_image_cue_value(exposed[col])) {
                auto *image = new LiveImageCueField(QString::fromStdString(title->live_text_rows[row][col]), text_table_);
                image->path_changed = [this, title, row, col](const QString &path) {
                    commit_live_text_cell_edit(title, row, col, path);
                };
                text_table_->setCellWidget(row, col + kLiveFirstValueColumn, image);
            } else {
                auto *edit = new LiveTextCueField(QString::fromStdString(title->live_text_rows[row][col]), text_table_);
                edit->setPlaceholderText(live_text_layer_header(exposed[col]));
                edit->setStyleSheet("QPlainTextEdit{padding:3px;}");
                edit->editing_finished = [this, title, row, col](const QString &text) {
                    commit_live_text_cell_edit(title, row, col, text);
                };
                QPointer<TitleDock> dock_guard(this);
                edit->focus_changed = [dock_guard, title, row](bool focused) {
                    if (dock_guard)
                        dock_guard->set_live_text_row_render_paused(title, row, focused);
                };
                text_table_->setCellWidget(row, col + kLiveFirstValueColumn, edit);
            }
        }

        const bool waiting_for_prerender = cache_waiting_title_id_ == QString::fromStdString(title->id) &&
            cache_waiting_cue_row_ == row;
        auto *cue = live_cue_button(text_table_, row, kLiveCueColumn);
        if (cue) {
            cue->setToolTip(bgl_tr("OBSTitles.PlayCueTooltip"));
            const bool ending = row == title->current_cue_row && title->cue_uncue_requested;
            cue->setIcon(obs_icon("cue.svg", live_cue_state_color(
                row == title->current_cue_row,
                row == title->pending_cue_row || waiting_for_prerender,
                ending)));
            cue->setStyleSheet(QStringLiteral(
                "QToolButton{background:transparent;border:none;padding:0;}"
                "QToolButton:hover{background:transparent;border:none;}"));
            connect(cue, &QToolButton::clicked, this, [this, row]() { cue_live_text_row(row, true); });
        }
        update_live_text_select_cell_status(row);
    }
    apply_live_text_row_heights();
    adjust_live_text_table_columns(first_width_fit_for_title);
    update_playlist_controls();
    update_persistence_controls();
    update_external_data_controls();
    update_live_text_select_all_state();
}

void TitleDock::on_export_live_text_data()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);

    QString path = QFileDialog::getSaveFileName(
        this, bgl_tr("OBSTitles.ExportData"), QString(),
        bgl_tr("OBSTitles.JsonFileFilter"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        path += QStringLiteral(".json");

    QJsonArray columns;
    for (const auto &layer : exposed) {
        QJsonObject column;
        column.insert(QStringLiteral("id"), QString::fromStdString(layer->id));
        column.insert(QStringLiteral("name"), live_text_layer_header(layer));
        columns.append(column);
    }

    QJsonObject root;
    root.insert(QStringLiteral("format"), QStringLiteral("Broadcast Graphics Live live text cues"));
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("title"), QString::fromStdString(title->name));
    root.insert(QStringLiteral("columns"), columns);
    root.insert(QStringLiteral("rows"), live_text_rows_to_json(title->live_text_rows));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, bgl_tr("OBSTitles.ExportData"),
                             bgl_tr("OBSTitles.ExportDataFailed"));
        return;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    status_lbl_->setText(bgl_tr("OBSTitles.ExportedStatusFormat").arg(QFileInfo(path).fileName()));
}

void TitleDock::on_import_live_text_data()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    const QString path = QFileDialog::getOpenFileName(
        this, bgl_tr("OBSTitles.ImportData"), QString(),
        bgl_tr("OBSTitles.JsonFileFilter"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, bgl_tr("OBSTitles.ImportData"),
                             bgl_tr("OBSTitles.ImportDataFailed"));
        return;
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        QMessageBox::warning(this, bgl_tr("OBSTitles.ImportData"), parse_error.errorString());
        return;
    }

    std::vector<std::vector<std::string>> imported_rows;
    QString error;
    if (!live_text_rows_from_json(doc, imported_rows, &error)) {
        QMessageBox::warning(this, bgl_tr("OBSTitles.ImportData"), error);
        return;
    }

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);
    const int existing_rows = (int)title->live_text_rows.size();
    const int imported_row_count = (int)imported_rows.size();
    if (imported_row_count > existing_rows) {
        const auto answer = QMessageBox::question(
            this, bgl_tr("OBSTitles.ImportData"),
            bgl_tr("OBSTitles.ImportDataCropWarning")
                .arg(imported_row_count)
                .arg(existing_rows),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        imported_rows.resize(existing_rows);
    }

    for (auto &row : imported_rows)
        row.resize(exposed.size());
    title->live_text_rows = std::move(imported_rows);
    title->live_text_row_ids.clear();
    normalize_live_text_rows(title, exposed);
    title->current_cue_row = -1;
    title->pending_cue_row = -1;
    ++title->cue_revision;
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    CacheManager::instance().refreshLiveCueStructure(title);
    populate_exposed_text();
    status_lbl_->setText(bgl_tr("OBSTitles.ImportedStatusFormat").arg(QFileInfo(path).fileName()));
}

void TitleDock::on_import_append_live_text_data()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;

    const QString path = QFileDialog::getOpenFileName(
        this, bgl_tr("OBSTitles.ImportAppendData"), QString(),
        bgl_tr("OBSTitles.JsonFileFilter"));
    if (path.isEmpty()) return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, bgl_tr("OBSTitles.ImportAppendData"),
                             bgl_tr("OBSTitles.ImportDataFailed"));
        return;
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        QMessageBox::warning(this, bgl_tr("OBSTitles.ImportAppendData"), parse_error.errorString());
        return;
    }

    std::vector<std::vector<std::string>> imported_rows;
    QString error;
    if (!live_text_rows_from_json(doc, imported_rows, &error)) {
        QMessageBox::warning(this, bgl_tr("OBSTitles.ImportAppendData"), error);
        return;
    }

    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);
    for (auto &row : imported_rows) {
        row.resize(exposed.size());
        title->live_text_rows.push_back(std::move(row));
    }
    normalize_live_text_rows(title, exposed);
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    CacheManager::instance().refreshLiveCueStructure(title);
    populate_exposed_text();
    status_lbl_->setText(bgl_tr("OBSTitles.ImportedStatusFormat").arg(QFileInfo(path).fileName()));
}

void TitleDock::on_toggle_external_data_source()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;
    title->external_data_enabled = !title->external_data_enabled;
    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();
    update_external_data_controls();
}

void TitleDock::on_show_external_data_settings()
{
    QDialog dialog(this);
    bgl_apply_brand_icon(&dialog);
    dialog.setWindowTitle(bgl_tr("OBSTitles.ExternalDataSource"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *header = new QLabel(bgl_tr("OBSTitles.ExternalDataSource"), &dialog);
    set_bold_label(header);
    layout->addWidget(header);

    auto *tabs = new QTabWidget(&dialog);
    const QStringList names = {
        bgl_tr("OBSTitles.CSV"),
        bgl_tr("OBSTitles.URL"),
        bgl_tr("OBSTitles.RSSFeed"),
        bgl_tr("OBSTitles.DataSourcesSettings")
    };
    for (const QString &name : names) {
        auto *tab = new QWidget(tabs);
        auto *tab_layout = new QVBoxLayout(tab);
        auto *placeholder = new QLabel(bgl_tr("OBSTitles.ExternalDataSettingsPlaceholder"), tab);
        placeholder->setWordWrap(true);
        tab_layout->addWidget(placeholder);
        tab_layout->addStretch();
        tabs->addTab(tab, name);
    }
    layout->addWidget(tabs);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(buttons);
    dialog.resize(520, 360);
    dialog.exec();
}

void TitleDock::on_refresh_external_data()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;
    auto exposed = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed);
    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    seen_store_revision_ = TitleDataStore::instance().revision();
    populate_exposed_text();
    status_lbl_->setText(bgl_tr("OBSTitles.ExternalDataRefreshed"));
}

void TitleDock::on_add_live_text_row()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title) return;
    auto exposed = exposed_text_layers(title);
    if (exposed.empty()) return;

    auto selected_rows = selected_live_text_rows();
    std::vector<std::string> row(exposed.size());
    if (selected_rows.size() == 1) {
        const int source_row = selected_rows.front();
        if (source_row >= 0 && source_row < (int)title->live_text_rows.size())
            row = title->live_text_rows[source_row];
    }
    row.resize(exposed.size());

    title->live_text_rows.push_back(std::move(row));
    title->live_text_row_ids.push_back(TitleDataStore::make_uuid());
    const int added_row = (int)title->live_text_rows.size() - 1;
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();

    /* Adding a row creates only the transition variants to/from that row.
     * Refresh all requirement sets so those new states are queued, while stable
     * content-addressed keys reuse every previously rendered frame. */
    CacheManager::instance().refreshLiveCueStructure(title);
    populate_exposed_text();
    apply_live_text_row_selection({added_row}, false);
    update_live_text_cache_cell(title, added_row);
}

void TitleDock::on_delete_live_text_rows()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_) return;

    auto rows = selected_live_text_rows();
    if (rows.empty()) return;

    updating_exposed_text_ = true;
    int next_row = rows.front();
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const int row = *it;
        if (row < 0 || row >= (int)title->live_text_rows.size())
            continue;
        title->live_text_rows.erase(title->live_text_rows.begin() + row);
        if (row < static_cast<int>(title->live_text_row_ids.size()))
            title->live_text_row_ids.erase(title->live_text_row_ids.begin() + row);
        if (title->current_cue_row == row)
            title->current_cue_row = -1;
        else if (title->current_cue_row > row)
            --title->current_cue_row;
        if (title->pending_cue_row == row)
            title->pending_cue_row = -1;
        else if (title->pending_cue_row > row)
            --title->pending_cue_row;
    }

    auto exposed_now = exposed_text_layers(title);
    normalize_live_text_rows(title, exposed_now);
    TitleDataStore::instance().save();
    TitleDataStore::instance().notify_change();
    CacheManager::instance().refreshLiveCueStructure(title);
    updating_exposed_text_ = false;
    populate_exposed_text();
    if (!title->live_text_rows.empty())
        text_table_->selectRow(std::min(next_row, (int)title->live_text_rows.size() - 1));
}

void TitleDock::on_move_live_text_row_up()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_) return;

    auto rows = selected_live_text_rows();
    if (rows.empty()) return;

    const bool restore_checked = has_checked_live_text_rows();
    const int row_count = (int)title->live_text_rows.size();
    std::vector<bool> selected(row_count, false);
    for (int row : rows) {
        if (row >= 0 && row < row_count)
            selected[row] = true;
    }

    std::vector<int> order(row_count);
    std::iota(order.begin(), order.end(), 0);
    bool moved = false;
    for (int visual = 1; visual < row_count; ++visual) {
        if (selected[order[visual]] && !selected[order[visual - 1]]) {
            std::swap(order[visual], order[visual - 1]);
            moved = true;
        }
    }
    if (!moved) return;

    ensure_live_text_row_ids(*title);
    std::vector<std::vector<std::string>> reordered;
    std::vector<std::string> reordered_ids;
    reordered.reserve(title->live_text_rows.size());
    reordered_ids.reserve(title->live_text_row_ids.size());
    std::vector<int> new_index(row_count, -1);
    for (int visual = 0; visual < row_count; ++visual) {
        new_index[order[visual]] = visual;
        reordered.push_back(std::move(title->live_text_rows[order[visual]]));
        reordered_ids.push_back(std::move(title->live_text_row_ids[order[visual]]));
    }
    title->live_text_rows = std::move(reordered);
    title->live_text_row_ids = std::move(reordered_ids);
    if (title->current_cue_row >= 0 && title->current_cue_row < row_count)
        title->current_cue_row = new_index[title->current_cue_row];
    if (title->pending_cue_row >= 0 && title->pending_cue_row < row_count)
        title->pending_cue_row = new_index[title->pending_cue_row];

    std::vector<int> moved_rows;
    for (int row : rows) {
        if (row >= 0 && row < row_count)
            moved_rows.push_back(new_index[row]);
    }
    std::sort(moved_rows.begin(), moved_rows.end());

    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    CacheManager::instance().refreshLiveCueStructure(title);
    seen_store_revision_ = TitleDataStore::instance().revision();
    populate_exposed_text();
    apply_live_text_row_selection(moved_rows, restore_checked);
}

void TitleDock::on_move_live_text_row_down()
{
    auto title = TitleDataStore::instance().get_title(selected_id());
    if (!title || !text_table_) return;

    auto rows = selected_live_text_rows();
    if (rows.empty()) return;

    const bool restore_checked = has_checked_live_text_rows();
    const int row_count = (int)title->live_text_rows.size();
    std::vector<bool> selected(row_count, false);
    for (int row : rows) {
        if (row >= 0 && row < row_count)
            selected[row] = true;
    }

    std::vector<int> order(row_count);
    std::iota(order.begin(), order.end(), 0);
    bool moved = false;
    for (int visual = row_count - 2; visual >= 0; --visual) {
        if (selected[order[visual]] && !selected[order[visual + 1]]) {
            std::swap(order[visual], order[visual + 1]);
            moved = true;
        }
    }
    if (!moved) return;

    ensure_live_text_row_ids(*title);
    std::vector<std::vector<std::string>> reordered;
    std::vector<std::string> reordered_ids;
    reordered.reserve(title->live_text_rows.size());
    reordered_ids.reserve(title->live_text_row_ids.size());
    std::vector<int> new_index(row_count, -1);
    for (int visual = 0; visual < row_count; ++visual) {
        new_index[order[visual]] = visual;
        reordered.push_back(std::move(title->live_text_rows[order[visual]]));
        reordered_ids.push_back(std::move(title->live_text_row_ids[order[visual]]));
    }
    title->live_text_rows = std::move(reordered);
    title->live_text_row_ids = std::move(reordered_ids);
    if (title->current_cue_row >= 0 && title->current_cue_row < row_count)
        title->current_cue_row = new_index[title->current_cue_row];
    if (title->pending_cue_row >= 0 && title->pending_cue_row < row_count)
        title->pending_cue_row = new_index[title->pending_cue_row];

    std::vector<int> moved_rows;
    for (int row : rows) {
        if (row >= 0 && row < row_count)
            moved_rows.push_back(new_index[row]);
    }
    std::sort(moved_rows.begin(), moved_rows.end());

    TitleDataStore::instance().save();
    TitleDataStore::instance().touch_runtime_change();
    CacheManager::instance().refreshLiveCueStructure(title);
    seen_store_revision_ = TitleDataStore::instance().revision();
    populate_exposed_text();
    apply_live_text_row_selection(moved_rows, restore_checked);
}


void TitleDock::select_title(const std::string &id)
{
    populate_list();
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString().toStdString() == id) {
            list_->setCurrentRow(i);
            break;
        }
    }
}

std::shared_ptr<Title> TitleDock::create_template_title(const std::string &name,
                                                         int template_id)
{
    auto title = TitleDataStore::instance().create_title(name);
    title->layers.clear();
    title->bg_color = 0x00000000;
    title->duration = 7.0;

