#include "title-editor-internal.h"

TitlePropertiesPanel::TitlePropertiesPanel(QWidget *parent)
    : QGroupBox(parent)
{
    apply_theme_style();

    auto *fl = new QFormLayout(this);
    fl->setContentsMargins(14, 12, 14, 12);
    fl->setHorizontalSpacing(8);
    fl->setVerticalSpacing(6);
    fl->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    fl->setFormAlignment(Qt::AlignTop);
    fl->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    auto add_form_row = [this](QFormLayout *form, const QString &label_text, QWidget *field) {
        auto *label = new NumericDragLabel(label_text, field, form->parentWidget(),
                                           [this]() {
                                               if (loading_values_) return;
                                               numeric_label_dragging_ = true;
                                               emit title_changed(true);
                                           },
                                           [this]() {
                                               if (loading_values_) return;
                                               numeric_label_dragging_ = false;
                                               emit title_changed(true);
                                           });
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->addRow(label, field);
    };

    spn_duration_ = new TimecodeSpinBox(this);
    spn_duration_->setRange(0.1, 3600.0);
    spn_duration_->setFixedHeight(22);
    add_form_row(fl, bgl_tr("OBSTitles.LengthLabel"), spn_duration_);

    cmb_cue_end_behavior_ = new QComboBox(this);
    cmb_cue_end_behavior_->addItem(bgl_tr("OBSTitles.CueEndShowLastFrame"), 0);
    cmb_cue_end_behavior_->addItem(bgl_tr("OBSTitles.CueEndShowNothing"), 1);
    cmb_cue_end_behavior_->addItem(bgl_tr("OBSTitles.CueEndShowFirstFrame"), 2);
    cmb_cue_end_behavior_->setToolTip(bgl_tr("OBSTitles.CueEndBehaviorTooltip"));
    cmb_cue_end_behavior_->setFixedHeight(22);
    add_form_row(fl, bgl_tr("OBSTitles.CueEndBehaviorLabel"), cmb_cue_end_behavior_);

    auto *playback_row = new QWidget(this);
    auto *playback_layout = new QHBoxLayout(playback_row);
    playback_layout->setContentsMargins(0, 0, 0, 0);
    playback_layout->setSpacing(3);
    grp_playback_mode_ = new QButtonGroup(playback_row);
    grp_playback_mode_->setExclusive(true);
    auto add_playback_button = [this, playback_row, playback_layout](int id, const char *icon_name,
                                                                     const QString &text) {
        auto *button = new QToolButton(playback_row);
        button->setCheckable(true);
        button->setIcon(obs_icon(icon_name));
        button->setIconSize(QSize(16, 16));
        button->setToolTip(text);
        button->setAccessibleName(text);
        button->setFixedSize(30, 24);
        grp_playback_mode_->addButton(button, id);
        playback_layout->addWidget(button);
    };
    add_playback_button(0, "play-once.svg", bgl_tr("OBSTitles.PlayOnce"));
    add_playback_button(1, "restart-loop.svg", bgl_tr("OBSTitles.RestartLoop"));
    add_playback_button(2, "ping-pong-loop.svg", bgl_tr("OBSTitles.PingPongLoop"));
    add_playback_button(3, "pause-at-timeline-position.svg", bgl_tr("OBSTitles.PauseAtTimelinePosition"));
    playback_layout->addStretch(1);
    add_form_row(fl, bgl_tr("OBSTitles.PlaybackModeLabel"), playback_row);

    spn_pause_frame_ = new TimecodeSpinBox(this);
    spn_pause_frame_->setRange(0.0, 3600.0);
    spn_pause_frame_->setToolTip(bgl_tr("OBSTitles.PauseFrameTooltip"));
    spn_pause_frame_->setFixedHeight(22);
    add_form_row(fl, bgl_tr("OBSTitles.PauseFrameLabel"), spn_pause_frame_);


    spn_loop_start_ = new TimecodeSpinBox(this);
    spn_loop_start_->setRange(0.0, 3600.0);
    spn_loop_start_->setToolTip(bgl_tr("OBSTitles.LoopStartTooltip"));
    spn_loop_start_->setFixedHeight(22);

    spn_loop_end_ = new TimecodeSpinBox(this);
    spn_loop_end_->setRange(0.0, 3600.0);
    spn_loop_end_->setToolTip(bgl_tr("OBSTitles.LoopEndTooltip"));
    spn_loop_end_->setFixedHeight(22);

    loop_area_row_ = new QWidget(this);
    auto *loop_area_layout = new QHBoxLayout(loop_area_row_);
    loop_area_layout->setContentsMargins(0, 0, 0, 0);
    loop_area_layout->setSpacing(4);
    auto *loop_start_label = new QLabel(bgl_tr("OBSTitles.StartLabel"), loop_area_row_);
    auto *loop_end_label = new QLabel(bgl_tr("OBSTitles.EndLabel"), loop_area_row_);
    spn_loop_start_->setMinimumWidth(78);
    spn_loop_end_->setMinimumWidth(78);
    loop_area_layout->addWidget(loop_start_label);
    loop_area_layout->addWidget(spn_loop_start_, 1);
    loop_area_layout->addWidget(loop_end_label);
    loop_area_layout->addWidget(spn_loop_end_, 1);
    add_form_row(fl, bgl_tr("OBSTitles.LoopAreaLabel"), loop_area_row_);

    connect(grp_playback_mode_, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
            this, [this](QAbstractButton *button) {
                if (!title_ || loading_values_ || !button) return;
                int selection = grp_playback_mode_->id(button);
                if (selection == 1 || selection == 2) {
                    title_->playback_mode = 1;
                    title_->loop_type = selection == 2 ? 1 : 0;
                } else {
                    title_->playback_mode = selection == 3 ? 2 : 0;
                }
                if (title_->playback_mode == 2 && title_->pause_time <= 0.0)
                    title_->pause_time = title_->duration;
                load_values();
                emit title_changed(!numeric_label_dragging_);
            });

    connect(spn_pause_frame_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!title_ || loading_values_) return;
                title_->pause_time = std::clamp(v, 0.0, title_->duration);
                load_values();
                emit title_changed(!numeric_label_dragging_);
            });

    connect(cmb_cue_end_behavior_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (!title_ || loading_values_ || !cmb_cue_end_behavior_) return;
                title_->cue_end_behavior = std::clamp(cmb_cue_end_behavior_->currentData().toInt(), 0, 2);
                emit title_changed(!numeric_label_dragging_);
            });


    connect(spn_duration_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!title_ || loading_values_) return;
                double old_duration = title_->duration;
                title_->duration = v;
                for (auto &layer : title_->layers) {
                    if (std::abs(layer->out_time - old_duration) < 0.001 || layer->out_time > v)
                        layer->out_time = v;
                }
                title_->loop_start = std::clamp(title_->loop_start, 0.0, title_->duration);
                title_->loop_end = std::clamp(title_->loop_end, title_->loop_start, title_->duration);
                title_->pause_time = std::clamp(title_->pause_time, 0.0, title_->duration);
                load_values();
                emit title_changed(!numeric_label_dragging_);
            });

    connect(spn_loop_start_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!title_ || loading_values_) return;
                title_->loop_start = std::clamp(v, 0.0, title_->duration);
                title_->loop_end = std::clamp(title_->loop_end, title_->loop_start, title_->duration);
                load_values();
                emit title_changed(!numeric_label_dragging_);
            });

    connect(spn_loop_end_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) {
                if (!title_ || loading_values_) return;
                title_->loop_end = std::clamp(v, title_->loop_start, title_->duration);
                load_values();
                emit title_changed(!numeric_label_dragging_);
            });
}

bool TitlePropertiesPanel::event(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange)
        apply_theme_style();
    return QGroupBox::event(event);
}

void TitlePropertiesPanel::apply_theme_style()
{
    if (applying_theme_style_)
        return;

    const QPalette pal = palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QColor base = pal.color(QPalette::Base);
    const QColor button = pal.color(QPalette::Button);
    const QColor button_text = pal.color(QPalette::ButtonText);
    const QColor border = pal.color(QPalette::Mid);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlighted_text = pal.color(QPalette::HighlightedText);
    const QColor subtle_text = text.lightness() < 128 ? text.lighter(135) : text.darker(135);
    const QColor section_bg = window.lightness() < 128 ? window.lighter(112) : window.darker(104);
    const QColor hover = button.lightness() < 128 ? button.lighter(125) : button.darker(110);

    const QString theme_style = QStringLiteral(
        "QGroupBox{color:%1;background:%2;border:none;margin:0;padding:0;font-size:14px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:14px;top:8px;padding:0;background:transparent;}"
        "QDoubleSpinBox,QSpinBox,QComboBox{color:%4;background:%5;border:1px solid %3;"
        "border-radius:2px;padding:1px 3px;selection-background-color:%9;}"
        "QDoubleSpinBox:focus,QSpinBox:focus,QComboBox:focus{border-color:%9;}"
        "QToolButton{color:%6;background:%7;border:1px solid %3;"
        "border-radius:2px;padding:2px;}"
        "QToolButton:hover{background:%8;border-color:%3;}"
        "QToolButton:checked{background:%9;color:%10;border-color:%9;}"
        "QLabel{color:%11;font-size:10px;background:transparent;}")
        .arg(text.name(QColor::HexRgb),
             section_bg.name(QColor::HexRgb),
             border.name(QColor::HexRgb),
             pal.color(QPalette::Text).name(QColor::HexRgb),
             base.name(QColor::HexRgb),
             button_text.name(QColor::HexRgb),
             button.name(QColor::HexRgb),
             hover.name(QColor::HexRgb),
             highlight.name(QColor::HexRgb),
             highlighted_text.name(QColor::HexRgb),
             subtle_text.name(QColor::HexRgb));

    if (styleSheet() == theme_style)
        return;

    applying_theme_style_ = true;
    setStyleSheet(theme_style);
    applying_theme_style_ = false;
}

void TitlePropertiesPanel::set_title(std::shared_ptr<Title> t)
{
    const bool same_title = title_ && t && title_.get() == t.get();
    title_ = t;
    setTitle(QString());

    if (same_title && !loading_values_) {
        const double duration = title_ ? title_->duration : 5.0;
        const double loop_start = title_ ? title_->loop_start : 1.0;
        const double loop_end = title_ ? title_->loop_end : 4.0;
        const int playback_mode = title_ ? std::clamp(title_->playback_mode, 0, 2) : 0;
        const int loop_type = title_ ? std::clamp(title_->loop_type, 0, 1) : 0;
        const int cue_end_behavior = title_ ? std::clamp(title_->cue_end_behavior, 0, 2) : 0;
        const int playback_selection = playback_mode == 1 ? (loop_type == 1 ? 2 : 1)
                                                           : (playback_mode == 2 ? 3 : 0);
        const double clamped_loop_start = std::clamp(loop_start, 0.0, duration);
        const double clamped_loop_end = std::clamp(loop_end, clamped_loop_start, duration);
        const double pause_time = title_ ? std::clamp(title_->pause_time, 0.0, duration) : 0.0;
        const int cue_index = cmb_cue_end_behavior_ ? cmb_cue_end_behavior_->findData(cue_end_behavior) : -1;
        auto same_value = [](QDoubleSpinBox *spin, double value) {
            return spin && std::abs(spin->value() - value) < 0.000001;
        };

        if (same_value(spn_duration_, duration) &&
            same_value(spn_loop_start_, clamped_loop_start) &&
            same_value(spn_loop_end_, clamped_loop_end) &&
            same_value(spn_pause_frame_, pause_time) &&
            (!grp_playback_mode_ || grp_playback_mode_->checkedId() == playback_selection) &&
            (!cmb_cue_end_behavior_ ||
             cmb_cue_end_behavior_->currentIndex() == (cue_index >= 0 ? cue_index : 0))) {
            return;
        }
    }

    load_values();
}

void TitlePropertiesPanel::load_values()
{
    loading_values_ = true;
    double duration = title_ ? title_->duration : 5.0;
    double loop_start = title_ ? title_->loop_start : 1.0;
    double loop_end = title_ ? title_->loop_end : 4.0;
    int playback_mode = title_ ? std::clamp(title_->playback_mode, 0, 2) : 0;
    int loop_type = title_ ? std::clamp(title_->loop_type, 0, 1) : 0;
    int cue_end_behavior = title_ ? std::clamp(title_->cue_end_behavior, 0, 2) : 0;
    int playback_selection = playback_mode == 1 ? (loop_type == 1 ? 2 : 1)
                                                : (playback_mode == 2 ? 3 : 0);
    double pause_time = title_ ? std::clamp(title_->pause_time, 0.0, duration) : 0.0;

    if (auto *button = grp_playback_mode_->button(playback_selection))
        button->setChecked(true);
    spn_duration_->setValue(duration);
    if (cmb_cue_end_behavior_) {
        int idx = cmb_cue_end_behavior_->findData(cue_end_behavior);
        cmb_cue_end_behavior_->setCurrentIndex(idx >= 0 ? idx : 0);
    }
    spn_loop_start_->setMaximum(duration);
    spn_loop_end_->setMaximum(duration);
    spn_loop_start_->setValue(std::clamp(loop_start, 0.0, duration));
    spn_loop_end_->setValue(std::clamp(loop_end, std::clamp(loop_start, 0.0, duration), duration));
    spn_pause_frame_->setMaximum(duration);
    spn_pause_frame_->setValue(pause_time);

    bool show_loop = playback_mode == 1;
    bool show_pause = playback_mode == 2;
    auto *form = qobject_cast<QFormLayout *>(layout());
    loop_area_row_->setVisible(show_loop);
    if (form) if (auto *label = qobject_cast<QWidget *>(form->labelForField(loop_area_row_))) label->setVisible(show_loop);
    spn_pause_frame_->setVisible(show_pause);
    if (form) if (auto *label = qobject_cast<QWidget *>(form->labelForField(spn_pause_frame_))) label->setVisible(show_pause);
    loading_values_ = false;
}

/* ══════════════════════════════════════════════════════════════════
 *  PropertiesPanel
 * ══════════════════════════════════════════════════════════════════ */
