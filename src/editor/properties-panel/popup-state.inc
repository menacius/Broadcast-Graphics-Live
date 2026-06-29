static QString auto_style_marker_label(const std::string &marker, size_t offset, const std::string &custom_chars)
{
    QString name;
    if (marker == "text_start") name = "start";
    else if (marker == "text_end") name = "end";
    else if (marker == "character_index" || marker == "character_count") name = "chars";
    else if (marker == "word_count") name = "words";
    else if (marker == "space") name = "space";
    else if (marker == "line_break") name = "line break";
    else if (marker == "newline") name = "new line";
    else if (marker == "paragraph_start") name = "paragraph start";
    else if (marker == "paragraph_end") name = "paragraph end";
    else if (marker == "custom_char") name = QStringLiteral("'%1'").arg(QString::fromStdString(custom_chars));
    else name = QString::fromStdString(marker);
    if (marker == "text_start" || marker == "text_end")
        return name;
    return QStringLiteral("%1[%2]").arg(name).arg((int)offset);
}

static std::vector<std::string> auto_style_split_ids(const QString &text)
{
    std::vector<std::string> ids;
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
    for (const QString &part : parts)
        ids.push_back(part.trimmed().toStdString());
    return ids;
}

struct PropertiesColorLibraryColor {
    QString name;
    QColor color;
};

struct PropertiesColorLibrary {
    QString name;
    std::vector<PropertiesColorLibraryColor> colors;
};

static QString properties_color_libraries_path()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/Broadcast Graphics Live");
    QDir dir(base);
    dir.mkpath(QStringLiteral("palettes"));
    return dir.filePath(QStringLiteral("palettes/user-color-libraries.palette.json"));
}

static bool properties_parse_color_hex(QString text, QColor &color)
{
    text = text.trimmed();
    if (text.startsWith(QStringLiteral("#")))
        text.remove(0, 1);
    if (text.size() != 6 && text.size() != 8)
        return false;
    bool ok = false;
    const uint value = text.toUInt(&ok, 16);
    if (!ok)
        return false;
    color = text.size() == 6
        ? QColor((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF, 255)
        : QColor((value >> 24) & 0xFF, (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
    return color.isValid();
}

static void properties_limit_gradient_stop_color_dialog(color_widgets::ColorDialog *dialog)
{
    if (!dialog)
        return;

    dialog->setAlphaEnabled(true);
    dialog->setButtonMode(color_widgets::ColorDialog::Close);
    const QStringList hidden_names = {
        QStringLiteral("wheel"), QStringLiteral("preview"), QStringLiteral("line"),
        QStringLiteral("label"), QStringLiteral("slide_red"), QStringLiteral("spin_red"),
        QStringLiteral("label_2"), QStringLiteral("slide_green"), QStringLiteral("spin_green"),
        QStringLiteral("label_3"), QStringLiteral("slide_blue"), QStringLiteral("spin_blue")
    };
    for (const QString &name : hidden_names) {
        if (auto *widget = dialog->findChild<QWidget *>(name))
            widget->hide();
    }
    if (auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("buttonBox"))) {
        buttons->show();
        for (auto *button : buttons->buttons()) {
            const bool is_picker = buttons->buttonRole(button) == QDialogButtonBox::ActionRole;
            button->setVisible(is_picker);
            if (is_picker)
                button->setToolTip(bgl_tr("OBSTitles.PickColor"));
        }
    }
    dialog->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

static std::vector<PropertiesColorLibrary> properties_load_color_libraries()
{
    std::vector<PropertiesColorLibrary> libraries;
    PropertiesColorLibrary open_color;
    open_color.name = QStringLiteral("Open Color");
    for (const auto &entry : bgl_open_color_palette())
        open_color.colors.push_back({entry.first, entry.second});
    libraries.push_back(open_color);

    QFile file(properties_color_libraries_path());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        for (const QJsonValue &value : doc.object().value(QStringLiteral("palettes")).toArray()) {
            const QJsonObject object = value.toObject();
            PropertiesColorLibrary library;
            library.name = object.value(QStringLiteral("name")).toString().trimmed();
            if (library.name.isEmpty())
                continue;
            for (const QJsonValue &color_value : object.value(QStringLiteral("colors")).toArray()) {
                const QJsonObject color_object = color_value.toObject();
                QColor color;
                if (!properties_parse_color_hex(color_object.value(QStringLiteral("hex")).toString(), color))
                    continue;
                library.colors.push_back({color_object.value(QStringLiteral("name")).toString(color.name(QColor::HexRgb)), color});
            }
            libraries.push_back(library);
        }
    }
    return libraries;
}

static QString auto_style_join_ids(const std::vector<std::string> &ids)
{
    QStringList parts;
    for (const auto &id : ids)
        if (!id.empty()) parts << QString::fromStdString(id);
    return parts.join(QStringLiteral(", "));
}


PropertiesPanel::PropertiesPanel(QWidget *parent) : QScrollArea(parent)
{
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const QPalette pal = qApp->palette();
    const QColor panel_bg = pal.color(QPalette::Window);
    const QColor panel_text = pal.color(QPalette::WindowText);
    const QColor control_bg = pal.color(QPalette::Base);
    const QColor control_text = pal.color(QPalette::Text);
    const QColor button_bg = pal.color(QPalette::Button);
    const QColor button_text = pal.color(QPalette::ButtonText);
    const QColor border = pal.color(QPalette::Mid);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlighted_text = pal.color(QPalette::HighlightedText);
    const QColor subtle_text = panel_text.lightness() < 128 ? panel_text.lighter(135) : panel_text.darker(135);
    const QColor section_bg = panel_bg.lightness() < 128 ? panel_bg.lighter(112) : panel_bg.darker(104);
    const QColor hover_bg = button_bg.lightness() < 128 ? button_bg.lighter(122) : button_bg.darker(108);
    const QString panel_bg_name = panel_bg.name(QColor::HexRgb);
    const QString panel_text_name = panel_text.name(QColor::HexRgb);
    const QString control_bg_name = control_bg.name(QColor::HexRgb);
    const QString control_text_name = control_text.name(QColor::HexRgb);
    const QString button_bg_name = button_bg.name(QColor::HexRgb);
    const QString button_text_name = button_text.name(QColor::HexRgb);
    const QString border_name = border.name(QColor::HexRgb);
    const QString highlight_name = highlight.name(QColor::HexRgb);
    const QString highlighted_text_name = highlighted_text.name(QColor::HexRgb);
    const QString subtle_text_name = subtle_text.name(QColor::HexRgb);
    const QString section_bg_name = section_bg.name(QColor::HexRgb);
    const QString hover_bg_name = hover_bg.name(QColor::HexRgb);
    setStyleSheet(QStringLiteral("QScrollArea{background:%1;border:none;}").arg(panel_bg_name));

    auto *inner = new QWidget(this);
    inner->setStyleSheet(QStringLiteral("background:%1;").arg(panel_bg_name));
    auto *vl = new QVBoxLayout(inner);
    vl->setContentsMargins(3, 3, 3, 3);
    vl->setSpacing(5);

    const QString section_style =
        QStringLiteral("QGroupBox{color:%1;background:%2;border:none;"
        "border-radius:0;margin-top:14px;font-size:10px;font-weight:bold;}"
        "QGroupBox::title{subcontrol-origin:margin;left:6px;top:2px;padding:0 4px;}"
        "QGroupBox::indicator{width:10px;height:10px;margin-left:2px;}"
        "QLabel{color:%4;font-size:10px;}")
            .arg(panel_text_name, section_bg_name, border_name, subtle_text_name);
    const QString control_style =
        QStringLiteral("QDoubleSpinBox,QSpinBox,QComboBox,QLineEdit,QTextEdit{color:%1;background:%2;"
        "border:1px solid %3;border-radius:2px;padding:1px 3px;selection-background-color:%4;}"
        "QDoubleSpinBox:focus,QSpinBox:focus,QComboBox:focus,QLineEdit:focus,QTextEdit:focus{border-color:%4;}")
            .arg(control_text_name, control_bg_name, border_name, highlight_name);
    const QString menu_style =
        QStringLiteral("QMenu{color:%1;background:%2;border:1px solid %3;}"
        "QMenu::item{padding:5px 22px;}"
        "QMenu::item:selected{background:%4;color:%5;}"
        "QMenu::item:disabled{color:%6;}")
            .arg(panel_text_name, panel_bg_name, border_name, highlight_name, highlighted_text_name, subtle_text_name);
    const QString themed_dialog_style =
        QStringLiteral("QDialog{background:%1;border:1px solid %2;}"
        "QLabel{color:%3;font-size:10px;background:transparent;}"
        "QToolButton{color:%4;background:%5;border:1px solid %2;border-radius:2px;padding:2px 6px;font-size:10px;}"
        "QToolButton:hover{background:%6;}"
        "QToolButton:checked{background:%7;border-color:%7;color:%8;}"
        "QToolButton:disabled{color:%9;background:%5;border-color:%2;}")
            .arg(panel_bg_name, border_name, panel_text_name, button_text_name, button_bg_name,
                 hover_bg_name, highlight_name, highlighted_text_name, subtle_text_name);

    auto style_form = [](QFormLayout *form) {
        form->setContentsMargins(6, 5, 6, 6);
        form->setHorizontalSpacing(5);
        form->setVerticalSpacing(3);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFormAlignment(Qt::AlignTop);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    };

    auto add_form_row = [this](QFormLayout *form, const QString &label_text, QWidget *field) {
        if (!form || label_text.isEmpty()) {
            if (form) form->addRow(label_text, field);
            return;
        }

        auto *label = new NumericDragLabel(label_text, field, form->parentWidget(),
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
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->addRow(label, field);
    };

    const QString checkbox_style =
        QStringLiteral("QCheckBox{color:%1;font-size:10px;spacing:5px;}"
        "QCheckBox::indicator{width:13px;height:13px;border:1px solid %2;"
        "background:%3;border-radius:2px;}"
        "QCheckBox::indicator:hover{border-color:%2;background:%4;}"
        "QCheckBox::indicator:checked{background:%5;border-color:%5;}")
            .arg(panel_text_name, border_name, button_bg_name, hover_bg_name, highlight_name);
    const QString push_button_style =
        QStringLiteral("QPushButton{color:%1;background:%2;border:1px solid %3;"
        "border-radius:2px;font-size:10px;padding:2px 8px;}"
        "QPushButton:hover{background:%4;border-color:%3;}"
        "QPushButton:pressed{background:%5;color:%6;border-color:%5;}")
            .arg(button_text_name, button_bg_name, border_name, hover_bg_name, highlight_name, highlighted_text_name);

    auto style_checkbox = [&](QCheckBox *box) {
        box->setFixedHeight(22);
        box->setStyleSheet(checkbox_style);
    };
    auto style_push_button = [&](QPushButton *button) {
        button->setFixedHeight(22);
        button->setStyleSheet(push_button_style);
    };

    auto mk_dspin = [&](double lo, double hi, double step) {
        auto *s = new QDoubleSpinBox(inner);
        s->setRange(lo, hi);
        s->setSingleStep(step);
        s->setDecimals(1);
        s->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
        s->setFixedHeight(22);
        s->setStyleSheet(control_style);
        return s;
    };

    auto mk_kf_button = [&](const QString &tip) {
        auto *b = new QPushButton(inner);
        b->setFixedSize(22, 22);
        b->setIconSize(QSize(16, 16));
        b->setIcon(keyframe_diamond_icon(false));
        b->setToolTip(tip);
        b->setAccessibleName(tip);
        b->setProperty("active", false);
        b->setProperty("outlined", false);
        const QColor kf_hover = panel_bg.lightness() < 128 ? panel_bg.lighter(128) : panel_bg.darker(108);
        const QColor kf_mark = highlight;
        QColor kf_mark_bg = kf_mark;
        kf_mark_bg.setAlpha(44);
        b->setStyleSheet(QStringLiteral(
            "QPushButton{background:transparent;border:none;border-radius:2px;padding:0;}"
            "QPushButton:hover{background:%1;}"
            "QPushButton[outlined=\"true\"]{background:%2;}"
            "QPushButton[active=\"true\"]{background:%2;}")
            .arg(kf_hover.name(QColor::HexRgb), kf_mark_bg.name(QColor::HexArgb)));
        return b;
    };

    auto with_kf = [&](QWidget *field, QPushButton *button) {
        auto *row = new QWidget(inner);
        auto *hl = new QHBoxLayout(row);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(3);
        field->setSizePolicy(QSizePolicy::Expanding, field->sizePolicy().verticalPolicy());
        hl->addWidget(button);
        hl->addWidget(field, 1);
        return row;
    };


    spn_px_      = mk_dspin(-9999, 9999, 1.0);
    spn_py_      = mk_dspin(-9999, 9999, 1.0);
    spn_scale_x_ = mk_dspin(-10000.0, 10000.0, 1.0);
    spn_scale_y_ = mk_dspin(-10000.0, 10000.0, 1.0);
    spn_scale_x_->setSuffix("%");
    spn_scale_y_->setSuffix("%");
    spn_transform_size_w_ = mk_dspin(0.0, 9999.0, 10.0);
    spn_transform_size_h_ = mk_dspin(0.0, 9999.0, 10.0);
    chk_scale_lock_ = new TransformLockCheckBox(inner);
    chk_scale_lock_->setChecked(true);
    chk_transform_size_lock_ = new TransformLockCheckBox(inner);
    chk_transform_size_lock_->setChecked(true);
    spn_rot_     = mk_dspin(-9999,  9999,  0.5);
    spn_opacity_ = mk_dspin(0.0,   1.0,  0.01);
    chk_scene_mask_ = new BglSwitch(bgl_tr("OBSTitles.UseAsSceneMask"), inner);
    chk_scene_mask_->setToolTip(bgl_tr("OBSTitles.SceneMaskRenderTooltip"));
    style_checkbox(chk_scene_mask_);
    chk_shape_scale_stroke_ = new BglSwitch(bgl_tr("OBSTitles.ScaleStroke"), inner);
    chk_shape_scale_stroke_->setToolTip(bgl_tr("OBSTitles.ScaleStrokeTooltip"));
    style_checkbox(chk_shape_scale_stroke_);
    chk_shape_scale_corners_ = new BglSwitch(bgl_tr("OBSTitles.ScaleCorners"), inner);
    chk_shape_scale_corners_->setToolTip(bgl_tr("OBSTitles.ScaleCornersTooltip"));
    style_checkbox(chk_shape_scale_corners_);
    spn_origin_x_ = mk_dspin(0.0, 1.0, 0.05);
    spn_origin_y_ = mk_dspin(0.0, 1.0, 0.05);
    spn_origin_x_->setDecimals(2);
    spn_origin_y_->setDecimals(2);
    spn_origin_x_->setToolTip(bgl_tr("OBSTitles.OriginXTooltip"));
    spn_origin_y_->setToolTip(bgl_tr("OBSTitles.OriginYTooltip"));
    cmb_anchor_ = new QComboBox(inner);
    for (const QString &label : QStringList{bgl_tr("OBSTitles.TopLeft"), bgl_tr("OBSTitles.TopCenter"), bgl_tr("OBSTitles.TopRight"), bgl_tr("OBSTitles.CenterLeft"), bgl_tr("OBSTitles.Center"), bgl_tr("OBSTitles.CenterRight"), bgl_tr("OBSTitles.BottomLeft"), bgl_tr("OBSTitles.BottomCenter"), bgl_tr("OBSTitles.BottomRight")})
        cmb_anchor_->addItem(label);
    cmb_anchor_->setToolTip(bgl_tr("OBSTitles.AnchorChangeTooltip"));
    cmb_anchor_->setFixedHeight(22);
    cmb_anchor_->setStyleSheet(control_style);

    btn_kf_pos_x_ = mk_kf_button(bgl_tr("OBSTitles.ToggleXKeyframe"));
    btn_kf_pos_y_ = mk_kf_button(bgl_tr("OBSTitles.ToggleYKeyframe"));
    btn_kf_scale_x_ = mk_kf_button(bgl_tr("OBSTitles.ToggleScaleXKeyframe"));
    btn_kf_scale_y_ = mk_kf_button(bgl_tr("OBSTitles.ToggleScaleYKeyframe"));
    btn_kf_transform_size_ = mk_kf_button(bgl_tr("OBSTitles.ToggleWidthKeyframe"));
    btn_kf_transform_size_->setToolTip(bgl_tr("OBSTitles.ToggleSizeKeyframe"));
    btn_kf_rotation_ = mk_kf_button(bgl_tr("OBSTitles.ToggleRotationKeyframe"));
    btn_kf_opacity_ = mk_kf_button(bgl_tr("OBSTitles.ToggleOpacityKeyframe"));
    btn_kf_origin_x_ = mk_kf_button(bgl_tr("OBSTitles.ToggleOriginXKeyframe"));
    btn_kf_origin_y_ = mk_kf_button(bgl_tr("OBSTitles.ToggleOriginYKeyframe"));
    btn_kf_pos_y_->hide();
    btn_kf_scale_y_->hide();
    btn_kf_opacity_->hide();
    btn_kf_origin_y_->hide();
    spn_opacity_->hide();
    chk_scene_mask_->hide();

    /* ── Transform ── */
    transform_box_ = new QWidget(inner);
    transform_box_->setStyleSheet(QStringLiteral("background:%1;").arg(section_bg_name));
    auto *tform_layout = new QVBoxLayout(transform_box_);
    tform_layout->setContentsMargins(10, 8, 10, 10);
    tform_layout->setSpacing(8);

    btn_transform_defaults_ = new QPushButton(bgl_tr("OBSTitles.Defaults"), transform_box_);
    btn_transform_defaults_->setFixedHeight(20);
    btn_transform_defaults_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    btn_transform_defaults_->setIconSize(QSize(12, 12));
    btn_transform_defaults_->setToolTip(bgl_tr("OBSTitles.RestoreTransformDefaults"));
    btn_transform_defaults_->setStyleSheet(push_button_style);

    auto *transform_grid = new QGridLayout();
    transform_grid->setContentsMargins(0, 4, 0, 0);
    transform_grid->setHorizontalSpacing(8);
    transform_grid->setVerticalSpacing(8);
    transform_grid->setColumnMinimumWidth(0, 24);
    transform_grid->setColumnMinimumWidth(1, 82);
    transform_grid->setColumnMinimumWidth(2, 86);
    transform_grid->setColumnMinimumWidth(3, 22);
    transform_grid->setColumnMinimumWidth(4, 86);
    transform_grid->setColumnStretch(5, 1);

    auto style_transform_spin = [&](QDoubleSpinBox *spin) {
        spin->setFixedSize(78, 22);
        spin->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
        spin->setPrefix(QString());
        spin->setStyleSheet(QStringLiteral("QDoubleSpinBox{color:%1;background:transparent;border:none;"
                            "padding:1px 2px;font-size:12px;selection-background-color:%2;}"
                            "QDoubleSpinBox::up-button,QDoubleSpinBox::down-button{width:12px;background:%3;"
                            "border-left:1px solid %4;}"
                            "QDoubleSpinBox::up-button:hover,QDoubleSpinBox::down-button:hover{background:%5;}")
                            .arg(control_text_name, highlight_name, button_bg_name, border_name, hover_bg_name));
    };
    style_transform_spin(spn_px_);
    style_transform_spin(spn_py_);
    style_transform_spin(spn_scale_x_);
    style_transform_spin(spn_scale_y_);
    style_transform_spin(spn_transform_size_w_);
    style_transform_spin(spn_transform_size_h_);
    style_transform_spin(spn_origin_x_);
    style_transform_spin(spn_origin_y_);
    style_transform_spin(spn_rot_);
    spn_rot_->setPrefix(QString());

    auto make_transform_field = [&](const QString &label, QDoubleSpinBox *spin, const char *icon_name = nullptr) {
        auto *field = new QWidget(transform_box_);
        field->setObjectName(QStringLiteral("OBSTitlesTransformNumericField"));
        field->setStyleSheet(QStringLiteral("QWidget#OBSTitlesTransformNumericField{background:%1;border:1px solid %2;"
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
        field_label->setFixedWidth(16);
        field_label->setAlignment(Qt::AlignCenter);
        if (icon_name && *icon_name) {
            field_label->setPixmap(obs_icon(icon_name).pixmap(14, 14));
            field_label->setText(QString());
        }
        field_label->setStyleSheet(QStringLiteral("QLabel{color:%1;background:transparent;font-size:11px;padding:0;}").arg(subtle_text_name));
        field_layout->addWidget(field_label);
        field_layout->addWidget(spin);
        field->setFixedSize(104, 24);
        return field;
    };

    QWidget *field_pos_x = make_transform_field(bgl_tr("OBSTitles.X"), spn_px_);
    QWidget *field_pos_y = make_transform_field(bgl_tr("OBSTitles.Y"), spn_py_);
    QWidget *field_scale_x = make_transform_field(bgl_tr("OBSTitles.W"), spn_scale_x_);
    QWidget *field_scale_y = make_transform_field(bgl_tr("OBSTitles.H"), spn_scale_y_);
    QWidget *field_transform_size_w = make_transform_field(bgl_tr("OBSTitles.W"), spn_transform_size_w_);
    QWidget *field_transform_size_h = make_transform_field(bgl_tr("OBSTitles.H"), spn_transform_size_h_);
    QWidget *field_origin_x = make_transform_field(bgl_tr("OBSTitles.X"), spn_origin_x_);
    QWidget *field_origin_y = make_transform_field(bgl_tr("OBSTitles.Y"), spn_origin_y_);
    QWidget *field_rotation = make_transform_field(bgl_tr("OBSTitles.R"), spn_rot_, "transform-rotation.svg");

    btn_anchor_grid_ = new AnchorGridButton(transform_box_);
    btn_anchor_grid_->setFixedSize(36, 36);
    btn_anchor_grid_->setToolTip(bgl_tr("OBSTitles.AnchorChangeTooltip"));
    if (auto *anchor_button = static_cast<AnchorGridButton *>(btn_anchor_grid_)) {
        anchor_button->anchor_selected = [this](int i) {
            if (cmb_anchor_) cmb_anchor_->setCurrentIndex(i);
        };
    }

    chk_scale_lock_->setText(QString());
    chk_scale_lock_->setToolTip(bgl_tr("OBSTitles.ScaleLock"));
    chk_scale_lock_->setFixedSize(24, 24);
    chk_transform_size_lock_->setText(QString());
    chk_transform_size_lock_->setToolTip(bgl_tr("OBSTitles.LockAspectRatio"));
    chk_transform_size_lock_->setFixedSize(24, 24);

    const QString transform_label_style = QStringLiteral("color:%1;font-size:12px;background:transparent;").arg(panel_text_name);
    auto add_transform_row = [&](int row, QPushButton *kf, const QString &label, QWidget *drag_field, QWidget *first,
                                 QWidget *middle, QWidget *second) {
        transform_grid->addWidget(kf, row, 0, Qt::AlignCenter);
        auto *text = new NumericDragLabel(label, drag_field, transform_box_,
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
        text->setStyleSheet(transform_label_style);
        transform_grid->addWidget(text, row, 1, Qt::AlignVCenter);
        transform_grid->addWidget(first, row, 2, Qt::AlignLeft);
        if (middle) transform_grid->addWidget(middle, row, 3, Qt::AlignCenter);
        if (second) transform_grid->addWidget(second, row, 4, Qt::AlignLeft);
        return text;
    };

    add_transform_row(0, btn_kf_pos_x_, bgl_tr("OBSTitles.Location"), field_pos_x, field_pos_x, nullptr, field_pos_y);
    transform_scale_label_ = add_transform_row(1, btn_kf_scale_x_, bgl_tr("OBSTitles.Scale"), field_scale_x, field_scale_x, chk_scale_lock_, field_scale_y);
    transform_scale_field_x_ = field_scale_x;
    transform_scale_field_y_ = field_scale_y;
    transform_size_label_ = add_transform_row(1, btn_kf_transform_size_, bgl_tr("OBSTitles.Size"), field_transform_size_w,
                                              field_transform_size_w, chk_transform_size_lock_, field_transform_size_h);
    transform_size_field_w_ = field_transform_size_w;
    transform_size_field_h_ = field_transform_size_h;
    auto *anchor_label = new NumericDragLabel(bgl_tr("OBSTitles.Anchor"), field_origin_x, transform_box_,
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
    anchor_label->setStyleSheet(transform_label_style);
    transform_grid->addWidget(anchor_label, 2, 1, Qt::AlignVCenter);
    transform_grid->addWidget(btn_kf_origin_x_, 2, 0, Qt::AlignCenter);
    transform_grid->addWidget(field_origin_x, 2, 2, Qt::AlignLeft);
    transform_grid->addWidget(field_origin_y, 2, 4, Qt::AlignLeft);
    add_transform_row(3, btn_kf_rotation_, bgl_tr("OBSTitles.Rotation"), field_rotation, field_rotation, nullptr, btn_anchor_grid_);
    row_shape_scale_options_ = new QWidget(transform_box_);
    row_shape_scale_options_->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *shape_scale_options_layout = new QHBoxLayout(row_shape_scale_options_);
    shape_scale_options_layout->setContentsMargins(0, 0, 0, 0);
    shape_scale_options_layout->setSpacing(10);
    shape_scale_options_layout->addWidget(chk_shape_scale_stroke_);
    shape_scale_options_layout->addWidget(chk_shape_scale_corners_);
    shape_scale_options_layout->addStretch(1);
    transform_grid->addWidget(row_shape_scale_options_, 4, 1, 1, 4);

    cmb_anchor_->hide();
    tform_layout->addLayout(transform_grid);
    bgl_add_panel_section(vl, transform_box_, bgl_tr("OBSTitles.Transform"), btn_transform_defaults_);

    /* ── Asset Layer ── */
    asset_box_ = new QGroupBox(bgl_tr("OBSTitles.AssetLayer"), inner);
    asset_box_->setStyleSheet(section_style);
    auto *asset_form = new QFormLayout(asset_box_);
    style_form(asset_form);
    lbl_asset_name_ = new QLabel(asset_box_);
    lbl_asset_name_->setWordWrap(true);
    cmb_asset_playback_ = new QComboBox(asset_box_);
    cmb_asset_playback_->addItem(bgl_tr("OBSTitles.AssetPlaybackSynchronized"), 0);
    cmb_asset_playback_->addItem(bgl_tr("OBSTitles.AssetPlaybackIndependent"), 1);
    cmb_asset_playback_->setStyleSheet(control_style);
    spn_asset_offset_ = new QDoubleSpinBox(asset_box_);
    spn_asset_offset_->setRange(-86400.0, 86400.0);
    spn_asset_offset_->setDecimals(3);
    spn_asset_offset_->setSuffix(QStringLiteral(" s"));
    spn_asset_offset_->setStyleSheet(control_style);
    spn_asset_pause_duration_ = new TimecodeSpinBox(asset_box_);
    spn_asset_pause_duration_->setRange(0.0, 86400.0);
    spn_asset_pause_duration_->setStyleSheet(control_style);
    spn_asset_loop_count_ = new QSpinBox(asset_box_);
    spn_asset_loop_count_->setRange(1, 1000000);
    spn_asset_loop_count_->setSuffix(bgl_tr("OBSTitles.AssetLoopTimesSuffix"));
    spn_asset_loop_count_->setStyleSheet(control_style);
    chk_asset_loop_ = new BglSwitch(bgl_tr("OBSTitles.AssetLoop"), asset_box_);
    chk_asset_loop_->setStyleSheet(checkbox_style);
    btn_asset_overrides_ = new QPushButton(bgl_tr("OBSTitles.EditAssetOverrides"), asset_box_);
    add_form_row(asset_form, bgl_tr("OBSTitles.AssetSource"), lbl_asset_name_);
    add_form_row(asset_form, bgl_tr("OBSTitles.AssetPlayback"), cmb_asset_playback_);
    add_form_row(asset_form, bgl_tr("OBSTitles.AssetPlaybackOffset"), spn_asset_offset_);
    add_form_row(asset_form, bgl_tr("OBSTitles.AssetPauseFor"), spn_asset_pause_duration_);
    add_form_row(asset_form, bgl_tr("OBSTitles.AssetLoopCount"), spn_asset_loop_count_);
    add_form_row(asset_form, QString(), chk_asset_loop_);
    add_form_row(asset_form, QString(), btn_asset_overrides_);
    asset_box_->setVisible(false);
    bgl_add_panel_section(vl, asset_box_);

    connect(cmb_asset_playback_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (loading_values_ || !layer_ || layer_->type != LayerType::Asset) return;
                layer_->asset_playback_mode = cmb_asset_playback_->currentData().toInt();
                bgs::asset_runtime::reset_layer(title_ ? title_->id : std::string(), layer_->id);
                update_asset_playback_controls_visibility();
                emit property_changed(true);
            });
    connect(spn_asset_offset_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) {
                if (loading_values_ || !layer_ || layer_->type != LayerType::Asset) return;
                layer_->asset_playback_offset = value;
                bgs::asset_runtime::reset_layer(title_ ? title_->id : std::string(), layer_->id);
                emit property_changed(true);
            });
    connect(spn_asset_pause_duration_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double value) {
                if (loading_values_ || !layer_ || layer_->type != LayerType::Asset) return;
                layer_->asset_pause_duration = value;
                bgs::asset_runtime::reset_layer(title_ ? title_->id : std::string(), layer_->id);
                emit property_changed(true);
            });
    connect(spn_asset_loop_count_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this](int value) {
                if (loading_values_ || !layer_ || layer_->type != LayerType::Asset) return;
                layer_->asset_loop_count = value;
                bgs::asset_runtime::reset_layer(title_ ? title_->id : std::string(), layer_->id);
                emit property_changed(true);
            });
    connect(chk_asset_loop_, &QCheckBox::toggled, this, [this](bool checked) {
        if (loading_values_ || !layer_ || layer_->type != LayerType::Asset) return;
        layer_->asset_loop = checked;
        bgs::asset_runtime::reset_layer(title_ ? title_->id : std::string(), layer_->id);
        emit property_changed(true);
    });
    connect(btn_asset_overrides_, &QPushButton::clicked, this, [this]() {
        if (layer_ && layer_->type == LayerType::Asset)
            emit asset_overrides_requested(layer_->id);
    });

    /* ── Appearance ── */
    appearance_box_ = new QWidget(inner);
    appearance_box_->setStyleSheet(QStringLiteral("background:%1;").arg(section_bg_name));
    auto *appearance_layout = new QVBoxLayout(appearance_box_);
    appearance_layout->setContentsMargins(10, 8, 10, 10);
    appearance_layout->setSpacing(8);

    auto *appearance_grid = new QGridLayout();
    appearance_grid->setContentsMargins(0, 4, 0, 0);
    appearance_grid->setHorizontalSpacing(8);
    appearance_grid->setVerticalSpacing(8);
    appearance_grid->setColumnMinimumWidth(0, 24);
    appearance_grid->setColumnMinimumWidth(1, 82);
    appearance_grid->setColumnMinimumWidth(2, 48);
    appearance_grid->setColumnMinimumWidth(3, 32);
    appearance_grid->setColumnMinimumWidth(4, 86);
    appearance_grid->setColumnStretch(5, 1);

    auto make_swatch = [&](const QString &tip) {
        auto *button = new QPushButton(appearance_box_);
        button->setFixedSize(32, 32);
        button->setToolTip(tip);
        button->setText(QString());
        return button;
    };
    btn_appearance_fill_color_ = make_swatch(bgl_tr("OBSTitles.FillColorTooltip"));
    btn_appearance_stroke_color_ = make_swatch(bgl_tr("OBSTitles.OutlineColorLabel"));
    spn_appearance_stroke_width_ = mk_dspin(0.0, 200.0, 1.0);
    spn_appearance_stroke_width_->setFixedSize(86, 24);
    spn_appearance_stroke_width_->setSuffix(QStringLiteral("px"));
    spn_appearance_stroke_width_->setDecimals(0);
    spn_appearance_stroke_width_->setStyleSheet(control_style);
    spn_appearance_opacity_ = mk_dspin(0.0, 100.0, 1.0);
    spn_appearance_opacity_->setFixedSize(86, 24);
    spn_appearance_opacity_->setSuffix(QStringLiteral("%"));
    spn_appearance_opacity_->setDecimals(0);
    spn_appearance_opacity_->setStyleSheet(control_style);
    btn_kf_appearance_fill_ = mk_kf_button(bgl_tr("OBSTitles.ToggleFillColorKeyframe"));
    btn_kf_appearance_stroke_ = mk_kf_button(bgl_tr("OBSTitles.OutlineColorLabel"));
    btn_kf_appearance_stroke_->setEnabled(false);
    btn_kf_appearance_opacity_ = btn_kf_opacity_;
    btn_kf_appearance_opacity_->show();
    auto *stroke_options_trigger = new QPushButton(appearance_box_);
    stroke_options_trigger->hide();
    btn_appearance_stroke_label_ = new StrokeOptionsLabel(
        bgl_tr("OBSTitles.Stroke"), spn_appearance_stroke_width_, appearance_box_,
        [stroke_options_trigger]() { stroke_options_trigger->click(); },
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
    btn_appearance_stroke_label_->setStyleSheet(
        QStringLiteral("QLabel{color:%1;background:transparent;text-decoration:underline;font-size:12px;padding:0;}"
        "QLabel:hover{color:%2;}").arg(panel_text_name, highlighted_text_name));

    auto add_appearance_row = [&](int row, QPushButton *kf, const QString &label, QWidget *primary,
                                  QWidget *secondary = nullptr, QWidget *label_widget = nullptr) {
        appearance_grid->addWidget(kf, row, 0, Qt::AlignCenter);
        QWidget *text = label_widget;
        if (!text) {
            text = new NumericDragLabel(label, secondary ? secondary : primary, appearance_box_,
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
            text->setStyleSheet(transform_label_style);
        }
        appearance_grid->addWidget(text, row, 1, Qt::AlignVCenter);
        if (primary)
            appearance_grid->addWidget(primary, row, 2, Qt::AlignLeft | Qt::AlignVCenter);
        if (secondary)
            appearance_grid->addWidget(secondary, row, 4, Qt::AlignLeft | Qt::AlignVCenter);
    };

    add_appearance_row(0, btn_kf_appearance_fill_, bgl_tr("OBSTitles.Fill"), btn_appearance_fill_color_);
    add_appearance_row(1, btn_kf_appearance_stroke_, bgl_tr("OBSTitles.Stroke"), btn_appearance_stroke_color_,
                       spn_appearance_stroke_width_, btn_appearance_stroke_label_);
    add_appearance_row(2, btn_kf_appearance_opacity_, bgl_tr("OBSTitles.Opacity"), nullptr,
                       spn_appearance_opacity_);
    appearance_layout->addLayout(appearance_grid);
    bgl_add_panel_section(vl, appearance_box_, bgl_tr("OBSTitles.Appearance"));

    auto make_property_grid = [&](QWidget *parent_widget) {
        auto *grid = new QGridLayout(parent_widget);
        grid->setContentsMargins(6, 5, 6, 6);
        grid->setHorizontalSpacing(5);
        grid->setVerticalSpacing(3);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);
        return grid;
    };
    auto grid_label = [&](const QString &text, QWidget *parent_widget, QWidget *field = nullptr) {
        auto *label = new NumericDragLabel(text, field, parent_widget,
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
        label->setStyleSheet(QStringLiteral("color:%1;font-size:10px;background:transparent;").arg(subtle_text_name));
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return label;
    };
    auto add_grid_field = [&](QGridLayout *grid, int row, int col, const QString &label_text, QWidget *field) {
        QWidget *parent_widget = grid->parentWidget();
        grid->addWidget(grid_label(label_text, parent_widget, field), row, col * 2);
        grid->addWidget(field, row, col * 2 + 1);
    };
    auto add_full_width_field = [&](QGridLayout *grid, int row, const QString &label_text, QWidget *field) {
        QWidget *parent_widget = grid->parentWidget();
        grid->addWidget(grid_label(label_text, parent_widget, field), row, 0);
        grid->addWidget(field, row, 1, 1, 3);
    };
    auto mk_combo = [&](const QStringList &labels, const QList<int> &values) {
        auto *combo = new QComboBox(inner);
        for (int i = 0; i < labels.size(); ++i)
            combo->addItem(labels[i], i < values.size() ? values[i] : i);
        combo->setFixedHeight(22);
        combo->setStyleSheet(control_style);
        return combo;
    };
    auto mk_type_button = [&](const QString &label, const QString &tip) {
        auto *button = new QToolButton(inner);
        button->setText(label);
        button->setToolTip(tip);
        button->setCheckable(true);
        button->setFixedSize(28, 22);
        button->setAutoRaise(false);
        button->setStyleSheet(QStringLiteral(
            "QToolButton{color:%1;background:%2;border:1px solid %3;border-radius:2px;"
            "font-size:10px;font-weight:bold;padding:0;}"
            "QToolButton:hover{background:%4;border-color:%3;}"
            "QToolButton:checked{background:%5;color:%6;border-color:%5;}")
            .arg(button_text_name, button_bg_name, border_name, hover_bg_name,
                 highlight_name, highlighted_text_name));
        return button;
    };
    auto mk_type_icon_button = [&](const char *icon_name, const QString &tip) {
        auto *button = mk_type_button(QString(), tip);
        button->setIcon(obs_icon(icon_name));
        button->setIconSize(QSize(16, 16));
        button->setAccessibleName(tip);
        return button;
    };
    auto mk_paragraph_alignment_button = [&](const char *icon_name, const QString &tip) {
        auto *button = new QToolButton(inner);
        button->setIcon(obs_icon(icon_name));
        button->setIconSize(QSize(14, 14));
        button->setToolTip(tip);
        button->setAccessibleName(tip);
        button->setCheckable(true);
        button->setFixedSize(30, 24);
        button->setAutoRaise(false);
        button->setStyleSheet(QStringLiteral(
            "QToolButton{color:%1;background:%2;border:1px solid %3;border-radius:2px;padding:2px;}"
            "QToolButton:hover{background:%4;border-color:%3;}"
            "QToolButton:checked{background:%5;color:%6;border-color:%5;}")
            .arg(button_text_name, button_bg_name, border_name, hover_bg_name,
                 highlight_name, highlighted_text_name));
        return button;
    };

    /* ── Character ── */
    text_box_ = new QGroupBox("Character", inner);
    text_box_->setStyleSheet(QStringLiteral(
        "QGroupBox{color:%1;background:%2;border:none;margin:0;padding:0;font-size:14px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:14px;top:8px;padding:0;background:transparent;}"
        "QLabel{color:%3;font-size:10px;background:transparent;}")
        .arg(panel_text_name, section_bg_name, subtle_text_name));
    auto *char_grid = make_property_grid(text_box_);
    char_grid->setContentsMargins(14, 28, 14, 12);
    char_grid->setHorizontalSpacing(8);
    char_grid->setVerticalSpacing(6);

    txt_content_ = new QTextEdit(inner);
    txt_content_->setAcceptRichText(false);
    txt_content_->setMinimumHeight(72);
    txt_content_->setMaximumHeight(92);
    txt_content_->setPlaceholderText(bgl_tr("OBSTitles.EnterTextPlaceholder"));
    txt_content_->setStyleSheet(control_style);

    cmb_font_ = new QComboBox(inner);
    cmb_font_->setFixedHeight(22);
    cmb_font_->setEditable(true);
    cmb_font_->setInsertPolicy(QComboBox::NoInsert);
    cmb_font_->setMaxVisibleItems(24);
    cmb_font_->setStyleSheet(control_style);
    QFontDatabase fdb;
    for (auto &fam : fdb.families())
        cmb_font_->addItem(fam, fam);

    cmb_font_style_ = new QComboBox(inner);
    cmb_font_style_->setFixedHeight(22);
    cmb_font_style_->setStyleSheet(control_style);

    spn_size_ = new QSpinBox(inner);
    spn_size_->setRange(6, 500);
    spn_size_->setFixedHeight(22);
    spn_size_->setStyleSheet(control_style);

