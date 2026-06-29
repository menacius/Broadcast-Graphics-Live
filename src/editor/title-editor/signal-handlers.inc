void TitleEditor::step_forward()
{
    if (!title_) return;
    on_playhead_changed(std::min(snap_to_obs_frame(playhead_ + obs_frame_duration()), title_->duration));
}


static void collect_timeline_keyframes(const std::shared_ptr<Layer> &layer,
                                       std::vector<double> &times)
{
    if (!layer) return;
    for (auto prop : timeline_properties(*layer)) {
        for (size_t i = 0; i < prop.keyframe_count(); ++i)
            times.push_back(layer->in_time + prop.keyframe_time(i));
    }
}

void TitleEditor::previous_keyframe()
{
    if (!title_) return;
    std::vector<double> times;
    if (!sel_layer_id_.empty())
        collect_timeline_keyframes(title_->find_layer(sel_layer_id_), times);
    if (times.empty())
        for (const auto &layer : title_->layers) collect_timeline_keyframes(layer, times);

    constexpr double kEpsilon = 1.0 / 240.0;
    double target = -1.0;
    for (double t : times) {
        if (t < playhead_ - kEpsilon)
            target = std::max(target, t);
    }
    if (target >= 0.0) on_playhead_changed(target);
}

void TitleEditor::next_keyframe()
{
    if (!title_) return;
    std::vector<double> times;
    if (!sel_layer_id_.empty())
        collect_timeline_keyframes(title_->find_layer(sel_layer_id_), times);
    if (times.empty())
        for (const auto &layer : title_->layers) collect_timeline_keyframes(layer, times);

    constexpr double kEpsilon = 1.0 / 240.0;
    double target = title_->duration + 1.0;
    for (double t : times) {
        if (t > playhead_ + kEpsilon)
            target = std::min(target, t);
    }
    if (target <= title_->duration) on_playhead_changed(target);
}



void TitleEditor::tick()
{
    if (shutting_down_)
        return;
    if (!title_ || !playing_) return;
    const int project_interval_ms = editor_playback_ui_timer_interval_ms();
    if (play_timer_ && play_timer_->interval() != project_interval_ms)
        play_timer_->setInterval(project_interval_ms);
    const CachePlaybackSettings cache_settings = CacheManager::instance().playbackSettings();
    double dt = obs_frame_duration();
    if (!cache_settings.play_every_frame || obs_frame_duration() < editor_playback_ui_frame_duration()) {
        dt = playback_clock_.isValid() ? playback_clock_.restart() / 1000.0 : 0.0;
        if (dt <= 0.0 || dt > 0.25) dt = play_timer_->interval() / 1000.0;
        dt *= std::max(0.01, cache_settings.speed_percent / 100.0);
        if (cache_settings.skip_frames > 0)
            dt += obs_frame_duration() * cache_settings.skip_frames;
    } else if (playback_clock_.isValid()) {
        playback_clock_.restart();
    }

    double duration = std::max(0.001, title_->duration);
    double loop_start = std::clamp(title_->loop_start, 0.0, title_->duration);
    double loop_end = std::clamp(title_->loop_end, loop_start, title_->duration);
    double loop_len = std::max(0.001, loop_end - loop_start);
    double t = playhead_;

    if (manual_reverse_playback_) {
        t = playhead_ - dt;
        if (t <= 0.0) {
            t = 0.0;
            playing_ = false;
            if (canvas_) canvas_->set_playback_active(false);
            manual_reverse_playback_ = false;
            playback_reverse_ = false;
            play_timer_->stop();
            act_play_->setText("▶");
            act_play_->setIcon(obs_icon("play.svg"));
        }
        const double next_playhead = snap_to_obs_frame(t);
        if (cache_settings.cached_frames_only && canvas_ &&
            !canvas_->prepare_cached_playback_frame(next_playhead))
            return;
        apply_playhead_change(next_playhead, true);
        return;
    }

    const bool follow_title_mode = cache_settings.follow_title_playback_mode;
    const bool preview_loop = (!follow_title_mode && cache_settings.mode == CachePlaybackMode::Loop) || full_loop_playback_;
    const bool preview_ping_pong = !follow_title_mode && cache_settings.mode == CachePlaybackMode::PingPong;
    const bool preview_play_once = !follow_title_mode && cache_settings.mode == CachePlaybackMode::PlayOnce;
    if (preview_loop && !preview_ping_pong && full_loop_playback_) {
        t = std::fmod(playhead_ + dt, duration);
    } else if (preview_ping_pong) {
        t += (playback_reverse_ ? -dt : dt);
        if (!playback_reverse_ && t >= duration) {
            t = duration - std::fmod(t - duration, duration);
            playback_reverse_ = true;
        } else if (playback_reverse_ && t <= 0.0) {
            t = std::fmod(-t, duration);
            playback_reverse_ = false;
        }
    } else if (preview_loop) {
        t = std::fmod(playhead_ + dt, duration);
    } else if (preview_play_once) {
        t = playhead_ + dt;
        if (t >= title_->duration) {
            t = title_->duration;
            playing_ = false;
            if (canvas_) canvas_->set_playback_active(false);
            play_timer_->stop();
            act_play_->setText("▶");
            act_play_->setIcon(obs_icon("play.svg"));
        }
    } else {
        switch (title_->playback_mode) {
        case 1: /* Loop in/out between Loop Start and Loop End */
            if (loop_end <= loop_start + 0.0001) {
                t = std::fmod(playhead_ + dt, duration);
            } else if (title_->loop_type == 1) {
                t += (playback_reverse_ ? -dt : dt);
                if (!playback_reverse_ && t >= loop_end) {
                    t = loop_end - std::fmod(t - loop_end, loop_len);
                    playback_reverse_ = true;
                } else if (playback_reverse_ && t <= loop_start) {
                    t = loop_start + std::fmod(loop_start - t, loop_len);
                    playback_reverse_ = false;
                }
            } else {
                t = playhead_ + dt;
                if (t >= loop_end)
                    t = loop_start + std::fmod(t - loop_end, loop_len);
            }
            break;
        case 2: { /* Pause at timeline position */
            double pause_time = std::clamp(title_->pause_time, 0.0, title_->duration);
            t = playhead_ + dt;
            if (t >= pause_time) {
                t = pause_time;
                playing_ = false;
                if (canvas_) canvas_->set_playback_active(false);
                play_timer_->stop();
                act_play_->setText("▶");
                act_play_->setIcon(obs_icon("play.svg"));
            }
            break;
        }
        default: /* Play once */
            t = playhead_ + dt;
            if (t >= title_->duration) {
                t = title_->duration;
                playing_ = false;
                if (canvas_) canvas_->set_playback_active(false);
                play_timer_->stop();
                act_play_->setText("▶");
                act_play_->setIcon(obs_icon("play.svg"));
            }
            break;
        }
    }
    const double next_playhead = snap_to_obs_frame(t);
    if (cache_settings.cached_frames_only && canvas_ &&
        !canvas_->prepare_cached_playback_frame(next_playhead)) {
        /* Play after rendering means the playhead must not outrun the cache.
         * Keep the current visible frame and let the realtime queue prepare the
         * exact next frame; the next timer tick will advance as soon as it is
         * available. */
        return;
    }
    apply_playhead_change(next_playhead, true);
}

static bool editor_focus_accepts_text(QWidget *widget)
{
    return qobject_cast<QLineEdit *>(widget) ||
           qobject_cast<QTextEdit *>(widget) ||
           qobject_cast<QPlainTextEdit *>(widget) ||
           qobject_cast<QAbstractSpinBox *>(widget) ||
           qobject_cast<QComboBox *>(widget);
}

void TitleEditor::show_about()
{
    QDialog dialog(this);
    bgl_apply_brand_icon(&dialog);
    dialog.setWindowTitle(bgl_tr("OBSTitles.AboutTitle"));
    dialog.setModal(true);
    dialog.resize(620, 760);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(10);

    auto *logo = new BroadcastGraphicsLiveLogo(&dialog);
    layout->addWidget(logo, 1);

    auto *text = new QLabel(
        bgl_tr("OBSTitles.AboutTextFormat").arg(QStringLiteral(BGL_FULL_VERSION_DISPLAY)),
        &dialog);
    text->setTextFormat(Qt::RichText);
    text->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    text->setWordWrap(true);
    layout->addWidget(text);

    auto *developed_by = new QLabel(bgl_tr("OBSTitles.DevelopedBy"), &dialog);
    QFont developed_by_font = developed_by->font();
    developed_by_font.setBold(true);
    developed_by->setFont(developed_by_font);
    developed_by->setAlignment(Qt::AlignHCenter);
    layout->addWidget(developed_by);

    auto *omnia_logo = new OmniaTvLogo(&dialog);
    layout->addWidget(omnia_logo, 0, Qt::AlignHCenter);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    dialog.exec();
}

void TitleEditor::show_preferences()
{
    show_preferences_dialog(this, this);
}

void TitleEditor::show_global_preferences(QWidget *parent)
{
    show_preferences_dialog(parent, nullptr);
}

void TitleEditor::show_preferences_dialog(QWidget *parent, TitleEditor *editor)
{
    auto *dialog = new QDialog(parent);
    bgl_apply_brand_icon(dialog);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(bgl_tr("OBSTitles.PreferencesWindowTitle"));
    dialog->setModal(false);
    dialog->resize(700, 540);

    const QPalette pal = dialog->palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor button = pal.color(QPalette::Button);
    const QColor button_text = pal.color(QPalette::ButtonText);
    const QColor border = pal.color(QPalette::Mid);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlighted_text = pal.color(QPalette::HighlightedText);
    const QColor disabled_text = pal.color(QPalette::Disabled, QPalette::WindowText);
    const QColor hover = button.lightness() < 128 ? button.lighter(125) : button.darker(108);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto *content = new QWidget(dialog);
    auto *content_layout = new QHBoxLayout(content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(12);

    auto *tabs = new QListWidget(content);
    tabs->setFixedWidth(138);
    tabs->setFrameShape(QFrame::NoFrame);
    tabs->setSelectionMode(QAbstractItemView::SingleSelection);
    tabs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tabs->addItem(bgl_tr("OBSTitles.EditorPreferences"));
    tabs->addItem(bgl_tr("OBSTitles.Appearance"));
    tabs->addItem(bgl_tr("OBSTitles.Advanced"));
    tabs->addItem(bgl_tr("OBSTitles.Logging"));
    tabs->setStyleSheet(QStringLiteral(
        "QListWidget{background:%1;border:1px solid %2;color:%3;outline:none;}"
        "QListWidget::item{padding:8px 10px;border-bottom:1px solid %2;}"
        "QListWidget::item:hover{background:%4;}"
        "QListWidget::item:selected{background:%5;color:%6;}")
        .arg(base.name(QColor::HexRgb),
             border.name(QColor::HexRgb),
             text.name(QColor::HexRgb),
             hover.name(QColor::HexRgb),
             highlight.name(QColor::HexRgb),
             highlighted_text.name(QColor::HexRgb)));

    auto *pages = new QStackedWidget(content);
    pages->setStyleSheet(QStringLiteral("QWidget{background:%1;color:%2;}")
                             .arg(window.name(QColor::HexRgb),
                                  text.name(QColor::HexRgb)));

    auto *editor_page = new QWidget(pages);
    auto *editor_page_layout = new QVBoxLayout(editor_page);
    editor_page_layout->setContentsMargins(0, 0, 0, 0);
    editor_page_layout->setSpacing(10);

    auto *editor_page_title = new QLabel(bgl_tr("OBSTitles.EditorPreferences"), editor_page);
    QFont title_font = editor_page_title->font();
    title_font.setPointSize(title_font.pointSize() + 2);
    title_font.setBold(true);
    editor_page_title->setFont(title_font);
    editor_page_layout->addWidget(editor_page_title);

    auto *autosave_group = new QGroupBox(bgl_tr("OBSTitles.Autosave"), editor_page);
    auto *autosave_layout = new QFormLayout(autosave_group);
    autosave_layout->setContentsMargins(10, 12, 10, 10);
    autosave_layout->setSpacing(8);
    auto *autosave_enabled = new BglSwitch(bgl_tr("OBSTitles.EnableAutosave"), autosave_group);
    autosave_enabled->setChecked(TitlePreferences::autosave_enabled());
    autosave_enabled->setToolTip(bgl_tr("OBSTitles.EnableAutosaveTooltip"));
    auto *autosave_interval = new QSpinBox(autosave_group);
    autosave_interval->setRange(1, 60);
    autosave_interval->setSuffix(bgl_tr("OBSTitles.MinutesSuffix"));
    autosave_interval->setValue(TitlePreferences::autosave_interval_minutes());
    autosave_interval->setEnabled(autosave_enabled->isChecked());
    autosave_interval->setToolTip(bgl_tr("OBSTitles.AutosaveIntervalTooltip"));
    autosave_layout->addRow(QString(), autosave_enabled);
    autosave_layout->addRow(bgl_tr("OBSTitles.AutosaveInterval"), autosave_interval);
    editor_page_layout->addWidget(autosave_group);

    auto *autosave_hint = new QLabel(bgl_tr("OBSTitles.AutosaveHint"), editor_page);
    autosave_hint->setWordWrap(true);
    autosave_hint->setStyleSheet(QStringLiteral("color:%1;").arg(disabled_text.name(QColor::HexRgb)));
    editor_page_layout->addWidget(autosave_hint);
    editor_page_layout->addStretch(1);

    auto *colors_page = new QWidget(pages);
    auto *colors_layout = new QVBoxLayout(colors_page);
    colors_layout->setContentsMargins(0, 0, 0, 0);
    colors_layout->setSpacing(10);

    auto *colors_title = new QLabel(bgl_tr("OBSTitles.Appearance"), colors_page);
    colors_title->setFont(title_font);
    colors_layout->addWidget(colors_title);

    auto *appearance_scroll = new QScrollArea(colors_page);
    appearance_scroll->setWidgetResizable(true);
    appearance_scroll->setFrameShape(QFrame::NoFrame);
    auto *appearance_body = new QWidget(appearance_scroll);
    auto *appearance_body_layout = new QVBoxLayout(appearance_body);
    appearance_body_layout->setContentsMargins(0, 0, 0, 0);
    appearance_body_layout->setSpacing(12);

    auto *timeline_group = new QGroupBox(bgl_tr("OBSTitles.Timeline"), appearance_body);
    auto *timeline_group_layout = new QVBoxLayout(timeline_group);
    timeline_group_layout->setContentsMargins(10, 12, 10, 10);
    auto *timeline_grid = new QGridLayout();
    timeline_grid->setContentsMargins(0, 0, 0, 0);
    timeline_grid->setHorizontalSpacing(12);
    timeline_grid->setVerticalSpacing(8);
    timeline_group_layout->addLayout(timeline_grid);

    auto *canvas_group = new QGroupBox(bgl_tr("OBSTitles.Canvas"), appearance_body);
    auto *canvas_group_layout = new QVBoxLayout(canvas_group);
    canvas_group_layout->setContentsMargins(10, 12, 10, 10);
    auto *canvas_grid = new QGridLayout();
    canvas_grid->setContentsMargins(0, 0, 0, 0);
    canvas_grid->setHorizontalSpacing(12);
    canvas_grid->setVerticalSpacing(8);
    canvas_group_layout->addLayout(canvas_grid);

    const QString group_style = QStringLiteral(
        "QGroupBox{color:%1;border:1px solid %2;border-radius:5px;margin-top:8px;font-weight:bold;}"
        "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 4px;background:%3;}")
        .arg(text.name(QColor::HexRgb), border.name(QColor::HexRgb), window.name(QColor::HexRgb));
    timeline_group->setStyleSheet(group_style);
    canvas_group->setStyleSheet(group_style);

    appearance_body_layout->addWidget(timeline_group);
    appearance_body_layout->addWidget(canvas_group);
    appearance_body_layout->addStretch(1);
    appearance_scroll->setWidget(appearance_body);
    colors_layout->addWidget(appearance_scroll, 1);

    auto update_appearance = [editor]() {
        if (!editor) {
            TitlePreferences::notify_changed(nullptr);
            return;
        }
        if (editor->layers_)
            editor->layers_->refresh();
        if (editor->timeline_)
            editor->timeline_->update();
        if (editor->canvas_)
            editor->canvas_->update();
    };

    auto color_to_text = [](const QColor &color) {
        return color.alpha() < 255 ? color.name(QColor::HexArgb).toUpper()
                                   : color.name(QColor::HexRgb).toUpper();
    };
    auto apply_color_button = [border, highlight, color_to_text](QPushButton *button, const QColor &color) {
        const QColor label_color = color.lightness() < 128 ? Qt::white : Qt::black;
        button->setText(color_to_text(color));
        button->setStyleSheet(QStringLiteral(
            "QPushButton{color:%1;background:%2;border:1px solid %3;border-radius:3px;padding:4px 8px;text-align:left;}"
            "QPushButton:hover{border-color:%4;}")
            .arg(label_color.name(QColor::HexRgb),
                 color.name(QColor::HexArgb),
                 border.name(QColor::HexRgb),
                 highlight.name(QColor::HexRgb)));
    };
    auto add_color_button_row = [&](QGridLayout *target_grid, int row, const QString &label, std::function<QColor()> current_color,
                                    std::function<void(const QColor &)> apply_color) {
        auto *name = new QLabel(label, colors_page);
        name->setStyleSheet(QStringLiteral("color:%1;").arg(text.name(QColor::HexRgb)));
        auto *button = new QPushButton(colors_page);
        button->setMinimumWidth(116);
        button->setCursor(Qt::PointingHandCursor);
        apply_color_button(button, current_color());
        target_grid->addWidget(name, row, 0);
        target_grid->addWidget(button, row, 1);
        connect(button, &QPushButton::clicked, dialog, [dialog, button, label, apply_color_button, apply_color, current_color]() mutable {
            auto *picker = new color_widgets::ColorDialog(dialog);
            picker->setAttribute(Qt::WA_DeleteOnClose);
            picker->setWindowTitle(label);
            picker->setAlphaEnabled(true);
            picker->setButtonMode(color_widgets::ColorDialog::OkApplyCancel);
            picker->setColor(current_color());
            bgl_apply_color_dialog_theme(picker);
            connect(picker, &color_widgets::ColorDialog::colorChanged, dialog, [button, apply_color_button, apply_color](const QColor &color) mutable {
                if (!color.isValid())
                    return;
                apply_color(color);
                apply_color_button(button, color);
            });
            picker->open();
        });
    };
    auto add_timeline_color_row = [&](int row, const QString &label, TitlePreferences::TimelineColorRole role) {
        add_color_button_row(timeline_grid, row, label, [role]() { return TitlePreferences::timeline_color(role); }, [role, update_appearance](const QColor &color) {
            TitlePreferences::set_timeline_color(role, color);
            update_appearance();
        });
    };

    int color_row = 0;
    add_timeline_color_row(color_row++, bgl_tr("OBSTitles.TextLayers"), TitlePreferences::TimelineColorRole::TextLayer);
    add_timeline_color_row(color_row++, bgl_tr("OBSTitles.ClockLayers"), TitlePreferences::TimelineColorRole::ClockLayer);
    add_timeline_color_row(color_row++, bgl_tr("OBSTitles.TickerLayers"), TitlePreferences::TimelineColorRole::TickerLayer);
    add_timeline_color_row(color_row++, bgl_tr("OBSTitles.ObjectLayers"), TitlePreferences::TimelineColorRole::ObjectLayer);
    add_timeline_color_row(color_row++, bgl_tr("OBSTitles.ImageLayers"), TitlePreferences::TimelineColorRole::ImageLayer);
    add_timeline_color_row(color_row++, bgl_tr("OBSTitles.GroupLayers"), TitlePreferences::TimelineColorRole::GroupLayer);
    add_timeline_color_row(color_row++, bgl_tr("OBSTitles.CurrentTime"), TitlePreferences::TimelineColorRole::Current);
    add_timeline_color_row(color_row++, bgl_tr("OBSTitles.PauseMarker"), TitlePreferences::TimelineColorRole::Pause);
    add_timeline_color_row(color_row++, bgl_tr("OBSTitles.LoopStartEnd"), TitlePreferences::TimelineColorRole::Loop);
    timeline_grid->setColumnStretch(2, 1);

    auto add_canvas_color_row = [&](int row, const QString &label, TitlePreferences::CanvasHelperColorRole role) {
        add_color_button_row(canvas_grid, row, label, [role]() { return TitlePreferences::canvas_helper_color(role); }, [role, update_appearance](const QColor &color) {
            TitlePreferences::set_canvas_helper_color(role, color);
            update_appearance();
        });
    };
    int canvas_row = 0;
    add_canvas_color_row(canvas_row++, bgl_tr("OBSTitles.CanvasGuidelines"), TitlePreferences::CanvasHelperColorRole::Guides);
    add_canvas_color_row(canvas_row++, bgl_tr("OBSTitles.ActiveGuidelines"), TitlePreferences::CanvasHelperColorRole::ActiveGuide);
    add_canvas_color_row(canvas_row++, bgl_tr("OBSTitles.RulerMouseIndicators"), TitlePreferences::CanvasHelperColorRole::RulerMouseIndicator);
    add_canvas_color_row(canvas_row++, bgl_tr("OBSTitles.HoverBoundingBoxes"), TitlePreferences::CanvasHelperColorRole::HoverBoundingBox);
    add_canvas_color_row(canvas_row++, bgl_tr("OBSTitles.SelectionBoundingBoxes"), TitlePreferences::CanvasHelperColorRole::SelectionBoundingBox);
    add_canvas_color_row(canvas_row++, bgl_tr("OBSTitles.TextBoundingBoxes"), TitlePreferences::CanvasHelperColorRole::TextBoundingBox);
    add_canvas_color_row(canvas_row++, bgl_tr("OBSTitles.CanvasSnapLines"), TitlePreferences::CanvasHelperColorRole::CanvasSnapLines);
    add_canvas_color_row(canvas_row++, bgl_tr("OBSTitles.ObjectSnapLines"), TitlePreferences::CanvasHelperColorRole::ObjectSnapLines);
    add_canvas_color_row(canvas_row++, bgl_tr("OBSTitles.CanvasBorderColor"), TitlePreferences::CanvasHelperColorRole::CanvasBorder);
    add_canvas_color_row(canvas_row++, bgl_tr("OBSTitles.ActionSafeGuides"), TitlePreferences::CanvasHelperColorRole::ActionSafe);
    add_canvas_color_row(canvas_row++, bgl_tr("OBSTitles.GraphicsSafeGuides"), TitlePreferences::CanvasHelperColorRole::GraphicsSafe);
    add_color_button_row(canvas_grid, canvas_row++, bgl_tr("OBSTitles.SceneMaskObjects"), []() { return TitlePreferences::scene_mask_color(); }, [update_appearance](const QColor &color) {
        TitlePreferences::set_scene_mask_color(color);
        update_appearance();
    });
    canvas_grid->setColumnStretch(2, 1);

    auto *advanced_page = new QWidget(pages);
    auto *advanced_layout = new QVBoxLayout(advanced_page);
    advanced_layout->setContentsMargins(0, 0, 0, 0);
    advanced_layout->setSpacing(10);

    auto *advanced_title = new QLabel(bgl_tr("OBSTitles.Advanced"), advanced_page);
    advanced_title->setFont(title_font);
    advanced_layout->addWidget(advanced_title);

    auto *cache_enabled = new BglSwitch(bgl_tr("OBSTitles.EnableCachingPrerender"), advanced_page);
    cache_enabled->setChecked(CacheManager::instance().cacheEnabled());
    cache_enabled->setToolTip(bgl_tr("OBSTitles.EnableCachingPrerenderTooltip"));
    cache_enabled->setStyleSheet(QStringLiteral("QCheckBox{color:%1;}QCheckBox:disabled{color:%2;}")
                                     .arg(text.name(QColor::HexRgb),
                                          disabled_text.name(QColor::HexRgb)));
    advanced_layout->addWidget(cache_enabled);
    auto *cache_form = new QFormLayout();
    cache_form->setContentsMargins(0, 0, 0, 0);
    cache_form->setSpacing(8);
    auto *ram_limit = new QSpinBox(advanced_page);
    const int maximum_ram_cache_mb = bgs::system_memory::maximum_cache_ram_mb();
    ram_limit->setRange(bgs::system_memory::kMinimumCacheRamMb, maximum_ram_cache_mb);
    ram_limit->setSingleStep(maximum_ram_cache_mb >= 2048 ? 128 : 16);
    ram_limit->setSuffix(QStringLiteral(" MB"));
    ram_limit->setValue(TitlePreferences::cache_ram_limit_mb());
    ram_limit->setToolTip(bgl_tr("OBSTitles.RamCacheLimitTooltip")
        .arg(maximum_ram_cache_mb));
    cache_form->addRow(bgl_tr("OBSTitles.RamCacheLimit"), ram_limit);

    auto *disk_path_row = new QWidget(advanced_page);
    auto *disk_path_layout = new QHBoxLayout(disk_path_row);
    disk_path_layout->setContentsMargins(0, 0, 0, 0);
    disk_path_layout->setSpacing(6);
    auto *disk_path = new QLineEdit(CacheManager::instance().diskCacheLocation(), disk_path_row);
    auto *browse_cache = new QPushButton(bgl_tr("OBSTitles.Browse"), disk_path_row);
    disk_path_layout->addWidget(disk_path, 1);
    disk_path_layout->addWidget(browse_cache);
    cache_form->addRow(bgl_tr("OBSTitles.DiskCacheFolder"), disk_path_row);

    auto *cache_usage = new QLabel(advanced_page);
    cache_usage->setWordWrap(true);
    cache_form->addRow(bgl_tr("OBSTitles.CacheUsage"), cache_usage);

    auto *cache_actions_row = new QWidget(advanced_page);
    auto *cache_actions_layout = new QHBoxLayout(cache_actions_row);
    cache_actions_layout->setContentsMargins(0, 0, 0, 0);
    cache_actions_layout->setSpacing(6);
    auto *clear_cache_now = new QPushButton(bgl_tr("OBSTitles.ClearCacheNow"), cache_actions_row);
    clear_cache_now->setToolTip(bgl_tr("OBSTitles.ClearCacheNowTooltip"));
    cache_actions_layout->addWidget(clear_cache_now);
    cache_actions_layout->addStretch(1);
    cache_form->addRow(QString(), cache_actions_row);

    auto *clear_cache_on_exit = new BglSwitch(bgl_tr("OBSTitles.ClearCacheOnExit"), advanced_page);
    clear_cache_on_exit->setChecked(TitlePreferences::clear_cache_on_exit());
    clear_cache_on_exit->setToolTip(bgl_tr("OBSTitles.ClearCacheOnExitTooltip"));
    clear_cache_on_exit->setStyleSheet(QStringLiteral("QCheckBox{color:%1;}QCheckBox:disabled{color:%2;}")
                                           .arg(text.name(QColor::HexRgb),
                                                disabled_text.name(QColor::HexRgb)));
    cache_form->addRow(QString(), clear_cache_on_exit);

    advanced_layout->addLayout(cache_form);
    advanced_layout->addStretch(1);

    auto *logging_page = new QWidget(pages);
    auto *logging_layout = new QVBoxLayout(logging_page);
    logging_layout->setContentsMargins(0, 0, 0, 0);
    logging_layout->setSpacing(10);

    auto *logging_title = new QLabel(bgl_tr("OBSTitles.Logging"), logging_page);
    logging_title->setFont(title_font);
    logging_layout->addWidget(logging_title);

    auto *logging_enabled = new BglSwitch(bgl_tr("OBSTitles.EnableFileLogging"), logging_page);
    logging_enabled->setChecked(TitlePreferences::logging_enabled());
    logging_enabled->setToolTip(bgl_tr("OBSTitles.FileLoggingTooltip"));
    logging_enabled->setStyleSheet(QStringLiteral("QCheckBox{color:%1;}QCheckBox:disabled{color:%2;}")
                                       .arg(text.name(QColor::HexRgb),
                                            disabled_text.name(QColor::HexRgb)));
    logging_layout->addWidget(logging_enabled);

    auto *logging_form = new QFormLayout();
    logging_form->setContentsMargins(0, 0, 0, 0);
    logging_form->setSpacing(8);

    auto *logging_level = new QComboBox(logging_page);
    logging_level->addItem(bgl_tr("OBSTitles.Off"), 0);
    logging_level->addItem(bgl_tr("OBSTitles.Error"), 1);
    logging_level->addItem(bgl_tr("OBSTitles.Warning"), 2);
    logging_level->addItem(bgl_tr("OBSTitles.Info"), 3);
    logging_level->addItem(bgl_tr("OBSTitles.Debug"), 4);
    logging_level->addItem(bgl_tr("OBSTitles.Trace"), 5);
    const int logging_level_index = logging_level->findData(TitlePreferences::logging_level());
    logging_level->setCurrentIndex(logging_level_index >= 0 ? logging_level_index : 3);
    logging_form->addRow(bgl_tr("OBSTitles.Level"), logging_level);

    auto *logging_path_row = new QWidget(logging_page);
    auto *logging_path_layout = new QHBoxLayout(logging_path_row);
    logging_path_layout->setContentsMargins(0, 0, 0, 0);
    logging_path_layout->setSpacing(6);
    auto *logging_path = new QLineEdit(TitlePreferences::logging_directory(), logging_path_row);
    auto *browse_log = new QPushButton(bgl_tr("OBSTitles.Browse"), logging_path_row);
    logging_path_layout->addWidget(logging_path, 1);
    logging_path_layout->addWidget(browse_log);
    logging_form->addRow(bgl_tr("OBSTitles.LogFolder"), logging_path_row);

    auto *current_session_log = new QLabel(TitleLogger::currentSessionFilePath(), logging_page);
    current_session_log->setTextInteractionFlags(Qt::TextSelectableByMouse);
    current_session_log->setWordWrap(true);
    current_session_log->setStyleSheet(QStringLiteral("color:%1;")
                                           .arg(disabled_text.name(QColor::HexRgb)));
    logging_form->addRow(bgl_tr("OBSTitles.CurrentSessionLog"), current_session_log);

    auto *mirror_obs = new BglSwitch(bgl_tr("OBSTitles.AlsoWriteObsLog"), logging_page);
    mirror_obs->setChecked(TitlePreferences::logging_mirror_to_obs());
    mirror_obs->setStyleSheet(QStringLiteral("QCheckBox{color:%1;}QCheckBox:disabled{color:%2;}")
                                  .arg(text.name(QColor::HexRgb),
                                       disabled_text.name(QColor::HexRgb)));
    logging_form->addRow(QString(), mirror_obs);

    auto *categories_label = new QLabel(bgl_tr("OBSTitles.LoggingCategories"), logging_page);
    QFont logging_section_font = categories_label->font();
    logging_section_font.setBold(true);
    categories_label->setFont(logging_section_font);
    logging_layout->addLayout(logging_form);
    logging_layout->addWidget(categories_label);

    auto *category_scroll = new QScrollArea(logging_page);
    category_scroll->setWidgetResizable(true);
    category_scroll->setFrameShape(QFrame::StyledPanel);
    category_scroll->setMinimumHeight(230);
    auto *category_container = new QWidget(category_scroll);
    auto *category_layout = new QVBoxLayout(category_container);
    category_layout->setContentsMargins(8, 8, 8, 8);
    category_layout->setSpacing(5);
    QVector<QPair<QString, QCheckBox *>> category_checks;
    for (const TitleLogCategory &category : TitleLogger::categories()) {
        auto *check = new BglSwitch(category.display_name, category_container);
        check->setChecked(TitlePreferences::logging_category_enabled(
            category.key, category.default_enabled));
        check->setToolTip(category.description);
        check->setStyleSheet(QStringLiteral("QCheckBox{color:%1;}QCheckBox:disabled{color:%2;}")
                                 .arg(text.name(QColor::HexRgb),
                                      disabled_text.name(QColor::HexRgb)));
        category_layout->addWidget(check);
        category_checks.push_back(qMakePair(category.key, check));
    }
    category_layout->addStretch(1);
    category_scroll->setWidget(category_container);
    logging_layout->addWidget(category_scroll, 1);

    auto *category_actions = new QWidget(logging_page);
    auto *category_actions_layout = new QHBoxLayout(category_actions);
    category_actions_layout->setContentsMargins(0, 0, 0, 0);
    category_actions_layout->setSpacing(6);
    auto *select_all_categories = new QPushButton(bgl_tr("OBSTitles.SelectAll"), category_actions);
    auto *select_no_categories = new QPushButton(bgl_tr("OBSTitles.SelectNone"), category_actions);
    category_actions_layout->addWidget(select_all_categories);
    category_actions_layout->addWidget(select_no_categories);
    category_actions_layout->addStretch(1);
    logging_layout->addWidget(category_actions);

    auto *logging_buttons_row = new QWidget(logging_page);
    auto *logging_buttons_layout = new QHBoxLayout(logging_buttons_row);
    logging_buttons_layout->setContentsMargins(0, 0, 0, 0);
    logging_buttons_layout->setSpacing(6);
    auto *open_log_folder = new QPushButton(bgl_tr("OBSTitles.OpenFolder"), logging_buttons_row);
    auto *clear_log = new QPushButton(bgl_tr("OBSTitles.ClearLog"), logging_buttons_row);
    logging_buttons_layout->addWidget(open_log_folder);
    logging_buttons_layout->addWidget(clear_log);
    logging_buttons_layout->addStretch(1);
    logging_layout->addWidget(logging_buttons_row);

    auto *logging_hint = new QLabel(bgl_tr("OBSTitles.LoggingSessionHint"), logging_page);
    logging_hint->setWordWrap(true);
    logging_hint->setStyleSheet(QStringLiteral("color:%1;").arg(disabled_text.name(QColor::HexRgb)));
    logging_layout->addWidget(logging_hint);

    pages->addWidget(editor_page);
    pages->addWidget(colors_page);
    pages->addWidget(advanced_page);
    pages->addWidget(logging_page);
    content_layout->addWidget(tabs);
    content_layout->addWidget(pages, 1);
    layout->addWidget(content, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    layout->addWidget(buttons);

    tabs->setCurrentRow(0);
    connect(tabs, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    auto refresh_open_editor_autosave = []() {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *open_editor = qobject_cast<TitleEditor *>(widget))
                open_editor->update_autosave_timer();
        }
    };
    connect(autosave_enabled, &QCheckBox::toggled, dialog,
            [autosave_interval, refresh_open_editor_autosave](bool enabled) {
                autosave_interval->setEnabled(enabled);
                TitlePreferences::set_autosave_enabled(enabled);
                refresh_open_editor_autosave();
                BGL_LOG_INFO("Preferences", QStringLiteral("Set autosave enabled=%1").arg(enabled));
            });
    connect(autosave_interval, QOverload<int>::of(&QSpinBox::valueChanged), dialog,
            [refresh_open_editor_autosave](int minutes) {
                TitlePreferences::set_autosave_interval_minutes(minutes);
                refresh_open_editor_autosave();
                BGL_LOG_INFO("Preferences", QStringLiteral("Set autosave interval=%1 minute(s)").arg(minutes));
            });
    connect(cache_enabled, &QCheckBox::toggled, dialog, [](bool enabled) {
        BGL_LOG_INFO("Preferences", QStringLiteral("Set cache enabled=%1").arg(enabled));
        CacheManager::instance().setCacheEnabled(enabled);
    });
    connect(ram_limit, QOverload<int>::of(&QSpinBox::valueChanged), dialog, [](int value) {
        BGL_LOG_INFO("Preferences", QStringLiteral("Set RAM cache limit=%1 MB").arg(value));
        CacheManager::instance().setRamCacheLimitMb(value);
    });
    connect(disk_path, &QLineEdit::editingFinished, dialog, [disk_path]() {
        CacheManager::instance().setDiskCacheLocation(disk_path->text());
        disk_path->setText(CacheManager::instance().diskCacheLocation());
    });
    connect(browse_cache, &QPushButton::clicked, dialog, [dialog, disk_path]() {
        const QString path = QFileDialog::getExistingDirectory(dialog, bgl_tr("OBSTitles.DiskCacheFolder"),
                                                               disk_path->text());
        if (path.isEmpty())
            return;
        disk_path->setText(path);
        BGL_LOG_INFO("Preferences", QStringLiteral("Set disk cache location=%1").arg(path));
        CacheManager::instance().setDiskCacheLocation(path);
    });
    connect(clear_cache_now, &QPushButton::clicked, dialog, []() {
        BGL_LOG_INFO("Preferences", QStringLiteral("Clear cache now requested"));
        CacheManager::instance().clearAll();
    });
    connect(clear_cache_on_exit, &QCheckBox::toggled, dialog, [](bool enabled) {
        BGL_LOG_INFO("Preferences", QStringLiteral("Set clear cache on exit=%1").arg(enabled));
        TitlePreferences::set_clear_cache_on_exit(enabled);
    });
    connect(logging_enabled, &QCheckBox::toggled, dialog, [](bool enabled) {
        TitlePreferences::set_logging_enabled(enabled);
        BGL_LOG_INFO("Preferences", QStringLiteral("Set logging enabled=%1").arg(enabled));
    });
    connect(logging_level, QOverload<int>::of(&QComboBox::currentIndexChanged), dialog, [logging_level](int index) {
        TitlePreferences::set_logging_level(logging_level->itemData(index).toInt());
        BGL_LOG_INFO("Preferences", QStringLiteral("Set logging level=%1").arg(logging_level->itemData(index).toInt()));
    });
    auto apply_log_directory = [logging_path, current_session_log]() {
        const QString requested = logging_path->text().trimmed();
        if (requested.isEmpty()) {
            logging_path->setText(TitlePreferences::logging_directory());
            return;
        }
        TitlePreferences::set_logging_directory(requested);
        const QString directory = TitlePreferences::logging_directory();
        if (!TitleLogger::relocateCurrentSession(directory)) {
            BGL_LOG_WARNING("Preferences", QStringLiteral(
                "Could not relocate current logging session to %1").arg(directory));
        }
        logging_path->setText(directory);
        current_session_log->setText(TitleLogger::currentSessionFilePath());
        BGL_LOG_INFO("Preferences", QStringLiteral("Set log directory=%1").arg(directory));
    };
    connect(logging_path, &QLineEdit::editingFinished, dialog, apply_log_directory);
    connect(browse_log, &QPushButton::clicked, dialog,
            [dialog, logging_path, apply_log_directory]() {
        const QString path = QFileDialog::getExistingDirectory(
            dialog, bgl_tr("OBSTitles.LogFolder"), logging_path->text());
        if (path.isEmpty())
            return;
        logging_path->setText(path);
        apply_log_directory();
    });
    connect(mirror_obs, &QCheckBox::toggled, dialog, [](bool enabled) {
        TitlePreferences::set_logging_mirror_to_obs(enabled);
        BGL_LOG_INFO("Preferences", QStringLiteral("Set logging mirror to OBS=%1").arg(enabled));
    });
    for (const auto &entry : category_checks) {
        const QString category_key = entry.first;
        QCheckBox *check = entry.second;
        connect(check, &QCheckBox::toggled, dialog,
                [category_key](bool enabled) {
            TitlePreferences::set_logging_category_enabled(category_key, enabled);
            BGL_LOG_INFO("Preferences", QStringLiteral(
                "Set logging category %1=%2").arg(category_key).arg(enabled));
        });
    }
    connect(select_all_categories, &QPushButton::clicked, dialog,
            [category_checks]() {
        for (const auto &entry : category_checks)
            entry.second->setChecked(true);
    });
    connect(select_no_categories, &QPushButton::clicked, dialog,
            [category_checks]() {
        for (const auto &entry : category_checks)
            entry.second->setChecked(false);
    });
    connect(open_log_folder, &QPushButton::clicked, dialog, []() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(
            QFileInfo(TitleLogger::currentSessionFilePath()).absolutePath()));
    });
    connect(clear_log, &QPushButton::clicked, dialog, []() {
        QFile file(TitleLogger::currentSessionFilePath());
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            file.close();
        BGL_LOG_INFO("Preferences", QStringLiteral("Cleared current session log"));
    });
    auto format_cache_bytes = [](quint64 bytes) {
        const double mib = (double)bytes / 1024.0 / 1024.0;
        if (mib < 1024.0)
            return QStringLiteral("%1 MB").arg(mib, 0, 'f', 1);
        return QStringLiteral("%1 GB").arg(mib / 1024.0, 0, 'f', 2);
    };
    auto update_cache_usage = [cache_usage, format_cache_bytes]() {
        CacheManager &cache = CacheManager::instance();
        cache_usage->setText(bgl_tr("OBSTitles.CacheUsageSummary")
                                 .arg(format_cache_bytes(cache.ramBytesUsed()),
                                      format_cache_bytes(cache.ramBytesLimit()),
                                      format_cache_bytes(cache.diskBytesUsed())));
    };
    update_cache_usage();
    connect(&CacheManager::instance(), &CacheManager::diagnosticsChanged, dialog, update_cache_usage);
    connect(&CacheManager::instance(), &CacheManager::cacheEnabledChanged, dialog, [cache_enabled](bool enabled) {
        if (cache_enabled->isChecked() == enabled) return;
        QSignalBlocker blocker(cache_enabled);
        cache_enabled->setChecked(enabled);
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);

    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}



void TitleEditor::update_display_refresh_pacing()
{
    if (shutting_down_)
        return;
    QScreen *screen = nullptr;
    if (windowHandle())
        screen = windowHandle()->screen();
    if (!screen)
        screen = QGuiApplication::screenAt(frameGeometry().center());
    if (!screen)
        screen = QApplication::primaryScreen();

    double hz = screen ? screen->refreshRate() : 60.0;
    if (!std::isfinite(hz) || hz < 1.0)
        hz = 60.0;
    hz = std::clamp(hz, 1.0, 1000.0);
    display_refresh_hz_ = hz;
    if (canvas_)
        canvas_->set_display_refresh_rate(hz);

    // Only GUI/canvas editing follows the monitor cadence. Playback remains
    // locked to the project/OBS frame rate through play_timer_.
    if (gui_refresh_timer_) {
        /* This is a maximum editing refresh rate, therefore round upward:
         * e.g. 75 Hz becomes 14 ms rather than 13 ms (>75 Hz). */
        gui_refresh_timer_->setInterval(
            std::max(1, static_cast<int>(std::ceil(1000.0 / hz))));
        if (!gui_refresh_timer_->isActive())
            gui_refresh_timer_->start();
    }

}

bool TitleEditor::eventFilter(QObject *watched, QEvent *event)
{
    if (shutting_down_)
        return QMainWindow::eventFilter(watched, event);
    QWidget *watched_widget = qobject_cast<QWidget *>(watched);
    const bool editor_widget = watched_widget &&
        (watched_widget == this || isAncestorOf(watched_widget));
    if (editor_widget &&
        (event->type() == QEvent::LayoutRequest ||
         event->type() == QEvent::ParentAboutToChange ||
         event->type() == QEvent::ParentChange ||
         event->type() == QEvent::Move ||
         event->type() == QEvent::Resize)) {
        dock_layout_transition_ = true;
        if (layout_settle_timer_)
            layout_settle_timer_->start();
    }
    if (qobject_cast<QDockWidget *>(watched) &&
        (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        QTimer::singleShot(0, this, [this]() { save_editor_layout(); });
    }

    if (watched == this && (event->type() == QEvent::Show || event->type() == QEvent::Move ||
                            event->type() == QEvent::ScreenChangeInternal)) {
        // Defer monitor refresh recalculation until the dock/layout operation has
        // completed; querying/painting through a half-rebuilt widget hierarchy is unsafe.
        if (layout_settle_timer_)
            layout_settle_timer_->start();
        else
            QTimer::singleShot(0, this, &TitleEditor::update_display_refresh_pacing);
    }
    if (auto *spin = qobject_cast<QAbstractSpinBox *>(watched)) {
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
            spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
            if (!spin->property("bgsDefaultValue").isValid()) {
                if (auto *double_spin = qobject_cast<QDoubleSpinBox *>(spin))
                    spin->setProperty("bgsDefaultValue", double_spin->value());
                else if (auto *int_spin = qobject_cast<QSpinBox *>(spin))
                    spin->setProperty("bgsDefaultValue", int_spin->value());
            }
        }
        if (event->type() == QEvent::Wheel && spin->isEnabled()) {
            // Spin boxes intentionally accept wheel changes while hovered, without
            // stealing keyboard focus from the canvas/timeline.
            auto *wheel = static_cast<QWheelEvent *>(event);
            const int steps = wheel->angleDelta().y() / 120;
            if (steps != 0) {
                for (int i = 0; i < std::abs(steps); ++i)
                    steps > 0 ? spin->stepUp() : spin->stepDown();
                wheel->accept();
                return true;
            }
        }
    }
    if (watched == title_lbl_ && event->type() == QEvent::MouseButtonDblClick) {
        auto *mouse_event = static_cast<QMouseEvent *>(event);
        if (mouse_event->button() == Qt::LeftButton) {
            begin_title_name_edit();
            event->accept();
            return true;
        }
    }

    if (watched == title_name_edit_) {
        if (event->type() == QEvent::KeyPress) {
            auto *key_event = static_cast<QKeyEvent *>(event);
            if (key_event->key() == Qt::Key_Escape) {
                commit_title_name_edit(false);
                key_event->accept();
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            commit_title_name_edit(false);
            return false;
        }
    }

    QWidget *focused_widget = QApplication::focusWidget();
    QWidget *active_window = QApplication::activeWindow();
    const bool focus_in_editor = focused_widget &&
        (focused_widget == this || isAncestorOf(focused_widget));
    const bool modal_child_active = active_window && active_window != this &&
        active_window->isWindow() && active_window->windowModality() != Qt::NonModal;
    /* A QWindow-backed canvas can own the key event without becoming a QWidget
     * descendant. Route commands whenever the focused/watched object belongs to
     * this editor, or this editor is the active top-level window. Only modal
     * child windows keep their own shortcut scope. */
    const bool watched_in_editor = watched_widget &&
        (watched_widget == this || isAncestorOf(watched_widget));
    const bool editor_is_active = !modal_child_active &&
        (focus_in_editor || watched_in_editor || active_window == this);
    if ((event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) && editor_is_active) {
        auto *key_event = static_cast<QKeyEvent *>(event);
        const bool shortcut_override = event->type() == QEvent::ShortcutOverride;
        auto *widget = qobject_cast<QWidget *>(watched);
        const bool in_editor = editor_is_active ||
            (widget && (widget == this || isAncestorOf(widget)));
        const bool editing_value = editor_focus_accepts_text(focused_widget);
        const Qt::KeyboardModifiers shortcut_modifiers =
            key_event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier |
                                      Qt::AltModifier | Qt::MetaModifier);
        const Qt::KeyboardModifiers tool_modifiers =
            shortcut_modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);

        if (shortcut_override && in_editor && !editing_value) {
            /* Claim every editor command during ShortcutOverride, but never
             * execute it here. QWindow-backed canvas/viewport controls can
             * otherwise consume Space or tool keys before the real KeyPress
             * reaches this filter. Executing commands during ShortcutOverride
             * is also incorrect and can cause duplicate/uneven activation. */
            const bool plain_tool_key =
                tool_modifiers == Qt::NoModifier &&
                (key_event->key() == Qt::Key_V ||
                 key_event->key() == Qt::Key_A ||
                 key_event->key() == Qt::Key_E ||
                 key_event->key() == Qt::Key_P ||
                 key_event->key() == Qt::Key_M ||
                 key_event->key() == Qt::Key_L ||
                 key_event->key() == Qt::Key_T ||
                 key_event->key() == Qt::Key_I ||
                 key_event->key() == Qt::Key_G);
            const bool editor_command =
                key_event->matches(QKeySequence::Undo) ||
                key_event->matches(QKeySequence::Redo) ||
                (key_event->key() == Qt::Key_Z &&
                 shortcut_modifiers == (Qt::ControlModifier | Qt::ShiftModifier)) ||
                (key_event->key() == Qt::Key_Y &&
                 shortcut_modifiers == Qt::ControlModifier) ||
                key_event->matches(QKeySequence::New) ||
                key_event->matches(QKeySequence::Save) ||
                key_event->matches(QKeySequence::Copy) ||
                key_event->matches(QKeySequence::Cut) ||
                key_event->matches(QKeySequence::Paste) ||
                key_event->key() == Qt::Key_Delete ||
                key_event->key() == Qt::Key_Backspace ||
                (key_event->key() == Qt::Key_D &&
                 shortcut_modifiers == Qt::ControlModifier) ||
                (key_event->key() == Qt::Key_G &&
                 (shortcut_modifiers == Qt::ControlModifier ||
                  shortcut_modifiers == (Qt::ControlModifier | Qt::ShiftModifier))) ||
                (key_event->key() == Qt::Key_R &&
                 shortcut_modifiers == Qt::ControlModifier) ||
                (key_event->key() == Qt::Key_QuoteLeft &&
                 shortcut_modifiers == Qt::ControlModifier) ||
                (key_event->key() == Qt::Key_Semicolon &&
                 shortcut_modifiers == Qt::ControlModifier) ||
                (key_event->key() == Qt::Key_Space &&
                 shortcut_modifiers == Qt::NoModifier &&
                 QApplication::mouseButtons() == Qt::NoButton) ||
                plain_tool_key;
            if (editor_command) {
                key_event->accept();
                return true;
            }
        }

