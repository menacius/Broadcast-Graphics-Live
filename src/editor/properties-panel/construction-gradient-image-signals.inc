    cmb_kerning_mode_ = mk_combo({bgl_tr("OBSTitles.KerningMetrics"), bgl_tr("OBSTitles.KerningOptical"), bgl_tr("OBSTitles.KerningManual")}, {0, 1, 2});
    spn_kerning_value_ = mk_dspin(-100.0, 500.0, 1.0);
    spn_kerning_value_->setSuffix(" px");
    spn_kerning_value_->setToolTip(bgl_tr("OBSTitles.ManualKerningTooltip"));
    spn_text_leading_ = mk_dspin(-200.0, 500.0, 1.0);
    spn_text_leading_->setSuffix(" px");
    spn_text_leading_->setToolTip(bgl_tr("OBSTitles.LeadingTooltip"));
    spn_char_tracking_ = mk_dspin(-100.0, 500.0, 1.0);
    spn_char_tracking_->setSuffix(" px");
    spn_char_tracking_->setToolTip(bgl_tr("OBSTitles.TrackingTooltip"));
    spn_char_scale_x_ = mk_dspin(10.0, 500.0, 1.0);
    spn_char_scale_x_->setSuffix("%");
    spn_char_scale_y_ = mk_dspin(10.0, 500.0, 1.0);
    spn_char_scale_y_->setSuffix("%");
    spn_baseline_shift_ = mk_dspin(-500.0, 500.0, 1.0);
    spn_baseline_shift_->setSuffix(" px");
    btn_text_color_ = new QPushButton(inner);
    btn_text_color_->setFixedHeight(22);
    btn_kf_font_size_ = mk_kf_button("Toggle Size keyframe");
    btn_kf_char_scale_x_ = mk_kf_button("Toggle H Scale keyframe");
    btn_kf_char_scale_y_ = mk_kf_button("Toggle V Scale keyframe");
    btn_kf_char_tracking_ = mk_kf_button("Toggle Tracking keyframe");
    btn_kf_baseline_shift_ = mk_kf_button("Toggle Baseline keyframe");
    btn_kf_text_color_ = mk_kf_button(bgl_tr("OBSTitles.ToggleTextColorKeyframe"));

    char_grid->addWidget(grid_label(bgl_tr("OBSTitles.TextLabel"), text_box_), 0, 0);
    char_grid->addWidget(txt_content_, 0, 1, 1, 3);
    char_grid->addWidget(btn_text_external_binding_, 0, 4, Qt::AlignTop);
    // Font family and style are the primary Character controls. Keep them
    // label-free and span the complete inspector width so long family/style
    // names do not become truncated by a redundant label column.
    cmb_font_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    cmb_font_style_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    char_grid->addWidget(cmb_font_, 1, 0, 1, 5);
    char_grid->addWidget(cmb_font_style_, 2, 0, 1, 5);
    add_grid_field(char_grid, 3, 0, "Size", with_kf(spn_size_, btn_kf_font_size_));
    add_grid_field(char_grid, 3, 1, "Leading", spn_text_leading_);
    add_grid_field(char_grid, 4, 0, "Kerning", cmb_kerning_mode_);
    add_grid_field(char_grid, 4, 1, "Value", spn_kerning_value_);
    add_grid_field(char_grid, 5, 0, "H Scale", with_kf(spn_char_scale_x_, btn_kf_char_scale_x_));
    add_grid_field(char_grid, 5, 1, "V Scale", with_kf(spn_char_scale_y_, btn_kf_char_scale_y_));
    add_grid_field(char_grid, 6, 0, "Tracking", with_kf(spn_char_tracking_, btn_kf_char_tracking_));
    add_grid_field(char_grid, 6, 1, "Baseline", with_kf(spn_baseline_shift_, btn_kf_baseline_shift_));
    row_text_color_ = with_kf(btn_text_color_, btn_kf_text_color_);
    add_full_width_field(char_grid, 7, "Fill Color", row_text_color_);
    bgl_add_panel_section(vl, text_box_);

    /* The animator editor is a normal properties-panel section. It writes the
     * same AnimatedProperty instances exposed by the layer timeline. */
    build_text_animator_section(inner, vl);

    /* ── Type Options ── */
    type_options_box_ = new QGroupBox("Type", inner);
    type_options_box_->setObjectName(QStringLiteral("BroadcastGraphicsLiveTextTypePanel"));
    type_options_box_->setStyleSheet(QStringLiteral(
        "QGroupBox#BroadcastGraphicsLiveTextTypePanel{color:%1;background:%2;border:1px solid %3;"
        "border-radius:2px;margin-top:16px;padding:0;font-size:14px;}"
        "QGroupBox#BroadcastGraphicsLiveTextTypePanel::title{subcontrol-origin:margin;left:14px;top:2px;padding:0 4px;background:%2;}")
        .arg(panel_text_name, section_bg_name, border_name));
    auto *type_grid = new QGridLayout(type_options_box_);
    type_grid->setContentsMargins(10, 8, 10, 10);
    type_grid->setHorizontalSpacing(4);
    type_grid->setVerticalSpacing(4);
    chk_bold_ = mk_type_icon_button("text-style-bold.svg", bgl_tr("OBSTitles.Bold"));
    chk_italic_ = mk_type_icon_button("text-style-italic.svg", bgl_tr("OBSTitles.Italic"));
    chk_font_kerning_ = mk_type_button("K", bgl_tr("OBSTitles.Kerning"));
    chk_font_kerning_->setToolTip(bgl_tr("OBSTitles.KerningTooltip"));
    btn_all_caps_ = mk_type_icon_button("text-style-allcaps.svg", "All Caps");
    btn_small_caps_ = mk_type_icon_button("text-style-smallcaps.svg", "Small Caps");
    btn_superscript_ = mk_type_button("x²", "Superscript");
    btn_subscript_ = mk_type_button("x₂", "Subscript");
    btn_underline_ = mk_type_button("U", "Underline");
    btn_strikethrough_ = mk_type_button("S", "Strikethrough");
    btn_ligatures_ = mk_type_button("fi", "Ligatures");
    btn_stylistic_alternates_ = mk_type_button("Sw", "Stylistic Alternates");
    btn_fractions_ = mk_type_button("½", "Fractions");
    btn_opentype_features_ = mk_type_button("OT", "OpenType Features");
    QList<QToolButton *> type_buttons{chk_bold_, chk_italic_, btn_all_caps_, btn_small_caps_, btn_superscript_,
                                      btn_subscript_, btn_underline_, btn_strikethrough_, btn_ligatures_, btn_stylistic_alternates_,
                                      btn_fractions_, btn_opentype_features_, chk_font_kerning_};
    for (int i = 0; i < type_buttons.size(); ++i) type_grid->addWidget(type_buttons[i], i / 5, i % 5);
    type_grid->setColumnStretch(5, 1);
    bgl_add_panel_section(vl, type_options_box_);

    /* ── Paragraph ── */
    paragraph_box_ = new QGroupBox("Paragraph", inner);
    paragraph_box_->setStyleSheet(QStringLiteral(
        "QGroupBox{color:%1;background:%2;border:none;margin:0;padding:0;font-size:14px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:14px;top:8px;padding:0;background:transparent;}"
        "QLabel{color:%3;font-size:10px;background:transparent;}")
        .arg(panel_text_name, section_bg_name, subtle_text_name));
    auto *paragraph_layout = new QVBoxLayout(paragraph_box_);
    paragraph_layout->setContentsMargins(10, 8, 10, 10);
    paragraph_layout->setSpacing(7);

    auto add_paragraph_button = [&](QHBoxLayout *layout, QButtonGroup *group,
                                    const char *icon_name, const QString &tip, int id) {
        auto *button = mk_paragraph_alignment_button(icon_name, tip);
        group->addButton(button, id);
        layout->addWidget(button);
        return button;
    };
    auto add_paragraph_gap = [](QHBoxLayout *layout) {
        auto *gap = new QWidget();
        gap->setFixedWidth(12);
        layout->addWidget(gap);
    };
    auto mk_paragraph_spin = [&]() {
        auto *spin = mk_dspin(-10000.0, 10000.0, 1.0);
        spin->setSuffix(QStringLiteral(" pt"));
        spin->setDecimals(0);
        spin->setFixedWidth(94);
        return spin;
    };
    auto add_metric_control = [&](QGridLayout *grid, int row, int column,
                                  const char *icon_name, const QString &tip, QDoubleSpinBox *spin, QWidget *field) {
        auto *icon = new NumericDragLabel(QString(), field, paragraph_box_,
                                          [this]() {
                                              if (loading_values_) return;
                                              numeric_label_dragging_ = true;
                                              emit property_changed(true);
                                          },
                                          [this]() {
                                              if (loading_values_) return;
                                              numeric_label_dragging_ = false;
                                              emit property_changed(true);
                                          });
        icon->setPixmap(obs_icon(icon_name).pixmap(16, 16));
        icon->setToolTip(QStringLiteral("%1\n%2").arg(tip, bgl_tr("OBSTitles.DragNumericLabelTooltip")));
        icon->setAccessibleName(tip);
        icon->setFixedWidth(20);
        icon->setAlignment(Qt::AlignCenter);
        spin->setToolTip(tip);
        grid->addWidget(icon, row, column * 2, Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(field, row, column * 2 + 1);
    };

    auto *horizontal_buttons = new QWidget(paragraph_box_);
    auto *horizontal_button_layout = new QHBoxLayout(horizontal_buttons);
    horizontal_button_layout->setContentsMargins(0, 0, 0, 0);
    horizontal_button_layout->setSpacing(4);
    grp_text_align_ = new QButtonGroup(horizontal_buttons);
    grp_text_align_->setExclusive(true);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "text-align-left.svg", bgl_tr("OBSTitles.AlignLeft"), 0);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "text-align-center.svg", bgl_tr("OBSTitles.AlignCenter"), 1);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "text-align-right.svg", bgl_tr("OBSTitles.AlignRight"), 2);
    add_paragraph_gap(horizontal_button_layout);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "text-justify-left.svg", bgl_tr("OBSTitles.JustifyLastLeft"), 3);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "text-justify-center.svg", bgl_tr("OBSTitles.JustifyLastCenter"), 4);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "text-justify-right.svg", bgl_tr("OBSTitles.JustifyLastRight"), 5);
    add_paragraph_gap(horizontal_button_layout);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "text-justify-full.svg", bgl_tr("OBSTitles.JustifyAll"), 6);
    horizontal_button_layout->addStretch(1);
    paragraph_layout->addWidget(horizontal_buttons);

    auto *vertical_buttons = new QWidget(paragraph_box_);
    auto *vertical_button_layout = new QHBoxLayout(vertical_buttons);
    vertical_button_layout->setContentsMargins(0, 0, 0, 0);
    vertical_button_layout->setSpacing(4);
    grp_text_valign_ = new QButtonGroup(vertical_buttons);
    grp_text_valign_->setExclusive(true);
    add_paragraph_button(vertical_button_layout, grp_text_valign_, "text-verticalalign-top.svg", bgl_tr("OBSTitles.AlignTop"), 0);
    add_paragraph_button(vertical_button_layout, grp_text_valign_, "text-verticalalign-center.svg", bgl_tr("OBSTitles.AlignMiddle"), 1);
    add_paragraph_button(vertical_button_layout, grp_text_valign_, "text-verticalalign-bottom.svg", bgl_tr("OBSTitles.AlignBottom"), 2);
    add_paragraph_button(vertical_button_layout, grp_text_valign_, "text-verticalalign-justify.svg", bgl_tr("OBSTitles.DistributeVertically"), 3);
    vertical_button_layout->addStretch(1);
    paragraph_layout->addWidget(vertical_buttons);

    spn_paragraph_indent_left_ = mk_paragraph_spin();
    spn_paragraph_indent_right_ = mk_paragraph_spin();
    spn_paragraph_indent_first_line_ = mk_paragraph_spin();
    btn_kf_paragraph_indent_left_ = mk_kf_button(bgl_tr("OBSTitles.ToggleParagraphIndentLeftKeyframe"));
    btn_kf_paragraph_indent_right_ = mk_kf_button(bgl_tr("OBSTitles.ToggleParagraphIndentRightKeyframe"));
    btn_kf_paragraph_indent_first_line_ = mk_kf_button(bgl_tr("OBSTitles.ToggleParagraphIndentFirstLineKeyframe"));
    auto *paragraph_indent_left_field = with_kf(spn_paragraph_indent_left_, btn_kf_paragraph_indent_left_);
    auto *paragraph_indent_right_field = with_kf(spn_paragraph_indent_right_, btn_kf_paragraph_indent_right_);
    auto *paragraph_indent_first_line_field = with_kf(spn_paragraph_indent_first_line_, btn_kf_paragraph_indent_first_line_);
    spn_paragraph_space_before_ = mk_paragraph_spin();
    spn_paragraph_space_after_ = mk_paragraph_spin();
    btn_kf_paragraph_space_before_ = mk_kf_button("Toggle Space Before Paragraph keyframe");
    btn_kf_paragraph_space_after_ = mk_kf_button("Toggle Space After Paragraph keyframe");
    auto *paragraph_space_before_field = with_kf(spn_paragraph_space_before_, btn_kf_paragraph_space_before_);
    auto *paragraph_space_after_field = with_kf(spn_paragraph_space_after_, btn_kf_paragraph_space_after_);
    auto *metric_grid = new QGridLayout();
    metric_grid->setContentsMargins(0, 0, 0, 0);
    metric_grid->setHorizontalSpacing(8);
    metric_grid->setVerticalSpacing(4);
    metric_grid->setColumnStretch(1, 1);
    metric_grid->setColumnStretch(3, 1);
    add_metric_control(metric_grid, 0, 0, "paragraph-indent-left.svg", bgl_tr("OBSTitles.ParagraphIndentLeft"), spn_paragraph_indent_left_, paragraph_indent_left_field);
    add_metric_control(metric_grid, 0, 1, "paragraph-indent-right.svg", bgl_tr("OBSTitles.ParagraphIndentRight"), spn_paragraph_indent_right_, paragraph_indent_right_field);
    add_metric_control(metric_grid, 1, 0, "paragraph-indent-first-line.svg", bgl_tr("OBSTitles.ParagraphIndentFirstLine"), spn_paragraph_indent_first_line_, paragraph_indent_first_line_field);
    add_metric_control(metric_grid, 2, 0, "paragraph-space-before.svg", bgl_tr("OBSTitles.ParagraphSpaceBefore"), spn_paragraph_space_before_, paragraph_space_before_field);
    add_metric_control(metric_grid, 2, 1, "paragraph-space-after.svg", bgl_tr("OBSTitles.ParagraphSpaceAfter"), spn_paragraph_space_after_, paragraph_space_after_field);
    paragraph_layout->addLayout(metric_grid);

    chk_paragraph_hyphenate_ = new BglSwitch(bgl_tr("OBSTitles.Hyphenate"), paragraph_box_);
    style_checkbox(chk_paragraph_hyphenate_);
    paragraph_layout->addWidget(chk_paragraph_hyphenate_);

    bgl_add_panel_section(vl, paragraph_box_);

    /* ── Dynamic Text ── */
    dynamic_text_box_ = new QGroupBox("Text Flow", inner);
    dynamic_text_box_->setStyleSheet(QStringLiteral(
        "QGroupBox{color:%1;background:%2;border:none;margin:0;padding:0;font-size:14px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:14px;top:8px;padding:0;background:transparent;}"
        "QLabel{color:%3;font-size:10px;background:transparent;}")
        .arg(panel_text_name, section_bg_name, subtle_text_name));
    auto *dynamic_form = new QFormLayout(dynamic_text_box_);
    style_form(dynamic_form);
    dynamic_form->setContentsMargins(10, 8, 10, 10);
    dynamic_form->setHorizontalSpacing(8);
    dynamic_form->setVerticalSpacing(6);
    cmb_text_style_ = new QComboBox(inner);
    cmb_text_style_->addItem(bgl_tr("OBSTitles.Normal"), 0);
    cmb_text_style_->addItem(bgl_tr("OBSTitles.AllCaps"), 1);
    cmb_text_style_->addItem(bgl_tr("OBSTitles.SmallCaps"), 2);
    cmb_text_style_->addItem(bgl_tr("OBSTitles.Superscript"), 3);
    cmb_text_style_->addItem(bgl_tr("OBSTitles.Subscript"), 4);
    cmb_text_style_->setToolTip(bgl_tr("OBSTitles.TextStyleTooltip"));
    cmb_text_style_->setFixedHeight(22);
    cmb_text_style_->setStyleSheet(control_style);
    cmb_text_overflow_ = new QComboBox(inner);
    cmb_text_overflow_->addItem(bgl_tr("OBSTitles.Wrap"), 0);
    cmb_text_overflow_->addItem(bgl_tr("OBSTitles.Clip"), 1);
    cmb_text_overflow_->addItem(bgl_tr("OBSTitles.HorizontalFit"), 2);
    cmb_text_overflow_->setToolTip(bgl_tr("OBSTitles.TextOverflowTooltip"));
    cmb_text_overflow_->setFixedHeight(22);
    cmb_text_overflow_->setStyleSheet(control_style);
    spn_text_fit_min_scale_ = mk_dspin(0.05, 1.0, 0.05);
    spn_text_fit_min_scale_->setDecimals(2);
    spn_text_fit_min_scale_->setToolTip(bgl_tr("OBSTitles.MinFitScaleTooltip"));
    lbl_text_fit_scale_ = new QLabel(bgl_tr("OBSTitles.Scale100"), inner);
    lbl_text_fit_scale_->setStyleSheet(QStringLiteral("color:%1;font-size:10px;background:transparent;").arg(subtle_text_name));
    chk_expose_text_ = new BglSwitch(bgl_tr("OBSTitles.ExposeInDock"), inner);
    chk_expose_text_->setToolTip(bgl_tr("OBSTitles.ExposeInDockTooltip"));
    style_checkbox(chk_expose_text_);
    chk_exposed_hide_if_empty_ = new BglSwitch(bgl_tr("OBSTitles.ExposedHideIfEmpty"), inner);
    chk_exposed_hide_if_empty_->setToolTip(bgl_tr("OBSTitles.ExposedHideIfEmptyTooltip"));
    style_checkbox(chk_exposed_hide_if_empty_);
    chk_exposed_single_value_ = new BglSwitch(bgl_tr("OBSTitles.ExposedSingleValue"), inner);
    chk_exposed_single_value_->setToolTip(bgl_tr("OBSTitles.ExposedSingleValueTooltip"));
    style_checkbox(chk_exposed_single_value_);
    chk_ignore_persistence_ = new BglSwitch(bgl_tr("OBSTitles.IgnorePersistence"), inner);
    chk_ignore_persistence_->setToolTip(bgl_tr("OBSTitles.IgnorePersistenceTooltip"));
    style_checkbox(chk_ignore_persistence_);
    row_ticker_playback_ = new QWidget(inner);
    auto *ticker_playback_layout = new QHBoxLayout(row_ticker_playback_);
    ticker_playback_layout->setContentsMargins(0, 0, 0, 0);
    ticker_playback_layout->setSpacing(6);
    btn_ticker_pause_ = new QToolButton(row_ticker_playback_);
    btn_ticker_pause_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    btn_ticker_pause_->setFixedHeight(22);
    btn_ticker_pause_->setAutoRaise(false);
    btn_ticker_pause_->setStyleSheet(control_style);
    cmb_ticker_playback_mode_ = new QComboBox(row_ticker_playback_);
    cmb_ticker_playback_mode_->addItem(bgl_tr("OBSTitles.TickerAlwaysPlay"),
                                       static_cast<int>(TickerPlaybackMode::AlwaysPlay));
    cmb_ticker_playback_mode_->addItem(bgl_tr("OBSTitles.TickerPausedUntilCued"),
                                       static_cast<int>(TickerPlaybackMode::PausedUntilCued));
    cmb_ticker_playback_mode_->addItem(bgl_tr("OBSTitles.TickerPausedUntilHotkey"),
                                       static_cast<int>(TickerPlaybackMode::PausedUntilHotkey));
    cmb_ticker_playback_mode_->addItem(bgl_tr("OBSTitles.TickerCustomPlayback"),
                                       static_cast<int>(TickerPlaybackMode::CustomPlayback));
    cmb_ticker_playback_mode_->setFixedHeight(22);
    cmb_ticker_playback_mode_->setStyleSheet(control_style);
    ticker_playback_layout->addWidget(btn_ticker_pause_);
    ticker_playback_layout->addWidget(cmb_ticker_playback_mode_, 1);

    row_ticker_completion_ = new QWidget(inner);
    auto *ticker_completion_layout = new QHBoxLayout(row_ticker_completion_);
    ticker_completion_layout->setContentsMargins(0, 0, 0, 0);
    ticker_completion_layout->setSpacing(4);
    spn_ticker_completion_ = mk_dspin(0.0, 100.0, 1.0);
    spn_ticker_completion_->setSuffix(" %");
    spn_ticker_completion_->setDecimals(1);
    btn_kf_ticker_completion_ = mk_kf_button(bgl_tr("OBSTitles.ToggleTickerCompletionKeyframe"));
    ticker_completion_layout->addWidget(spn_ticker_completion_, 1);
    ticker_completion_layout->addWidget(btn_kf_ticker_completion_);

    cmb_ticker_style_ = new QComboBox(inner);
    cmb_ticker_style_->addItem(bgl_tr("OBSTitles.TickerHorizontal"), 0);
    cmb_ticker_style_->addItem(bgl_tr("OBSTitles.TickerVerticalLine"), 1);
    cmb_ticker_style_->addItem(bgl_tr("OBSTitles.TickerVerticalSmooth"), 2);
    cmb_ticker_style_->setFixedHeight(22);
    cmb_ticker_style_->setStyleSheet(control_style);
    spn_ticker_speed_ = mk_dspin(1.0, 5000.0, 1.0);
    spn_ticker_speed_->setSuffix(" px/s");
    spn_ticker_line_hold_ = new TimecodeSpinBox(inner);
    spn_ticker_line_hold_->setRange(0.1, 60.0);
    spn_ticker_line_hold_->setFixedHeight(22);
    spn_ticker_line_hold_->setStyleSheet(control_style);
    cmb_ticker_direction_ = new QComboBox(inner);
    cmb_ticker_direction_->setFixedHeight(22);
    cmb_ticker_direction_->setStyleSheet(control_style);
    add_form_row(dynamic_form, "Text Style", cmb_text_style_);
    add_form_row(dynamic_form, bgl_tr("OBSTitles.OverflowLabel"), cmb_text_overflow_);
    add_form_row(dynamic_form, bgl_tr("OBSTitles.MinFitScaleLabel"), spn_text_fit_min_scale_);
    add_form_row(dynamic_form, "", lbl_text_fit_scale_);
    add_form_row(dynamic_form, bgl_tr("OBSTitles.TickerPlaybackLabel"), row_ticker_playback_);
    add_form_row(dynamic_form, bgl_tr("OBSTitles.TickerCompletionLabel"), row_ticker_completion_);
    add_form_row(dynamic_form, bgl_tr("OBSTitles.TickerStyleLabel"), cmb_ticker_style_);
    add_form_row(dynamic_form, bgl_tr("OBSTitles.TickerSpeedLabel"), spn_ticker_speed_);
    add_form_row(dynamic_form, bgl_tr("OBSTitles.TickerLineHoldLabel"), spn_ticker_line_hold_);
    add_form_row(dynamic_form, bgl_tr("OBSTitles.DirectionLabel"), cmb_ticker_direction_);
    bgl_add_panel_section(vl, dynamic_text_box_);

    /* ── Auto Styling ── */
    auto_style_box_ = new QGroupBox("Auto Styling Rules", inner);
    auto_style_box_->setStyleSheet(QStringLiteral(
        "QGroupBox{color:%1;background:%2;border:none;margin:0;padding:0;font-size:14px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:14px;top:8px;padding:0;background:transparent;}"
        "QLabel{color:%3;font-size:10px;background:transparent;}")
        .arg(panel_text_name, section_bg_name, subtle_text_name));
    auto *auto_layout = new QVBoxLayout(auto_style_box_);
    auto_layout->setContentsMargins(10, 8, 10, 10);
    auto_layout->setSpacing(6);
    chk_auto_style_enabled_ = new BglSwitch("Enable automatic text styling", auto_style_box_);
    chk_auto_style_enabled_->setToolTip("When enabled, the selected text layer is styled by the rule engine before rendering. The original text stays editable; rules only produce final style runs.");
    style_checkbox(chk_auto_style_enabled_);
    auto_layout->addWidget(chk_auto_style_enabled_);

    auto *auto_help = new QLabel("Automatically format names, labels, dates and other text patterns. Start by formatting an example, then let GSP learn it.", auto_style_box_);
    auto_help->setWordWrap(true);
    auto_help->setToolTip("Auto Styling also applies to Live Text Cue values. Manual inline formatting remains editable and is applied after automatic rules.");
    auto_layout->addWidget(auto_help);

    auto *quick_setup_box = new QGroupBox("Quick Setup", auto_style_box_);
    auto *quick_setup_layout = new QVBoxLayout(quick_setup_box);
    quick_setup_layout->setContentsMargins(8, 8, 8, 8);
    quick_setup_layout->setSpacing(5);
    btn_auto_learn_formatting_ = new QPushButton("Learn from formatted example…", quick_setup_box);
    btn_auto_learn_formatting_->setToolTip("Format part of the current text, then click here. GSP will create reusable rules from the visible structure, including spaces and line or paragraph breaks.");
    style_push_button(btn_auto_learn_formatting_);
    quick_setup_layout->addWidget(btn_auto_learn_formatting_);
    auto *learn_hint = new QLabel("1. Format an example  2. Learn  3. Test with different text", quick_setup_box);
    learn_hint->setWordWrap(true);
    quick_setup_layout->addWidget(learn_hint);
    auto_layout->addWidget(quick_setup_box);

    auto *file_row = new QHBoxLayout();
    btn_auto_rules_load_ = new QPushButton("Load Set…", auto_style_box_);
    btn_auto_rules_save_ = new QPushButton("Save Set", auto_style_box_);
    btn_auto_rules_save_as_ = new QPushButton("Save Set As…", auto_style_box_);
    for (auto *button : {btn_auto_rules_load_, btn_auto_rules_save_, btn_auto_rules_save_as_}) {
        style_push_button(button);
        file_row->addWidget(button);
    }
    btn_auto_rules_load_->setToolTip("Load or import an Auto Styling rule set file. You can replace the current rules or append the loaded rules.");
    btn_auto_rules_save_->setToolTip("Save the current complete rule set to its last file.");
    btn_auto_rules_save_as_->setToolTip("Save the current base style and all rules as a reusable .gsp-auto-style.json file.");
    auto_layout->addLayout(file_row);
    lbl_auto_rules_status_ = new QLabel("Rule set: Not saved", auto_style_box_);
    lbl_auto_rules_status_->setWordWrap(true);
    auto_layout->addWidget(lbl_auto_rules_status_);

    auto *rules_heading = new QLabel("Rules — double-click to edit", auto_style_box_);
    auto_layout->addWidget(rules_heading);
    lst_auto_style_rules_ = new QListWidget(auto_style_box_);
    lst_auto_style_rules_->setMinimumHeight(150);
    lst_auto_style_rules_->setStyleSheet(control_style);
    lst_auto_style_rules_->setToolTip("Double-click a rule to open the full Rule Editor. Rules are evaluated from top to bottom.");
    auto_layout->addWidget(lst_auto_style_rules_);
    auto *main_rule_actions = new QHBoxLayout();
    btn_auto_rules_edit_ = new QPushButton("Open Rule Editor…", auto_style_box_);
    btn_auto_rules_clear_ = new QPushButton("Clear All Rules…", auto_style_box_);
    btn_auto_rules_edit_->setToolTip("Open the Auto Styling Rule Editor with demo text, the complete rule list and all settings.");
    btn_auto_rules_clear_->setToolTip("Remove every Auto Styling rule after confirmation.");
    for (auto *button : {btn_auto_rules_edit_, btn_auto_rules_clear_}) {
        style_push_button(button);
        main_rule_actions->addWidget(button);
    }
    auto_layout->addLayout(main_rule_actions);

    auto_rules_dialog_ = new QDialog(this);
    auto_rules_dialog_->setWindowTitle("Auto Styling Rule Editor");
    auto_rules_dialog_->setModal(true);
    auto_rules_dialog_->resize(980, 720);
    auto *dialog_layout = new QVBoxLayout(auto_rules_dialog_);
    dialog_layout->setContentsMargins(12, 12, 12, 12);
    dialog_layout->setSpacing(8);

    auto *demo_box = new QGroupBox("Demo / Test", auto_rules_dialog_);
    auto *demo_layout = new QVBoxLayout(demo_box);
    demo_layout->setContentsMargins(8, 8, 8, 8);
    auto *demo_hint = new QLabel("Type or paste sample text here to test the current rule set. Invisible characters and line/paragraph breaks are preserved.", demo_box);
    demo_hint->setWordWrap(true);
    txt_auto_rules_demo_ = new QTextEdit(demo_box);
    txt_auto_rules_demo_->setPlaceholderText("NAME: Example text\nSECOND NAME: Another example");
    txt_auto_rules_demo_->setMinimumHeight(115);
    txt_auto_rules_demo_->setStyleSheet(control_style);
    lbl_auto_rules_demo_status_ = new QLabel("No demo text", demo_box);
    lbl_auto_rules_demo_status_->setWordWrap(true);
    demo_layout->addWidget(demo_hint);
    demo_layout->addWidget(txt_auto_rules_demo_);
    demo_layout->addWidget(lbl_auto_rules_demo_status_);
    dialog_layout->addWidget(demo_box);

    auto *dialog_splitter = new QSplitter(Qt::Horizontal, auto_rules_dialog_);
    auto *dialog_left = new QWidget(dialog_splitter);
    auto *dialog_left_layout = new QVBoxLayout(dialog_left);
    dialog_left_layout->setContentsMargins(0, 0, 4, 0);
    dialog_left_layout->addWidget(new QLabel("Rules", dialog_left));
    lst_auto_rules_dialog_ = new QListWidget(dialog_left);
    lst_auto_rules_dialog_->setMinimumWidth(300);
    lst_auto_rules_dialog_->setStyleSheet(control_style);
    dialog_left_layout->addWidget(lst_auto_rules_dialog_);

    auto *dialog_right = new QWidget(dialog_splitter);
    auto *dialog_right_layout = new QVBoxLayout(dialog_right);
    dialog_right_layout->setContentsMargins(4, 0, 0, 0);
    auto *default_style_form = new QFormLayout();
    style_form(default_style_form);
    cmb_auto_default_style_ = new QComboBox(auto_rules_dialog_);
    cmb_auto_default_style_->setFixedHeight(22);
    cmb_auto_default_style_->setStyleSheet(control_style);
    cmb_auto_default_style_->setToolTip("Base preset for the entire text box. Rules are applied on top of this default style.");
    default_style_form->addRow("Base style", cmb_auto_default_style_);
    dialog_right_layout->addLayout(default_style_form);
    auto_rule_editor_box_ = new QGroupBox("Selected Rule", auto_rules_dialog_);
    auto_rule_editor_box_->setEnabled(false);
    auto *rule_editor_layout = new QVBoxLayout(auto_rule_editor_box_);
    rule_editor_layout->setContentsMargins(8, 8, 8, 8);
    rule_editor_layout->setSpacing(5);
    auto *rule_form = new QFormLayout();
    style_form(rule_form);
    cmb_auto_rule_style_ = new QComboBox(auto_rules_dialog_);
    cmb_auto_rule_style_->setFixedHeight(22);
    cmb_auto_rule_style_->setStyleSheet(control_style);
    cmb_auto_rule_style_->setToolTip("Text style preset to apply when this rule matches.");
    edt_auto_rule_name_ = new QLineEdit(auto_rules_dialog_);
    edt_auto_rule_name_->setPlaceholderText("Optional rule name");
    edt_auto_rule_name_->setFixedHeight(22);
    edt_auto_rule_name_->setStyleSheet(control_style);
    edt_auto_rule_name_->setToolTip("Optional friendly name shown in the rule list. Leave empty to use the preset name.");
    chk_auto_rule_enabled_ = new BglSwitch("This rule is active", auto_rules_dialog_);
    chk_auto_rule_enabled_->setToolTip("Disable this rule without deleting it.");
    chk_auto_rule_enabled_->setChecked(true);
    style_checkbox(chk_auto_rule_enabled_);
    auto fill_auto_marker_combo = [](QComboBox *combo) {
        combo->addItem("Start of text", "text_start");
        combo->addItem("End of text", "text_end");
        combo->addItem("Character index", "character_index");
        combo->addItem("Number of characters", "character_count");
        combo->addItem("Number of words", "word_count");
        combo->addItem("Space / tab", "space");
        combo->addItem("Line break", "line_break");
        combo->addItem("New line", "newline");
        combo->addItem("Start of paragraph", "paragraph_start");
        combo->addItem("End of paragraph", "paragraph_end");
        combo->addItem("Custom character", "custom_char");
    };
    cmb_auto_rule_start_condition_ = new QComboBox(auto_rules_dialog_);
    cmb_auto_rule_end_condition_ = new QComboBox(auto_rules_dialog_);
    fill_auto_marker_combo(cmb_auto_rule_start_condition_);
    fill_auto_marker_combo(cmb_auto_rule_end_condition_);
    cmb_auto_rule_start_condition_->setFixedHeight(22);
    cmb_auto_rule_end_condition_->setFixedHeight(22);
    cmb_auto_rule_start_condition_->setStyleSheet(control_style);
    cmb_auto_rule_end_condition_->setStyleSheet(control_style);
    cmb_auto_rule_start_condition_->setToolTip("Where the styled range begins. Marker rules scan the entire text box, not only the beginning.");
    cmb_auto_rule_end_condition_->setToolTip("Where the styled range ends. For marker-based rules, this is searched after each matching From marker.");
    spn_auto_rule_start_offset_ = new QSpinBox(auto_rules_dialog_);
    spn_auto_rule_end_offset_ = new QSpinBox(auto_rules_dialog_);
    for (auto *spin : {spn_auto_rule_start_offset_, spn_auto_rule_end_offset_}) {
        spin->setRange(0, 100000);
        spin->setFixedHeight(22);
        spin->setStyleSheet(control_style);
        spin->setToolTip("Offset / occurrence number. For Character index, this is the exact character position. For marker rules, 0 means the first matching marker.");
    }
    edt_auto_rule_start_chars_ = new QLineEdit(auto_rules_dialog_);
    edt_auto_rule_end_chars_ = new QLineEdit(auto_rules_dialog_);
    edt_auto_rule_start_chars_->setMaxLength(8);
    edt_auto_rule_end_chars_->setMaxLength(8);
    edt_auto_rule_start_chars_->setPlaceholderText("custom");
    edt_auto_rule_end_chars_->setPlaceholderText("custom");
    edt_auto_rule_start_chars_->setFixedHeight(22);
    edt_auto_rule_end_chars_->setFixedHeight(22);
    edt_auto_rule_start_chars_->setStyleSheet(control_style);
    edt_auto_rule_end_chars_->setStyleSheet(control_style);
    edt_auto_rule_start_chars_->setToolTip("Characters to search for when From is set to Custom character. Example: : or #.");
    edt_auto_rule_end_chars_->setToolTip("Characters to search for when To is set to Custom character. Example: , or ).");
    auto *start_row = new QWidget(auto_rules_dialog_);
    auto *start_lay = new QHBoxLayout(start_row);
    start_lay->setContentsMargins(0, 0, 0, 0);
    start_lay->setSpacing(4);
    start_lay->addWidget(cmb_auto_rule_start_condition_, 2);
    start_lay->addWidget(spn_auto_rule_start_offset_, 1);
    start_lay->addWidget(edt_auto_rule_start_chars_, 1);
    auto *end_row = new QWidget(auto_rules_dialog_);
    auto *end_lay = new QHBoxLayout(end_row);
    end_lay->setContentsMargins(0, 0, 0, 0);
    end_lay->setSpacing(4);
    end_lay->addWidget(cmb_auto_rule_end_condition_, 2);
    end_lay->addWidget(spn_auto_rule_end_offset_, 1);
    end_lay->addWidget(edt_auto_rule_end_chars_, 1);

    chk_auto_rule_require_stop_match_ = new BglSwitch("Require stop match", auto_rules_dialog_);
    chk_auto_rule_require_stop_match_->setChecked(true);
    chk_auto_rule_require_stop_match_->setToolTip("When enabled, the rule is skipped if the Stop matching at condition is not found after the Start condition. This prevents accidental styling to the line/text end when a delimiter is missing in live text cues.");
    style_checkbox(chk_auto_rule_require_stop_match_);
    chk_auto_rule_include_start_marker_ = new BglSwitch("Include From custom character", auto_rules_dialog_);
    chk_auto_rule_include_start_marker_->setChecked(true);
    chk_auto_rule_include_start_marker_->setToolTip("When From is Custom character, include that character in the styled range. Disable it to start styling after the marker.");
    style_checkbox(chk_auto_rule_include_start_marker_);
    chk_auto_rule_include_end_marker_ = new BglSwitch("Include To custom character", auto_rules_dialog_);
    chk_auto_rule_include_end_marker_->setChecked(false);
    chk_auto_rule_include_end_marker_->setToolTip("When Stop matching at is Custom character, include that stop character in the styled range. Leave disabled to stop before the marker.");
    style_checkbox(chk_auto_rule_include_end_marker_);
    auto *marker_options_row = new QWidget(auto_rules_dialog_);
    auto *marker_options_lay = new QVBoxLayout(marker_options_row);
    marker_options_lay->setContentsMargins(0, 0, 0, 0);
    marker_options_lay->setSpacing(2);
    marker_options_lay->addWidget(chk_auto_rule_require_stop_match_);
    marker_options_lay->addWidget(chk_auto_rule_include_start_marker_);
    marker_options_lay->addWidget(chk_auto_rule_include_end_marker_);

    cmb_auto_rule_conflict_mode_ = new QComboBox(auto_rules_dialog_);
    cmb_auto_rule_conflict_mode_->addItem("Override previous rules", "override_previous");
    cmb_auto_rule_conflict_mode_->addItem("Respect previous rules", "respect_previous");
    cmb_auto_rule_conflict_mode_->addItem("Apply only if empty", "apply_if_empty");
    cmb_auto_rule_conflict_mode_->addItem("Merge", "merge");
    cmb_auto_rule_conflict_mode_->addItem("Exclude selected rules", "exclude_other_rules");
    cmb_auto_rule_conflict_mode_->setFixedHeight(22);
    cmb_auto_rule_conflict_mode_->setStyleSheet(control_style);
    cmb_auto_rule_conflict_mode_->setToolTip("How this rule behaves when another rule already styled the same text. Override wins, Respect keeps previous fields, Apply only if empty affects unstyled text, Exclude blocks selected rules.");
    cmb_auto_rule_match_mode_ = new QComboBox(auto_rules_dialog_);
    cmb_auto_rule_match_mode_->addItem("All matches", "all_matches");
    cmb_auto_rule_match_mode_->addItem("First match only", "first_match");
    cmb_auto_rule_match_mode_->setFixedHeight(22);
    cmb_auto_rule_match_mode_->setStyleSheet(control_style);
    cmb_auto_rule_match_mode_->setToolTip("Apply to every matching range in the text box or only to the first one.");
    cmb_auto_rule_generalization_ = new QComboBox(auto_rules_dialog_);
    cmb_auto_rule_generalization_->addItem("Smart merge equivalent examples", "auto_merge");
    cmb_auto_rule_generalization_->addItem("Exact structure only", "exact_only");
    cmb_auto_rule_generalization_->addItem("Always keep separate", "keep_separate");
    cmb_auto_rule_generalization_->setFixedHeight(22);
    cmb_auto_rule_generalization_->setStyleSheet(control_style);
    cmb_auto_rule_generalization_->setToolTip("Controls whether learned examples that describe the same reusable structure are folded into one rule.");
    chk_auto_rule_prevent_duplicates_ = new QCheckBox("Skip exact duplicate rules", auto_rules_dialog_);
    chk_auto_rule_prevent_duplicates_->setChecked(true);
    chk_auto_rule_allow_multiple_cases_ = new QCheckBox("Apply one rule to every matching case", auto_rules_dialog_);
    chk_auto_rule_allow_multiple_cases_->setChecked(true);
    edt_auto_rule_excludes_ = new QLineEdit(auto_rules_dialog_);
    edt_auto_rule_excludes_->setPlaceholderText("Rule IDs to exclude, comma-separated");
    edt_auto_rule_excludes_->setFixedHeight(22);
    edt_auto_rule_excludes_->setStyleSheet(control_style);
    edt_auto_rule_excludes_->setToolTip("Comma-separated rule IDs to block where this rule matches. Example: rule_1, rule_3. Use with Conflict: Exclude selected rules.");
    chk_auto_rule_stop_processing_ = new BglSwitch("Stop later rules on this matched text", auto_rules_dialog_);
    chk_auto_rule_stop_processing_->setToolTip("After this rule matches, later rules in the list cannot change the same characters. Useful for speaker names, labels, or protected prefixes.");
    style_checkbox(chk_auto_rule_stop_processing_);
    spn_auto_rule_chars_ = new QSpinBox(auto_rules_dialog_);
    spn_auto_rule_chars_->hide();
    rule_form->addRow("Rule name", edt_auto_rule_name_);
    rule_form->addRow("Apply preset", cmb_auto_rule_style_);
    rule_form->addRow("Active", chk_auto_rule_enabled_);
    rule_form->addRow("Start matching at", start_row);
    rule_form->addRow("Stop matching at", end_row);
    rule_form->addRow("Stop safety", marker_options_row);
    rule_editor_layout->addLayout(rule_form);
    btn_auto_rule_advanced_ = new QToolButton(auto_rule_editor_box_);
    btn_auto_rule_advanced_->setText("Advanced options");
    btn_auto_rule_advanced_->setCheckable(true);
    btn_auto_rule_advanced_->setChecked(false);
    btn_auto_rule_advanced_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    btn_auto_rule_advanced_->setArrowType(Qt::RightArrow);
    rule_editor_layout->addWidget(btn_auto_rule_advanced_);
    auto_rule_advanced_widget_ = new QWidget(auto_rule_editor_box_);
    auto *advanced_form = new QFormLayout(auto_rule_advanced_widget_);
    style_form(advanced_form);
    advanced_form->setContentsMargins(0, 0, 0, 0);
    advanced_form->addRow("Apply to", cmb_auto_rule_match_mode_);
    advanced_form->addRow("Learned examples", cmb_auto_rule_generalization_);
    advanced_form->addRow("Duplicates", chk_auto_rule_prevent_duplicates_);
    advanced_form->addRow("Reusable cases", chk_auto_rule_allow_multiple_cases_);
    advanced_form->addRow("If rules overlap", cmb_auto_rule_conflict_mode_);
    advanced_form->addRow("Exclude rules", edt_auto_rule_excludes_);
    advanced_form->addRow("Protect match", chk_auto_rule_stop_processing_);
    auto_rule_advanced_widget_->setVisible(false);
    rule_editor_layout->addWidget(auto_rule_advanced_widget_);
    QObject::connect(btn_auto_rule_advanced_, &QToolButton::toggled, auto_rule_advanced_widget_, &QWidget::setVisible);
    QObject::connect(btn_auto_rule_advanced_, &QToolButton::toggled, btn_auto_rule_advanced_, [this](bool open) {
        btn_auto_rule_advanced_->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    dialog_right_layout->addWidget(auto_rule_editor_box_);
    auto *rule_buttons = new QHBoxLayout();
    btn_auto_rule_add_ = new QPushButton("Add rule", auto_rules_dialog_);
    btn_auto_rule_update_ = new QPushButton("Save rule", auto_rules_dialog_);
    btn_auto_rule_delete_ = new QPushButton("Delete", auto_rules_dialog_);
    btn_auto_rules_clear_dialog_ = new QPushButton("Clear All…", auto_rules_dialog_);
    btn_auto_rule_up_ = new QPushButton("↑", auto_rules_dialog_);
    btn_auto_rule_down_ = new QPushButton("↓", auto_rules_dialog_);
    btn_auto_rule_add_->setToolTip("Create a new rule from the editor fields below.");
    btn_auto_rule_update_->setToolTip("Save the current editor fields into the selected rule.");
    btn_auto_rule_delete_->setToolTip("Delete the selected rule.");
    btn_auto_rules_clear_dialog_->setToolTip("Remove every Auto Styling rule after confirmation.");
    btn_auto_rule_up_->setToolTip("Move selected rule earlier. Earlier rules are evaluated first.");
    btn_auto_rule_down_->setToolTip("Move selected rule later. Later rules can override earlier ones depending on conflict mode.");
    for (auto *b : {btn_auto_rule_add_, btn_auto_rule_update_, btn_auto_rule_delete_, btn_auto_rules_clear_dialog_, btn_auto_rule_up_, btn_auto_rule_down_}) {
        style_push_button(b);
        rule_buttons->addWidget(b);
    }
    rule_editor_layout->addLayout(rule_buttons);

    dialog_splitter->addWidget(dialog_left);
    dialog_splitter->addWidget(dialog_right);
    dialog_splitter->setStretchFactor(0, 1);
    dialog_splitter->setStretchFactor(1, 2);
    dialog_layout->addWidget(dialog_splitter, 1);
    auto *dialog_buttons = new QDialogButtonBox(QDialogButtonBox::Close, auto_rules_dialog_);
    QObject::connect(dialog_buttons, &QDialogButtonBox::rejected, auto_rules_dialog_, &QDialog::reject);
    dialog_layout->addWidget(dialog_buttons);
    bgl_add_panel_section(vl, auto_style_box_);

    /* ── Live Edit ── */
    live_edit_box_ = new QGroupBox(bgl_tr("OBSTitles.LiveEdit"), inner);
    live_edit_box_->setStyleSheet(QStringLiteral(
        "QGroupBox{color:%1;background:%2;border:none;margin:0;padding:0;font-size:14px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:14px;top:8px;padding:0;background:transparent;}"
        "QLabel{color:%3;font-size:10px;background:transparent;}")
        .arg(panel_text_name, section_bg_name, subtle_text_name));
    auto *live_edit_form = new QFormLayout(live_edit_box_);
    style_form(live_edit_form);
    live_edit_form->setContentsMargins(10, 8, 10, 10);
    live_edit_form->setHorizontalSpacing(8);
    live_edit_form->setVerticalSpacing(6);
    chk_scene_mask_->setText(bgl_tr("OBSTitles.SetAsSceneMask"));
    chk_scene_mask_->setToolTip(bgl_tr("OBSTitles.LiveSceneMaskTooltip"));
    chk_scene_mask_->setFixedHeight(22);
    chk_scene_mask_->setStyleSheet(checkbox_style);
    chk_expose_text_->setText(bgl_tr("OBSTitles.ExposeToDock"));
    chk_expose_text_->setToolTip(bgl_tr("OBSTitles.ExposeInDockTooltip"));
    add_form_row(live_edit_form, bgl_tr("OBSTitles.SceneMask"), chk_scene_mask_);
    add_form_row(live_edit_form, bgl_tr("OBSTitles.DockEditing"), chk_expose_text_);
    add_form_row(live_edit_form, QString(), chk_exposed_hide_if_empty_);
    add_form_row(live_edit_form, QString(), chk_exposed_single_value_);
    add_form_row(live_edit_form, bgl_tr("OBSTitles.Persistence"), chk_ignore_persistence_);
    bgl_add_panel_section(vl, live_edit_box_);

    /* ── Bullets and Numbering ── */
    bullets_box_ = new QGroupBox("Bullets and Numbering", inner);
    bullets_box_->setStyleSheet(section_style);
    auto *bullets_layout = new QVBoxLayout(bullets_box_);
    bullets_layout->setContentsMargins(10, 8, 10, 10);
    auto *bullets_hint = new QLabel("Broadcast lower thirds typically use manual bullet glyphs; this group is ready for list presets.", inner);
    bullets_hint->setWordWrap(true);
    bullets_hint->setStyleSheet(QStringLiteral("color:%1;font-size:10px;background:transparent;").arg(subtle_text_name));
    bullets_layout->addWidget(bullets_hint);
    bgl_add_panel_section(vl, bullets_box_);
    bullets_box_->hide();

    /* ── Shape ── */
    rect_box_ = new QGroupBox(inner);
    rect_box_->setTitle(QString());
    rect_box_->setStyleSheet(QStringLiteral("QGroupBox{background:%1;border:none;margin:0;padding:0;}").arg(section_bg_name));
    auto *shape_layout = new QVBoxLayout(rect_box_);
    shape_layout->setContentsMargins(10, 8, 10, 10);
    shape_layout->setSpacing(8);

    btn_shape_defaults_ = new QPushButton(bgl_tr("OBSTitles.Defaults"), rect_box_);
    btn_shape_defaults_->setFixedHeight(20);
    btn_shape_defaults_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    btn_shape_defaults_->setIconSize(QSize(12, 12));
    btn_shape_defaults_->setToolTip(bgl_tr("OBSTitles.RestoreShapeDefaults"));
    btn_shape_defaults_->setStyleSheet(push_button_style);

    auto *shape_types_row = new QWidget(rect_box_);
    shape_types_row->setObjectName(QStringLiteral("OBSTitlesShapeTypeButtonsRow"));
    shape_types_row->setStyleSheet("background:transparent;");
    auto *shape_types_layout = new QHBoxLayout(shape_types_row);
    shape_types_layout->setContentsMargins(0, 0, 0, 0);
    shape_types_layout->setSpacing(4);
    grp_shape_type_ = new QButtonGroup(shape_types_row);
    grp_shape_type_->setExclusive(true);
    auto add_shape_button = [&](ShapeType shape_type) {
        auto *button = new QToolButton(shape_types_row);
        button->setCheckable(true);
        button->setIcon(shape_tool_icon(shape_type));
        button->setIconSize(QSize(18, 18));
        button->setFixedSize(30, 28);
        button->setToolTip(shape_display_name(shape_type));
        button->setAccessibleName(shape_display_name(shape_type));
        button->setStyleSheet(QStringLiteral(
            "QToolButton{color:%1;background:%2;border:1px solid %3;border-radius:2px;padding:0;}"
            "QToolButton:hover{background:%4;border-color:%3;}"
            "QToolButton:checked{background:%5;color:%6;border-color:%5;}")
            .arg(button_text_name, button_bg_name, border_name, hover_bg_name, highlight_name, highlighted_text_name));
        grp_shape_type_->addButton(button, (int)shape_type);
        shape_types_layout->addWidget(button);
    };
    for (ShapeType shape_type : {ShapeType::Rectangle, ShapeType::RoundedRectangle, ShapeType::Ellipse,
                                 ShapeType::Triangle, ShapeType::Star, ShapeType::Polygon,
                                 ShapeType::Diamond}) {
        add_shape_button(shape_type);
    }
    shape_types_layout->addStretch(1);
    shape_layout->addWidget(shape_types_row);

    auto *shape_grid = new QGridLayout();
    shape_grid->setContentsMargins(0, 0, 0, 0);
    shape_grid->setHorizontalSpacing(8);
    shape_grid->setVerticalSpacing(8);
    shape_grid->setColumnMinimumWidth(0, 24);
    shape_grid->setColumnMinimumWidth(1, 82);
    shape_grid->setColumnMinimumWidth(2, 86);
    shape_grid->setColumnMinimumWidth(3, 22);
    shape_grid->setColumnMinimumWidth(4, 86);
    shape_grid->setColumnStretch(5, 1);

    auto make_shape_field = [&](const QString &label, QDoubleSpinBox *spin, const QIcon &icon = QIcon()) {
        auto *field = new QWidget(rect_box_);
        field->setObjectName(QStringLiteral("OBSTitlesShapeNumericField"));
        field->setStyleSheet(QStringLiteral("QWidget#OBSTitlesShapeNumericField{background:%1;border:1px solid %2;"
                             "border-radius:2px;}").arg(control_bg_name, border_name));
        auto *field_layout = new QHBoxLayout(field);
        field_layout->setContentsMargins(5, 0, 0, 0);
        field_layout->setSpacing(2);
        auto *field_label = new NumericDragLabel(label, spin, field,
                                                 [this]() {
                                                     if (loading_values_) return;
                                                     numeric_label_dragging_ = true;
                                                     emit property_changed(true);
                                                 },
                                                 [this]() {
                                                     if (loading_values_) return;
                                                     numeric_label_dragging_ = false;
                                                     emit property_changed(true);
                                                 });
        field_label->setFixedWidth(icon.isNull() ? 16 : 18);
        field_label->setAlignment(icon.isNull() ? (Qt::AlignLeft | Qt::AlignVCenter) : Qt::AlignCenter);
        if (!icon.isNull()) {
            field_label->setText(QString());
            field_label->setPixmap(icon.pixmap(QSize(14, 14)));
            field_label->setAccessibleName(label);
        }
        field_label->setStyleSheet(QStringLiteral("color:%1;background:transparent;font-size:12px;").arg(control_text_name));
        field_layout->addWidget(field_label);
        field_layout->addWidget(spin, 1);
        field->setFixedSize(104, 24);
        return field;
    };

    spn_layer_w_ = mk_dspin(0.0, 9999.0, 10.0);
    spn_layer_h_ = mk_dspin(0.0, 9999.0, 10.0);
    spn_image_box_w_ = mk_dspin(0.0, 9999.0, 10.0);
    spn_image_box_h_ = mk_dspin(0.0, 9999.0, 10.0);
    chk_text_box_width_to_text_ = new BglSwitch(bgl_tr("OBSTitles.TextBoxWidthToText"), inner);
    chk_text_box_height_to_text_ = new BglSwitch(bgl_tr("OBSTitles.TextBoxHeightToText"), inner);
    style_checkbox(chk_text_box_width_to_text_);
    style_checkbox(chk_text_box_height_to_text_);
    spn_max_text_box_width_ = mk_dspin(1.0, 9999.0, 10.0);
    spn_max_text_box_height_ = mk_dspin(1.0, 9999.0, 10.0);
    spn_rect_corner_tl_ = mk_dspin(0.0, 1000.0, 1.0);
    spn_rect_corner_tr_ = mk_dspin(0.0, 1000.0, 1.0);
    spn_rect_corner_br_ = mk_dspin(0.0, 1000.0, 1.0);
    spn_rect_corner_bl_ = mk_dspin(0.0, 1000.0, 1.0);
    spn_corner_bevel_roundness_ = mk_dspin(-100.0, 100.0, 1.0);
    spn_corner_bevel_roundness_->setDecimals(0);
    spn_corner_bevel_roundness_->setSuffix(QStringLiteral("%"));
    spn_corner_bevel_roundness_->setToolTip(QStringLiteral("100 = round, 0 = flat bevel, -100 = inverted round"));
    cmb_shape_type_ = new QComboBox(inner);
    cmb_shape_type_->addItem("Rectangle", (int)ShapeType::Rectangle);
    cmb_shape_type_->addItem("Rounded Rectangle", (int)ShapeType::RoundedRectangle);
    cmb_shape_type_->addItem("Ellipse", (int)ShapeType::Ellipse);
    cmb_shape_type_->addItem("Triangle", (int)ShapeType::Triangle);
    cmb_shape_type_->addItem("Star", (int)ShapeType::Star);
    cmb_shape_type_->addItem("Polygon", (int)ShapeType::Polygon);
    cmb_shape_type_->addItem("Diamond", (int)ShapeType::Diamond);
    cmb_shape_type_->addItem(bgl_tr("OBSTitles.Path"), (int)ShapeType::Path);
    cmb_shape_type_->setFixedHeight(22);
    cmb_shape_type_->setStyleSheet(control_style);
    cmb_shape_type_->hide();
    spn_shape_points_ = new QSpinBox(inner); spn_shape_points_->setRange(3, 64); spn_shape_points_->setFixedHeight(22); spn_shape_points_->setStyleSheet(control_style);
    spn_shape_sides_ = new QSpinBox(inner); spn_shape_sides_->setRange(3, 64); spn_shape_sides_->setFixedHeight(22); spn_shape_sides_->setStyleSheet(control_style);
    spn_shape_inner_radius_ = mk_dspin(0.0, 1.0, 0.05); spn_shape_inner_radius_->setDecimals(2);
    spn_shape_outer_radius_ = mk_dspin(0.0, 1.0, 0.05); spn_shape_outer_radius_->setDecimals(2);
    spn_shape_roundness_ = mk_dspin(0.0, 1000.0, 1.0);
    btn_kf_width_ = mk_kf_button(bgl_tr("OBSTitles.ToggleWidthKeyframe"));
    btn_kf_width_->setToolTip(bgl_tr("OBSTitles.ToggleSizeKeyframe"));
    btn_kf_image_box_size_ = mk_kf_button(bgl_tr("OBSTitles.ToggleSizeKeyframe"));

    style_transform_spin(spn_layer_w_);
    style_transform_spin(spn_layer_h_);
    style_transform_spin(spn_image_box_w_);
    style_transform_spin(spn_image_box_h_);
    style_transform_spin(spn_rect_corner_tl_);
    style_transform_spin(spn_rect_corner_tr_);
    style_transform_spin(spn_rect_corner_br_);
    style_transform_spin(spn_rect_corner_bl_);
    style_transform_spin(spn_corner_bevel_roundness_);
    auto *field_width = make_shape_field(bgl_tr("OBSTitles.W"), spn_layer_w_);
    auto *field_height = make_shape_field(bgl_tr("OBSTitles.H"), spn_layer_h_);
    chk_size_lock_ = new TransformLockCheckBox(rect_box_);
    chk_size_lock_->setText(QString());
    chk_size_lock_->setToolTip(bgl_tr("OBSTitles.LockAspectRatio"));
    chk_size_lock_->setFixedSize(24, 24);
    chk_size_lock_->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *size_label = new QLabel(bgl_tr("OBSTitles.Size"), rect_box_);
    size_label->setStyleSheet(QStringLiteral("color:%1;background:transparent;font-size:13px;").arg(panel_text_name));
    shape_size_label_ = size_label;
    shape_size_field_w_ = field_width;
    shape_size_field_h_ = field_height;
    shape_grid->addWidget(btn_kf_width_, 0, 0, Qt::AlignCenter);
    shape_grid->addWidget(size_label, 0, 1);
    shape_grid->addWidget(field_width, 0, 2);
    shape_grid->addWidget(chk_size_lock_, 0, 3, Qt::AlignCenter);
    shape_grid->addWidget(field_height, 0, 4);
    shape_layout->addLayout(shape_grid);

    row_rect_corners_ = new QWidget(rect_box_);
    row_rect_corners_->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *corners_layout = new QGridLayout(row_rect_corners_);
    corners_layout->setContentsMargins(0, 0, 0, 0);
    corners_layout->setHorizontalSpacing(8);
    corners_layout->setVerticalSpacing(6);
    corners_layout->addWidget(make_shape_field(bgl_tr("OBSTitles.TL"), spn_rect_corner_tl_, obs_icon("corner-radius-tl.svg")), 0, 0);
    corners_layout->addWidget(make_shape_field(bgl_tr("OBSTitles.TR"), spn_rect_corner_tr_, obs_icon("corner-radius-tr.svg")), 0, 1);
    chk_corner_lock_ = new TransformLockCheckBox(row_rect_corners_);
    chk_corner_lock_->setText(QString());
    chk_corner_lock_->setToolTip(bgl_tr("OBSTitles.SyncCornerRadii"));
    chk_corner_lock_->setFixedSize(24, 24);
    chk_corner_lock_->setStyleSheet(QStringLiteral("background:transparent;"));
    corners_layout->addWidget(chk_corner_lock_, 0, 2, 2, 1, Qt::AlignCenter);
    corners_layout->addWidget(make_shape_field(bgl_tr("OBSTitles.BL"), spn_rect_corner_bl_, obs_icon("corner-radius-bl.svg")), 1, 0);
    corners_layout->addWidget(make_shape_field(bgl_tr("OBSTitles.BR"), spn_rect_corner_br_, obs_icon("corner-radius-br.svg")), 1, 1);

    auto *shape_form_widget = new QWidget(rect_box_);
    shape_form_widget->setStyleSheet("background:transparent;");
    auto *rfl = new QFormLayout(shape_form_widget);
    style_form(rfl);
    add_form_row(rfl, "", chk_text_box_width_to_text_);
    add_form_row(rfl, bgl_tr("OBSTitles.MaxTextBoxWidthLabel"), spn_max_text_box_width_);
    add_form_row(rfl, "", chk_text_box_height_to_text_);
    add_form_row(rfl, bgl_tr("OBSTitles.MaxTextBoxHeightLabel"), spn_max_text_box_height_);
    add_form_row(rfl, bgl_tr("OBSTitles.CornerLabel"), row_rect_corners_);
    add_form_row(rfl, QStringLiteral("Corner Roundness"), spn_corner_bevel_roundness_);
    add_form_row(rfl, "Points", spn_shape_points_);
    add_form_row(rfl, "Sides", spn_shape_sides_);
    add_form_row(rfl, "Inner Radius", spn_shape_inner_radius_);
    add_form_row(rfl, "Outer Radius", spn_shape_outer_radius_);
    add_form_row(rfl, "Roundness", spn_shape_roundness_);
    cmb_fill_type_ = new QComboBox(inner);
    cmb_fill_type_->addItem(bgl_tr("OBSTitles.Solid"), 0);
    cmb_fill_type_->addItem(bgl_tr("OBSTitles.Gradient"), 1);
    cmb_fill_type_->setFixedHeight(22);
    cmb_fill_type_->setStyleSheet(control_style);
    row_fill_type_ = cmb_fill_type_;
    add_form_row(rfl, bgl_tr("OBSTitles.FillTypeLabel"), row_fill_type_);
    btn_fill_color_ = new QPushButton(inner);
    btn_kf_fill_color_ = mk_kf_button(bgl_tr("OBSTitles.ToggleFillColorKeyframe"));
    row_fill_color_ = with_kf(btn_fill_color_, btn_kf_fill_color_);
    add_form_row(rfl, bgl_tr("OBSTitles.ColorLabel"), row_fill_color_);
    shape_layout->addWidget(shape_form_widget);
    bgl_add_panel_section(vl, rect_box_, bgl_tr("OBSTitles.Shape"), btn_shape_defaults_);

    /* ── Gradient Properties ── */
    gradient_box_ = new QGroupBox(bgl_tr("OBSTitles.GradientProperties"), inner);
    gradient_box_->setStyleSheet(section_style);
    auto *gfl = new QFormLayout(gradient_box_);
    style_form(gfl);
    cmb_gradient_type_ = new QComboBox(inner);
    cmb_gradient_type_->addItem(bgl_tr("OBSTitles.LinearGradient"), 0);
    cmb_gradient_type_->addItem(bgl_tr("OBSTitles.RadialGradient"), 1);
    cmb_gradient_type_->addItem(bgl_tr("OBSTitles.ConicalGradient"), 2);
    cmb_gradient_type_->setFixedHeight(22);
    cmb_gradient_type_->setStyleSheet(control_style);
    cmb_gradient_spread_ = new QComboBox(inner);
    cmb_gradient_spread_->addItem(bgl_tr("OBSTitles.No"), 0);
    cmb_gradient_spread_->addItem(bgl_tr("OBSTitles.Repeat"), 2);
    cmb_gradient_spread_->addItem(bgl_tr("OBSTitles.Reflect"), 1);
    cmb_gradient_spread_->setFixedHeight(22);
    cmb_gradient_spread_->setStyleSheet(control_style);
    btn_gradient_start_color_ = new QPushButton(inner);
    btn_gradient_end_color_ = new QPushButton(inner);
    spn_gradient_start_pos_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_end_pos_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_start_opacity_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_end_opacity_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_opacity_ = mk_dspin(0.0, 1.0, 0.01);
    spn_gradient_angle_ = mk_dspin(-360.0, 360.0, 1.0);
    spn_gradient_center_x_ = mk_dspin(-100.0, 100.0, 0.01);
    spn_gradient_center_y_ = mk_dspin(-100.0, 100.0, 0.01);
    spn_gradient_scale_ = mk_dspin(0.01, 100.0, 0.05);
    spn_gradient_focal_x_ = mk_dspin(-100.0, 100.0, 0.01);
    spn_gradient_focal_y_ = mk_dspin(-100.0, 100.0, 0.01);
    for (auto *spin : std::initializer_list<QDoubleSpinBox *>{spn_gradient_start_pos_, spn_gradient_end_pos_,
                                                               spn_gradient_start_opacity_, spn_gradient_end_opacity_,
                                                               spn_gradient_opacity_, spn_gradient_center_x_,
                                                               spn_gradient_center_y_, spn_gradient_scale_,
                                                               spn_gradient_focal_x_, spn_gradient_focal_y_})
        spin->setDecimals(2);
    spn_gradient_angle_->setSuffix("°");
    add_form_row(gfl, bgl_tr("OBSTitles.GradientTypeLabel"), cmb_gradient_type_);
    add_form_row(gfl, bgl_tr("OBSTitles.SpreadLabel"), cmb_gradient_spread_);
    add_form_row(gfl, bgl_tr("OBSTitles.StartColorLabel"), btn_gradient_start_color_);
    add_form_row(gfl, bgl_tr("OBSTitles.StartStopLabel"), spn_gradient_start_pos_);
    add_form_row(gfl, bgl_tr("OBSTitles.StartOpacityLabel"), spn_gradient_start_opacity_);
    add_form_row(gfl, bgl_tr("OBSTitles.EndColorLabel"), btn_gradient_end_color_);
    add_form_row(gfl, bgl_tr("OBSTitles.EndStopLabel"), spn_gradient_end_pos_);
    add_form_row(gfl, bgl_tr("OBSTitles.EndOpacityLabel"), spn_gradient_end_opacity_);
    add_form_row(gfl, bgl_tr("OBSTitles.OpacityLabel"), spn_gradient_opacity_);
    add_form_row(gfl, bgl_tr("OBSTitles.AngleLabel"), spn_gradient_angle_);
    add_form_row(gfl, bgl_tr("OBSTitles.CenterXLabel"), spn_gradient_center_x_);
    add_form_row(gfl, bgl_tr("OBSTitles.CenterYLabel"), spn_gradient_center_y_);
    add_form_row(gfl, bgl_tr("OBSTitles.ScaleLabel"), spn_gradient_scale_);
    add_form_row(gfl, bgl_tr("OBSTitles.FocalXLabel"), spn_gradient_focal_x_);
    add_form_row(gfl, bgl_tr("OBSTitles.FocalYLabel"), spn_gradient_focal_y_);
    bgl_add_panel_section(vl, gradient_box_);

    /* ── Image ── */
    image_box_ = new QWidget(inner);
    image_box_->setStyleSheet(QStringLiteral("background:%1;").arg(section_bg_name));
    auto *image_layout = new QVBoxLayout(image_box_);
    image_layout->setContentsMargins(10, 8, 10, 10);
    image_layout->setSpacing(8);

    auto *image_content = new QWidget(image_box_);
    image_content->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *image_content_layout = new QHBoxLayout(image_content);
    image_content_layout->setContentsMargins(0, 0, 0, 0);
    image_content_layout->setSpacing(10);

    lbl_image_preview_ = new QLabel(image_content);
    lbl_image_preview_->setFixedSize(104, 104);
    lbl_image_preview_->setAlignment(Qt::AlignCenter);
    lbl_image_preview_->setStyleSheet(QStringLiteral("background:transparent;"));
    set_image_preview_label(lbl_image_preview_, QString());
    image_content_layout->addWidget(lbl_image_preview_, 0, Qt::AlignTop);

