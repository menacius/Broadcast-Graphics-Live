#include "title-editor-internal.h"

#include <memory>
#include <QRegularExpression>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QScopedValueRollback>
#include <QStandardPaths>

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
        base = QDir::homePath() + QStringLiteral("/OBS Graphics Studio Pro");
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
                button->setToolTip(obsgs_tr("OBSTitles.PickColor"));
        }
    }
    dialog->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

static std::vector<PropertiesColorLibrary> properties_load_color_libraries()
{
    std::vector<PropertiesColorLibrary> libraries;
    PropertiesColorLibrary open_color;
    open_color.name = QStringLiteral("Open Color");
    auto add_open_color = [&](const QString &group, const QStringList &hexes) {
        for (int i = 0; i < hexes.size(); ++i) {
            QColor color(hexes[i]);
            if (color.isValid())
                open_color.colors.push_back({QStringLiteral("%1 %2").arg(group).arg(i), color});
        }
    };
    open_color.colors.push_back({QStringLiteral("white"), QColor(QStringLiteral("#ffffff"))});
    open_color.colors.push_back({QStringLiteral("black"), QColor(QStringLiteral("#000000"))});
    add_open_color(QStringLiteral("gray"), {"#f8f9fa", "#f1f3f5", "#e9ecef", "#dee2e6", "#ced4da", "#adb5bd", "#868e96", "#495057", "#343a40", "#212529"});
    add_open_color(QStringLiteral("red"), {"#fff5f5", "#ffe3e3", "#ffc9c9", "#ffa8a8", "#ff8787", "#ff6b6b", "#fa5252", "#f03e3e", "#e03131", "#c92a2a"});
    add_open_color(QStringLiteral("pink"), {"#fff0f6", "#ffdeeb", "#fcc2d7", "#faa2c1", "#f783ac", "#f06595", "#e64980", "#d6336c", "#c2255c", "#a61e4d"});
    add_open_color(QStringLiteral("grape"), {"#f8f0fc", "#f3d9fa", "#eebefa", "#e599f7", "#da77f2", "#cc5de8", "#be4bdb", "#ae3ec9", "#9c36b5", "#862e9c"});
    add_open_color(QStringLiteral("violet"), {"#f3f0ff", "#e5dbff", "#d0bfff", "#b197fc", "#9775fa", "#845ef7", "#7950f2", "#7048e8", "#6741d9", "#5f3dc4"});
    add_open_color(QStringLiteral("indigo"), {"#edf2ff", "#dbe4ff", "#bac8ff", "#91a7ff", "#748ffc", "#5c7cfa", "#4c6ef5", "#4263eb", "#3b5bdb", "#364fc7"});
    add_open_color(QStringLiteral("blue"), {"#e7f5ff", "#d0ebff", "#a5d8ff", "#74c0fc", "#4dabf7", "#339af0", "#228be6", "#1c7ed6", "#1971c2", "#1864ab"});
    add_open_color(QStringLiteral("cyan"), {"#e3fafc", "#c5f6fa", "#99e9f2", "#66d9e8", "#3bc9db", "#22b8cf", "#15aabf", "#1098ad", "#0c8599", "#0b7285"});
    add_open_color(QStringLiteral("teal"), {"#e6fcf5", "#c3fae8", "#96f2d7", "#63e6be", "#38d9a9", "#20c997", "#12b886", "#0ca678", "#099268", "#087f5b"});
    add_open_color(QStringLiteral("green"), {"#ebfbee", "#d3f9d8", "#b2f2bb", "#8ce99a", "#69db7c", "#51cf66", "#40c057", "#37b24d", "#2f9e44", "#2b8a3e"});
    add_open_color(QStringLiteral("lime"), {"#f4fce3", "#e9fac8", "#d8f5a2", "#c0eb75", "#a9e34b", "#94d82d", "#82c91e", "#74b816", "#66a80f", "#5c940d"});
    add_open_color(QStringLiteral("yellow"), {"#fff9db", "#fff3bf", "#ffec99", "#ffe066", "#ffd43b", "#fcc419", "#fab005", "#f59f00", "#f08c00", "#e67700"});
    add_open_color(QStringLiteral("orange"), {"#fff4e6", "#ffe8cc", "#ffd8a8", "#ffc078", "#ffa94d", "#ff922b", "#fd7e14", "#f76707", "#e8590c", "#d9480f"});
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
    vl->setContentsMargins(4, 4, 4, 4);
    vl->setSpacing(3);

    const QString section_style =
        QStringLiteral("QGroupBox{color:%1;background:%2;border:1px solid %3;"
        "border-radius:2px;margin-top:16px;font-size:10px;font-weight:bold;}"
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

    auto make_collapsible = [this](QGroupBox *box) {
        box->setCheckable(true);
        box->setChecked(true);
        QObject::connect(box, &QGroupBox::toggled, box, [this, box](bool expanded) {
            const int scroll = verticalScrollBar() ? verticalScrollBar()->value() : 0;
            if (auto *form = qobject_cast<QFormLayout *>(box->layout())) {
                for (int row = 0; row < form->rowCount(); ++row) {
                    for (auto role : {QFormLayout::LabelRole, QFormLayout::FieldRole}) {
                        if (auto *item = form->itemAt(row, role)) {
                            if (auto *widget = item->widget()) widget->setVisible(expanded);
                            if (auto *child_layout = item->layout()) {
                                for (int j = 0; j < child_layout->count(); ++j)
                                    if (auto *child = child_layout->itemAt(j)->widget()) child->setVisible(expanded);
                            }
                        }
                    }
                }
            } else if (box->layout()) {
                for (int i = 0; i < box->layout()->count(); ++i)
                    if (auto *widget = box->layout()->itemAt(i)->widget()) widget->setVisible(expanded);
            }
            QTimer::singleShot(0, this, [this, scroll]() {
                if (verticalScrollBar()) verticalScrollBar()->setValue(scroll);
            });
        });
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
    chk_scene_mask_ = new QCheckBox(obsgs_tr("OBSTitles.UseAsSceneMask"), inner);
    chk_scene_mask_->setToolTip(obsgs_tr("OBSTitles.SceneMaskRenderTooltip"));
    style_checkbox(chk_scene_mask_);
    chk_shape_scale_stroke_ = new QCheckBox(obsgs_tr("OBSTitles.ScaleStroke"), inner);
    chk_shape_scale_stroke_->setToolTip(obsgs_tr("OBSTitles.ScaleStrokeTooltip"));
    style_checkbox(chk_shape_scale_stroke_);
    chk_shape_scale_corners_ = new QCheckBox(obsgs_tr("OBSTitles.ScaleCorners"), inner);
    chk_shape_scale_corners_->setToolTip(obsgs_tr("OBSTitles.ScaleCornersTooltip"));
    style_checkbox(chk_shape_scale_corners_);
    spn_origin_x_ = mk_dspin(0.0, 1.0, 0.05);
    spn_origin_y_ = mk_dspin(0.0, 1.0, 0.05);
    spn_origin_x_->setDecimals(2);
    spn_origin_y_->setDecimals(2);
    spn_origin_x_->setToolTip(obsgs_tr("OBSTitles.OriginXTooltip"));
    spn_origin_y_->setToolTip(obsgs_tr("OBSTitles.OriginYTooltip"));
    cmb_anchor_ = new QComboBox(inner);
    for (const QString &label : QStringList{obsgs_tr("OBSTitles.TopLeft"), obsgs_tr("OBSTitles.TopCenter"), obsgs_tr("OBSTitles.TopRight"), obsgs_tr("OBSTitles.CenterLeft"), obsgs_tr("OBSTitles.Center"), obsgs_tr("OBSTitles.CenterRight"), obsgs_tr("OBSTitles.BottomLeft"), obsgs_tr("OBSTitles.BottomCenter"), obsgs_tr("OBSTitles.BottomRight")})
        cmb_anchor_->addItem(label);
    cmb_anchor_->setToolTip(obsgs_tr("OBSTitles.AnchorChangeTooltip"));
    cmb_anchor_->setFixedHeight(22);
    cmb_anchor_->setStyleSheet(control_style);

    btn_kf_pos_x_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleXKeyframe"));
    btn_kf_pos_y_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleYKeyframe"));
    btn_kf_scale_x_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleScaleXKeyframe"));
    btn_kf_scale_y_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleScaleYKeyframe"));
    btn_kf_transform_size_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleWidthKeyframe"));
    btn_kf_transform_size_->setToolTip(obsgs_tr("OBSTitles.ToggleSizeKeyframe"));
    btn_kf_rotation_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleRotationKeyframe"));
    btn_kf_opacity_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleOpacityKeyframe"));
    btn_kf_origin_x_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleOriginXKeyframe"));
    btn_kf_origin_y_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleOriginYKeyframe"));
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
    tform_layout->setContentsMargins(14, 0, 14, 12);
    tform_layout->setSpacing(8);

    auto *transform_header = new QWidget(transform_box_);
    transform_header->setStyleSheet("background:transparent;");
    auto *transform_header_layout = new QHBoxLayout(transform_header);
    transform_header_layout->setContentsMargins(0, 8, 0, 0);
    transform_header_layout->setSpacing(8);
    auto *transform_title = new QLabel(obsgs_tr("OBSTitles.Transform"), transform_header);
    transform_title->setStyleSheet(QStringLiteral("color:%1;font-size:14px;background:transparent;").arg(panel_text_name));
    transform_header_layout->addWidget(transform_title);
    transform_header_layout->addStretch();
    btn_transform_defaults_ = new QPushButton(obsgs_tr("OBSTitles.Defaults"), transform_header);
    btn_transform_defaults_->setFixedHeight(22);
    btn_transform_defaults_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    btn_transform_defaults_->setIconSize(QSize(13, 13));
    btn_transform_defaults_->setToolTip(obsgs_tr("OBSTitles.RestoreTransformDefaults"));
    btn_transform_defaults_->setStyleSheet(push_button_style);
    transform_header_layout->addWidget(btn_transform_defaults_);
    tform_layout->addWidget(transform_header);

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

    auto make_transform_field = [&](const QString &label, QDoubleSpinBox *spin) {
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
        field_label->setStyleSheet(QStringLiteral("QLabel{color:%1;background:transparent;font-size:11px;padding:0;}").arg(subtle_text_name));
        field_layout->addWidget(field_label);
        field_layout->addWidget(spin);
        field->setFixedSize(104, 24);
        return field;
    };

    QWidget *field_pos_x = make_transform_field(obsgs_tr("OBSTitles.X"), spn_px_);
    QWidget *field_pos_y = make_transform_field(obsgs_tr("OBSTitles.Y"), spn_py_);
    QWidget *field_scale_x = make_transform_field(obsgs_tr("OBSTitles.W"), spn_scale_x_);
    QWidget *field_scale_y = make_transform_field(obsgs_tr("OBSTitles.H"), spn_scale_y_);
    QWidget *field_transform_size_w = make_transform_field(obsgs_tr("OBSTitles.W"), spn_transform_size_w_);
    QWidget *field_transform_size_h = make_transform_field(obsgs_tr("OBSTitles.H"), spn_transform_size_h_);
    QWidget *field_origin_x = make_transform_field(obsgs_tr("OBSTitles.X"), spn_origin_x_);
    QWidget *field_origin_y = make_transform_field(obsgs_tr("OBSTitles.Y"), spn_origin_y_);
    QWidget *field_rotation = make_transform_field(obsgs_tr("OBSTitles.R"), spn_rot_);

    btn_anchor_grid_ = new AnchorGridButton(transform_box_);
    btn_anchor_grid_->setFixedSize(36, 36);
    btn_anchor_grid_->setToolTip(obsgs_tr("OBSTitles.AnchorChangeTooltip"));
    if (auto *anchor_button = static_cast<AnchorGridButton *>(btn_anchor_grid_)) {
        anchor_button->anchor_selected = [this](int i) {
            if (cmb_anchor_) cmb_anchor_->setCurrentIndex(i);
        };
    }

    chk_scale_lock_->setText(QString());
    chk_scale_lock_->setToolTip(obsgs_tr("OBSTitles.ScaleLock"));
    chk_scale_lock_->setFixedSize(24, 24);
    chk_transform_size_lock_->setText(QString());
    chk_transform_size_lock_->setToolTip(obsgs_tr("OBSTitles.LockAspectRatio"));
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

    add_transform_row(0, btn_kf_pos_x_, obsgs_tr("OBSTitles.Location"), field_pos_x, field_pos_x, nullptr, field_pos_y);
    transform_scale_label_ = add_transform_row(1, btn_kf_scale_x_, obsgs_tr("OBSTitles.Scale"), field_scale_x, field_scale_x, chk_scale_lock_, field_scale_y);
    transform_scale_field_x_ = field_scale_x;
    transform_scale_field_y_ = field_scale_y;
    transform_size_label_ = add_transform_row(1, btn_kf_transform_size_, obsgs_tr("OBSTitles.Size"), field_transform_size_w,
                                              field_transform_size_w, chk_transform_size_lock_, field_transform_size_h);
    transform_size_field_w_ = field_transform_size_w;
    transform_size_field_h_ = field_transform_size_h;
    auto *anchor_label = new NumericDragLabel(obsgs_tr("OBSTitles.Anchor"), field_origin_x, transform_box_,
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
    add_transform_row(3, btn_kf_rotation_, obsgs_tr("OBSTitles.Rotation"), field_rotation, field_rotation, nullptr, btn_anchor_grid_);
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
    vl->addWidget(transform_box_);

    /* ── Appearance ── */
    appearance_box_ = new QWidget(inner);
    appearance_box_->setStyleSheet(QStringLiteral("background:%1;").arg(section_bg_name));
    auto *appearance_layout = new QVBoxLayout(appearance_box_);
    appearance_layout->setContentsMargins(14, 2, 14, 12);
    appearance_layout->setSpacing(8);

    auto *appearance_title = new QLabel(obsgs_tr("OBSTitles.Appearance"), appearance_box_);
    appearance_title->setStyleSheet(QStringLiteral("color:%1;font-size:14px;background:transparent;").arg(panel_text_name));
    appearance_layout->addWidget(appearance_title);

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
    btn_appearance_fill_color_ = make_swatch(obsgs_tr("OBSTitles.FillColorTooltip"));
    btn_appearance_stroke_color_ = make_swatch(obsgs_tr("OBSTitles.OutlineColorLabel"));
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
    btn_kf_appearance_fill_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleFillColorKeyframe"));
    btn_kf_appearance_stroke_ = mk_kf_button(obsgs_tr("OBSTitles.OutlineColorLabel"));
    btn_kf_appearance_stroke_->setEnabled(false);
    btn_kf_appearance_opacity_ = btn_kf_opacity_;
    btn_kf_appearance_opacity_->show();
    auto *stroke_options_trigger = new QPushButton(appearance_box_);
    stroke_options_trigger->hide();
    btn_appearance_stroke_label_ = new StrokeOptionsLabel(
        obsgs_tr("OBSTitles.Stroke"), spn_appearance_stroke_width_, appearance_box_,
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

    add_appearance_row(0, btn_kf_appearance_fill_, obsgs_tr("OBSTitles.Fill"), btn_appearance_fill_color_);
    add_appearance_row(1, btn_kf_appearance_stroke_, obsgs_tr("OBSTitles.Stroke"), btn_appearance_stroke_color_,
                       spn_appearance_stroke_width_, btn_appearance_stroke_label_);
    add_appearance_row(2, btn_kf_appearance_opacity_, obsgs_tr("OBSTitles.Opacity"), nullptr,
                       spn_appearance_opacity_);
    appearance_layout->addLayout(appearance_grid);
    vl->addWidget(appearance_box_);

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
    txt_content_->setPlaceholderText(obsgs_tr("OBSTitles.EnterTextPlaceholder"));
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

    cmb_kerning_mode_ = mk_combo({obsgs_tr("OBSTitles.KerningMetrics"), obsgs_tr("OBSTitles.KerningOptical"), obsgs_tr("OBSTitles.KerningManual")}, {0, 1, 2});
    spn_kerning_value_ = mk_dspin(-100.0, 500.0, 1.0);
    spn_kerning_value_->setSuffix(" px");
    spn_kerning_value_->setToolTip(obsgs_tr("OBSTitles.ManualKerningTooltip"));
    spn_text_leading_ = mk_dspin(-200.0, 500.0, 1.0);
    spn_text_leading_->setSuffix(" px");
    spn_text_leading_->setToolTip(obsgs_tr("OBSTitles.LeadingTooltip"));
    spn_char_tracking_ = mk_dspin(-100.0, 500.0, 1.0);
    spn_char_tracking_->setSuffix(" px");
    spn_char_tracking_->setToolTip(obsgs_tr("OBSTitles.TrackingTooltip"));
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
    btn_kf_text_color_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleTextColorKeyframe"));

    char_grid->addWidget(grid_label(obsgs_tr("OBSTitles.TextLabel"), text_box_), 0, 0);
    char_grid->addWidget(txt_content_, 0, 1, 1, 3);
    add_full_width_field(char_grid, 1, "Font", cmb_font_);
    add_full_width_field(char_grid, 2, "Style", cmb_font_style_);
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
    vl->addWidget(text_box_);

    /* ── Type Options ── */
    type_options_box_ = new QGroupBox("Type", inner);
    type_options_box_->setObjectName(QStringLiteral("OBSGraphicsStudioProTextTypePanel"));
    type_options_box_->setStyleSheet(QStringLiteral(
        "QGroupBox#OBSGraphicsStudioProTextTypePanel{color:%1;background:%2;border:1px solid %3;"
        "border-radius:2px;margin-top:16px;padding:0;font-size:14px;}"
        "QGroupBox#OBSGraphicsStudioProTextTypePanel::title{subcontrol-origin:margin;left:14px;top:2px;padding:0 4px;background:%2;}")
        .arg(panel_text_name, section_bg_name, border_name));
    auto *type_grid = new QGridLayout(type_options_box_);
    type_grid->setContentsMargins(14, 24, 14, 12);
    type_grid->setHorizontalSpacing(4);
    type_grid->setVerticalSpacing(4);
    chk_bold_ = mk_type_button("B", obsgs_tr("OBSTitles.Bold"));
    chk_italic_ = mk_type_button("I", obsgs_tr("OBSTitles.Italic"));
    chk_font_kerning_ = mk_type_button("K", obsgs_tr("OBSTitles.Kerning"));
    chk_font_kerning_->setToolTip(obsgs_tr("OBSTitles.KerningTooltip"));
    btn_all_caps_ = mk_type_button("TT", "All Caps");
    btn_small_caps_ = mk_type_button("Tᴛ", "Small Caps");
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
    vl->addWidget(type_options_box_);

    /* ── Paragraph ── */
    paragraph_box_ = new QGroupBox("Paragraph", inner);
    paragraph_box_->setStyleSheet(QStringLiteral(
        "QGroupBox{color:%1;background:%2;border:none;margin:0;padding:0;font-size:14px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:14px;top:8px;padding:0;background:transparent;}"
        "QLabel{color:%3;font-size:10px;background:transparent;}")
        .arg(panel_text_name, section_bg_name, subtle_text_name));
    auto *paragraph_layout = new QVBoxLayout(paragraph_box_);
    paragraph_layout->setContentsMargins(14, 28, 14, 12);
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
        icon->setToolTip(QStringLiteral("%1\n%2").arg(tip, obsgs_tr("OBSTitles.DragNumericLabelTooltip")));
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
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "align-left.svg", obsgs_tr("OBSTitles.AlignLeft"), 0);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "align-center.svg", obsgs_tr("OBSTitles.AlignCenter"), 1);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "align-right.svg", obsgs_tr("OBSTitles.AlignRight"), 2);
    add_paragraph_gap(horizontal_button_layout);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "align-justify-left.svg", obsgs_tr("OBSTitles.JustifyLastLeft"), 3);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "align-justify-center.svg", obsgs_tr("OBSTitles.JustifyLastCenter"), 4);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "align-justify-right.svg", obsgs_tr("OBSTitles.JustifyLastRight"), 5);
    add_paragraph_gap(horizontal_button_layout);
    add_paragraph_button(horizontal_button_layout, grp_text_align_, "align-justify.svg", obsgs_tr("OBSTitles.JustifyAll"), 6);
    horizontal_button_layout->addStretch(1);
    paragraph_layout->addWidget(horizontal_buttons);

    auto *vertical_buttons = new QWidget(paragraph_box_);
    auto *vertical_button_layout = new QHBoxLayout(vertical_buttons);
    vertical_button_layout->setContentsMargins(0, 0, 0, 0);
    vertical_button_layout->setSpacing(4);
    grp_text_valign_ = new QButtonGroup(vertical_buttons);
    grp_text_valign_->setExclusive(true);
    add_paragraph_button(vertical_button_layout, grp_text_valign_, "align-top.svg", obsgs_tr("OBSTitles.AlignTop"), 0);
    add_paragraph_button(vertical_button_layout, grp_text_valign_, "align-vertical-center.svg", obsgs_tr("OBSTitles.AlignMiddle"), 1);
    add_paragraph_button(vertical_button_layout, grp_text_valign_, "align-bottom.svg", obsgs_tr("OBSTitles.AlignBottom"), 2);
    add_paragraph_button(vertical_button_layout, grp_text_valign_, "distribute-vertical.svg", obsgs_tr("OBSTitles.DistributeVertically"), 3);
    vertical_button_layout->addStretch(1);
    paragraph_layout->addWidget(vertical_buttons);

    spn_paragraph_indent_left_ = mk_paragraph_spin();
    spn_paragraph_indent_right_ = mk_paragraph_spin();
    spn_paragraph_indent_first_line_ = mk_paragraph_spin();
    btn_kf_paragraph_indent_left_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleParagraphIndentLeftKeyframe"));
    btn_kf_paragraph_indent_right_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleParagraphIndentRightKeyframe"));
    btn_kf_paragraph_indent_first_line_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleParagraphIndentFirstLineKeyframe"));
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
    add_metric_control(metric_grid, 0, 0, "paragraph-indent-left.svg", obsgs_tr("OBSTitles.ParagraphIndentLeft"), spn_paragraph_indent_left_, paragraph_indent_left_field);
    add_metric_control(metric_grid, 0, 1, "paragraph-indent-right.svg", obsgs_tr("OBSTitles.ParagraphIndentRight"), spn_paragraph_indent_right_, paragraph_indent_right_field);
    add_metric_control(metric_grid, 1, 0, "paragraph-indent-first-line.svg", obsgs_tr("OBSTitles.ParagraphIndentFirstLine"), spn_paragraph_indent_first_line_, paragraph_indent_first_line_field);
    add_metric_control(metric_grid, 2, 0, "paragraph-space-before.svg", obsgs_tr("OBSTitles.ParagraphSpaceBefore"), spn_paragraph_space_before_, paragraph_space_before_field);
    add_metric_control(metric_grid, 2, 1, "paragraph-space-after.svg", obsgs_tr("OBSTitles.ParagraphSpaceAfter"), spn_paragraph_space_after_, paragraph_space_after_field);
    paragraph_layout->addLayout(metric_grid);

    chk_paragraph_hyphenate_ = new QCheckBox(obsgs_tr("OBSTitles.Hyphenate"), paragraph_box_);
    style_checkbox(chk_paragraph_hyphenate_);
    paragraph_layout->addWidget(chk_paragraph_hyphenate_);

    vl->addWidget(paragraph_box_);

    /* ── Dynamic Text ── */
    dynamic_text_box_ = new QGroupBox("Text Flow", inner);
    dynamic_text_box_->setStyleSheet(QStringLiteral(
        "QGroupBox{color:%1;background:%2;border:none;margin:0;padding:0;font-size:14px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:14px;top:8px;padding:0;background:transparent;}"
        "QLabel{color:%3;font-size:10px;background:transparent;}")
        .arg(panel_text_name, section_bg_name, subtle_text_name));
    auto *dynamic_form = new QFormLayout(dynamic_text_box_);
    style_form(dynamic_form);
    dynamic_form->setContentsMargins(14, 28, 14, 12);
    dynamic_form->setHorizontalSpacing(8);
    dynamic_form->setVerticalSpacing(6);
    cmb_text_style_ = new QComboBox(inner);
    cmb_text_style_->addItem(obsgs_tr("OBSTitles.Normal"), 0);
    cmb_text_style_->addItem(obsgs_tr("OBSTitles.AllCaps"), 1);
    cmb_text_style_->addItem(obsgs_tr("OBSTitles.SmallCaps"), 2);
    cmb_text_style_->addItem(obsgs_tr("OBSTitles.Superscript"), 3);
    cmb_text_style_->addItem(obsgs_tr("OBSTitles.Subscript"), 4);
    cmb_text_style_->setToolTip(obsgs_tr("OBSTitles.TextStyleTooltip"));
    cmb_text_style_->setFixedHeight(22);
    cmb_text_style_->setStyleSheet(control_style);
    cmb_text_overflow_ = new QComboBox(inner);
    cmb_text_overflow_->addItem(obsgs_tr("OBSTitles.Wrap"), 0);
    cmb_text_overflow_->addItem(obsgs_tr("OBSTitles.Clip"), 1);
    cmb_text_overflow_->addItem(obsgs_tr("OBSTitles.HorizontalFit"), 2);
    cmb_text_overflow_->setToolTip(obsgs_tr("OBSTitles.TextOverflowTooltip"));
    cmb_text_overflow_->setFixedHeight(22);
    cmb_text_overflow_->setStyleSheet(control_style);
    spn_text_fit_min_scale_ = mk_dspin(0.05, 1.0, 0.05);
    spn_text_fit_min_scale_->setDecimals(2);
    spn_text_fit_min_scale_->setToolTip(obsgs_tr("OBSTitles.MinFitScaleTooltip"));
    lbl_text_fit_scale_ = new QLabel(obsgs_tr("OBSTitles.Scale100"), inner);
    lbl_text_fit_scale_->setStyleSheet(QStringLiteral("color:%1;font-size:10px;background:transparent;").arg(subtle_text_name));
    chk_expose_text_ = new QCheckBox(obsgs_tr("OBSTitles.ExposeInDock"), inner);
    chk_expose_text_->setToolTip(obsgs_tr("OBSTitles.ExposeInDockTooltip"));
    style_checkbox(chk_expose_text_);
    chk_exposed_hide_if_empty_ = new QCheckBox(obsgs_tr("OBSTitles.ExposedHideIfEmpty"), inner);
    chk_exposed_hide_if_empty_->setToolTip(obsgs_tr("OBSTitles.ExposedHideIfEmptyTooltip"));
    style_checkbox(chk_exposed_hide_if_empty_);
    chk_exposed_single_value_ = new QCheckBox(obsgs_tr("OBSTitles.ExposedSingleValue"), inner);
    chk_exposed_single_value_->setToolTip(obsgs_tr("OBSTitles.ExposedSingleValueTooltip"));
    style_checkbox(chk_exposed_single_value_);
    chk_ignore_persistence_ = new QCheckBox(obsgs_tr("OBSTitles.IgnorePersistence"), inner);
    chk_ignore_persistence_->setToolTip(obsgs_tr("OBSTitles.IgnorePersistenceTooltip"));
    style_checkbox(chk_ignore_persistence_);
    cmb_ticker_style_ = new QComboBox(inner);
    cmb_ticker_style_->addItem(obsgs_tr("OBSTitles.TickerHorizontal"), 0);
    cmb_ticker_style_->addItem(obsgs_tr("OBSTitles.TickerVerticalLine"), 1);
    cmb_ticker_style_->addItem(obsgs_tr("OBSTitles.TickerVerticalSmooth"), 2);
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
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.OverflowLabel"), cmb_text_overflow_);
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.MinFitScaleLabel"), spn_text_fit_min_scale_);
    add_form_row(dynamic_form, "", lbl_text_fit_scale_);
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.TickerStyleLabel"), cmb_ticker_style_);
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.TickerSpeedLabel"), spn_ticker_speed_);
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.TickerLineHoldLabel"), spn_ticker_line_hold_);
    add_form_row(dynamic_form, obsgs_tr("OBSTitles.DirectionLabel"), cmb_ticker_direction_);
    vl->addWidget(dynamic_text_box_);

    /* ── Auto Styling ── */
    auto_style_box_ = new QGroupBox("Auto Styling Rules", inner);
    auto_style_box_->setStyleSheet(QStringLiteral(
        "QGroupBox{color:%1;background:%2;border:none;margin:0;padding:0;font-size:14px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:14px;top:8px;padding:0;background:transparent;}"
        "QLabel{color:%3;font-size:10px;background:transparent;}")
        .arg(panel_text_name, section_bg_name, subtle_text_name));
    auto *auto_layout = new QVBoxLayout(auto_style_box_);
    auto_layout->setContentsMargins(14, 28, 14, 12);
    auto_layout->setSpacing(6);
    chk_auto_style_enabled_ = new QCheckBox("Enable automatic text styling", auto_style_box_);
    chk_auto_style_enabled_->setToolTip("When enabled, the selected text layer is styled by the rule engine before rendering. The original text stays editable; rules only produce final style runs.");
    style_checkbox(chk_auto_style_enabled_);
    auto_layout->addWidget(chk_auto_style_enabled_);

    auto *auto_help = new QLabel("Create rules that find text ranges, apply presets, and resolve conflicts before the renderer draws the final text runs.", auto_style_box_);
    auto_help->setWordWrap(true);
    auto_help->setToolTip("Auto Styling works on the whole text box and also on live text cue values. Manual inline formatting is applied last, so it can still override automatic rules.");
    auto_layout->addWidget(auto_help);

    auto *auto_form = new QFormLayout();
    style_form(auto_form);
    cmb_auto_default_style_ = new QComboBox(auto_style_box_);
    cmb_auto_default_style_->setFixedHeight(22);
    cmb_auto_default_style_->setStyleSheet(control_style);
    cmb_auto_default_style_->setToolTip("Base preset for the entire text box. Rules are applied on top of this default style.");
    auto_form->addRow("Base style", cmb_auto_default_style_);
    auto_layout->addLayout(auto_form);
    lst_auto_style_rules_ = new QListWidget(auto_style_box_);
    lst_auto_style_rules_->setFixedHeight(132);
    lst_auto_style_rules_->setStyleSheet(control_style);
    lst_auto_style_rules_->setToolTip("Rules are evaluated from top to bottom. Use the arrow buttons to change priority. Later rules can override, respect or exclude earlier/later rules depending on their conflict mode.");
    auto_layout->addWidget(lst_auto_style_rules_);
    auto *rule_form = new QFormLayout();
    style_form(rule_form);
    cmb_auto_rule_style_ = new QComboBox(auto_style_box_);
    cmb_auto_rule_style_->setFixedHeight(22);
    cmb_auto_rule_style_->setStyleSheet(control_style);
    cmb_auto_rule_style_->setToolTip("Text style preset to apply when this rule matches.");
    edt_auto_rule_name_ = new QLineEdit(auto_style_box_);
    edt_auto_rule_name_->setPlaceholderText("Optional rule name");
    edt_auto_rule_name_->setFixedHeight(22);
    edt_auto_rule_name_->setStyleSheet(control_style);
    edt_auto_rule_name_->setToolTip("Optional friendly name shown in the rule list. Leave empty to use the preset name.");
    chk_auto_rule_enabled_ = new QCheckBox("This rule is active", auto_style_box_);
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
    cmb_auto_rule_start_condition_ = new QComboBox(auto_style_box_);
    cmb_auto_rule_end_condition_ = new QComboBox(auto_style_box_);
    fill_auto_marker_combo(cmb_auto_rule_start_condition_);
    fill_auto_marker_combo(cmb_auto_rule_end_condition_);
    cmb_auto_rule_start_condition_->setFixedHeight(22);
    cmb_auto_rule_end_condition_->setFixedHeight(22);
    cmb_auto_rule_start_condition_->setStyleSheet(control_style);
    cmb_auto_rule_end_condition_->setStyleSheet(control_style);
    cmb_auto_rule_start_condition_->setToolTip("Where the styled range begins. Marker rules scan the entire text box, not only the beginning.");
    cmb_auto_rule_end_condition_->setToolTip("Where the styled range ends. For marker-based rules, this is searched after each matching From marker.");
    spn_auto_rule_start_offset_ = new QSpinBox(auto_style_box_);
    spn_auto_rule_end_offset_ = new QSpinBox(auto_style_box_);
    for (auto *spin : {spn_auto_rule_start_offset_, spn_auto_rule_end_offset_}) {
        spin->setRange(0, 100000);
        spin->setFixedHeight(22);
        spin->setStyleSheet(control_style);
        spin->setToolTip("Offset / occurrence number. For Character index, this is the exact character position. For marker rules, 0 means the first matching marker.");
    }
    edt_auto_rule_start_chars_ = new QLineEdit(auto_style_box_);
    edt_auto_rule_end_chars_ = new QLineEdit(auto_style_box_);
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
    auto *start_row = new QWidget(auto_style_box_);
    auto *start_lay = new QHBoxLayout(start_row);
    start_lay->setContentsMargins(0, 0, 0, 0);
    start_lay->setSpacing(4);
    start_lay->addWidget(cmb_auto_rule_start_condition_, 2);
    start_lay->addWidget(spn_auto_rule_start_offset_, 1);
    start_lay->addWidget(edt_auto_rule_start_chars_, 1);
    auto *end_row = new QWidget(auto_style_box_);
    auto *end_lay = new QHBoxLayout(end_row);
    end_lay->setContentsMargins(0, 0, 0, 0);
    end_lay->setSpacing(4);
    end_lay->addWidget(cmb_auto_rule_end_condition_, 2);
    end_lay->addWidget(spn_auto_rule_end_offset_, 1);
    end_lay->addWidget(edt_auto_rule_end_chars_, 1);

    chk_auto_rule_require_stop_match_ = new QCheckBox("Require stop match", auto_style_box_);
    chk_auto_rule_require_stop_match_->setChecked(true);
    chk_auto_rule_require_stop_match_->setToolTip("When enabled, the rule is skipped if the Stop matching at condition is not found after the Start condition. This prevents accidental styling to the line/text end when a delimiter is missing in live text cues.");
    style_checkbox(chk_auto_rule_require_stop_match_);
    chk_auto_rule_include_start_marker_ = new QCheckBox("Include From custom character", auto_style_box_);
    chk_auto_rule_include_start_marker_->setChecked(true);
    chk_auto_rule_include_start_marker_->setToolTip("When From is Custom character, include that character in the styled range. Disable it to start styling after the marker.");
    style_checkbox(chk_auto_rule_include_start_marker_);
    chk_auto_rule_include_end_marker_ = new QCheckBox("Include To custom character", auto_style_box_);
    chk_auto_rule_include_end_marker_->setChecked(false);
    chk_auto_rule_include_end_marker_->setToolTip("When Stop matching at is Custom character, include that stop character in the styled range. Leave disabled to stop before the marker.");
    style_checkbox(chk_auto_rule_include_end_marker_);
    auto *marker_options_row = new QWidget(auto_style_box_);
    auto *marker_options_lay = new QVBoxLayout(marker_options_row);
    marker_options_lay->setContentsMargins(0, 0, 0, 0);
    marker_options_lay->setSpacing(2);
    marker_options_lay->addWidget(chk_auto_rule_require_stop_match_);
    marker_options_lay->addWidget(chk_auto_rule_include_start_marker_);
    marker_options_lay->addWidget(chk_auto_rule_include_end_marker_);

    cmb_auto_rule_conflict_mode_ = new QComboBox(auto_style_box_);
    cmb_auto_rule_conflict_mode_->addItem("Override previous rules", "override_previous");
    cmb_auto_rule_conflict_mode_->addItem("Respect previous rules", "respect_previous");
    cmb_auto_rule_conflict_mode_->addItem("Apply only if empty", "apply_if_empty");
    cmb_auto_rule_conflict_mode_->addItem("Merge", "merge");
    cmb_auto_rule_conflict_mode_->addItem("Exclude selected rules", "exclude_other_rules");
    cmb_auto_rule_conflict_mode_->setFixedHeight(22);
    cmb_auto_rule_conflict_mode_->setStyleSheet(control_style);
    cmb_auto_rule_conflict_mode_->setToolTip("How this rule behaves when another rule already styled the same text. Override wins, Respect keeps previous fields, Apply only if empty affects unstyled text, Exclude blocks selected rules.");
    cmb_auto_rule_match_mode_ = new QComboBox(auto_style_box_);
    cmb_auto_rule_match_mode_->addItem("All matches", "all_matches");
    cmb_auto_rule_match_mode_->addItem("First match only", "first_match");
    cmb_auto_rule_match_mode_->setFixedHeight(22);
    cmb_auto_rule_match_mode_->setStyleSheet(control_style);
    cmb_auto_rule_match_mode_->setToolTip("Apply to every matching range in the text box or only to the first one.");
    edt_auto_rule_excludes_ = new QLineEdit(auto_style_box_);
    edt_auto_rule_excludes_->setPlaceholderText("Rule IDs to exclude, comma-separated");
    edt_auto_rule_excludes_->setFixedHeight(22);
    edt_auto_rule_excludes_->setStyleSheet(control_style);
    edt_auto_rule_excludes_->setToolTip("Comma-separated rule IDs to block where this rule matches. Example: rule_1, rule_3. Use with Conflict: Exclude selected rules.");
    chk_auto_rule_stop_processing_ = new QCheckBox("Stop later rules on this matched text", auto_style_box_);
    chk_auto_rule_stop_processing_->setToolTip("After this rule matches, later rules in the list cannot change the same characters. Useful for speaker names, labels, or protected prefixes.");
    style_checkbox(chk_auto_rule_stop_processing_);
    spn_auto_rule_chars_ = new QSpinBox(auto_style_box_);
    spn_auto_rule_chars_->hide();
    rule_form->addRow("Rule name", edt_auto_rule_name_);
    rule_form->addRow("Apply preset", cmb_auto_rule_style_);
    rule_form->addRow("Active", chk_auto_rule_enabled_);
    rule_form->addRow("Start matching at", start_row);
    rule_form->addRow("Stop matching at", end_row);
    rule_form->addRow("Stop safety", marker_options_row);
    rule_form->addRow("Apply to", cmb_auto_rule_match_mode_);
    rule_form->addRow("If rules overlap", cmb_auto_rule_conflict_mode_);
    rule_form->addRow("Exclude rules", edt_auto_rule_excludes_);
    rule_form->addRow("Protect match", chk_auto_rule_stop_processing_);
    auto_layout->addLayout(rule_form);
    auto *rule_buttons = new QHBoxLayout();
    btn_auto_rule_add_ = new QPushButton("Add rule", auto_style_box_);
    btn_auto_rule_update_ = new QPushButton("Save rule", auto_style_box_);
    btn_auto_rule_delete_ = new QPushButton("Delete", auto_style_box_);
    btn_auto_rule_up_ = new QPushButton("↑", auto_style_box_);
    btn_auto_rule_down_ = new QPushButton("↓", auto_style_box_);
    btn_auto_rule_add_->setToolTip("Create a new rule from the editor fields below.");
    btn_auto_rule_update_->setToolTip("Save the current editor fields into the selected rule.");
    btn_auto_rule_delete_->setToolTip("Delete the selected rule.");
    btn_auto_rule_up_->setToolTip("Move selected rule earlier. Earlier rules are evaluated first.");
    btn_auto_rule_down_->setToolTip("Move selected rule later. Later rules can override earlier ones depending on conflict mode.");
    for (auto *b : {btn_auto_rule_add_, btn_auto_rule_update_, btn_auto_rule_delete_, btn_auto_rule_up_, btn_auto_rule_down_}) {
        style_push_button(b);
        rule_buttons->addWidget(b);
    }
    auto_layout->addLayout(rule_buttons);
    vl->addWidget(auto_style_box_);

    /* ── Live Edit ── */
    live_edit_box_ = new QGroupBox(obsgs_tr("OBSTitles.LiveEdit"), inner);
    live_edit_box_->setStyleSheet(QStringLiteral(
        "QGroupBox{color:%1;background:%2;border:none;margin:0;padding:0;font-size:14px;}"
        "QGroupBox::title{subcontrol-origin:margin;left:14px;top:8px;padding:0;background:transparent;}"
        "QLabel{color:%3;font-size:10px;background:transparent;}")
        .arg(panel_text_name, section_bg_name, subtle_text_name));
    auto *live_edit_form = new QFormLayout(live_edit_box_);
    style_form(live_edit_form);
    live_edit_form->setContentsMargins(14, 28, 14, 12);
    live_edit_form->setHorizontalSpacing(8);
    live_edit_form->setVerticalSpacing(6);
    chk_scene_mask_->setText(obsgs_tr("OBSTitles.SetAsSceneMask"));
    chk_scene_mask_->setToolTip(obsgs_tr("OBSTitles.LiveSceneMaskTooltip"));
    chk_scene_mask_->setFixedHeight(22);
    chk_scene_mask_->setStyleSheet(checkbox_style);
    chk_expose_text_->setText(obsgs_tr("OBSTitles.ExposeToDock"));
    chk_expose_text_->setToolTip(obsgs_tr("OBSTitles.ExposeInDockTooltip"));
    add_form_row(live_edit_form, obsgs_tr("OBSTitles.SceneMask"), chk_scene_mask_);
    add_form_row(live_edit_form, obsgs_tr("OBSTitles.DockEditing"), chk_expose_text_);
    add_form_row(live_edit_form, QString(), chk_exposed_hide_if_empty_);
    add_form_row(live_edit_form, QString(), chk_exposed_single_value_);
    add_form_row(live_edit_form, obsgs_tr("OBSTitles.Persistence"), chk_ignore_persistence_);
    vl->addWidget(live_edit_box_);

    /* ── Bullets and Numbering ── */
    bullets_box_ = new QGroupBox("Bullets and Numbering", inner);
    bullets_box_->setStyleSheet(section_style);
    auto *bullets_layout = new QVBoxLayout(bullets_box_);
    bullets_layout->setContentsMargins(6, 5, 6, 6);
    auto *bullets_hint = new QLabel("Broadcast lower thirds typically use manual bullet glyphs; this group is ready for list presets.", inner);
    bullets_hint->setWordWrap(true);
    bullets_hint->setStyleSheet(QStringLiteral("color:%1;font-size:10px;background:transparent;").arg(subtle_text_name));
    bullets_layout->addWidget(bullets_hint);
    vl->addWidget(bullets_box_);
    bullets_box_->hide();

    /* ── Shape ── */
    rect_box_ = new QGroupBox(inner);
    rect_box_->setTitle(QString());
    rect_box_->setStyleSheet(QStringLiteral("QGroupBox{background:%1;border:none;margin:0;padding:0;}").arg(section_bg_name));
    auto *shape_layout = new QVBoxLayout(rect_box_);
    shape_layout->setContentsMargins(14, 0, 14, 12);
    shape_layout->setSpacing(8);

    auto *shape_header = new QWidget(rect_box_);
    shape_header->setStyleSheet("background:transparent;");
    auto *shape_header_layout = new QHBoxLayout(shape_header);
    shape_header_layout->setContentsMargins(0, 8, 0, 0);
    shape_header_layout->setSpacing(8);
    auto *shape_title = new QLabel(obsgs_tr("OBSTitles.Shape"), shape_header);
    shape_title->setObjectName(QStringLiteral("OBSTitlesShapePanelTitle"));
    shape_title->setStyleSheet(QStringLiteral("color:%1;font-size:14px;background:transparent;").arg(panel_text_name));
    shape_header_layout->addWidget(shape_title);
    shape_header_layout->addStretch();
    btn_shape_defaults_ = new QPushButton(obsgs_tr("OBSTitles.Defaults"), shape_header);
    btn_shape_defaults_->setFixedHeight(22);
    btn_shape_defaults_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    btn_shape_defaults_->setIconSize(QSize(13, 13));
    btn_shape_defaults_->setToolTip(obsgs_tr("OBSTitles.RestoreShapeDefaults"));
    btn_shape_defaults_->setStyleSheet(push_button_style);
    shape_header_layout->addWidget(btn_shape_defaults_);
    shape_layout->addWidget(shape_header);

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
                                 ShapeType::Diamond, ShapeType::Line}) {
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
    chk_text_box_width_to_text_ = new QCheckBox(obsgs_tr("OBSTitles.TextBoxWidthToText"), inner);
    chk_text_box_height_to_text_ = new QCheckBox(obsgs_tr("OBSTitles.TextBoxHeightToText"), inner);
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
    cmb_shape_type_->addItem(obsgs_tr("OBSTitles.Path"), (int)ShapeType::Path);
    cmb_shape_type_->addItem("Line", (int)ShapeType::Line);
    cmb_shape_type_->setFixedHeight(22);
    cmb_shape_type_->setStyleSheet(control_style);
    cmb_shape_type_->hide();
    spn_shape_points_ = new QSpinBox(inner); spn_shape_points_->setRange(3, 64); spn_shape_points_->setFixedHeight(22); spn_shape_points_->setStyleSheet(control_style);
    spn_shape_sides_ = new QSpinBox(inner); spn_shape_sides_->setRange(3, 64); spn_shape_sides_->setFixedHeight(22); spn_shape_sides_->setStyleSheet(control_style);
    spn_shape_inner_radius_ = mk_dspin(0.0, 1.0, 0.05); spn_shape_inner_radius_->setDecimals(2);
    spn_shape_outer_radius_ = mk_dspin(0.0, 1.0, 0.05); spn_shape_outer_radius_->setDecimals(2);
    spn_shape_roundness_ = mk_dspin(0.0, 1000.0, 1.0);
    btn_kf_width_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleWidthKeyframe"));
    btn_kf_width_->setToolTip(obsgs_tr("OBSTitles.ToggleSizeKeyframe"));
    btn_kf_image_box_size_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleSizeKeyframe"));

    style_transform_spin(spn_layer_w_);
    style_transform_spin(spn_layer_h_);
    style_transform_spin(spn_image_box_w_);
    style_transform_spin(spn_image_box_h_);
    style_transform_spin(spn_rect_corner_tl_);
    style_transform_spin(spn_rect_corner_tr_);
    style_transform_spin(spn_rect_corner_br_);
    style_transform_spin(spn_rect_corner_bl_);
    style_transform_spin(spn_corner_bevel_roundness_);
    auto *field_width = make_shape_field(obsgs_tr("OBSTitles.W"), spn_layer_w_);
    auto *field_height = make_shape_field(obsgs_tr("OBSTitles.H"), spn_layer_h_);
    chk_size_lock_ = new TransformLockCheckBox(rect_box_);
    chk_size_lock_->setText(QString());
    chk_size_lock_->setToolTip(obsgs_tr("OBSTitles.LockAspectRatio"));
    chk_size_lock_->setFixedSize(24, 24);
    chk_size_lock_->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *size_label = new QLabel(obsgs_tr("OBSTitles.Size"), rect_box_);
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
    corners_layout->addWidget(make_shape_field(obsgs_tr("OBSTitles.TL"), spn_rect_corner_tl_, obs_icon("corner-radius-tl.svg")), 0, 0);
    corners_layout->addWidget(make_shape_field(obsgs_tr("OBSTitles.TR"), spn_rect_corner_tr_, obs_icon("corner-radius-tr.svg")), 0, 1);
    chk_corner_lock_ = new TransformLockCheckBox(row_rect_corners_);
    chk_corner_lock_->setText(QString());
    chk_corner_lock_->setToolTip(obsgs_tr("OBSTitles.SyncCornerRadii"));
    chk_corner_lock_->setFixedSize(24, 24);
    chk_corner_lock_->setStyleSheet(QStringLiteral("background:transparent;"));
    corners_layout->addWidget(chk_corner_lock_, 0, 2, 2, 1, Qt::AlignCenter);
    corners_layout->addWidget(make_shape_field(obsgs_tr("OBSTitles.BL"), spn_rect_corner_bl_, obs_icon("corner-radius-bl.svg")), 1, 0);
    corners_layout->addWidget(make_shape_field(obsgs_tr("OBSTitles.BR"), spn_rect_corner_br_, obs_icon("corner-radius-br.svg")), 1, 1);

    auto *shape_form_widget = new QWidget(rect_box_);
    shape_form_widget->setStyleSheet("background:transparent;");
    auto *rfl = new QFormLayout(shape_form_widget);
    style_form(rfl);
    add_form_row(rfl, "", chk_text_box_width_to_text_);
    add_form_row(rfl, obsgs_tr("OBSTitles.MaxTextBoxWidthLabel"), spn_max_text_box_width_);
    add_form_row(rfl, "", chk_text_box_height_to_text_);
    add_form_row(rfl, obsgs_tr("OBSTitles.MaxTextBoxHeightLabel"), spn_max_text_box_height_);
    add_form_row(rfl, obsgs_tr("OBSTitles.CornerLabel"), row_rect_corners_);
    add_form_row(rfl, QStringLiteral("Corner Roundness"), spn_corner_bevel_roundness_);
    add_form_row(rfl, "Points", spn_shape_points_);
    add_form_row(rfl, "Sides", spn_shape_sides_);
    add_form_row(rfl, "Inner Radius", spn_shape_inner_radius_);
    add_form_row(rfl, "Outer Radius", spn_shape_outer_radius_);
    add_form_row(rfl, "Roundness", spn_shape_roundness_);
    cmb_fill_type_ = new QComboBox(inner);
    cmb_fill_type_->addItem(obsgs_tr("OBSTitles.Solid"), 0);
    cmb_fill_type_->addItem(obsgs_tr("OBSTitles.Gradient"), 1);
    cmb_fill_type_->setFixedHeight(22);
    cmb_fill_type_->setStyleSheet(control_style);
    row_fill_type_ = cmb_fill_type_;
    add_form_row(rfl, obsgs_tr("OBSTitles.FillTypeLabel"), row_fill_type_);
    btn_fill_color_ = new QPushButton(inner);
    btn_kf_fill_color_ = mk_kf_button(obsgs_tr("OBSTitles.ToggleFillColorKeyframe"));
    row_fill_color_ = with_kf(btn_fill_color_, btn_kf_fill_color_);
    add_form_row(rfl, obsgs_tr("OBSTitles.ColorLabel"), row_fill_color_);
    shape_layout->addWidget(shape_form_widget);
    vl->addWidget(rect_box_);

    /* ── Gradient Properties ── */
    gradient_box_ = new QGroupBox(obsgs_tr("OBSTitles.GradientProperties"), inner);
    gradient_box_->setStyleSheet(section_style);
    auto *gfl = new QFormLayout(gradient_box_);
    style_form(gfl);
    cmb_gradient_type_ = new QComboBox(inner);
    cmb_gradient_type_->addItem(obsgs_tr("OBSTitles.LinearGradient"), 0);
    cmb_gradient_type_->addItem(obsgs_tr("OBSTitles.RadialGradient"), 1);
    cmb_gradient_type_->addItem(obsgs_tr("OBSTitles.ConicalGradient"), 2);
    cmb_gradient_type_->setFixedHeight(22);
    cmb_gradient_type_->setStyleSheet(control_style);
    cmb_gradient_spread_ = new QComboBox(inner);
    cmb_gradient_spread_->addItem(obsgs_tr("OBSTitles.No"), 0);
    cmb_gradient_spread_->addItem(obsgs_tr("OBSTitles.Repeat"), 2);
    cmb_gradient_spread_->addItem(obsgs_tr("OBSTitles.Reflect"), 1);
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
    add_form_row(gfl, obsgs_tr("OBSTitles.GradientTypeLabel"), cmb_gradient_type_);
    add_form_row(gfl, obsgs_tr("OBSTitles.SpreadLabel"), cmb_gradient_spread_);
    add_form_row(gfl, obsgs_tr("OBSTitles.StartColorLabel"), btn_gradient_start_color_);
    add_form_row(gfl, obsgs_tr("OBSTitles.StartStopLabel"), spn_gradient_start_pos_);
    add_form_row(gfl, obsgs_tr("OBSTitles.StartOpacityLabel"), spn_gradient_start_opacity_);
    add_form_row(gfl, obsgs_tr("OBSTitles.EndColorLabel"), btn_gradient_end_color_);
    add_form_row(gfl, obsgs_tr("OBSTitles.EndStopLabel"), spn_gradient_end_pos_);
    add_form_row(gfl, obsgs_tr("OBSTitles.EndOpacityLabel"), spn_gradient_end_opacity_);
    add_form_row(gfl, obsgs_tr("OBSTitles.OpacityLabel"), spn_gradient_opacity_);
    add_form_row(gfl, obsgs_tr("OBSTitles.AngleLabel"), spn_gradient_angle_);
    add_form_row(gfl, obsgs_tr("OBSTitles.CenterXLabel"), spn_gradient_center_x_);
    add_form_row(gfl, obsgs_tr("OBSTitles.CenterYLabel"), spn_gradient_center_y_);
    add_form_row(gfl, obsgs_tr("OBSTitles.ScaleLabel"), spn_gradient_scale_);
    add_form_row(gfl, obsgs_tr("OBSTitles.FocalXLabel"), spn_gradient_focal_x_);
    add_form_row(gfl, obsgs_tr("OBSTitles.FocalYLabel"), spn_gradient_focal_y_);
    vl->addWidget(gradient_box_);
    make_collapsible(gradient_box_);

    /* ── Image ── */
    image_box_ = new QWidget(inner);
    image_box_->setStyleSheet(QStringLiteral("background:%1;").arg(section_bg_name));
    auto *image_layout = new QVBoxLayout(image_box_);
    image_layout->setContentsMargins(14, 0, 14, 12);
    image_layout->setSpacing(8);

    auto *image_header = new QWidget(image_box_);
    image_header->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *image_header_layout = new QHBoxLayout(image_header);
    image_header_layout->setContentsMargins(0, 8, 0, 0);
    image_header_layout->setSpacing(8);
    auto *image_title = new QLabel(obsgs_tr("OBSTitles.Image"), image_header);
    image_title->setStyleSheet(QStringLiteral("color:%1;font-size:14px;background:transparent;").arg(panel_text_name));
    image_header_layout->addWidget(image_title);
    image_header_layout->addStretch(1);
    image_layout->addWidget(image_header);

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

    auto *image_form_widget = new QWidget(image_content);
    image_form_widget->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *image_form = new QFormLayout(image_form_widget);
    style_form(image_form);
    edit_image_path_ = new QLineEdit(image_form_widget);
    edit_image_path_->setFixedHeight(22);
    edit_image_path_->setStyleSheet(control_style);
    btn_pick_image_ = new QPushButton(obsgs_tr("OBSTitles.Browse"), image_form_widget);
    style_push_button(btn_pick_image_);
    spn_layer_w_->setToolTip(obsgs_tr("OBSTitles.ImageWidthTooltip"));
    spn_layer_h_->setToolTip(obsgs_tr("OBSTitles.ImageHeightTooltip"));
    cmb_image_scale_filter_ = new QComboBox(image_form_widget);
    cmb_image_scale_filter_->setFixedHeight(22);
    cmb_image_scale_filter_->setStyleSheet(control_style);
    cmb_image_scale_filter_->addItem(obsgs_tr("OBSTitles.ScaleFilterDisable"), (int)ImageScaleFilter::Disable);
    cmb_image_scale_filter_->addItem(obsgs_tr("OBSTitles.ScaleFilterBilinear"), (int)ImageScaleFilter::Bilinear);
    cmb_image_scale_filter_->addItem(obsgs_tr("OBSTitles.ScaleFilterBicubic"), (int)ImageScaleFilter::Bicubic);
    cmb_image_scale_filter_->addItem(obsgs_tr("OBSTitles.ScaleFilterLanczos"), (int)ImageScaleFilter::Lanczos);
    cmb_image_scale_filter_->addItem(obsgs_tr("OBSTitles.ScaleFilterArea"), (int)ImageScaleFilter::Area);
    cmb_image_box_mode_ = new QComboBox(image_form_widget);
    cmb_image_box_mode_->setFixedHeight(22);
    cmb_image_box_mode_->setStyleSheet(control_style);
    cmb_image_box_mode_->addItem(obsgs_tr("OBSTitles.ImageBoxFitImageToBox"), (int)ImageBoxMode::FitImageToBox);
    cmb_image_box_mode_->addItem(obsgs_tr("OBSTitles.ImageBoxFillHorizontal"), (int)ImageBoxMode::FillHorizontal);
    cmb_image_box_mode_->addItem(obsgs_tr("OBSTitles.ImageBoxFillVertical"), (int)ImageBoxMode::FillVertical);
    cmb_image_box_mode_->addItem(obsgs_tr("OBSTitles.ImageBoxFitToLongSide"), (int)ImageBoxMode::FitToLongSide);
    cmb_image_box_mode_->addItem(obsgs_tr("OBSTitles.ImageBoxFitToShortSide"), (int)ImageBoxMode::FitToShortSide);
    cmb_image_box_mode_->addItem(obsgs_tr("OBSTitles.ImageBoxStretchToFill"), (int)ImageBoxMode::StretchToFill);
    chk_image_crop_when_outside_box_ = new QCheckBox(obsgs_tr("OBSTitles.ImageBoxCropWhenOutside"), image_form_widget);
    style_checkbox(chk_image_crop_when_outside_box_);
    btn_image_anchor_grid_ = new AnchorGridButton(image_form_widget);
    btn_image_anchor_grid_->setFixedSize(36, 36);
    btn_image_anchor_grid_->setToolTip(obsgs_tr("OBSTitles.ImageBoxAnchorTooltip"));
    add_form_row(image_form, obsgs_tr("OBSTitles.PathLabel"), edit_image_path_);
    add_form_row(image_form, "", btn_pick_image_);
    add_form_row(image_form, obsgs_tr("OBSTitles.ImageBoxMode"), cmb_image_box_mode_);
    add_form_row(image_form, QString(), chk_image_crop_when_outside_box_);
    add_form_row(image_form, obsgs_tr("OBSTitles.ImageBoxAnchor"), btn_image_anchor_grid_);
    add_form_row(image_form, obsgs_tr("OBSTitles.Filtering"), cmb_image_scale_filter_);
    image_content_layout->addWidget(image_form_widget, 1);
    image_layout->addWidget(image_content);
    const int image_size_index = vl->indexOf(rect_box_);
    if (image_size_index >= 0)
        vl->insertWidget(image_size_index, image_box_);
    else
        vl->addWidget(image_box_);

    image_box_size_box_ = new QWidget(inner);
    image_box_size_box_->setStyleSheet(QStringLiteral("background:%1;").arg(section_bg_name));
    auto *image_box_size_layout = new QVBoxLayout(image_box_size_box_);
    image_box_size_layout->setContentsMargins(14, 0, 14, 12);
    image_box_size_layout->setSpacing(8);
    auto *image_box_size_header = new QWidget(image_box_size_box_);
    image_box_size_header->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *image_box_size_header_layout = new QHBoxLayout(image_box_size_header);
    image_box_size_header_layout->setContentsMargins(0, 8, 0, 0);
    image_box_size_header_layout->setSpacing(8);
    auto *image_box_size_title = new QLabel(obsgs_tr("OBSTitles.ImageBoxSize"), image_box_size_header);
    image_box_size_title->setStyleSheet(QStringLiteral("color:%1;font-size:14px;background:transparent;").arg(panel_text_name));
    image_box_size_header_layout->addWidget(image_box_size_title);
    image_box_size_header_layout->addStretch(1);
    image_box_size_layout->addWidget(image_box_size_header);

    auto *image_box_size_grid = new QGridLayout();
    image_box_size_grid->setContentsMargins(0, 0, 0, 0);
    image_box_size_grid->setHorizontalSpacing(8);
    image_box_size_grid->setVerticalSpacing(8);
    image_box_size_grid->setColumnMinimumWidth(0, 24);
    image_box_size_grid->setColumnMinimumWidth(1, 82);
    image_box_size_grid->setColumnMinimumWidth(2, 86);
    image_box_size_grid->setColumnMinimumWidth(3, 22);
    image_box_size_grid->setColumnMinimumWidth(4, 86);
    image_box_size_grid->setColumnStretch(5, 1);
    auto make_image_box_size_field = [&](const QString &label, QDoubleSpinBox *spin) {
        auto *field = new QWidget(image_box_size_box_);
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
        field_label->setFixedWidth(16);
        field_label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        field_label->setStyleSheet(QStringLiteral("color:%1;background:transparent;font-size:12px;").arg(control_text_name));
        field_layout->addWidget(field_label);
        field_layout->addWidget(spin, 1);
        field->setFixedSize(104, 24);
        return field;
    };
    chk_image_box_size_lock_ = new TransformLockCheckBox(image_box_size_box_);
    chk_image_box_size_lock_->setText(QString());
    chk_image_box_size_lock_->setToolTip(obsgs_tr("OBSTitles.LockAspectRatio"));
    chk_image_box_size_lock_->setFixedSize(24, 24);
    chk_image_box_size_lock_->setStyleSheet(QStringLiteral("background:transparent;"));
    auto *image_box_size_label = new QLabel(obsgs_tr("OBSTitles.Size"), image_box_size_box_);
    image_box_size_label->setStyleSheet(QStringLiteral("color:%1;background:transparent;font-size:13px;").arg(panel_text_name));
    image_box_size_grid->addWidget(btn_kf_image_box_size_, 0, 0, Qt::AlignCenter);
    image_box_size_grid->addWidget(image_box_size_label, 0, 1);
    image_box_size_grid->addWidget(make_image_box_size_field(obsgs_tr("OBSTitles.W"), spn_image_box_w_), 0, 2);
    image_box_size_grid->addWidget(chk_image_box_size_lock_, 0, 3, Qt::AlignCenter);
    image_box_size_grid->addWidget(make_image_box_size_field(obsgs_tr("OBSTitles.H"), spn_image_box_h_), 0, 4);
    image_box_size_layout->addLayout(image_box_size_grid);
    const int image_box_size_index = vl->indexOf(image_box_);
    if (image_box_size_index >= 0)
        vl->insertWidget(image_box_size_index + 1, image_box_size_box_);
    else
        vl->addWidget(image_box_size_box_);

    vl->addStretch();
    setWidget(inner);

    /* ── Connect signals → property_changed ── */
    auto emit_change = [this]() { if (!loading_values_) emit property_changed(!numeric_label_dragging_); };
    auto can_edit = [this]() { return layer_ && !loading_values_; };
    auto apply_text_char_format = [this](const RichTextCharFormat &format, uint32_t mask) {
        if (!layer_ || loading_values_) return;
        const bool active = active_text_edit_layer_id_ == layer_->id;
        apply_rich_text_format_to_layer_range(*layer_, format, mask, active);
        emit text_char_format_changed(layer_->id, format, mask);
    };
    auto current_text_char_format = [this]() {
        RichTextCharFormat fmt;
        if (!layer_) return fmt;
        const bool active = active_text_edit_layer_id_ == layer_->id;
        RichTextCharFormatSummary summary = summarize_rich_text_char_format(*layer_, active);
        return summary.valid ? summary.format : fmt;
    };
    auto apply_text_fill_format = [this, apply_text_char_format]() {
        if (!layer_ || loading_values_) return;
        const bool active = active_text_edit_layer_id_ == layer_->id;
        RichTextCharFormatSummary summary = summarize_rich_text_char_format(*layer_, active);
        RichTextCharFormat fmt = summary.valid ? summary.format : RichTextCharFormat();
        fmt.fill.type = layer_->fill_type;
        fmt.fill.color = layer_->text_color;
        fmt.fill.gradient_type = layer_->gradient_type;
        fmt.fill.gradient_spread = layer_->gradient_spread;
        fmt.fill.gradient_start_color = layer_->gradient_start_color;
        fmt.fill.gradient_end_color = layer_->gradient_end_color;
        fmt.fill.gradient_start_pos = layer_->gradient_start_pos;
        fmt.fill.gradient_end_pos = layer_->gradient_end_pos;
        fmt.fill.gradient_start_opacity = layer_->gradient_start_opacity;
        fmt.fill.gradient_end_opacity = layer_->gradient_end_opacity;
        fmt.fill.gradient_opacity = layer_->gradient_opacity;
        fmt.fill.gradient_angle = layer_->gradient_angle;
        fmt.fill.gradient_center_x = layer_->gradient_center_x;
        fmt.fill.gradient_center_y = layer_->gradient_center_y;
        fmt.fill.gradient_scale = layer_->gradient_scale;
        fmt.fill.gradient_focal_x = layer_->gradient_focal_x;
        fmt.fill.gradient_focal_y = layer_->gradient_focal_y;
        apply_text_char_format(fmt, RichTextCharFillColor);
    };
    auto local_time = [this]() {
        return layer_ ? std::clamp(playhead_ - layer_->in_time, 0.0,
                                   std::max(0.0, layer_->out_time - layer_->in_time)) : 0.0;
    };
    auto fit_image_size_to_current_image = [this, local_time]() {
        if (!layer_ || layer_->image_path.empty()) return;
        const QSize image_size = editor_image_intrinsic_size(QString::fromStdString(layer_->image_path));
        if (!image_size.isValid() || image_size.isEmpty()) return;
        const double t = local_time();
        layer_->image_width = (float)image_size.width();
        layer_->image_height = (float)image_size.height();
        set_animated_x(layer_->image_size, t, layer_->image_width);
        set_animated_y(layer_->image_size, t, layer_->image_height);
        layer_->image_size_auto_fit = true;
        layer_->lock_aspect_ratio = layer_->image_box_mode != ImageBoxMode::StretchToFill;
    };
    auto refresh_image_size_controls = [this, local_time]() {
        if (!layer_ || layer_->type != LayerType::Image) return;
        const double t = local_time();
        double raw_w = eval_image_width(*layer_, t);
        double raw_h = eval_image_height(*layer_, t);
        if (raw_w <= 0.0 || raw_h <= 0.0) {
            const QSize intrinsic = editor_image_intrinsic_size(QString::fromStdString(layer_->image_path));
            if (intrinsic.isValid() && !intrinsic.isEmpty()) {
                raw_w = intrinsic.width();
                raw_h = intrinsic.height();
            }
        }
        const gsp::ImageDisplaySize display = gsp::calculate_image_display_size(
            layer_->image_box_mode, layer_->image_size_auto_fit,
            eval_box_width(*layer_, t), eval_box_height(*layer_, t), raw_w, raw_h);
        if (spn_layer_w_) {
            QSignalBlocker block(spn_layer_w_);
            spn_layer_w_->setValue(display.width);
        }
        if (spn_layer_h_) {
            QSignalBlocker block(spn_layer_h_);
            spn_layer_h_->setValue(display.height);
        }
        const bool stretch = layer_->image_size_auto_fit &&
                             layer_->image_box_mode == ImageBoxMode::StretchToFill;
        if (spn_layer_w_) spn_layer_w_->setEnabled(!stretch);
        if (spn_layer_h_) spn_layer_h_->setEnabled(!stretch);
        if (chk_size_lock_) chk_size_lock_->setEnabled(!stretch);
        if (btn_kf_width_) btn_kf_width_->setEnabled(!stretch);
    };
    auto update_text_box_auto_controls = [this]() {
        if (spn_max_text_box_width_)
            spn_max_text_box_width_->setEnabled(true);
        if (spn_max_text_box_height_)
            spn_max_text_box_height_->setEnabled(true);
    };
    auto install_delete_all_keyframes_menu =
        [this, can_edit, emit_change, menu_style](QPushButton *button, auto props_for_layer) {
            if (!button) return;
            button->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(button, &QPushButton::customContextMenuRequested,
                    this, [this, button, props_for_layer, can_edit, emit_change, menu_style](const QPoint &pos) {
                        if (!layer_) return;
                        std::vector<AnimatedProperty *> props = props_for_layer();
                        bool has_keyframes = false;
                        for (auto *prop : props) {
                            if (prop && prop->is_animated()) {
                                has_keyframes = true;
                                break;
                            }
                        }

                        QMenu menu(button);
                        menu.setStyleSheet(menu_style);
                        QAction *delete_all = menu.addAction(obsgs_tr("OBSTitles.DeleteAllKeyframes"));
                        delete_all->setEnabled(can_edit() && has_keyframes);
                        if (menu.exec(button->mapToGlobal(pos)) != delete_all || !can_edit()) return;

                        bool changed = false;
                        for (auto *prop : props) {
                            if (!prop || prop->keyframes.empty()) continue;
                            prop->keyframes.clear();
                            changed = true;
                        }
                        if (!changed) return;
                        load_values();
                        emit_change();
                    });
        };
    auto install_prop_delete_all = [&](QPushButton *button, AnimatedProperty Layer::*prop) {
        install_delete_all_keyframes_menu(button, [this, prop]() {
            return layer_ ? std::vector<AnimatedProperty *>{&(layer_.get()->*prop)}
                          : std::vector<AnimatedProperty *>{};
        });
    };
    auto install_vec_delete_all = [&](QPushButton *button, AnimatedVec2Property Layer::*prop) {
        if (!button) return;
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QPushButton::customContextMenuRequested,
                this, [this, button, prop, can_edit, emit_change, menu_style](const QPoint &pos) {
                    if (!layer_) return;
                    auto &vec = layer_.get()->*prop;
                    QMenu menu(button);
                    menu.setStyleSheet(menu_style);
                    QAction *delete_all = menu.addAction(obsgs_tr("OBSTitles.DeleteAllKeyframes"));
                    delete_all->setEnabled(can_edit() && !vec.keyframes.empty());
                    if (menu.exec(button->mapToGlobal(pos)) != delete_all || !can_edit()) return;
                    if (vec.keyframes.empty()) return;
                    vec.keyframes.clear();
                    load_values();
                    emit_change();
                });
    };
    auto install_vec_delete_all_resolved = [&](QPushButton *button, auto prop_for_layer) {
        if (!button) return;
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QPushButton::customContextMenuRequested,
                this, [this, button, prop_for_layer, can_edit, emit_change, menu_style](const QPoint &pos) {
                    if (!layer_) return;
                    AnimatedVec2Property *vec = prop_for_layer();
                    QMenu menu(button);
                    menu.setStyleSheet(menu_style);
                    QAction *delete_all = menu.addAction(obsgs_tr("OBSTitles.DeleteAllKeyframes"));
                    delete_all->setEnabled(can_edit() && vec && !vec->keyframes.empty());
                    if (menu.exec(button->mapToGlobal(pos)) != delete_all || !can_edit() || !vec) return;
                    if (vec->keyframes.empty()) return;
                    vec->keyframes.clear();
                    load_values();
                    emit_change();
                });
    };
    auto install_group_delete_all = [&](QPushButton *button, std::initializer_list<AnimatedProperty Layer::*> props) {
        std::vector<AnimatedProperty Layer::*> prop_members(props);
        install_delete_all_keyframes_menu(button, [this, prop_members]() {
            std::vector<AnimatedProperty *> result;
            if (!layer_) return result;
            result.reserve(prop_members.size());
            for (auto prop : prop_members)
                result.push_back(&(layer_.get()->*prop));
            return result;
        });
    };

    install_vec_delete_all(btn_kf_pos_x_, &Layer::position);
    install_vec_delete_all(btn_kf_pos_y_, &Layer::position);
    install_vec_delete_all(btn_kf_scale_x_, &Layer::scale);
    install_vec_delete_all(btn_kf_scale_y_, &Layer::scale);
    install_vec_delete_all(btn_kf_transform_size_, &Layer::size);
    install_prop_delete_all(btn_kf_rotation_, &Layer::rotation);
    install_prop_delete_all(btn_kf_opacity_, &Layer::opacity);
    install_vec_delete_all(btn_kf_origin_x_, &Layer::origin_prop);
    install_vec_delete_all(btn_kf_origin_y_, &Layer::origin_prop);
    install_prop_delete_all(btn_kf_paragraph_indent_left_, &Layer::paragraph_indent_left_prop);
    install_prop_delete_all(btn_kf_paragraph_indent_right_, &Layer::paragraph_indent_right_prop);
    install_prop_delete_all(btn_kf_paragraph_indent_first_line_, &Layer::paragraph_indent_first_line_prop);
    install_prop_delete_all(btn_kf_font_size_, &Layer::font_size_prop);
    install_prop_delete_all(btn_kf_char_scale_x_, &Layer::char_scale_x_prop);
    install_prop_delete_all(btn_kf_char_scale_y_, &Layer::char_scale_y_prop);
    install_prop_delete_all(btn_kf_char_tracking_, &Layer::char_tracking_prop);
    install_prop_delete_all(btn_kf_baseline_shift_, &Layer::baseline_shift_prop);
    install_prop_delete_all(btn_kf_paragraph_space_before_, &Layer::paragraph_space_before_prop);
    install_prop_delete_all(btn_kf_paragraph_space_after_, &Layer::paragraph_space_after_prop);
    install_vec_delete_all_resolved(btn_kf_width_, [this]() -> AnimatedVec2Property * {
        if (!layer_) return nullptr;
        return layer_->type == LayerType::Image ? &layer_->image_size : &layer_->size;
    });
    install_vec_delete_all(btn_kf_image_box_size_, &Layer::size);
    install_group_delete_all(btn_kf_text_color_, {&Layer::text_color_a, &Layer::text_color_r,
                                                  &Layer::text_color_g, &Layer::text_color_b});
    install_group_delete_all(btn_kf_fill_color_, {&Layer::fill_color_a, &Layer::fill_color_r,
                                                  &Layer::fill_color_g, &Layer::fill_color_b});

    connect(spn_px_,       QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) {
                    const double t = local_time();
                    set_animated_value(layer_->position, t, {v, layer_->position.evaluate(t).y});
                    emit_change();
                }
            });
    connect(spn_py_,       QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) {
                    const double t = local_time();
                    set_animated_value(layer_->position, t, {layer_->position.evaluate(t).x, v});
                    emit_change();
                }
            });
    connect(spn_scale_x_,  QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (!can_edit()) return;
                const double scale = v / 100.0;
                const double t = local_time();
                Vec2Value next = layer_->scale.evaluate(t);
                next.x = scale;
                if (layer_->scale_lock) {
                    QSignalBlocker blocker(spn_scale_y_);
                    spn_scale_y_->setValue(v);
                    next.y = scale;
                }
                set_animated_value(layer_->scale, t, next);
                emit_change();
            });
    connect(spn_scale_y_,  QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (!can_edit()) return;
                const double scale = v / 100.0;
                const double t = local_time();
                Vec2Value next = layer_->scale.evaluate(t);
                next.y = scale;
                if (layer_->scale_lock) {
                    QSignalBlocker blocker(spn_scale_x_);
                    spn_scale_x_->setValue(v);
                    next.x = scale;
                }
                set_animated_value(layer_->scale, t, next);
                emit_change();
            });
    connect(chk_scale_lock_, &QCheckBox::toggled,
            this, [this, can_edit, local_time, emit_change](bool locked) {
                if (!can_edit()) return;
                layer_->scale_lock = locked;
                if (locked) {
                    const double t = local_time();
                    const double scale = spn_scale_x_->value() / 100.0;
                    QSignalBlocker blocker(spn_scale_y_);
                    spn_scale_y_->setValue(spn_scale_x_->value());
                    set_animated_value(layer_->scale, t, {scale, scale});
                }
                emit_change();
            });
    connect(spn_rot_,      QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) { set_animated_value(layer_->rotation, local_time(), v); emit_change(); }
            });
    connect(spn_opacity_,  QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) { set_animated_value(layer_->opacity, local_time(), v); emit_change(); }
            });
    connect(spn_origin_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) {
                    const double t = local_time();
                    layer_->origin_x = (float)v;
                    set_animated_value(layer_->origin_prop, t, {v, layer_->origin_prop.evaluate(t).y});
                    emit_change();
                }
            });
    connect(spn_origin_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v){
                if (can_edit()) {
                    const double t = local_time();
                    layer_->origin_y = (float)v;
                    set_animated_value(layer_->origin_prop, t, {layer_->origin_prop.evaluate(t).x, v});
                    emit_change();
                }
            });
    connect(cmb_anchor_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, local_time, emit_change](int idx) {
                if (!can_edit()) return;
                double t = local_time();
                QPointF next = anchor_point_from_index(idx);
                double w = eval_box_width(*layer_, t);
                double h = eval_box_height(*layer_, t);
                QPointF keep = rotated_scaled_delta((next.x() - layer_->origin_x) * w,
                                                    (next.y() - layer_->origin_y) * h,
                                                    layer_->rotation.evaluate(t),
                                                    layer_->scale.evaluate(t).x,
                                                    layer_->scale.evaluate(t).y);
                layer_->origin_x = (float)next.x();
                layer_->origin_y = (float)next.y();
                set_animated_value(layer_->origin_prop, t, {next.x(), next.y()});
                const Vec2Value pos = layer_->position.evaluate(t);
                set_animated_value(layer_->position, t, {pos.x + keep.x(), pos.y + keep.y()});
                load_values();
                emit_change();
            });
    connect(btn_transform_defaults_, &QPushButton::clicked,
            this, [this, can_edit, local_time, emit_change]() {
                if (!can_edit()) return;
                const double t = local_time();
                set_animated_value(layer_->position, t, {0.0, 0.0});
                set_animated_value(layer_->scale, t, {1.0, 1.0});
                set_animated_value(layer_->rotation, t, 0.0);
                set_animated_value(layer_->opacity, t, 1.0);
                layer_->origin_x = 0.5f;
                layer_->origin_y = 0.5f;
                set_animated_value(layer_->origin_prop, t, {0.5, 0.5});
                layer_->scale_lock = true;
                load_values();
                emit_change();
            });
    connect(txt_content_, &QTextEdit::textChanged,
            this, [this, can_edit, emit_change]() {
                if (!can_edit()) return;
                std::string value = txt_content_->toPlainText().toStdString();
                if (layer_->type == LayerType::Clock) {
                    layer_->clock_format = value.empty() ? "H:i:s" : value;
                } else {
                    rich_text_document_ensure_canonical(*layer_);
                    RichTextCharFormat insertion_format = insertion_format_for_text_replace(layer_->rich_text);
                    rich_text_document_replace_text(layer_->rich_text, value, &insertion_format);
                    layer_->rich_text_html.clear();
                    rich_text_document_sync_layer_mirrors(*layer_);
                }
                emit_change();
            });
    connect(chk_text_box_width_to_text_, &QCheckBox::toggled,
            this, [this, can_edit, update_text_box_auto_controls, emit_change](bool v) {
                update_text_box_auto_controls();
                if (can_edit()) { layer_->text_box_width_to_text = v; emit_change(); }
            });
    connect(chk_text_box_height_to_text_, &QCheckBox::toggled,
            this, [this, can_edit, update_text_box_auto_controls, emit_change](bool v) {
                update_text_box_auto_controls();
                if (can_edit()) { layer_->text_box_height_to_text = v; emit_change(); }
            });
    connect(spn_max_text_box_width_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->max_text_box_width = (float)v; emit_change(); }
            });
    connect(spn_max_text_box_height_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->max_text_box_height = (float)v; emit_change(); }
            });
    connect(cmb_font_, &QComboBox::currentTextChanged,
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](const QString &s){
                if (!can_edit()) return;
                RichTextCharFormat fmt = current_text_char_format();
                populate_font_style_combo(cmb_font_style_, s, QString::fromStdString(fmt.font_style));
                fmt.font_family = s.toStdString();
                fmt.font_style = cmb_font_style_->currentText().toStdString();
                QFontDatabase fdb;
                fmt.bold = fdb.bold(s, cmb_font_style_->currentText());
                fmt.italic = fdb.italic(s, cmb_font_style_->currentText());
                apply_text_char_format(fmt, RichTextCharFontFamily | RichTextCharFontStyle |
                                       RichTextCharBold | RichTextCharItalic);
                emit_change();
            });
    connect(cmb_font_style_, &QComboBox::currentTextChanged,
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](const QString &s){
                if (!can_edit()) return;
                RichTextCharFormat fmt = current_text_char_format();
                fmt.font_style = s.toStdString();
                QFontDatabase fdb;
                const QString family = QString::fromStdString(fmt.font_family);
                fmt.bold = fdb.bold(family, s);
                fmt.italic = fdb.italic(family, s);
                apply_text_char_format(fmt, RichTextCharFontStyle | RichTextCharBold | RichTextCharItalic);
                emit_change();
            });
    connect(spn_size_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_text_char_format, current_text_char_format](int v){
                if (!can_edit()) return;
                layer_->font_size = v;
                set_animated_value(layer_->font_size_prop, local_time(), v);
                RichTextCharFormat fmt = current_text_char_format();
                fmt.font_size = v;
                apply_text_char_format(fmt, RichTextCharFontSize);
                emit_change();
            });
    connect(chk_bold_, &QToolButton::toggled,
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){
                if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.bold = v; apply_text_char_format(fmt, RichTextCharBold); emit_change(); }
            });
    connect(chk_italic_, &QToolButton::toggled,
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){
                if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.italic = v; apply_text_char_format(fmt, RichTextCharItalic); emit_change(); }
            });
    connect(chk_font_kerning_, &QToolButton::toggled,
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){
                if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.kerning = v; apply_text_char_format(fmt, RichTextCharKerning); emit_change(); }
            });
    connect(cmb_kerning_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](int idx) {
                if (!can_edit()) return;
                RichTextCharFormat fmt = current_text_char_format();
                fmt.kerning_mode = cmb_kerning_mode_->itemData(idx).toInt();
                fmt.kerning = fmt.kerning_mode != 2;
                if (chk_font_kerning_) chk_font_kerning_->setChecked(fmt.kerning);
                if (spn_kerning_value_) spn_kerning_value_->setEnabled(fmt.kerning_mode == 2);
                apply_text_char_format(fmt, RichTextCharKerning);
                emit_change();
            });
    connect(spn_kerning_value_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](double v) {
                if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.manual_kerning = (float)v; apply_text_char_format(fmt, RichTextCharKerning); emit_change(); }
            });
    connect(spn_text_leading_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){
                if (!can_edit()) return;
                RichTextParagraphFormat fmt = layer_paragraph_format_for_editor(*layer_);
                fmt.line_spacing = (float)v;
                apply_rich_text_paragraph_format_to_layer(*layer_, fmt);
                emit_change();
            });
    connect(spn_char_tracking_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_text_char_format, current_text_char_format](double v){
                if (!can_edit()) return;
                layer_->char_tracking = (float)v;
                set_animated_value(layer_->char_tracking_prop, local_time(), v);
                RichTextCharFormat fmt = current_text_char_format();
                fmt.tracking = (float)v;
                apply_text_char_format(fmt, RichTextCharTracking);
                emit_change();
            });
    connect(spn_char_scale_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_text_char_format, current_text_char_format](double v){
                if (can_edit()) { layer_->char_scale_x = (float)(v / 100.0); set_animated_value(layer_->char_scale_x_prop, local_time(), v / 100.0); RichTextCharFormat fmt = current_text_char_format(); fmt.scale_x = (float)(v / 100.0); apply_text_char_format(fmt, RichTextCharScaleX); emit_change(); }
            });
    connect(spn_char_scale_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_text_char_format, current_text_char_format](double v){
                if (can_edit()) { layer_->char_scale_y = (float)(v / 100.0); set_animated_value(layer_->char_scale_y_prop, local_time(), v / 100.0); RichTextCharFormat fmt = current_text_char_format(); fmt.scale_y = (float)(v / 100.0); apply_text_char_format(fmt, RichTextCharScaleY); emit_change(); }
            });
    connect(spn_baseline_shift_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_text_char_format, current_text_char_format](double v){
                if (!can_edit()) return;
                layer_->baseline_shift = (float)v;
                set_animated_value(layer_->baseline_shift_prop, local_time(), v);
                RichTextCharFormat fmt = current_text_char_format();
                fmt.baseline_shift = (float)v;
                apply_text_char_format(fmt, RichTextCharBaselineShift);
                emit_change();
            });
    auto set_exclusive_text_style = [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](int style, bool checked) {
        if (!can_edit()) return;
        RichTextCharFormat fmt = current_text_char_format();
        fmt.text_style = checked ? style : 0;
        apply_text_char_format(fmt, RichTextCharTextStyle);
        emit_change();
        load_values();
    };
    connect(btn_all_caps_, &QToolButton::toggled, this, [set_exclusive_text_style](bool v){ set_exclusive_text_style(1, v); });
    connect(btn_small_caps_, &QToolButton::toggled, this, [set_exclusive_text_style](bool v){ set_exclusive_text_style(2, v); });
    connect(btn_superscript_, &QToolButton::toggled, this, [set_exclusive_text_style](bool v){ set_exclusive_text_style(3, v); });
    connect(btn_subscript_, &QToolButton::toggled, this, [set_exclusive_text_style](bool v){ set_exclusive_text_style(4, v); });
    connect(btn_underline_, &QToolButton::toggled, this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){ if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.underline = v; apply_text_char_format(fmt, RichTextCharUnderline); emit_change(); }});
    connect(btn_strikethrough_, &QToolButton::toggled, this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){ if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.strikethrough = v; apply_text_char_format(fmt, RichTextCharStrikethrough); emit_change(); }});
    connect(btn_ligatures_, &QToolButton::toggled, this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){ if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.ligatures = v; apply_text_char_format(fmt, RichTextCharLigatures); emit_change(); }});
    connect(btn_stylistic_alternates_, &QToolButton::toggled, this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){ if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.stylistic_alternates = v; apply_text_char_format(fmt, RichTextCharStylisticAlternates); emit_change(); }});
    connect(btn_fractions_, &QToolButton::toggled, this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){ if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.fractions = v; apply_text_char_format(fmt, RichTextCharFractions); emit_change(); }});
    connect(btn_opentype_features_, &QToolButton::toggled, this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](bool v){ if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.opentype_features = v; apply_text_char_format(fmt, RichTextCharOpenTypeFeatures); emit_change(); }});
    connect(cmb_text_style_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change, apply_text_char_format, current_text_char_format](int idx) {
                if (can_edit()) { RichTextCharFormat fmt = current_text_char_format(); fmt.text_style = cmb_text_style_->itemData(idx).toInt(); apply_text_char_format(fmt, RichTextCharTextStyle); emit_change(); }
            });
    connect(cmb_text_overflow_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->text_overflow_mode = cmb_text_overflow_->itemData(idx).toInt(); emit_change(); }
            });
    connect(spn_text_fit_min_scale_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->text_fit_min_scale = (float)v; emit_change(); }
            });
    connect(cmb_ticker_style_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->ticker_style = cmb_ticker_style_->itemData(idx).toInt(); emit_change(); load_values(); }
            });
    connect(spn_ticker_speed_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->ticker_speed = v; emit_change(); }
            });
    connect(spn_ticker_line_hold_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (can_edit()) { layer_->ticker_line_hold = v; emit_change(); }
            });
    connect(cmb_ticker_direction_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx) {
                if (can_edit()) { layer_->ticker_direction = cmb_ticker_direction_->itemData(idx).toInt(); emit_change(); }
            });
    connect(chk_expose_text_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) {
                    layer_->expose_text = v;
                    if (v) layer_->ignore_persistence = false;
                    load_values();
                    emit_change();
                }
            });
    connect(chk_exposed_hide_if_empty_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) {
                    layer_->exposed_hide_if_empty = v;
                    if (!v) layer_->live_cue_hidden_if_empty = false;
                    load_values();
                    emit_change();
                }
            });
    connect(chk_exposed_single_value_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) {
                    layer_->exposed_single_value = v;
                    load_values();
                    emit_change();
                }
            });
    connect(chk_ignore_persistence_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) {
                    layer_->ignore_persistence = !layer_->expose_text && v;
                    load_values();
                    emit_change();
                }
            });
    connect(chk_scene_mask_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v){
                if (can_edit()) { layer_->use_as_scene_mask = v; load_values(); emit_change(); }
            });
    auto connect_alignment_group = [this, can_edit, emit_change](QButtonGroup *group, bool horizontal) {
        if (!group) return;
        for (auto *button : group->buttons()) {
            connect(button, &QAbstractButton::clicked, this, [this, can_edit, emit_change, group, horizontal, button]() {
                if (!can_edit()) return;
                const int value = group->id(button);
                RichTextParagraphFormat fmt = layer_paragraph_format_for_editor(*layer_);
                if (horizontal)
                    fmt.align_h = value;
                else
                    fmt.align_v = value;
                apply_rich_text_paragraph_format_to_layer(*layer_, fmt);
                emit_change();
            });
        }
    };
    connect_alignment_group(grp_text_align_, true);
    connect_alignment_group(grp_text_valign_, false);
    auto connect_paragraph_spin = [this, can_edit, emit_change](QDoubleSpinBox *spin, float Layer::*field) {
        if (!spin) return;
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, can_edit, emit_change, field](double value) {
                    if (!can_edit()) return;
                    RichTextParagraphFormat fmt = layer_paragraph_format_for_editor(*layer_);
                    if (field == &Layer::paragraph_space_before)
                        fmt.space_before = (float)value;
                    else if (field == &Layer::paragraph_space_after)
                        fmt.space_after = (float)value;
                    apply_rich_text_paragraph_format_to_layer(*layer_, fmt);
                    emit_change();
                });
    };
    auto connect_keyframed_paragraph_spin = [this, can_edit, local_time, emit_change](QDoubleSpinBox *spin, float Layer::*field, AnimatedProperty Layer::*prop) {
        if (!spin) return;
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, can_edit, local_time, emit_change, field, prop](double value) {
                    if (can_edit()) {
                        RichTextParagraphFormat fmt = layer_paragraph_format_for_editor(*layer_);
                        if (field == &Layer::paragraph_indent_left)
                            fmt.indent_left = (float)value;
                        else if (field == &Layer::paragraph_indent_right)
                            fmt.indent_right = (float)value;
                        else if (field == &Layer::paragraph_indent_first_line)
                            fmt.indent_first_line = (float)value;
                        else if (field == &Layer::paragraph_space_before)
                            fmt.space_before = (float)value;
                        else if (field == &Layer::paragraph_space_after)
                            fmt.space_after = (float)value;
                        apply_rich_text_paragraph_format_to_layer(*layer_, fmt);
                        set_animated_value(layer_.get()->*prop, local_time(), value);
                        emit_change();
                    }
                });
    };
    connect_keyframed_paragraph_spin(spn_paragraph_indent_left_, &Layer::paragraph_indent_left, &Layer::paragraph_indent_left_prop);
    connect_keyframed_paragraph_spin(spn_paragraph_indent_right_, &Layer::paragraph_indent_right, &Layer::paragraph_indent_right_prop);
    connect_keyframed_paragraph_spin(spn_paragraph_indent_first_line_, &Layer::paragraph_indent_first_line, &Layer::paragraph_indent_first_line_prop);
    connect_keyframed_paragraph_spin(spn_paragraph_space_before_, &Layer::paragraph_space_before, &Layer::paragraph_space_before_prop);
    connect_keyframed_paragraph_spin(spn_paragraph_space_after_, &Layer::paragraph_space_after, &Layer::paragraph_space_after_prop);
    connect(chk_paragraph_hyphenate_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool v) {
                if (!can_edit()) return;
                RichTextParagraphFormat fmt = layer_paragraph_format_for_editor(*layer_);
                fmt.hyphenate = v;
                apply_rich_text_paragraph_format_to_layer(*layer_, fmt);
                emit_change();
            });
    connect(btn_text_color_, &QPushButton::clicked,
            this, [this, can_edit, local_time, emit_change, apply_text_char_format, current_text_char_format]() {
                if (!can_edit()) return;
                QColor initial = color_from_argb(eval_text_color(*layer_, local_time()));
                QColor picked = obsgs_pick_color(initial, this, obsgs_tr("OBSTitles.TextColor"));
                if (!picked.isValid()) return;
                RichTextCharFormat fmt = current_text_char_format();
                fmt.fill.type = 0;
                fmt.fill.color = argb_from_color(picked);
                apply_text_char_format(fmt, RichTextCharFillColor);
                set_color_channels_at(*layer_, true, local_time(), fmt.fill.color);
                style_color_button(btn_text_color_, fmt.fill.color);
                emit_change();
            });
    auto open_color_selector = [this, can_edit, emit_change, apply_text_fill_format, local_time, control_style,
                                panel_bg_name, panel_text_name, control_bg_name, control_text_name,
                                button_bg_name, button_text_name, border_name, highlight_name,
                                highlighted_text_name, subtle_text_name, hover_bg_name, section_bg_name](bool stroke) {
        if (!can_edit()) return;
        const bool text_fill = !stroke && (layer_->type == LayerType::Text || layer_->type == LayerType::Clock ||
                                           layer_->type == LayerType::Ticker);

        QDialog popup(this, Qt::Popup | Qt::FramelessWindowHint);
        popup.setModal(true);
        popup.setMinimumWidth(560);
        popup.setMinimumHeight(560);
        QString popup_css = QStringLiteral(
            "QDialog{background:@panel@;border:1px solid @border@;}"
            "QTabWidget::pane{border:1px solid @border@;background:@panel@;}"
            "QTabBar::tab{color:@text@;background:@button@;border:1px solid @border@;padding:5px 10px;}"
            "QTabBar::tab:selected{background:@highlight@;color:@highlightedText@;}"
            "QTabBar::tab:disabled{color:@subtle@;background:@button@;}"
            "QLabel{color:@text@;font-size:10px;background:transparent;}"
            "QPushButton{color:@buttonText@;background:@button@;border:1px solid @border@;border-radius:2px;padding:3px 8px;font-size:10px;}"
            "QPushButton:hover{background:@hover@;}"
            "QSlider::groove:horizontal{height:12px;border:1px solid @border@;border-radius:2px;background:@section@;}"
            "QSlider::handle:horizontal{width:10px;margin:-3px 0;border:1px solid @border@;border-radius:2px;background:@text@;}"
            "QLineEdit,QSpinBox{color:@controlText@;background:@controlBg@;border:1px solid @border@;border-radius:2px;padding:2px 4px;selection-background-color:@highlight@;}"
            "QLineEdit:focus,QSpinBox:focus{border-color:@highlight@;}");
        popup_css.replace(QStringLiteral("@panel@"), panel_bg_name);
        popup_css.replace(QStringLiteral("@border@"), border_name);
        popup_css.replace(QStringLiteral("@text@"), panel_text_name);
        popup_css.replace(QStringLiteral("@button@"), button_bg_name);
        popup_css.replace(QStringLiteral("@highlight@"), highlight_name);
        popup_css.replace(QStringLiteral("@highlightedText@"), highlighted_text_name);
        popup_css.replace(QStringLiteral("@subtle@"), subtle_text_name);
        popup_css.replace(QStringLiteral("@buttonText@"), button_text_name);
        popup_css.replace(QStringLiteral("@hover@"), hover_bg_name);
        popup_css.replace(QStringLiteral("@section@"), section_bg_name);
        popup_css.replace(QStringLiteral("@controlText@"), control_text_name);
        popup_css.replace(QStringLiteral("@controlBg@"), control_bg_name);
        popup.setStyleSheet(popup_css);
        auto *root = new QVBoxLayout(&popup);
        root->setContentsMargins(8, 8, 8, 8);
        auto *tabs = new QTabWidget(&popup);
        // Keep the selector as the existing tabbed popup; do not open a separate Gradient Editor window.
        root->addWidget(tabs);
        bool live_visual_dirty = false;
        auto emit_live_visual_change = [&]() {
            if (loading_values_)
                return;
            live_visual_dirty = true;
            emit live_visual_changed();
        };

        auto update_main_swatch = [this, stroke, text_fill]() {
            if (stroke) {
                if (layer_->stroke_fill_type == 2)
                    style_gradient_button(btn_appearance_stroke_color_,
                                          layer_->stroke_gradient_start_color,
                                          layer_->stroke_gradient_end_color,
                                          layer_->stroke_gradient_type);
                else
                    style_color_button(btn_appearance_stroke_color_, layer_->stroke_color);
                btn_appearance_stroke_color_->setText(QString());
                if (btn_outline_color_) style_color_button(btn_outline_color_, layer_->stroke_color);
            } else {
                const double t = std::clamp(playhead_ - layer_->in_time, 0.0,
                                            std::max(0.0, layer_->out_time - layer_->in_time));
                if (layer_->fill_type == 1)
                    style_gradient_button(btn_appearance_fill_color_,
                                          layer_->gradient_start_color,
                                          layer_->gradient_end_color,
                                          layer_->gradient_type);
                else
                    style_color_button(btn_appearance_fill_color_,
                                       text_fill ? eval_text_color(*layer_, t) : eval_fill_color(*layer_, t));
                btn_appearance_fill_color_->setText(QString());
                if (text_fill && btn_text_color_) style_color_button(btn_text_color_, eval_text_color(*layer_, t));
                if (!text_fill && btn_fill_color_) style_color_button(btn_fill_color_, eval_fill_color(*layer_, t));
            }
        };
        auto apply_solid_color = [this, stroke, text_fill, local_time, &emit_live_visual_change, apply_text_fill_format,
                                  update_main_swatch](const QColor &color) {
            if (!layer_ || loading_values_ || !color.isValid()) return;
            const uint32_t argb = argb_from_color(color);
            if (stroke) {
                layer_->outline_enabled = true;
                layer_->stroke_fill_type = 1;
                layer_->stroke_color = argb;
            } else {
                layer_->fill_type = 0;
                if (text_fill) {
                    layer_->text_color = argb;
                    set_color_channels_at(*layer_, true, local_time(), argb);
                    apply_text_fill_format();
                } else {
                    layer_->fill_color = argb;
                    set_color_channels_at(*layer_, false, local_time(), argb);
                }
            }
            update_main_swatch();
            emit_live_visual_change();
        };

        auto *color_tab = new QWidget(tabs);
        color_tab->setMinimumSize(430, 430);
        auto *color_layout = new QHBoxLayout(color_tab);
        color_layout->setContentsMargins(8, 8, 8, 8);
        color_layout->setSpacing(8);

        QColor initial = stroke
            ? color_from_argb(layer_->stroke_color)
            : color_from_argb(text_fill ? eval_text_color(*layer_, local_time()) : eval_fill_color(*layer_, local_time()));
        QColor selected_color = initial.isValid() ? initial : QColor(Qt::white);
        QColor background_color = QColor(Qt::white);
        const QColor original_color = selected_color;
        bool syncing_color_controls = false;
        bool closing_for_eyedropper = false;
        QStringList recent_color_hexes = obsgs_load_recent_color_hexes();
        if (title_) {
            for (const auto &hex : title_->editor_recent_color_hexes) {
                const QString value = QString::fromStdString(hex);
                if (!recent_color_hexes.contains(value, Qt::CaseInsensitive))
                    recent_color_hexes << value;
            }
        } else {
            static QStringList fallback_recent_color_hexes;
            for (const QString &hex : fallback_recent_color_hexes) {
                if (!recent_color_hexes.contains(hex, Qt::CaseInsensitive))
                    recent_color_hexes << hex;
            }
        }

        auto color_hex = [](const QColor &color) {
            return color.alpha() < 255
                ? QStringLiteral("#%1%2%3%4")
                    .arg(color.red(), 2, 16, QLatin1Char('0'))
                    .arg(color.green(), 2, 16, QLatin1Char('0'))
                    .arg(color.blue(), 2, 16, QLatin1Char('0'))
                    .arg(color.alpha(), 2, 16, QLatin1Char('0'))
                    .toUpper()
                : QStringLiteral("#%1%2%3")
                    .arg(color.red(), 2, 16, QLatin1Char('0'))
                    .arg(color.green(), 2, 16, QLatin1Char('0'))
                    .arg(color.blue(), 2, 16, QLatin1Char('0'))
                    .toUpper();
        };
        auto parse_hex = [](QString text, QColor &color) {
            text = text.trimmed();
            if (text.startsWith(QStringLiteral("#"))) text.remove(0, 1);
            if (text.size() != 6 && text.size() != 8) return false;
            bool ok = false;
            const uint value = text.toUInt(&ok, 16);
            if (!ok) return false;
            color = text.size() == 6
                ? QColor((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF, 255)
                : QColor((value >> 24) & 0xFF, (value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
            return color.isValid();
        };
        auto swatch_style = [&](const QColor &color, int size = 32) {
            return QStringLiteral(
                "QPushButton{background:%1;border:1px solid %2;border-radius:3px;"
                "min-width:%3px;min-height:%3px;max-width:%3px;max-height:%3px;padding:0;}")
                .arg(color.name(QColor::HexArgb), border_name)
                .arg(size);
        };
        auto none_swatch_style = [&](int size = 36) {
            return QStringLiteral(
                "QPushButton{background:%1;border:1px solid %2;border-radius:3px;"
                "min-width:%3px;min-height:%3px;max-width:%3px;max-height:%3px;"
                "padding:0;color:#ff5c5c;font-size:24px;font-weight:bold;}")
                .arg(control_bg_name, border_name)
                .arg(size);
        };

        auto *wheel_column = new QWidget(color_tab);
        auto *wheel_layout = new QVBoxLayout(wheel_column);
        wheel_layout->setContentsMargins(0, 0, 0, 0);
        wheel_layout->setSpacing(6);

        auto *picker_row = new QWidget(wheel_column);
        auto *picker_row_layout = new QHBoxLayout(picker_row);
        picker_row_layout->setContentsMargins(0, 0, 0, 0);
        picker_row_layout->setSpacing(8);

        auto *picker_actions = new QWidget(picker_row);
        auto *picker_actions_layout = new QVBoxLayout(picker_actions);
        picker_actions_layout->setContentsMargins(0, 0, 0, 0);
        picker_actions_layout->setSpacing(6);
        auto *add_to_library_button = new QPushButton(obsgs_tr("OBSTitles.AddToLibrary"), picker_actions);
        add_to_library_button->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        picker_actions_layout->addWidget(add_to_library_button);
        picker_actions_layout->addStretch(1);

        auto *color_picker = new color_widgets::ColorDialog(picker_row, Qt::Widget);
        obsgs_prepare_embedded_color_dialog(color_picker, selected_color);
        picker_row_layout->addWidget(picker_actions);
        picker_row_layout->addWidget(color_picker, 1);
        wheel_layout->addWidget(picker_row, 1);
        connect(color_picker, &QDialog::rejected, &popup, &QDialog::reject);

        auto *swatch_row = new QWidget(wheel_column);
        auto *swatch_row_layout = new QHBoxLayout(swatch_row);
        swatch_row_layout->setContentsMargins(0, 0, 0, 0);
        swatch_row_layout->setSpacing(6);
        auto *none_button = new QPushButton(QStringLiteral("╱"), swatch_row);
        none_button->setToolTip(obsgs_tr("OBSTitles.NoColor"));
        none_button->setStyleSheet(none_swatch_style(30));
        auto *background_button = new QPushButton(swatch_row);
        background_button->setToolTip(obsgs_tr("OBSTitles.BackgroundColor"));
        auto *foreground_button = new QPushButton(swatch_row);
        foreground_button->setToolTip(obsgs_tr("OBSTitles.ForegroundColor"));
        auto *swap_button = new QPushButton(QStringLiteral("⇄"), swatch_row);
        swap_button->setToolTip(obsgs_tr("OBSTitles.SwapForegroundBackground"));
        swap_button->setFixedSize(30, 30);
        auto *eyedropper_button = new QPushButton(QStringLiteral("⌕"), swatch_row);
        eyedropper_button->setIcon(obs_icon("eyedropper.svg"));
        eyedropper_button->setText(QString());
        eyedropper_button->setToolTip(obsgs_tr("OBSTitles.PickColor"));
        eyedropper_button->setFixedSize(30, 30);
        swatch_row_layout->addWidget(none_button);
        swatch_row_layout->addWidget(background_button);
        swatch_row_layout->addWidget(foreground_button);
        swatch_row_layout->addWidget(swap_button);
        swatch_row_layout->addStretch(1);
        swatch_row_layout->addWidget(eyedropper_button);
        wheel_layout->addWidget(swatch_row);

        auto *recent_label = new QLabel(obsgs_tr("OBSTitles.RecentColors"), wheel_column);
        wheel_layout->addWidget(recent_label);
        auto *recent_row = new QWidget(wheel_column);
        auto *recent_layout = new QHBoxLayout(recent_row);
        recent_layout->setContentsMargins(0, 0, 0, 0);
        recent_layout->setSpacing(8);
        std::vector<QPushButton *> recent_buttons;
        for (int i = 0; i < 16; ++i) {
            auto *b = new QPushButton(recent_row);
            b->setFixedSize(20, 20);
            recent_layout->addWidget(b);
            recent_buttons.push_back(b);
        }
        recent_layout->addStretch(1);
        wheel_layout->addWidget(recent_row);

        color_layout->addWidget(wheel_column, 1);

        auto update_recent_buttons = [&]() {
            for (int i = 0; i < (int)recent_buttons.size(); ++i) {
                auto *b = recent_buttons[i];
                b->setText(QString());
                if (i < recent_color_hexes.size()) {
                    QColor c;
                    parse_hex(recent_color_hexes[i], c);
                    b->setEnabled(true);
                    b->setStyleSheet(swatch_style(c, 22));
                    b->setToolTip(obsgs_color_swatch_tooltip(obsgs_tr("OBSTitles.RecentColors"), c, color_hex(c)));
                } else {
                    b->setEnabled(false);
                    b->setStyleSheet(QStringLiteral(
                        "QPushButton{background:%1;border:1px solid %2;border-radius:3px;"
                        "min-width:22px;min-height:22px;max-width:22px;max-height:22px;padding:0;}")
                        .arg(section_bg_name, border_name));
                    b->setToolTip(QString());
                }
            }
        };

        auto persist_recent_colors = [&]() {
            if (title_) {
                title_->editor_recent_color_hexes.clear();
                for (const QString &hex : recent_color_hexes)
                    title_->editor_recent_color_hexes.push_back(hex.toStdString());
            } else {
                static QStringList fallback_recent_color_hexes;
                fallback_recent_color_hexes = recent_color_hexes;
            }
            obsgs_save_recent_color_hexes(recent_color_hexes);
            update_recent_buttons();
            emit recent_colors_changed();
        };

        auto remove_recent_color_at = [&](int index) {
            if (index < 0 || index >= recent_color_hexes.size())
                return;
            recent_color_hexes.removeAt(index);
            persist_recent_colors();
        };

        QString last_committed_recent_hex;
        auto commit_recent_color = [&](const QColor &color, bool force) {
            if (!color.isValid() || color.alpha() <= 0)
                return;

            const QString hex = color_hex(color).toUpper();
            if (!force && hex.compare(last_committed_recent_hex, Qt::CaseInsensitive) == 0)
                return;

            for (int i = recent_color_hexes.size() - 1; i >= 0; --i) {
                if (recent_color_hexes[i].compare(hex, Qt::CaseInsensitive) == 0)
                    recent_color_hexes.removeAt(i);
            }
            recent_color_hexes.prepend(hex);
            while (recent_color_hexes.size() > 16)
                recent_color_hexes.removeLast();

            last_committed_recent_hex = hex;
            persist_recent_colors();
        };

        QTimer recent_commit_timer(&popup);
        recent_commit_timer.setSingleShot(true);
        recent_commit_timer.setInterval(450);
        connect(&recent_commit_timer, &QTimer::timeout, &popup,
                [&]() { commit_recent_color(selected_color, false); });

        auto update_color_controls = [&](bool sync_picker = true, bool refresh_recent = true) {
            syncing_color_controls = true;
            foreground_button->setStyleSheet(swatch_style(selected_color, 30));
            background_button->setStyleSheet(swatch_style(background_color, 30));
            foreground_button->setToolTip(obsgs_color_swatch_tooltip(obsgs_tr("OBSTitles.ForegroundColor"), selected_color, color_hex(selected_color)));
            background_button->setToolTip(obsgs_color_swatch_tooltip(obsgs_tr("OBSTitles.BackgroundColor"), background_color, color_hex(background_color)));
            if (sync_picker) {
                QSignalBlocker blocker(color_picker);
                color_picker->setColor(selected_color);
            }
            if (refresh_recent)
                update_recent_buttons();
            syncing_color_controls = false;
        };

        auto apply_and_sync_color = [&](const QColor &color, bool update_picker) {
            if (!color.isValid()) return;
            selected_color = color;
            update_color_controls(update_picker, false);
            apply_solid_color(color);
            if (color.alpha() > 0)
                recent_commit_timer.start();
        };

        connect(color_picker, &color_widgets::ColorDialog::colorChanged, &popup,
                [=, &apply_and_sync_color, &syncing_color_controls](const QColor &color) {
                    if (syncing_color_controls) return;
                    apply_and_sync_color(color, false);
                });
        connect(add_to_library_button, &QPushButton::clicked, &popup, [this, &selected_color]() {
            if (selected_color.isValid() && selected_color.alpha() > 0)
                emit color_library_add_requested(selected_color);
        });
        connect(none_button, &QPushButton::clicked, &popup, [=, &selected_color, &update_color_controls, &update_main_swatch, &emit_live_visual_change, &apply_text_fill_format]() {
            if (!layer_) return;
            QColor none = selected_color;
            none.setAlpha(0);
            selected_color = none;
            if (stroke) {
                layer_->outline_enabled = false;
                layer_->stroke_fill_type = 0;
            } else {
                layer_->fill_type = 0;
                const uint32_t argb = argb_from_color(none);
                if (text_fill) {
                    layer_->text_color = argb;
                    set_color_channels_at(*layer_, true, local_time(), argb);
                    apply_text_fill_format();
                } else {
                    layer_->fill_color = argb;
                    set_color_channels_at(*layer_, false, local_time(), argb);
                }
            }
            update_color_controls();
            update_main_swatch();
            emit_live_visual_change();
        });
        connect(swap_button, &QPushButton::clicked, &popup, [=, &selected_color, &background_color, &apply_and_sync_color]() {
            const QColor new_foreground = background_color;
            background_color = selected_color;
            apply_and_sync_color(new_foreground, true);
        });
        connect(eyedropper_button, &QPushButton::clicked, &popup, [this, &popup, &closing_for_eyedropper]() {
            // Reuse the editor/sidebar eyedropper instead of spawning a second color picker.
            // Store the current popup position so the Color tab can reappear in exactly
            // the same place after the canvas pick completes.
            closing_for_eyedropper = true;
            remember_next_color_popup_position(popup.pos());
            popup.accept();
            emit color_picker_tool_requested();
        });
        for (int i = 0; i < (int)recent_buttons.size(); ++i) {
            auto *button = recent_buttons[i];
            connect(button, &QPushButton::clicked, &popup, [=, &apply_and_sync_color, &parse_hex]() {
                QColor c;
                if (i < recent_color_hexes.size() && parse_hex(recent_color_hexes[i], c)) {
                    apply_and_sync_color(c, true);
                    commit_recent_color(c, true);
                }
            });
            button->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(button, &QPushButton::customContextMenuRequested, &popup, [=, &parse_hex](const QPoint &pos) {
                QColor c;
                if (i >= recent_color_hexes.size() || !parse_hex(recent_color_hexes[i], c))
                    return;
                QMenu menu(button);
                QAction *add_action = menu.addAction(obsgs_tr("OBSTitles.AddToLibrary"));
                QAction *delete_action = menu.addAction(obsgs_tr("OBSTitles.Delete"));
                QAction *selected = menu.exec(button->mapToGlobal(pos));
                if (selected == add_action)
                    emit color_library_add_requested(c);
                else if (selected == delete_action)
                    remove_recent_color_at(i);
            });
        }
        connect(&popup, &QDialog::finished, &popup, [=, &recent_commit_timer, &commit_recent_color, &selected_color, &closing_for_eyedropper]() mutable {
            recent_commit_timer.stop();
            if (!closing_for_eyedropper)
                commit_recent_color(selected_color, true);
        });

        update_color_controls();
        tabs->addTab(color_tab, obsgs_tr("OBSTitles.Color"));

        auto *swatches_tab = new QWidget(tabs);
        auto *swatches_layout = new QVBoxLayout(swatches_tab);
        swatches_layout->setContentsMargins(8, 8, 8, 8);
        swatches_layout->setSpacing(6);
        auto *swatches_combo = new QComboBox(swatches_tab);
        auto *swatches_scroll = new QScrollArea(swatches_tab);
        swatches_scroll->setWidgetResizable(true);
        swatches_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *swatches_grid_widget = new ResponsiveSwatchGrid(swatches_scroll);
        swatches_scroll->setWidget(swatches_grid_widget);
        swatches_layout->addWidget(swatches_combo);
        swatches_layout->addWidget(swatches_scroll, 1);
        auto swatch_libraries = std::make_shared<std::vector<PropertiesColorLibrary>>(properties_load_color_libraries());
        for (const auto &library : *swatch_libraries)
            swatches_combo->addItem(library.name);
        auto refresh_swatches_tab = [swatches_grid_widget, swatches_combo, swatch_libraries,
                                     color_hex, swatch_style, &apply_and_sync_color, &popup]() {
            swatches_grid_widget->clearSwatches();
            const int library_index = swatches_combo->currentIndex();
            if (library_index < 0 || library_index >= (int)swatch_libraries->size())
                return;
            const auto &colors = (*swatch_libraries)[library_index].colors;
            for (int i = 0; i < (int)colors.size(); ++i) {
                const auto entry = colors[i];
                auto *button = new QPushButton(swatches_grid_widget);
                button->setFixedSize(24, 24);
                button->setText(QString());
                button->setToolTip(obsgs_color_swatch_tooltip(entry.name, entry.color, color_hex(entry.color)));
                button->setStyleSheet(swatch_style(entry.color, 24));
                connect(button, &QPushButton::clicked, &popup, [=, &apply_and_sync_color]() {
                    apply_and_sync_color(entry.color, true);
                });
                swatches_grid_widget->addSwatch(button);
            }
        };
        connect(swatches_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), &popup,
                [&refresh_swatches_tab](int) { refresh_swatches_tab(); });
        refresh_swatches_tab();
        tabs->addTab(swatches_tab, obsgs_tr("OBSTitles.Swatches"));

        auto *gradient_tab = new QWidget(tabs);
        auto *gradient_layout = new QVBoxLayout(gradient_tab);
        gradient_tab->setMinimumSize(430, 430);
        gradient_layout->setContentsMargins(8, 8, 8, 8);
        gradient_layout->setSpacing(6);

        auto *preset_box = new QGroupBox(obsgs_tr("OBSTitles.Presets"), gradient_tab);
        preset_box->setStyleSheet(QStringLiteral("QGroupBox{color:%1;background:%2;border:1px solid %3;"
                                "border-radius:2px;margin-top:15px;padding-top:8px;font-size:10px;}"
                                "QGroupBox::title{subcontrol-origin:margin;left:6px;padding:0 3px;background:%2;}")
                                .arg(panel_text_name, section_bg_name, border_name));
        auto *preset_layout = new QVBoxLayout(preset_box);
        preset_layout->setContentsMargins(6, 6, 6, 6);
        preset_layout->setSpacing(4);
        auto *preset_folders = new QWidget(preset_box);
        auto *preset_folders_layout = new QGridLayout(preset_folders);
        preset_folders_layout->setContentsMargins(0, 0, 0, 0);
        preset_folders_layout->setHorizontalSpacing(10);
        preset_folders_layout->setVerticalSpacing(4);
        const QStringList preset_groups = {obsgs_tr("OBSTitles.Cloud"), obsgs_tr("OBSTitles.Iridescent"),
                                           obsgs_tr("OBSTitles.Pastels"), obsgs_tr("OBSTitles.Neutrals")};
        for (int i = 0; i < preset_groups.size(); ++i) {
            auto *label = new QLabel(QStringLiteral("▸  📁  %1").arg(preset_groups[i]), preset_folders);
            label->setStyleSheet(QStringLiteral("QLabel{color:%1;background:transparent;font-size:10px;}").arg(panel_text_name));
            preset_folders_layout->addWidget(label, i / 2, i % 2);
        }
        // Keep presets compact: only the quick preset strip is shown.
        preset_folders->hide();
        auto *preset_strip = new QWidget(preset_box);
        auto *preset_strip_layout = new QHBoxLayout(preset_strip);
        preset_strip_layout->setContentsMargins(0, 0, 0, 0);
        preset_strip_layout->setSpacing(4);
        preset_layout->addWidget(preset_strip);

        auto *gradient_form = new QWidget(gradient_tab);
        auto *gradient_form_layout = new QGridLayout(gradient_form);
        gradient_form_layout->setContentsMargins(0, 0, 0, 0);
        gradient_form_layout->setHorizontalSpacing(6);
        gradient_form_layout->setVerticalSpacing(4);
        auto *name_edit = new QLineEdit(obsgs_tr("OBSTitles.Custom"), gradient_form);
        name_edit->setStyleSheet(control_style);
        auto *new_preset = new QPushButton(obsgs_tr("OBSTitles.New"), gradient_form);
        auto *type = new QComboBox(gradient_form);
        type->addItem(obsgs_tr("OBSTitles.Linear"), 0);
        type->addItem(obsgs_tr("OBSTitles.RadialGradient"), 1);
        type->addItem(obsgs_tr("OBSTitles.ConicalGradient"), 2);
        type->setStyleSheet(control_style);
        auto *repeat_mode = new QComboBox(gradient_form);
        repeat_mode->addItem(obsgs_tr("OBSTitles.No"), 0);
        repeat_mode->addItem(obsgs_tr("OBSTitles.Repeat"), 2);
        repeat_mode->addItem(obsgs_tr("OBSTitles.Reflect"), 1);
        repeat_mode->setStyleSheet(control_style);
        auto *smoothness = new QSpinBox(gradient_form);
        smoothness->setRange(0, 100);
        smoothness->setValue(100);
        smoothness->setSuffix(QStringLiteral("%"));
        smoothness->setStyleSheet(control_style);
        // Compact square-layout controls: keep only the essentials visible.
        name_edit->hide();
        new_preset->hide();
        gradient_form_layout->addWidget(new QLabel(obsgs_tr("OBSTitles.Type"), gradient_form), 0, 0);
        gradient_form_layout->addWidget(type, 0, 1);
        gradient_form_layout->addWidget(new QLabel(obsgs_tr("OBSTitles.Smooth"), gradient_form), 0, 2);
        gradient_form_layout->addWidget(smoothness, 0, 3);
        gradient_form_layout->addWidget(new QLabel(obsgs_tr("OBSTitles.SpreadLabel"), gradient_form), 1, 0);
        gradient_form_layout->addWidget(repeat_mode, 1, 1);
        gradient_form_layout->setColumnStretch(1, 1);

        auto *preview = new GradientEditorPreview(gradient_tab);
        preview->setMinimumHeight(88);
        preview->setMaximumHeight(100);

        auto *type_group = new QButtonGroup(gradient_tab);
        type_group->setExclusive(true);
        // Kept for existing signal wiring; gradient type is now chosen from the Photoshop-style Type combo.

        auto *start_color = new QPushButton(gradient_tab);
        auto *end_color = new QPushButton(gradient_tab);
        auto make_spin = [&](double lo, double hi, double step, int decimals = 2) {
            auto *spin = new QDoubleSpinBox(gradient_tab);
            spin->setRange(lo, hi);
            spin->setSingleStep(step);
            spin->setDecimals(decimals);
            spin->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
            spin->setStyleSheet(control_style);
            return spin;
        };
        auto *start_pos = make_spin(0.0, 1.0, 0.01);
        auto *end_pos = make_spin(0.0, 1.0, 0.01);
        auto *start_opacity = make_spin(0.0, 1.0, 0.01);
        auto *end_opacity = make_spin(0.0, 1.0, 0.01);
        auto *gradient_opacity = make_spin(0.0, 1.0, 0.01);
        auto *angle = make_spin(-360.0, 360.0, 1.0, 0);
        angle->setSuffix(QStringLiteral("°"));
        auto *center_x = make_spin(-100.0, 100.0, 0.01);
        auto *center_y = make_spin(-100.0, 100.0, 0.01);
        auto *scale = make_spin(0.01, 100.0, 0.05);
        auto *focal_x = make_spin(-100.0, 100.0, 0.01);
        auto *focal_y = make_spin(-100.0, 100.0, 0.01);
        auto *reverse_gradient = new QCheckBox(obsgs_tr("OBSTitles.ReverseGradient"), gradient_tab);
        auto *dither_gradient = new QCheckBox(obsgs_tr("OBSTitles.Dither"), gradient_tab);
        for (auto *w : {gradient_opacity, angle, center_x, center_y, scale, focal_x, focal_y})
            w->hide();
        reverse_gradient->hide();
        dither_gradient->hide();

        if (stroke) {
            type->setCurrentIndex(std::max(0, type->findData(layer_->stroke_gradient_type)));
            repeat_mode->setCurrentIndex(std::max(0, repeat_mode->findData(layer_->stroke_gradient_spread)));
            style_color_button(start_color, layer_->stroke_gradient_start_color);
            style_color_button(end_color, layer_->stroke_gradient_end_color);
            start_pos->setValue(layer_->stroke_gradient_start_pos);
            end_pos->setValue(layer_->stroke_gradient_end_pos);
            start_opacity->setValue(layer_->stroke_gradient_start_opacity);
            end_opacity->setValue(layer_->stroke_gradient_end_opacity);
            gradient_opacity->setValue(layer_->stroke_gradient_opacity);
            angle->setValue(layer_->stroke_gradient_angle);
            center_x->setValue(layer_->stroke_gradient_center_x);
            center_y->setValue(layer_->stroke_gradient_center_y);
            scale->setValue(layer_->stroke_gradient_scale);
            focal_x->setValue(layer_->stroke_gradient_focal_x);
            focal_y->setValue(layer_->stroke_gradient_focal_y);
        } else {
            type->setCurrentIndex(std::max(0, type->findData(layer_->gradient_type)));
            repeat_mode->setCurrentIndex(std::max(0, repeat_mode->findData(layer_->gradient_spread)));
            style_color_button(start_color, layer_->gradient_start_color);
            style_color_button(end_color, layer_->gradient_end_color);
            start_pos->setValue(layer_->gradient_start_pos);
            end_pos->setValue(layer_->gradient_end_pos);
            start_opacity->setValue(layer_->gradient_start_opacity);
            end_opacity->setValue(layer_->gradient_end_opacity);
            gradient_opacity->setValue(layer_->gradient_opacity);
            angle->setValue(layer_->gradient_angle);
            center_x->setValue(layer_->gradient_center_x);
            center_y->setValue(layer_->gradient_center_y);
            scale->setValue(layer_->gradient_scale);
            focal_x->setValue(layer_->gradient_focal_x);
            focal_y->setValue(layer_->gradient_focal_y);
        }
        start_color->setText(QString());
        end_color->setText(QString());

        auto *stops_box = new QGroupBox(obsgs_tr("OBSTitles.Stops"), gradient_tab);
        stops_box->setStyleSheet(preset_box->styleSheet());
        auto *stops_layout = new QVBoxLayout(stops_box);
        stops_layout->setContentsMargins(5, 5, 5, 5);
        stops_layout->setSpacing(3);
        auto make_stop_row = [&](int stop_index, QPushButton *color_button, QDoubleSpinBox *pos_spin,
                                 QDoubleSpinBox *opacity_spin) {
            auto *row = new QWidget(stops_box);
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(6);
            color_button->setFixedSize(28, 18);
            layout->addWidget(new QLabel(stop_index < 2 ? (stop_index == 0 ? obsgs_tr("OBSTitles.Start") : obsgs_tr("OBSTitles.End"))
                                                        : obsgs_tr("OBSTitles.Stop"), row));
            layout->addWidget(color_button);
            pos_spin->setSuffix(QStringLiteral("%"));
            pos_spin->setRange(0.0, 100.0);
            pos_spin->setDecimals(0);
            pos_spin->setFixedWidth(58);
            opacity_spin->setSuffix(QStringLiteral("%"));
            opacity_spin->setRange(0.0, 100.0);
            opacity_spin->setDecimals(0);
            opacity_spin->setFixedWidth(58);
            auto *delete_button = new QPushButton(QStringLiteral("×"), row);
            delete_button->setFixedSize(22, 22);
            delete_button->setEnabled(false);
            delete_button->setToolTip(obsgs_tr("OBSTitles.StopDeleteLockedTooltip"));
            layout->addWidget(new QLabel(obsgs_tr("OBSTitles.Location"), row));
            layout->addWidget(pos_spin);
            layout->addWidget(new QLabel(obsgs_tr("OBSTitles.Opacity"), row));
            layout->addWidget(opacity_spin);
            layout->addWidget(delete_button);
            row->setProperty("stop_index", stop_index);
            stops_layout->addWidget(row);
            return row;
        };
        auto *stop0_row = make_stop_row(0, start_color, start_pos, start_opacity);
        auto *stop1_row = make_stop_row(1, end_color, end_pos, end_opacity);
        // Start/end controls remain as hidden backing controls for existing layer serialization.
        stop0_row->hide();
        stop1_row->hide();
        start_pos->setValue(start_pos->value() * 100.0);
        end_pos->setValue(end_pos->value() * 100.0);
        start_opacity->setValue(start_opacity->value() * 100.0);
        end_opacity->setValue(end_opacity->value() * 100.0);

        auto *selected_stop_row = new QWidget(stops_box);
        auto *selected_stop_layout = new QGridLayout(selected_stop_row);
        selected_stop_layout->setContentsMargins(0, 0, 0, 0);
        selected_stop_layout->setHorizontalSpacing(6);
        selected_stop_layout->setVerticalSpacing(5);
        auto *selected_stop_label = new QLabel(obsgs_tr("OBSTitles.NoStopSelected"), selected_stop_row);
        auto *selected_stop_color = new QPushButton(selected_stop_row);
        selected_stop_color->setFixedSize(28, 18);
        auto *selected_stop_location = make_spin(0.0, 100.0, 1.0, 0);
        selected_stop_location->setSuffix(QStringLiteral("%"));
        selected_stop_location->setFixedWidth(58);
        auto *selected_stop_opacity = make_spin(0.0, 100.0, 1.0, 0);
        selected_stop_opacity->setSuffix(QStringLiteral("%"));
        selected_stop_opacity->setFixedWidth(58);
        auto *selected_stop_delete = new QPushButton(QStringLiteral("×"), selected_stop_row);
        selected_stop_delete->setFixedSize(22, 22);
        selected_stop_delete->setToolTip(obsgs_tr("OBSTitles.DeleteSelectedStopTooltip"));
        selected_stop_layout->addWidget(selected_stop_label, 0, 0, 1, 5);
        selected_stop_layout->addWidget(new QLabel(obsgs_tr("OBSTitles.Color"), selected_stop_row), 1, 0);
        selected_stop_layout->addWidget(selected_stop_color, 1, 1);
        selected_stop_layout->addWidget(new QLabel(obsgs_tr("OBSTitles.Location"), selected_stop_row), 1, 2);
        selected_stop_layout->addWidget(selected_stop_location, 1, 3);
        selected_stop_layout->addWidget(selected_stop_delete, 1, 4);
        selected_stop_layout->addWidget(new QLabel(obsgs_tr("OBSTitles.Opacity"), selected_stop_row), 2, 2);
        selected_stop_layout->addWidget(selected_stop_opacity, 2, 3);
        stops_layout->addWidget(selected_stop_row);

        bool syncing_selected_stop_controls = false;
        auto *stop_actions = new QWidget(stops_box);
        auto *stop_actions_layout = new QHBoxLayout(stop_actions);
        stop_actions_layout->setContentsMargins(0, 0, 0, 0);
        stop_actions_layout->setSpacing(5);
        auto *add_stop = new QPushButton(obsgs_tr("OBSTitles.AddStop"), stop_actions);
        auto *duplicate_stop = new QPushButton(obsgs_tr("OBSTitles.Duplicate"), stop_actions);
        auto *sort_stops = new QPushButton(obsgs_tr("OBSTitles.Sort"), stop_actions);
        auto *delete_stop = new QPushButton(obsgs_tr("OBSTitles.Delete"), stop_actions);
        delete_stop->setToolTip(obsgs_tr("OBSTitles.DeleteStopTooltip"));
        duplicate_stop->setToolTip(obsgs_tr("OBSTitles.DuplicateStopTooltip"));
        add_stop->setToolTip(obsgs_tr("OBSTitles.AddStopTooltip"));
        stop_actions_layout->addWidget(add_stop);
        stop_actions_layout->addWidget(duplicate_stop);
        stop_actions_layout->addWidget(sort_stops);
        stop_actions_layout->addWidget(delete_stop);
        stop_actions_layout->addStretch(1);
        stops_layout->addWidget(stop_actions);

        auto *gradient_color_picker = new color_widgets::ColorDialog(stops_box, Qt::Widget);
        obsgs_prepare_embedded_color_dialog(gradient_color_picker, color_from_argb(stroke ? layer_->stroke_gradient_start_color
                                                                                          : layer_->gradient_start_color));
        properties_limit_gradient_stop_color_dialog(gradient_color_picker);
        gradient_color_picker->setVisible(true);
        gradient_color_picker->setEnabled(false);
        stops_layout->addWidget(gradient_color_picker);
        int active_gradient_stop = -1;
        bool syncing_gradient_color_picker = false;
        auto set_gradient_color_picker_target = [&](int stop_index) {
            if (stop_index < 0 || stop_index >= preview->stop_count()) {
                active_gradient_stop = -1;
                gradient_color_picker->setEnabled(false);
                return;
            }

            active_gradient_stop = stop_index;
            gradient_color_picker->setEnabled(true);
            QColor color = color_from_argb(preview->stop_color_argb(stop_index));
            color.setAlphaF(preview->stop_opacity(stop_index));
            syncing_gradient_color_picker = true;
            {
                QSignalBlocker blocker(gradient_color_picker);
                gradient_color_picker->setColor(color);
            }
            syncing_gradient_color_picker = false;
        };

        auto make_preset = [&](uint32_t a, uint32_t b) {
            auto *button = new QPushButton(preset_strip);
            button->setFixedSize(28, 28);
            style_gradient_button(button, a, b, 0);
            button->setText(QString());
            connect(button, &QPushButton::clicked, &popup, [=]() {
                style_color_button(start_color, a);
                style_color_button(end_color, b);
                start_pos->setValue(0.0);
                end_pos->setValue(100.0);
                start_opacity->setValue(100.0);
                end_opacity->setValue(100.0);
                gradient_opacity->setValue(1.0);
                if (stroke) {
                    layer_->stroke_gradient_start_color = a;
                    layer_->stroke_gradient_end_color = b;
                    layer_->stroke_gradient_stops.clear();
                } else {
                    layer_->gradient_start_color = a;
                    layer_->gradient_end_color = b;
                    layer_->gradient_stops.clear();
                }
                preview->set_gradient(type->currentData().toInt(), a, b, 0.0, 1.0, 1.0, 1.0,
                                      gradient_opacity->value(), angle->value(), center_x->value(),
                                      center_y->value(), scale->value());
                preview->set_extra_stops({});
                preview->set_selected_stop(-1);
                if (preview->gradient_changed) preview->gradient_changed();
            });
            preset_strip_layout->addWidget(button);
        };
        make_preset(0xFFFFFFFF, 0xFF000000);
        make_preset(0xFF4B6EA8, 0xFF1B1B1B);
        make_preset(0xFFFF4D4D, 0xFFFFC857);
        make_preset(0xFF20C997, 0xFF4B6EA8);
        make_preset(0x00FFFFFF, 0xFFFFFFFF);
        preset_strip_layout->addStretch(1);

        gradient_layout->addWidget(gradient_form);
        gradient_layout->addWidget(preview);
        gradient_layout->addWidget(stops_box);
        gradient_layout->addWidget(preset_box, 0, Qt::AlignTop);
        tabs->addTab(gradient_tab, obsgs_tr("OBSTitles.Gradient"));
        // With the Color tab hidden, open directly on Gradient.
        tabs->setCurrentWidget(gradient_tab);

        auto *pattern_tab = new QWidget(tabs);
        auto *pattern_layout = new QVBoxLayout(pattern_tab);
        pattern_layout->addWidget(new QLabel(obsgs_tr("OBSTitles.PatternsNotImplemented"), pattern_tab));
        tabs->addTab(pattern_tab, obsgs_tr("OBSTitles.Pattern"));
        tabs->setTabEnabled(tabs->indexOf(pattern_tab), false);

        auto preview_change_in_progress = std::make_shared<bool>(false);

        auto sync_stop_rows = [=, &syncing_selected_stop_controls]() {
            const int selected = preview->selected_stop();
            const bool has_selection = selected >= 0 && selected < preview->stop_count();
            const bool can_delete_selected = has_selection && preview->stop_count() > 2;

            syncing_selected_stop_controls = true;
            selected_stop_label->setText(has_selection
                ? obsgs_tr("OBSTitles.SelectedStop").arg(selected < 2 ? (selected == 0 ? obsgs_tr("OBSTitles.StartLower") : obsgs_tr("OBSTitles.EndLower"))
                                                               : obsgs_tr("OBSTitles.IntermediateLower"))
                : obsgs_tr("OBSTitles.NoStopSelected"));
            selected_stop_color->setEnabled(has_selection);
            selected_stop_location->setEnabled(has_selection);
            selected_stop_opacity->setEnabled(has_selection);
            selected_stop_delete->setEnabled(can_delete_selected);
            delete_stop->setEnabled(can_delete_selected);
            duplicate_stop->setEnabled(has_selection);

            if (has_selection) {
                style_color_button(selected_stop_color, preview->stop_color_argb(selected));
                selected_stop_color->setText(QString());
                selected_stop_location->setValue(preview->stop_position(selected) * 100.0);
                selected_stop_opacity->setValue(preview->stop_opacity(selected) * 100.0);
            } else {
                selected_stop_color->setStyleSheet(QStringLiteral("QPushButton{background:%1;border:1px solid %2;border-radius:2px;padding:0;}")
                                                   .arg(control_bg_name, border_name));
                selected_stop_color->setText(QString());
                selected_stop_location->setValue(0.0);
                selected_stop_opacity->setValue(0.0);
            }
            syncing_selected_stop_controls = false;
        };
        auto update_preview = [=]() {
            const uint32_t start_argb = stroke ? layer_->stroke_gradient_start_color : layer_->gradient_start_color;
            const uint32_t end_argb = stroke ? layer_->stroke_gradient_end_color : layer_->gradient_end_color;
            preview->set_gradient(type->currentData().toInt(), start_argb, end_argb,
                                  start_pos->value() / 100.0, end_pos->value() / 100.0,
                                  start_opacity->value() / 100.0, end_opacity->value() / 100.0,
                                  gradient_opacity->value(), angle->value(),
                                  center_x->value(), center_y->value(), scale->value());
            preview->set_extra_stops(stroke ? layer_->stroke_gradient_stops : layer_->gradient_stops);
            sync_stop_rows();
        };
        auto apply_gradient = [=, &popup, &emit_live_visual_change]() {
            if (!layer_ || loading_values_) return;
            if (stroke) {
                layer_->outline_enabled = true;
                layer_->stroke_fill_type = 2;
                layer_->stroke_gradient_type = type->currentData().toInt();
                layer_->stroke_gradient_spread = repeat_mode->currentData().toInt();
                layer_->stroke_gradient_start_pos = (float)(start_pos->value() / 100.0);
                layer_->stroke_gradient_end_pos = (float)(end_pos->value() / 100.0);
                layer_->stroke_gradient_start_opacity = (float)(start_opacity->value() / 100.0);
                layer_->stroke_gradient_end_opacity = (float)(end_opacity->value() / 100.0);
                layer_->stroke_gradient_opacity = (float)gradient_opacity->value();
                layer_->stroke_gradient_angle = (float)angle->value();
                layer_->stroke_gradient_center_x = (float)center_x->value();
                layer_->stroke_gradient_center_y = (float)center_y->value();
                layer_->stroke_gradient_scale = (float)scale->value();
                layer_->stroke_gradient_focal_x = (float)focal_x->value();
                layer_->stroke_gradient_focal_y = (float)focal_y->value();
                layer_->stroke_gradient_stops = preview->extra_stops();
            } else {
                layer_->fill_type = 1;
                layer_->gradient_type = type->currentData().toInt();
                layer_->gradient_spread = repeat_mode->currentData().toInt();
                layer_->gradient_start_pos = (float)(start_pos->value() / 100.0);
                layer_->gradient_end_pos = (float)(end_pos->value() / 100.0);
                layer_->gradient_start_opacity = (float)(start_opacity->value() / 100.0);
                layer_->gradient_end_opacity = (float)(end_opacity->value() / 100.0);
                layer_->gradient_opacity = (float)gradient_opacity->value();
                layer_->gradient_angle = (float)angle->value();
                layer_->gradient_center_x = (float)center_x->value();
                layer_->gradient_center_y = (float)center_y->value();
                layer_->gradient_scale = (float)scale->value();
                layer_->gradient_focal_x = (float)focal_x->value();
                layer_->gradient_focal_y = (float)focal_y->value();
                layer_->gradient_stops = preview->extra_stops();
                if (text_fill) apply_text_fill_format();
            }
            if (!*preview_change_in_progress)
                update_preview();
            else
                sync_stop_rows();
            update_main_swatch();
            emit_live_visual_change();
        };
        auto apply_gradient_stop_color = [=](int stop_index, const QColor &picked) {
            if (!picked.isValid() || stop_index < 0 || stop_index >= preview->stop_count())
                return;
            preview->set_stop_color(stop_index, picked);
        };
        auto show_stop_color_popup = [=, &set_gradient_color_picker_target](int stop_index, const QPoint &global_pos) {
            Q_UNUSED(global_pos);
            if (stop_index < 0 || stop_index >= preview->stop_count())
                return;
            set_gradient_color_picker_target(stop_index);
        };

        connect(tabs, &QTabWidget::currentChanged, &popup, [=, &selected_color](int idx) {
            emit gradient_editor_active_changed(idx == 2);
            if (idx == 0)
                apply_solid_color(selected_color);
            else if (idx == 2)
                apply_gradient();
        });
        connect(type, QOverload<int>::of(&QComboBox::currentIndexChanged), &popup, [=](int){ apply_gradient(); });
        connect(repeat_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), &popup, [=](int){ apply_gradient(); });
        connect(type_group, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
                &popup, [=](QAbstractButton *button) {
                    type->setCurrentIndex(std::max(0, type->findData(type_group->id(button))));
                });
        connect(gradient_color_picker, &color_widgets::ColorDialog::colorChanged, &popup,
                [=, &active_gradient_stop, &syncing_gradient_color_picker, &apply_gradient_stop_color](const QColor &color) {
                    if (syncing_gradient_color_picker)
                        return;
                    apply_gradient_stop_color(active_gradient_stop, color);
                });
        preview->selection_changed = [=, &set_gradient_color_picker_target](int stop_index) {
            sync_stop_rows();
            if (gradient_color_picker->isVisible())
                set_gradient_color_picker_target(stop_index);
        };
        preview->gradient_changed = [=]() {
            *preview_change_in_progress = true;
            {
                QSignalBlocker b0(start_pos), b1(end_pos), b2(start_opacity), b3(end_opacity);
                start_pos->setValue(preview->stop_position(0) * 100.0);
                end_pos->setValue(preview->stop_position(1) * 100.0);
                start_opacity->setValue(preview->stop_opacity(0) * 100.0);
                end_opacity->setValue(preview->stop_opacity(1) * 100.0);
            }
            if (stroke) {
                layer_->stroke_gradient_start_color = preview->stop_color_argb(0);
                layer_->stroke_gradient_end_color = preview->stop_color_argb(1);
                layer_->stroke_gradient_stops = preview->extra_stops();
            } else {
                layer_->gradient_start_color = preview->stop_color_argb(0);
                layer_->gradient_end_color = preview->stop_color_argb(1);
                layer_->gradient_stops = preview->extra_stops();
            }
            style_color_button(start_color, preview->stop_color_argb(0));
            style_color_button(end_color, preview->stop_color_argb(1));
            start_color->setText(QString());
            end_color->setText(QString());
            apply_gradient();
            *preview_change_in_progress = false;
        };
        preview->color_popup_requested = [=, &show_stop_color_popup](int stop_index, const QPoint &global_pos) {
            show_stop_color_popup(stop_index, global_pos);
        };
        connect(selected_stop_color, &QPushButton::clicked, &popup, [=, &show_stop_color_popup]() {
            const int selected = preview->selected_stop();
            if (selected < 0) return;
            show_stop_color_popup(selected, preview->stop_global_anchor(selected));
        });
        connect(selected_stop_location, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &popup,
                [=, &syncing_selected_stop_controls](double value) {
                    if (syncing_selected_stop_controls) return;
                    const int selected = preview->selected_stop();
                    if (selected < 0) return;
                    preview->set_stop_position(selected, value / 100.0);
                });
        connect(selected_stop_opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &popup,
                [=, &syncing_selected_stop_controls](double value) {
                    if (syncing_selected_stop_controls) return;
                    const int selected = preview->selected_stop();
                    if (selected < 0) return;
                    preview->set_stop_opacity(selected, value / 100.0);
                });
        connect(selected_stop_delete, &QPushButton::clicked, &popup, [=]() {
            const int selected = preview->selected_stop();
            if (selected < 0 || preview->stop_count() <= 2) return;
            preview->remove_stop(selected);
        });
        for (auto *spin : {start_pos, end_pos, start_opacity, end_opacity, gradient_opacity,
                           angle, center_x, center_y, scale, focal_x, focal_y}) {
            connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), &popup, [=](double){ apply_gradient(); });
        }
        connect(start_color, &QPushButton::clicked, &popup, [=, &show_stop_color_popup]() {
            preview->set_selected_stop(0);
            show_stop_color_popup(0, preview->stop_global_anchor(0));
        });
        connect(end_color, &QPushButton::clicked, &popup, [=, &show_stop_color_popup]() {
            preview->set_selected_stop(1);
            show_stop_color_popup(1, preview->stop_global_anchor(1));
        });
        connect(duplicate_stop, &QPushButton::clicked, &popup, [=]() {
            int src = preview->selected_stop();
            if (src < 0) return;
            QColor color = color_from_argb(preview->stop_color_argb(src));
            color.setAlphaF(preview->stop_opacity(src));
            const double pos = std::clamp(preview->stop_position(src) + 0.08, 0.0, 1.0);
            preview->add_stop(pos, color);
        });
        connect(add_stop, &QPushButton::clicked, &popup, [=]() {
            preview->add_stop(0.5);
        });
        connect(delete_stop, &QPushButton::clicked, &popup, [=]() {
            const int selected = preview->selected_stop();
            if (selected < 0 || preview->stop_count() <= 2) return;
            preview->remove_stop(selected);
        });
        connect(sort_stops, &QPushButton::clicked, &popup, [=]() {
            if (start_pos->value() <= end_pos->value()) return;
            const double p0 = start_pos->value(), p1 = end_pos->value();
            const double o0 = start_opacity->value(), o1 = end_opacity->value();
            const uint32_t c0 = stroke ? layer_->stroke_gradient_start_color : layer_->gradient_start_color;
            const uint32_t c1 = stroke ? layer_->stroke_gradient_end_color : layer_->gradient_end_color;
            start_pos->setValue(p1); end_pos->setValue(p0);
            start_opacity->setValue(o1); end_opacity->setValue(o0);
            if (stroke) {
                layer_->stroke_gradient_start_color = c1;
                layer_->stroke_gradient_end_color = c0;
            } else {
                layer_->gradient_start_color = c1;
                layer_->gradient_end_color = c0;
            }
            apply_gradient();
        });

        update_preview();
        const bool is_gradient_value = stroke ? (layer_->stroke_fill_type == 2) : (layer_->fill_type == 1);
        tabs->setCurrentIndex(is_gradient_value ? 2 : 0);
        emit gradient_editor_active_changed(is_gradient_value);
        live_visual_dirty = false;
        auto *source_button = stroke ? btn_appearance_stroke_color_ : btn_appearance_fill_color_;
        popup.adjustSize();
        const QPoint cursor_pos = QCursor::pos();
        QPoint desired_pos(cursor_pos.x() + 14, cursor_pos.y() - popup.height() / 2);
        if (source_button && source_button->rect().contains(source_button->mapFromGlobal(cursor_pos))) {
            const QPoint button_right = source_button->mapToGlobal(QPoint(source_button->width() + 4, 0));
            desired_pos = QPoint(button_right.x(), button_right.y());
        }
        if (pending_color_popup_position_valid_) {
            desired_pos = pending_color_popup_position_;
            pending_color_popup_position_valid_ = false;
        }
        popup.move(clamp_popup_position_to_screen(desired_pos, popup.size(), source_button));
        popup.exec();
        emit gradient_editor_active_changed(false);
        if (live_visual_dirty)
            emit_change();
    };
    connect(btn_appearance_fill_color_, &QPushButton::clicked,
            this, [open_color_selector]() { open_color_selector(false); });
    connect(btn_appearance_stroke_color_, &QPushButton::clicked,
            this, [open_color_selector]() { open_color_selector(true); });
    connect(stroke_options_trigger, &QPushButton::clicked,
            this, [this, can_edit, emit_change, control_style, checkbox_style, themed_dialog_style]() {
                if (!can_edit()) return;

                QDialog popup(this, Qt::Popup | Qt::FramelessWindowHint);
                popup.setModal(true);
                popup.setStyleSheet(themed_dialog_style);
                auto *root = new QVBoxLayout(&popup);
                root->setContentsMargins(8, 8, 8, 8);
                root->setSpacing(6);

                auto *weight_row = new QWidget(&popup);
                auto *weight_layout = new QHBoxLayout(weight_row);
                weight_layout->setContentsMargins(0, 0, 0, 0);
                weight_layout->setSpacing(6);
                auto *weight_label = new QLabel(obsgs_tr("OBSTitles.WeightColon"), weight_row);
                auto *weight = new QDoubleSpinBox(weight_row);
                weight->setRange(0.0, 200.0);
                weight->setDecimals(0);
                weight->setSingleStep(1.0);
                weight->setSuffix(QStringLiteral(" px"));
                weight->setFixedWidth(82);
                weight->setButtonSymbols(QAbstractSpinBox::UpDownArrows);
                weight->setStyleSheet(control_style);
                weight->setValue(layer_->stroke_width);
                weight_layout->addWidget(weight_label);
                weight_layout->addWidget(weight);
                weight_layout->addStretch();
                root->addWidget(weight_row);

                auto make_button = [](const QString &text, const QString &tip, QWidget *parent) {
                    auto *button = new QToolButton(parent);
                    button->setText(text);
                    button->setToolTip(tip);
                    button->setCheckable(true);
                    button->setFixedSize(28, 22);
                    return button;
                };
                auto add_button_group_row = [&](const QString &label_text, const QList<QToolButton *> &buttons,
                                                QWidget *extra = nullptr) {
                    auto *row = new QWidget(&popup);
                    auto *layout = new QHBoxLayout(row);
                    layout->setContentsMargins(0, 0, 0, 0);
                    layout->setSpacing(4);
                    auto *label = new QLabel(label_text, row);
                    label->setFixedWidth(46);
                    layout->addWidget(label);
                    for (auto *button : buttons)
                        layout->addWidget(button);
                    if (extra) {
                        layout->addSpacing(8);
                        layout->addWidget(extra);
                    }
                    layout->addStretch();
                    root->addWidget(row);
                };

                auto *cap_butt = make_button(obsgs_tr("OBSTitles.Butt"), obsgs_tr("OBSTitles.CapStyleUnsupported"), &popup);
                auto *cap_round = make_button(obsgs_tr("OBSTitles.Rnd"), obsgs_tr("OBSTitles.CapStyleUnsupported"), &popup);
                auto *cap_square = make_button(obsgs_tr("OBSTitles.Sqr"), obsgs_tr("OBSTitles.CapStyleUnsupported"), &popup);
                for (auto *button : {cap_butt, cap_round, cap_square})
                    button->setEnabled(false);
                add_button_group_row(obsgs_tr("OBSTitles.CapColon"), {cap_butt, cap_round, cap_square});

                auto *corner_group = new QButtonGroup(&popup);
                corner_group->setExclusive(true);
                auto *corner_miter = make_button(obsgs_tr("OBSTitles.M"), obsgs_tr("OBSTitles.Miter"), &popup);
                auto *corner_round = make_button(obsgs_tr("OBSTitles.R"), obsgs_tr("OBSTitles.Round"), &popup);
                auto *corner_bevel = make_button(obsgs_tr("OBSTitles.B"), obsgs_tr("OBSTitles.Bevel"), &popup);
                corner_group->addButton(corner_miter, 0);
                corner_group->addButton(corner_round, 1);
                corner_group->addButton(corner_bevel, 2);
                if (auto *button = corner_group->button(std::clamp(layer_->outline_join_style, 0, 2)))
                    button->setChecked(true);
                auto *limit = new QSpinBox(&popup);
                limit->setRange(1, 100);
                limit->setValue(10);
                limit->setFixedWidth(52);
                limit->setEnabled(false);
                limit->setStyleSheet(control_style);
                auto *limit_wrap = new QWidget(&popup);
                auto *limit_layout = new QHBoxLayout(limit_wrap);
                limit_layout->setContentsMargins(0, 0, 0, 0);
                limit_layout->setSpacing(4);
                limit_layout->addWidget(new QLabel(obsgs_tr("OBSTitles.LimitColon"), limit_wrap));
                limit_layout->addWidget(limit);
                add_button_group_row(obsgs_tr("OBSTitles.CornerColon"), {corner_miter, corner_round, corner_bevel}, limit_wrap);

                auto *order_group = new QButtonGroup(&popup);
                order_group->setExclusive(true);
                auto *order_back = make_button(obsgs_tr("OBSTitles.Back"), obsgs_tr("OBSTitles.StrokeBehindFillTooltip"), &popup);
                auto *order_front = make_button(obsgs_tr("OBSTitles.Front"), obsgs_tr("OBSTitles.StrokeInFrontFillTooltip"), &popup);
                order_group->addButton(order_back, 0);
                order_group->addButton(order_front, 1);
                if (auto *button = order_group->button(layer_->outline_on_front ? 1 : 0))
                    button->setChecked(true);
                add_button_group_row(obsgs_tr("OBSTitles.StrokeOrderColon"), {order_back, order_front});

                auto *alignment_group = new QButtonGroup(&popup);
                alignment_group->setExclusive(true);
                auto *alignment_outer = make_button(obsgs_tr("OBSTitles.Outer"), obsgs_tr("OBSTitles.OuterStrokeTooltip"), &popup);
                auto *alignment_mid = make_button(obsgs_tr("OBSTitles.Mid"), obsgs_tr("OBSTitles.MidStrokeTooltip"), &popup);
                auto *alignment_inner = make_button(obsgs_tr("OBSTitles.Inner"), obsgs_tr("OBSTitles.InnerStrokeTooltip"), &popup);
                for (auto *button : {alignment_outer, alignment_mid, alignment_inner})
                    button->setFixedWidth(42);
                alignment_group->addButton(alignment_outer, 0);
                alignment_group->addButton(alignment_mid, 1);
                alignment_group->addButton(alignment_inner, 2);
                if (auto *button = alignment_group->button(std::clamp(layer_->outline_alignment, 0, 2)))
                    button->setChecked(true);
                add_button_group_row(obsgs_tr("OBSTitles.StrokeAlignmentColon"),
                                     {alignment_outer, alignment_mid, alignment_inner});

                auto *dash_row = new QWidget(&popup);
                auto *dash_layout = new QHBoxLayout(dash_row);
                dash_layout->setContentsMargins(0, 0, 0, 0);
                dash_layout->setSpacing(4);
                auto *dashed = new QCheckBox(obsgs_tr("OBSTitles.DashedLine"), dash_row);
                dashed->setEnabled(false);
                dashed->setStyleSheet(checkbox_style);
                dashed->setToolTip(obsgs_tr("OBSTitles.DashedStrokesUnsupported"));
                dash_layout->addWidget(dashed);
                dash_layout->addStretch();
                root->addWidget(dash_row);

                auto *dash_values = new QWidget(&popup);
                auto *dash_values_layout = new QHBoxLayout(dash_values);
                dash_values_layout->setContentsMargins(0, 0, 0, 0);
                dash_values_layout->setSpacing(4);
                for (const QString &label : {obsgs_tr("OBSTitles.Dash"), obsgs_tr("OBSTitles.Gap"),
                                             obsgs_tr("OBSTitles.Dash"), obsgs_tr("OBSTitles.Gap"),
                                             obsgs_tr("OBSTitles.Dash"), obsgs_tr("OBSTitles.Gap")}) {
                    auto *field = new QSpinBox(dash_values);
                    field->setRange(0, 999);
                    field->setValue(label == obsgs_tr("OBSTitles.Dash") ? 12 : 0);
                    if (label == obsgs_tr("OBSTitles.Dash"))
                        field->setSuffix(QStringLiteral(" pt"));
                    field->setFixedWidth(48);
                    field->setEnabled(false);
                    field->setStyleSheet(control_style);
                    dash_values_layout->addWidget(field);
                }
                root->addWidget(dash_values);

                connect(weight, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                        &popup, [this, emit_change](double v) {
                            if (!layer_ || loading_values_) return;
                            layer_->stroke_width = (float)v;
                            layer_->outline_enabled = v > 0.0;
                            if (v > 0.0 && layer_->stroke_fill_type == 0)
                                layer_->stroke_fill_type = 1;
                            if (spn_appearance_stroke_width_) {
                                QSignalBlocker block(spn_appearance_stroke_width_);
                                spn_appearance_stroke_width_->setValue(v);
                            }
                            if (spn_outline_width_) {
                                QSignalBlocker block(spn_outline_width_);
                                spn_outline_width_->setValue(v);
                            }
                            emit_change();
                        });
                connect(corner_group, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
                        &popup, [this, corner_group, emit_change](QAbstractButton *button) {
                            if (!layer_ || loading_values_) return;
                            layer_->outline_join_style = corner_group->id(button);
                            if (cmb_outline_join_) {
                                QSignalBlocker block(cmb_outline_join_);
                                int idx = cmb_outline_join_->findData(layer_->outline_join_style);
                                cmb_outline_join_->setCurrentIndex(idx >= 0 ? idx : layer_->outline_join_style);
                            }
                            emit_change();
                        });
                connect(order_group, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
                        &popup, [this, order_group, emit_change](QAbstractButton *button) {
                            if (!layer_ || loading_values_) return;
                            layer_->outline_on_front = order_group->id(button) == 1;
                            if (cmb_outline_position_) {
                                QSignalBlocker block(cmb_outline_position_);
                                int idx = cmb_outline_position_->findData(layer_->outline_on_front ? 1 : 0);
                                cmb_outline_position_->setCurrentIndex(idx >= 0 ? idx : (layer_->outline_on_front ? 1 : 0));
                            }
                            emit_change();
                        });
                connect(alignment_group, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked),
                        &popup, [this, alignment_group, emit_change](QAbstractButton *button) {
                            if (!layer_ || loading_values_) return;
                            layer_->outline_alignment = std::clamp(alignment_group->id(button), 0, 2);
                            emit_change();
                        });

                const QPoint popup_pos = btn_appearance_stroke_label_->mapToGlobal(
                    QPoint(0, btn_appearance_stroke_label_->height() + 2));
                popup.adjustSize();
                popup.move(clamp_popup_position_to_screen(popup_pos, popup.size(), btn_appearance_stroke_label_));
                popup.exec();
            });
    connect(spn_appearance_stroke_width_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v) {
                if (!can_edit()) return;
                layer_->stroke_width = (float)v;
                layer_->outline_enabled = v > 0.0 && layer_->stroke_fill_type != 0;
                emit_change();
            });
    connect(spn_appearance_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change](double v) {
                if (!can_edit()) return;
                const double opacity = v / 100.0;
                set_animated_value(layer_->opacity, local_time(), opacity);
                if (spn_opacity_) {
                    QSignalBlocker block(spn_opacity_);
                    spn_opacity_->setValue(opacity);
                }
                emit_change();
            });
    auto shape_resize_factor = [](double old_w, double old_h, double new_w, double new_h) {
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
    };
    auto apply_shape_size_metric_scaling = [this, shape_resize_factor](double old_w, double old_h, double new_w, double new_h) {
        if (!layer_ || (layer_->type != LayerType::Shape && layer_->type != LayerType::SolidRect))
            return;
        const double factor = shape_resize_factor(old_w, old_h, new_w, new_h);
        if (!std::isfinite(factor) || factor <= 0.0 || std::abs(factor - 1.0) < 1e-6)
            return;

        if (layer_->scale_stroke_with_shape &&
            layer_->outline_enabled &&
            layer_->stroke_fill_type != 0 &&
            layer_->stroke_width > 0.0f) {
            layer_->stroke_width = (float)std::clamp((double)layer_->stroke_width * factor, 0.0, 512.0);
            if (spn_appearance_stroke_width_) {
                QSignalBlocker block(spn_appearance_stroke_width_);
                spn_appearance_stroke_width_->setValue(layer_->stroke_width);
            }
            if (spn_outline_width_) {
                QSignalBlocker block(spn_outline_width_);
                spn_outline_width_->setValue(layer_->stroke_width);
            }
        }

        if (layer_->scale_corners_with_shape) {
            const float tl = (float)std::clamp((double)layer_->corner_radius_tl * factor, 0.0, 9999.0);
            const float tr = (float)std::clamp((double)layer_->corner_radius_tr * factor, 0.0, 9999.0);
            const float br = (float)std::clamp((double)layer_->corner_radius_br * factor, 0.0, 9999.0);
            const float bl = (float)std::clamp((double)layer_->corner_radius_bl * factor, 0.0, 9999.0);
            set_layer_corner_radii(*layer_, tl, tr, br, bl);
            layer_->shape_roundness = (float)std::clamp((double)layer_->shape_roundness * factor, 0.0, 9999.0);
            layer_->shape_inner_roundness = (float)std::clamp((double)layer_->shape_inner_roundness * factor, 0.0, 9999.0);
            for (auto &point : layer_->path_points)
                point.corner_radius = std::clamp(point.corner_radius * factor, 0.0, 9999.0);
            if (spn_rect_corner_tl_) { QSignalBlocker block(spn_rect_corner_tl_); spn_rect_corner_tl_->setValue(layer_->corner_radius_tl); }
            if (spn_rect_corner_tr_) { QSignalBlocker block(spn_rect_corner_tr_); spn_rect_corner_tr_->setValue(layer_->corner_radius_tr); }
            if (spn_rect_corner_br_) { QSignalBlocker block(spn_rect_corner_br_); spn_rect_corner_br_->setValue(layer_->corner_radius_br); }
            if (spn_rect_corner_bl_) { QSignalBlocker block(spn_rect_corner_bl_); spn_rect_corner_bl_->setValue(layer_->corner_radius_bl); }
            if (spn_shape_roundness_) {
                QSignalBlocker block(spn_shape_roundness_);
                spn_shape_roundness_->setValue(layer_->shape_roundness);
            }
        }
    };
    connect(spn_layer_w_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_shape_size_metric_scaling](double v){
                if (!can_edit()) return;
                double t = local_time();
                if (layer_->type == LayerType::Image) {
                    double raw_w = eval_image_width(*layer_, t);
                    double raw_h = eval_image_height(*layer_, t);
                    const gsp::ImageDisplaySize current = gsp::calculate_image_display_size(
                        layer_->image_box_mode, layer_->image_size_auto_fit,
                        eval_box_width(*layer_, t), eval_box_height(*layer_, t), raw_w, raw_h);
                    const double old_w = current.width;
                    const double old_h = current.height;
                    layer_->image_size_auto_fit = false;
                    layer_->image_width = (float)v;
                    set_animated_x(layer_->image_size, t, v);
                    if (layer_->lock_aspect_ratio && old_w > 0.0) {
                        layer_->image_height = (float)(v * old_h / old_w);
                        set_animated_y(layer_->image_size, t, layer_->image_height);
                        QSignalBlocker block(spn_layer_h_);
                        spn_layer_h_->setValue(layer_->image_height);
                    } else {
                        layer_->image_height = (float)old_h;
                        set_animated_y(layer_->image_size, t, old_h);
                    }
                    emit_change();
                    return;
                }
                double old_w = eval_box_width(*layer_, t);
                double old_h = eval_box_height(*layer_, t);
                layer_->rect_width = (float)v;
                set_animated_x(layer_->size, t, v);
                if (spn_transform_size_w_) {
                    QSignalBlocker block(spn_transform_size_w_);
                    spn_transform_size_w_->setValue(v);
                }
                const bool lock_size = layer_->lock_aspect_ratio &&
                                       (layer_->type == LayerType::Image ||
                                        layer_->type == LayerType::Shape ||
                                        layer_->type == LayerType::SolidRect);
                if (lock_size && old_w > 0.0) {
                    layer_->rect_height = (float)(v * old_h / old_w);
                    set_animated_y(layer_->size, t, layer_->rect_height);
                    QSignalBlocker block(spn_layer_h_);
                    spn_layer_h_->setValue(layer_->rect_height);
                    if (spn_transform_size_h_) {
                        QSignalBlocker transform_block(spn_transform_size_h_);
                        spn_transform_size_h_->setValue(layer_->rect_height);
                    }
                }
                apply_shape_size_metric_scaling(old_w, old_h, eval_box_width(*layer_, t), eval_box_height(*layer_, t));
                emit_change();
            });
    connect(spn_layer_h_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_shape_size_metric_scaling](double v){
                if (!can_edit()) return;
                double t = local_time();
                if (layer_->type == LayerType::Image) {
                    double raw_w = eval_image_width(*layer_, t);
                    double raw_h = eval_image_height(*layer_, t);
                    const gsp::ImageDisplaySize current = gsp::calculate_image_display_size(
                        layer_->image_box_mode, layer_->image_size_auto_fit,
                        eval_box_width(*layer_, t), eval_box_height(*layer_, t), raw_w, raw_h);
                    const double old_w = current.width;
                    const double old_h = current.height;
                    layer_->image_size_auto_fit = false;
                    layer_->image_height = (float)v;
                    set_animated_y(layer_->image_size, t, v);
                    if (layer_->lock_aspect_ratio && old_h > 0.0) {
                        layer_->image_width = (float)(v * old_w / old_h);
                        set_animated_x(layer_->image_size, t, layer_->image_width);
                        QSignalBlocker block(spn_layer_w_);
                        spn_layer_w_->setValue(layer_->image_width);
                    } else {
                        layer_->image_width = (float)old_w;
                        set_animated_x(layer_->image_size, t, old_w);
                    }
                    emit_change();
                    return;
                }
                double old_w = eval_box_width(*layer_, t);
                double old_h = eval_box_height(*layer_, t);
                layer_->rect_height = (float)v;
                set_animated_y(layer_->size, t, v);
                if (spn_transform_size_h_) {
                    QSignalBlocker block(spn_transform_size_h_);
                    spn_transform_size_h_->setValue(v);
                }
                const bool lock_size = layer_->lock_aspect_ratio &&
                                       (layer_->type == LayerType::Image ||
                                        layer_->type == LayerType::Shape ||
                                        layer_->type == LayerType::SolidRect);
                if (lock_size && old_h > 0.0) {
                    layer_->rect_width = (float)(v * old_w / old_h);
                    set_animated_x(layer_->size, t, layer_->rect_width);
                    QSignalBlocker block(spn_layer_w_);
                    spn_layer_w_->setValue(layer_->rect_width);
                    if (spn_transform_size_w_) {
                        QSignalBlocker transform_block(spn_transform_size_w_);
                        spn_transform_size_w_->setValue(layer_->rect_width);
                    }
                }
                apply_shape_size_metric_scaling(old_w, old_h, eval_box_width(*layer_, t), eval_box_height(*layer_, t));
                emit_change();
            });
    connect(spn_transform_size_w_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_shape_size_metric_scaling](double v){
                if (!can_edit()) return;
                double t = local_time();
                double old_w = eval_box_width(*layer_, t);
                double old_h = eval_box_height(*layer_, t);
                layer_->rect_width = (float)v;
                set_animated_x(layer_->size, t, v);
                if (spn_layer_w_) {
                    QSignalBlocker block(spn_layer_w_);
                    spn_layer_w_->setValue(v);
                }
                const bool lock_size = layer_->lock_aspect_ratio &&
                                       (layer_->type == LayerType::Shape ||
                                        layer_->type == LayerType::SolidRect);
                if (lock_size && old_w > 0.0) {
                    layer_->rect_height = (float)(v * old_h / old_w);
                    set_animated_y(layer_->size, t, layer_->rect_height);
                    if (spn_transform_size_h_) {
                        QSignalBlocker block(spn_transform_size_h_);
                        spn_transform_size_h_->setValue(layer_->rect_height);
                    }
                    if (spn_layer_h_) {
                        QSignalBlocker block(spn_layer_h_);
                        spn_layer_h_->setValue(layer_->rect_height);
                    }
                }
                apply_shape_size_metric_scaling(old_w, old_h, eval_box_width(*layer_, t), eval_box_height(*layer_, t));
                emit_change();
            });
    connect(spn_transform_size_h_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, apply_shape_size_metric_scaling](double v){
                if (!can_edit()) return;
                double t = local_time();
                double old_w = eval_box_width(*layer_, t);
                double old_h = eval_box_height(*layer_, t);
                layer_->rect_height = (float)v;
                set_animated_y(layer_->size, t, v);
                if (spn_layer_h_) {
                    QSignalBlocker block(spn_layer_h_);
                    spn_layer_h_->setValue(v);
                }
                const bool lock_size = layer_->lock_aspect_ratio &&
                                       (layer_->type == LayerType::Shape ||
                                        layer_->type == LayerType::SolidRect);
                if (lock_size && old_h > 0.0) {
                    layer_->rect_width = (float)(v * old_w / old_h);
                    set_animated_x(layer_->size, t, layer_->rect_width);
                    if (spn_transform_size_w_) {
                        QSignalBlocker block(spn_transform_size_w_);
                        spn_transform_size_w_->setValue(layer_->rect_width);
                    }
                    if (spn_layer_w_) {
                        QSignalBlocker block(spn_layer_w_);
                        spn_layer_w_->setValue(layer_->rect_width);
                    }
                }
                apply_shape_size_metric_scaling(old_w, old_h, eval_box_width(*layer_, t), eval_box_height(*layer_, t));
                emit_change();
            });
    connect(spn_image_box_w_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, refresh_image_size_controls](double v) {
                if (!can_edit() || layer_->type != LayerType::Image) return;
                const double t = local_time();
                const double old_w = eval_box_width(*layer_, t);
                const double old_h = eval_box_height(*layer_, t);
                layer_->rect_width = (float)v;
                set_animated_x(layer_->size, t, v);
                if (layer_->image_box_lock_aspect_ratio && old_w > 0.0) {
                    layer_->rect_height = (float)(v * old_h / old_w);
                    set_animated_y(layer_->size, t, layer_->rect_height);
                    QSignalBlocker block(spn_image_box_h_);
                    spn_image_box_h_->setValue(layer_->rect_height);
                }
                refresh_image_size_controls();
                emit_change();
            });
    connect(spn_image_box_h_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, local_time, emit_change, refresh_image_size_controls](double v) {
                if (!can_edit() || layer_->type != LayerType::Image) return;
                const double t = local_time();
                const double old_w = eval_box_width(*layer_, t);
                const double old_h = eval_box_height(*layer_, t);
                layer_->rect_height = (float)v;
                set_animated_y(layer_->size, t, v);
                if (layer_->image_box_lock_aspect_ratio && old_h > 0.0) {
                    layer_->rect_width = (float)(v * old_w / old_h);
                    set_animated_x(layer_->size, t, layer_->rect_width);
                    QSignalBlocker block(spn_image_box_w_);
                    spn_image_box_w_->setValue(layer_->rect_width);
                }
                refresh_image_size_controls();
                emit_change();
            });
    auto set_corner_spin_values = [this](double tl, double tr, double br, double bl, QDoubleSpinBox *except = nullptr) {
        auto set_spin = [except](QDoubleSpinBox *spin, double value) {
            if (!spin || spin == except) return;
            QSignalBlocker blocker(spin);
            spin->setValue(value);
        };
        set_spin(spn_rect_corner_tl_, tl);
        set_spin(spn_rect_corner_tr_, tr);
        set_spin(spn_rect_corner_br_, br);
        set_spin(spn_rect_corner_bl_, bl);
    };
    auto connect_corner_spin = [this, can_edit, emit_change, set_corner_spin_values](QDoubleSpinBox *spin, int corner_index) {
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                this, [this, can_edit, emit_change, set_corner_spin_values, spin, corner_index](double v) {
                    if (!can_edit()) return;
                    if (chk_corner_lock_ && chk_corner_lock_->isChecked()) {
                        set_layer_all_corner_radii(*layer_, (float)v);
                        set_corner_spin_values(v, v, v, v, spin);
                    } else {
                        switch (corner_index) {
                        case 0: layer_->corner_radius_tl = (float)v; break;
                        case 1: layer_->corner_radius_tr = (float)v; break;
                        case 2: layer_->corner_radius_br = (float)v; break;
                        case 3: layer_->corner_radius_bl = (float)v; break;
                        default: break;
                        }
                        set_layer_corner_radii(*layer_, layer_->corner_radius_tl, layer_->corner_radius_tr,
                                               layer_->corner_radius_br, layer_->corner_radius_bl);
                    }
                    emit_change();
                });
    };
    connect_corner_spin(spn_rect_corner_tl_, 0);
    connect_corner_spin(spn_rect_corner_tr_, 1);
    connect_corner_spin(spn_rect_corner_br_, 2);
    connect_corner_spin(spn_rect_corner_bl_, 3);
    connect(chk_corner_lock_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change, set_corner_spin_values](bool locked) {
                if (!can_edit()) return;
                layer_->corner_radius_locked = locked;
                if (locked) {
                    const double radius = spn_rect_corner_tl_ ? spn_rect_corner_tl_->value() : layer_->corner_radius_tl;
                    set_layer_all_corner_radii(*layer_, (float)radius);
                    set_corner_spin_values(radius, radius, radius, radius);
                }
                emit_change();
            });
    connect(spn_corner_bevel_roundness_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double value) {
                if (!can_edit()) return;
                layer_->corner_bevel_roundness = (float)std::clamp(value, -100.0, 100.0);
                emit_change();
            });
    connect(cmb_shape_type_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx){
                if (!can_edit()) return;
                const ShapeType previous_shape = layer_->shape_type;
                const ShapeType next_shape = (ShapeType)cmb_shape_type_->itemData(idx).toInt();
                if (next_shape == ShapeType::Path && previous_shape != ShapeType::Path) {
                    if (!gsp::ensure_editable_path(*layer_)) {
                        load_values();
                        return;
                    }
                } else {
                    layer_->shape_type = next_shape;
                }
                if (previous_shape == ShapeType::Path && next_shape != ShapeType::Path) {
                    layer_->path_points.clear();
                    layer_->path_closed = true;
                }
                if (next_shape == ShapeType::Rectangle) {
                    set_layer_all_corner_radii(*layer_, 0.0f);
                    layer_->corner_radius_locked = true;
                    layer_->shape_roundness = 0.0f;
                    layer_->shape_inner_roundness = 0.0f;
                } else if (next_shape == ShapeType::RoundedRectangle &&
                           previous_shape != ShapeType::Rectangle &&
                           previous_shape != ShapeType::RoundedRectangle &&
                           layer_->corner_radius <= 0.0f) {
                    set_layer_all_corner_radii(*layer_, 18.0f);
                    layer_->corner_radius_locked = true;
                    layer_->shape_roundness = layer_->corner_radius;
                    layer_->shape_inner_roundness = layer_->shape_roundness;
                } else if (next_shape == ShapeType::Star && previous_shape != ShapeType::Star) {
                    layer_->shape_inner_roundness = layer_->shape_roundness;
                }
                load_values();
                emit_change();
            });
    connect(grp_shape_type_, &QButtonGroup::idClicked,
            this, [this](int shape_id) {
                if (!cmb_shape_type_ || loading_values_) return;
                const int idx = cmb_shape_type_->findData(shape_id);
                if (idx >= 0 && cmb_shape_type_->currentIndex() != idx)
                    cmb_shape_type_->setCurrentIndex(idx);
            });
    connect(chk_size_lock_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool locked) {
                if (can_edit()) {
                    layer_->lock_aspect_ratio = locked;
                    if (chk_transform_size_lock_) {
                        QSignalBlocker block(chk_transform_size_lock_);
                        chk_transform_size_lock_->setChecked(locked);
                    }
                    emit_change();
                }
            });
    connect(chk_transform_size_lock_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool locked) {
                if (can_edit()) {
                    layer_->lock_aspect_ratio = locked;
                    if (chk_size_lock_) {
                        QSignalBlocker block(chk_size_lock_);
                        chk_size_lock_->setChecked(locked);
                    }
                    emit_change();
                }
            });
    connect(chk_image_box_size_lock_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool locked) {
                if (can_edit() && layer_->type == LayerType::Image) {
                    layer_->image_box_lock_aspect_ratio = locked;
                    emit_change();
                }
            });
    connect(chk_shape_scale_stroke_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool enabled) {
                if (can_edit()) {
                    layer_->scale_stroke_with_shape = enabled;
                    emit_change();
                }
            });
    connect(chk_shape_scale_corners_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool enabled) {
                if (can_edit()) {
                    layer_->scale_corners_with_shape = enabled;
                    emit_change();
                }
            });
    connect(btn_shape_defaults_, &QPushButton::clicked,
            this, [this, can_edit, local_time, emit_change]() {
                if (!can_edit()) return;
                const double t = local_time();
                if (layer_->type == LayerType::Image) {
                    QSize intrinsic = editor_image_intrinsic_size(QString::fromStdString(layer_->image_path));
                    if (!intrinsic.isValid() || intrinsic.isEmpty())
                        intrinsic = QSize(std::max(1, (int)std::lround(layer_->image_width)),
                                          std::max(1, (int)std::lround(layer_->image_height)));
                    layer_->image_width = (float)intrinsic.width();
                    layer_->image_height = (float)intrinsic.height();
                    set_animated_x(layer_->image_size, t, layer_->image_width);
                    set_animated_y(layer_->image_size, t, layer_->image_height);
                    layer_->image_size_auto_fit = true;
                    layer_->lock_aspect_ratio = layer_->image_box_mode != ImageBoxMode::StretchToFill;
                    load_values();
                    emit_change();
                    return;
                }
                const double default_w = title_ ? std::max(1.0, title_->width * 0.5) : 960.0;
                const double default_h = 160.0;
                layer_->shape_type = ShapeType::Rectangle;
                layer_->rect_width = (float)default_w;
                layer_->rect_height = (float)default_h;
                set_animated_x(layer_->size, t, default_w);
                set_animated_y(layer_->size, t, default_h);
                set_layer_all_corner_radii(*layer_, 0.0f);
                layer_->corner_radius_locked = true;
                layer_->corner_bevel_roundness = 100.0f;
                layer_->shape_points = 5;
                layer_->shape_sides = 6;
                layer_->shape_inner_radius = 0.20f;
                layer_->shape_outer_radius = 0.5f;
                layer_->shape_roundness = 0.0f;
                layer_->shape_inner_roundness = 0.0f;
                layer_->lock_aspect_ratio = false;
                layer_->scale_stroke_with_shape = false;
                layer_->scale_corners_with_shape = false;
                load_values();
                emit_change();
            });
    connect(spn_shape_points_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, can_edit, emit_change](int v){ if (can_edit()) { layer_->shape_points = v; emit_change(); } });
    connect(spn_shape_sides_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, can_edit, emit_change](int v){ if (can_edit()) { layer_->shape_sides = v; emit_change(); } });
    connect(spn_shape_inner_radius_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){ if (can_edit()) { layer_->shape_inner_radius = (float)v; emit_change(); } });
    connect(spn_shape_outer_radius_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){ if (can_edit()) { layer_->shape_outer_radius = (float)v; emit_change(); } });
    connect(spn_shape_roundness_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change](double v){
                if (!can_edit()) return;
                const bool rectangle_shape = layer_->type == LayerType::SolidRect ||
                                             (layer_->type == LayerType::Shape &&
                                              (layer_->shape_type == ShapeType::Rectangle ||
                                               layer_->shape_type == ShapeType::RoundedRectangle));
                layer_->shape_roundness = (float)v;
                if (layer_->type == LayerType::Shape && layer_->shape_type == ShapeType::Star)
                    layer_->shape_inner_roundness = (float)v;
                if (rectangle_shape) {
                    set_layer_all_corner_radii(*layer_, (float)v);
                } else if (layer_->type == LayerType::Shape && layer_->shape_type == ShapeType::Path) {
                    for (auto &point : layer_->path_points)
                        point.corner_radius = std::max(0.0, v);
                }
                emit_change();
            });
    connect(btn_fill_color_, &QPushButton::clicked,
            this, [this, can_edit, local_time, emit_change]() {
                if (!can_edit()) return;
                QColor initial = color_from_argb(eval_fill_color(*layer_, local_time()));
                QColor picked = obsgs_pick_color(initial, this, obsgs_tr("OBSTitles.FillColor"));
                if (!picked.isValid()) return;
                layer_->fill_color = argb_from_color(picked);
                set_color_channels_at(*layer_, false, local_time(), layer_->fill_color);
                style_color_button(btn_fill_color_, layer_->fill_color);
                emit_change();
            });
    connect(cmb_fill_type_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](int idx) {
                if (!can_edit()) return;
                layer_->fill_type = cmb_fill_type_->itemData(idx).toInt();
                apply_text_fill_format();
                load_values();
                emit_change();
            });
    connect(cmb_gradient_type_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](int idx) {
                if (can_edit()) { layer_->gradient_type = cmb_gradient_type_->itemData(idx).toInt(); apply_text_fill_format(); emit_change(); }
            });
    connect(cmb_gradient_spread_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](int idx) {
                if (can_edit()) { layer_->gradient_spread = cmb_gradient_spread_->itemData(idx).toInt(); apply_text_fill_format(); emit_change(); }
            });
    auto connect_gradient_color = [this, can_edit, emit_change, apply_text_fill_format](QPushButton *button, uint32_t Layer::*member,
                                                                 const char *title_key, bool text_fill) {
        connect(button, &QPushButton::clicked, this, [this, can_edit, emit_change, apply_text_fill_format, button, member, title_key, text_fill]() {
            if (!can_edit()) return;
            QColor picked = obsgs_pick_color(color_from_argb((*layer_).*member), this, obsgs_tr(title_key));
            if (!picked.isValid()) return;
            (*layer_).*member = argb_from_color(picked);
            style_color_button(button, (*layer_).*member);
            if (text_fill) apply_text_fill_format();
            emit_change();
        });
    };
    connect_gradient_color(btn_gradient_start_color_, &Layer::gradient_start_color, "OBSTitles.StartColorLabel", true);
    connect_gradient_color(btn_gradient_end_color_, &Layer::gradient_end_color, "OBSTitles.EndColorLabel", true);
    connect(spn_gradient_start_pos_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](double v) { if (can_edit()) { layer_->gradient_start_pos = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_end_pos_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](double v) { if (can_edit()) { layer_->gradient_end_pos = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_start_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](double v) { if (can_edit()) { layer_->gradient_start_opacity = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_end_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](double v) { if (can_edit()) { layer_->gradient_end_opacity = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_opacity_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](double v) { if (can_edit()) { layer_->gradient_opacity = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_angle_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](double v) { if (can_edit()) { layer_->gradient_angle = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_center_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](double v) { if (can_edit()) { layer_->gradient_center_x = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_center_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](double v) { if (can_edit()) { layer_->gradient_center_y = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_scale_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](double v) { if (can_edit()) { layer_->gradient_scale = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_focal_x_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](double v) { if (can_edit()) { layer_->gradient_focal_x = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(spn_gradient_focal_y_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this, can_edit, emit_change, apply_text_fill_format](double v) { if (can_edit()) { layer_->gradient_focal_y = (float)v; apply_text_fill_format(); emit_change(); } });
    connect(edit_image_path_, &QLineEdit::textChanged,
            this, [this, can_edit, emit_change, fit_image_size_to_current_image](const QString &path){
                set_image_preview_label(lbl_image_preview_, path);
                if (can_edit()) {
                    layer_->image_path = path.toStdString();
                    fit_image_size_to_current_image();
                    emit_change();
                }
            });
    connect(cmb_image_scale_filter_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change](int idx){
                if (!can_edit() || idx < 0) return;
                layer_->scale_filter = (ImageScaleFilter)cmb_image_scale_filter_->itemData(idx).toInt();
                emit_change();
            });
    connect(cmb_image_box_mode_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, can_edit, emit_change, refresh_image_size_controls](int idx){
                if (!can_edit() || idx < 0) return;
                layer_->image_box_mode = (ImageBoxMode)cmb_image_box_mode_->itemData(idx).toInt();
                layer_->image_size_auto_fit = true;
                layer_->lock_aspect_ratio = layer_->image_box_mode != ImageBoxMode::StretchToFill;
                if (chk_size_lock_) {
                    QSignalBlocker block(chk_size_lock_);
                    chk_size_lock_->setChecked(layer_->lock_aspect_ratio);
                }
                refresh_image_size_controls();
                emit_change();
            });
    connect(chk_image_crop_when_outside_box_, &QCheckBox::toggled,
            this, [this, can_edit, emit_change](bool enabled) {
                if (!can_edit() || layer_->type != LayerType::Image) return;
                layer_->image_crop_when_outside_box = enabled;
                emit_change();
            });
    if (auto *image_anchor_button = static_cast<AnchorGridButton *>(btn_image_anchor_grid_)) {
        image_anchor_button->anchor_selected = [this, can_edit, emit_change](int i) {
            if (!can_edit()) return;
            QPointF next = anchor_point_from_index(i);
            layer_->image_anchor_x = (float)next.x();
            layer_->image_anchor_y = (float)next.y();
            emit_change();
        };
    }
    connect(btn_pick_image_, &QPushButton::clicked,
            this, [this, can_edit, fit_image_size_to_current_image, emit_change]() {
                if (!can_edit()) return;
                QString path = QFileDialog::getOpenFileName(
                    this, obsgs_tr("OBSTitles.ChooseImage"),
                    QString::fromStdString(layer_->image_path),
                    obsgs_tr("OBSTitles.ImageFileFilter"));
                if (path.isEmpty()) return;
                layer_->image_path = path.toStdString();
                fit_image_size_to_current_image();
                load_values();
                emit_change();
            });

    auto toggle_vec2_keyframe = [](AnimatedVec2Property &prop, double time, double x_value, double y_value) {
        toggle_keyframe(prop, time, {x_value, y_value});
    };

    connect(btn_kf_pos_x_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->position, local_time(), spn_px_->value(), spn_py_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_pos_y_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->position, local_time(), spn_px_->value(), spn_py_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_scale_x_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->scale, local_time(),
                             spn_scale_x_->value() / 100.0, spn_scale_y_->value() / 100.0);
        load_values();
        emit_change();
    });
    connect(btn_kf_scale_y_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->scale, local_time(),
                             spn_scale_x_->value() / 100.0, spn_scale_y_->value() / 100.0);
        load_values();
        emit_change();
    });
    connect(btn_kf_transform_size_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->size, local_time(),
                             spn_transform_size_w_->value(), spn_transform_size_h_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_rotation_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->rotation, local_time(), spn_rot_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_opacity_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->opacity, local_time(), spn_opacity_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_origin_x_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->origin_prop, local_time(),
                             spn_origin_x_->value(), spn_origin_y_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_origin_y_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        toggle_vec2_keyframe(layer_->origin_prop, local_time(),
                             spn_origin_x_->value(), spn_origin_y_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_paragraph_indent_left_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->paragraph_indent_left_prop, local_time(), spn_paragraph_indent_left_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_paragraph_indent_right_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->paragraph_indent_right_prop, local_time(), spn_paragraph_indent_right_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_paragraph_indent_first_line_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->paragraph_indent_first_line_prop, local_time(), spn_paragraph_indent_first_line_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_font_size_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->font_size_prop, local_time(), spn_size_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_char_scale_x_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->char_scale_x_prop, local_time(), spn_char_scale_x_->value() / 100.0);
        load_values();
        emit_change();
    });
    connect(btn_kf_char_scale_y_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->char_scale_y_prop, local_time(), spn_char_scale_y_->value() / 100.0);
        load_values();
        emit_change();
    });
    connect(btn_kf_char_tracking_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->char_tracking_prop, local_time(), spn_char_tracking_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_baseline_shift_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->baseline_shift_prop, local_time(), spn_baseline_shift_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_paragraph_space_before_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->paragraph_space_before_prop, local_time(), spn_paragraph_space_before_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_paragraph_space_after_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        toggle_keyframe(layer_->paragraph_space_after_prop, local_time(), spn_paragraph_space_after_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_width_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit()) return;
        if (layer_->type == LayerType::Image) {
            layer_->image_size_auto_fit = false;
            toggle_vec2_keyframe(layer_->image_size, local_time(),
                                 spn_layer_w_->value(), spn_layer_h_->value());
        } else {
            toggle_vec2_keyframe(layer_->size, local_time(),
                                 spn_layer_w_->value(), spn_layer_h_->value());
        }
        load_values();
        emit_change();
    });
    connect(btn_kf_image_box_size_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change, toggle_vec2_keyframe]() {
        if (!can_edit() || layer_->type != LayerType::Image) return;
        toggle_vec2_keyframe(layer_->size, local_time(),
                             spn_image_box_w_->value(), spn_image_box_h_->value());
        load_values();
        emit_change();
    });
    connect(btn_kf_text_color_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        double t = local_time();
        uint32_t color = eval_text_color(*layer_, t);
        if (any_keyframe_at_time({&layer_->text_color_a, &layer_->text_color_r,
                                  &layer_->text_color_g, &layer_->text_color_b}, t)) {
            remove_keyframe_at(layer_->text_color_a, t);
            remove_keyframe_at(layer_->text_color_r, t);
            remove_keyframe_at(layer_->text_color_g, t);
            remove_keyframe_at(layer_->text_color_b, t);
        } else {
            add_or_replace_keyframe(layer_->text_color_a, t, (color >> 24) & 0xFF);
            add_or_replace_keyframe(layer_->text_color_r, t, (color >> 16) & 0xFF);
            add_or_replace_keyframe(layer_->text_color_g, t, (color >> 8) & 0xFF);
            add_or_replace_keyframe(layer_->text_color_b, t, color & 0xFF);
        }
        load_values();
        emit_change();
    });

    connect(btn_kf_appearance_fill_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        const bool text_fill = layer_->type == LayerType::Text || layer_->type == LayerType::Clock ||
                               layer_->type == LayerType::Ticker;
        double t = local_time();
        uint32_t color = text_fill ? eval_text_color(*layer_, t) : eval_fill_color(*layer_, t);
        auto &a = text_fill ? layer_->text_color_a : layer_->fill_color_a;
        auto &r = text_fill ? layer_->text_color_r : layer_->fill_color_r;
        auto &g = text_fill ? layer_->text_color_g : layer_->fill_color_g;
        auto &b = text_fill ? layer_->text_color_b : layer_->fill_color_b;
        if (any_keyframe_at_time({&a, &r, &g, &b}, t)) {
            remove_keyframe_at(a, t);
            remove_keyframe_at(r, t);
            remove_keyframe_at(g, t);
            remove_keyframe_at(b, t);
        } else {
            add_or_replace_keyframe(a, t, (color >> 24) & 0xFF);
            add_or_replace_keyframe(r, t, (color >> 16) & 0xFF);
            add_or_replace_keyframe(g, t, (color >> 8) & 0xFF);
            add_or_replace_keyframe(b, t, color & 0xFF);
        }
        load_values();
        emit_change();
    });
    connect(btn_kf_fill_color_, &QPushButton::clicked, this, [this, can_edit, local_time, emit_change]() {
        if (!can_edit()) return;
        double t = local_time();
        uint32_t color = eval_fill_color(*layer_, t);
        if (any_keyframe_at_time({&layer_->fill_color_a, &layer_->fill_color_r,
                                  &layer_->fill_color_g, &layer_->fill_color_b}, t)) {
            remove_keyframe_at(layer_->fill_color_a, t);
            remove_keyframe_at(layer_->fill_color_r, t);
            remove_keyframe_at(layer_->fill_color_g, t);
            remove_keyframe_at(layer_->fill_color_b, t);
        } else {
            add_or_replace_keyframe(layer_->fill_color_a, t, (color >> 24) & 0xFF);
            add_or_replace_keyframe(layer_->fill_color_r, t, (color >> 16) & 0xFF);
            add_or_replace_keyframe(layer_->fill_color_g, t, (color >> 8) & 0xFF);
            add_or_replace_keyframe(layer_->fill_color_b, t, color & 0xFF);
        }
        load_values();
        emit_change();
    });

    auto apply_rule_editor_to_rule = [this](RichTextAutoStyleRule &rule) {
        rule.enabled = chk_auto_rule_enabled_ ? chk_auto_rule_enabled_->isChecked() : true;
        rule.display_name = edt_auto_rule_name_ ? edt_auto_rule_name_->text().trimmed().toStdString() : std::string();
        rule.style_preset_id = cmb_auto_rule_style_->currentData().toString().toStdString();
        rule.conflict_mode = cmb_auto_rule_conflict_mode_ ? cmb_auto_rule_conflict_mode_->currentData().toString().toStdString() : std::string("override_previous");
        rule.match_mode = cmb_auto_rule_match_mode_ ? cmb_auto_rule_match_mode_->currentData().toString().toStdString() : std::string("all_matches");
        rule.stop_processing = chk_auto_rule_stop_processing_ ? chk_auto_rule_stop_processing_->isChecked() : false;
        rule.require_stop_match = chk_auto_rule_require_stop_match_ ? chk_auto_rule_require_stop_match_->isChecked() : true;
        rule.include_start_marker = chk_auto_rule_include_start_marker_ ? chk_auto_rule_include_start_marker_->isChecked() : true;
        rule.include_end_marker = chk_auto_rule_include_end_marker_ ? chk_auto_rule_include_end_marker_->isChecked() : false;
        rule.excludes_rule_ids = edt_auto_rule_excludes_ ? auto_style_split_ids(edt_auto_rule_excludes_->text()) : std::vector<std::string>();
        rule.condition_type = "range_markers";
        rule.start_condition = cmb_auto_rule_start_condition_->currentData().toString().toStdString();
        rule.end_condition = cmb_auto_rule_end_condition_->currentData().toString().toStdString();
        rule.start_offset = (size_t)spn_auto_rule_start_offset_->value();
        rule.end_offset = (size_t)spn_auto_rule_end_offset_->value();
        rule.start_custom_chars = edt_auto_rule_start_chars_->text().toStdString();
        rule.end_custom_chars = edt_auto_rule_end_chars_->text().toStdString();
        rule.start = rule.start_offset;
        rule.length = rule.end_offset > rule.start_offset ? rule.end_offset - rule.start_offset : 0;
    };
    auto load_rule_to_editor = [this](const RichTextAutoStyleRule &rule) {
        if (chk_auto_rule_enabled_) chk_auto_rule_enabled_->setChecked(rule.enabled);
        if (edt_auto_rule_name_) edt_auto_rule_name_->setText(QString::fromStdString(rule.display_name));
        int idx = cmb_auto_rule_style_->findData(QString::fromStdString(rule.style_preset_id));
        if (idx >= 0) cmb_auto_rule_style_->setCurrentIndex(idx);
        int sidx = cmb_auto_rule_start_condition_->findData(QString::fromStdString(rule.start_condition.empty() ? std::string("text_start") : rule.start_condition));
        int eidx = cmb_auto_rule_end_condition_->findData(QString::fromStdString(rule.end_condition.empty() ? std::string("character_index") : rule.end_condition));
        if (sidx >= 0) cmb_auto_rule_start_condition_->setCurrentIndex(sidx);
        if (eidx >= 0) cmb_auto_rule_end_condition_->setCurrentIndex(eidx);
        spn_auto_rule_start_offset_->setValue((int)rule.start_offset);
        spn_auto_rule_end_offset_->setValue((int)(rule.end_offset ? rule.end_offset : rule.length));
        edt_auto_rule_start_chars_->setText(QString::fromStdString(rule.start_custom_chars));
        edt_auto_rule_end_chars_->setText(QString::fromStdString(rule.end_custom_chars));
        if (cmb_auto_rule_conflict_mode_) { int cidx = cmb_auto_rule_conflict_mode_->findData(QString::fromStdString(rule.conflict_mode.empty() ? std::string("override_previous") : rule.conflict_mode)); if (cidx >= 0) cmb_auto_rule_conflict_mode_->setCurrentIndex(cidx); }
        if (cmb_auto_rule_match_mode_) { int midx = cmb_auto_rule_match_mode_->findData(QString::fromStdString(rule.match_mode.empty() ? std::string("all_matches") : rule.match_mode)); if (midx >= 0) cmb_auto_rule_match_mode_->setCurrentIndex(midx); }
        if (edt_auto_rule_excludes_) edt_auto_rule_excludes_->setText(auto_style_join_ids(rule.excludes_rule_ids));
        if (chk_auto_rule_stop_processing_) chk_auto_rule_stop_processing_->setChecked(rule.stop_processing);
        if (chk_auto_rule_require_stop_match_) chk_auto_rule_require_stop_match_->setChecked(rule.require_stop_match);
        if (chk_auto_rule_include_start_marker_) chk_auto_rule_include_start_marker_->setChecked(rule.include_start_marker);
        if (chk_auto_rule_include_end_marker_) chk_auto_rule_include_end_marker_->setChecked(rule.include_end_marker);
        if (spn_auto_rule_chars_) spn_auto_rule_chars_->setValue((int)(rule.end_offset ? rule.end_offset : rule.length));
    };
    auto refresh_auto_rule_list = [this]() {
        if (!lst_auto_style_rules_ || !layer_) return;
        obsgsp::StylePresetLibrary library;
        QSignalBlocker blocker(lst_auto_style_rules_);
        lst_auto_style_rules_->clear();
        int i = 1;
        for (const auto &rule : layer_->rich_text.auto_style_rules) {
            const QString preset_name = library.displayNameForId(QString::fromStdString(rule.style_preset_id));
            const QString display_name = rule.display_name.empty() ? preset_name : QString::fromStdString(rule.display_name);
            const QString rule_id = QString::fromStdString(rule.rule_id.empty() ? std::to_string(i) : rule.rule_id);
            const QString from = auto_style_marker_label(rule.start_condition, rule.start_offset, rule.start_custom_chars);
            const QString to = auto_style_marker_label(rule.end_condition, rule.end_offset ? rule.end_offset : rule.length, rule.end_custom_chars);
            auto *item = new QListWidgetItem(QStringLiteral("%1. %2  •  %3 → %4  •  %5%6")
                                             .arg(i++).arg(display_name).arg(from).arg(to)
                                             .arg(QString::fromStdString(rule.conflict_mode.empty() ? std::string("override_previous") : rule.conflict_mode))
                                             .arg(rule.enabled ? QString() : QStringLiteral("  [off]")),
                                             lst_auto_style_rules_);
            item->setData(Qt::UserRole, i - 2);
            item->setToolTip(QStringLiteral("Rule ID: %1\nPreset: %2\nRange: %3 → %4\nConflict mode: %5\n%6")
                             .arg(rule_id, preset_name, from, to,
                                  QString::fromStdString(rule.conflict_mode.empty() ? std::string("override_previous") : rule.conflict_mode),
                                  rule.enabled ? QStringLiteral("Enabled") : QStringLiteral("Disabled")));
        }
    };
    auto cache_text_preset_format = [](const std::string &preset_id, RichTextCharFormat &format, uint32_t &mask) {
        mask = 0;
        if (preset_id.empty()) return;
        obsgsp::StylePresetLibrary library;
        obsgsp::StylePreset preset;
        if (library.findById(QString::fromStdString(preset_id), &preset) &&
            preset.kind == obsgsp::StylePresetKind::Text &&
            obsgsp::StylePresetLibrary::textPresetToCharFormat(preset, format))
            mask = obsgsp::StylePresetLibrary::textPresetCharMask();
    };
    auto cache_rule_format = [cache_text_preset_format](RichTextAutoStyleRule &rule) {
        cache_text_preset_format(rule.style_preset_id, rule.cached_format, rule.cached_mask);
    };
    auto cache_default_auto_style = [this, cache_text_preset_format]() {
        if (!layer_) return;
        cache_text_preset_format(layer_->rich_text.auto_default_style_preset_id,
                                 layer_->rich_text.auto_default_style_cached_format,
                                 layer_->rich_text.auto_default_style_cached_mask);
    };
    auto invalidate_auto_text_styling = [this, cache_default_auto_style, cache_rule_format]() {
        if (!layer_) return;
        if (layer_->rich_text.empty())
            layer_->rich_text = rich_text_document_from_layer_defaults(*layer_);
        if (layer_->rich_text.plain_text != layer_->text_content) {
            RichTextCharFormat insertion_format = insertion_format_for_text_replace(layer_->rich_text);
            rich_text_document_replace_text(layer_->rich_text, layer_->text_content, &insertion_format);
        }
        cache_default_auto_style();
        for (auto &rule : layer_->rich_text.auto_style_rules)
            cache_rule_format(rule);
        layer_->rich_text.normalize();
        layer_->rich_text_html.clear();
    };
    connect(chk_auto_style_enabled_, &QCheckBox::toggled, this, [this, invalidate_auto_text_styling](bool v){
        if (loading_values_ || !layer_) return;
        if (layer_->rich_text.empty()) layer_->rich_text = rich_text_document_from_layer_defaults(*layer_);
        layer_->rich_text.auto_style_enabled = v;
        invalidate_auto_text_styling();
        emit property_changed();
    });
    connect(cmb_auto_default_style_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, invalidate_auto_text_styling](int){
        if (loading_values_ || !layer_ || !cmb_auto_default_style_) return;
        if (layer_->rich_text.empty()) layer_->rich_text = rich_text_document_from_layer_defaults(*layer_);
        layer_->rich_text.auto_default_style_preset_id = cmb_auto_default_style_->currentData().toString().toStdString();
        invalidate_auto_text_styling();
        emit property_changed();
    });
    connect(btn_auto_rule_add_, &QPushButton::clicked, this, [this, apply_rule_editor_to_rule, cache_rule_format, invalidate_auto_text_styling, refresh_auto_rule_list](){
        if (!layer_ || !cmb_auto_rule_style_) return;
        const QString id = cmb_auto_rule_style_->currentData().toString();
        if (id.isEmpty()) return;
        if (layer_->rich_text.empty()) layer_->rich_text = rich_text_document_from_layer_defaults(*layer_);
        RichTextAutoStyleRule rule;
        rule.rule_id = std::string("rule_") + std::to_string(layer_->rich_text.auto_style_rules.size() + 1);
        rule.enabled = true;
        rule.style_preset_id = id.toStdString();
        apply_rule_editor_to_rule(rule);
        if (rule.rule_id.empty()) rule.rule_id = std::string("rule_") + std::to_string(layer_->rich_text.auto_style_rules.size() + 1);
        cache_rule_format(rule);
        layer_->rich_text.auto_style_rules.push_back(rule);
        invalidate_auto_text_styling();
        refresh_auto_rule_list();
        emit property_changed();
    });
    connect(btn_auto_rule_update_, &QPushButton::clicked, this, [this, apply_rule_editor_to_rule, cache_rule_format, invalidate_auto_text_styling, refresh_auto_rule_list](){
        if (!layer_ || !lst_auto_style_rules_ || !lst_auto_style_rules_->currentItem()) return;
        int row = lst_auto_style_rules_->currentRow();
        if (row < 0 || row >= (int)layer_->rich_text.auto_style_rules.size()) return;
        auto &rule = layer_->rich_text.auto_style_rules[(size_t)row];
        apply_rule_editor_to_rule(rule);
        cache_rule_format(rule);
        invalidate_auto_text_styling();
        refresh_auto_rule_list();
        lst_auto_style_rules_->setCurrentRow(row);
        emit property_changed();
    });
    connect(btn_auto_rule_delete_, &QPushButton::clicked, this, [this, invalidate_auto_text_styling, refresh_auto_rule_list](){
        if (!layer_ || !lst_auto_style_rules_ || !lst_auto_style_rules_->currentItem()) return;
        int row = lst_auto_style_rules_->currentRow();
        if (row < 0 || row >= (int)layer_->rich_text.auto_style_rules.size()) return;
        layer_->rich_text.auto_style_rules.erase(layer_->rich_text.auto_style_rules.begin() + row);
        invalidate_auto_text_styling();
        refresh_auto_rule_list();
        emit property_changed();
    });
    connect(btn_auto_rule_up_, &QPushButton::clicked, this, [this, invalidate_auto_text_styling, refresh_auto_rule_list](){
        if (!layer_ || !lst_auto_style_rules_) return;
        int row = lst_auto_style_rules_->currentRow();
        if (row <= 0 || row >= (int)layer_->rich_text.auto_style_rules.size()) return;
        std::swap(layer_->rich_text.auto_style_rules[(size_t)row], layer_->rich_text.auto_style_rules[(size_t)row - 1]);
        invalidate_auto_text_styling();
        refresh_auto_rule_list(); lst_auto_style_rules_->setCurrentRow(row - 1); emit property_changed();
    });
    connect(btn_auto_rule_down_, &QPushButton::clicked, this, [this, invalidate_auto_text_styling, refresh_auto_rule_list](){
        if (!layer_ || !lst_auto_style_rules_) return;
        int row = lst_auto_style_rules_->currentRow();
        if (row < 0 || row + 1 >= (int)layer_->rich_text.auto_style_rules.size()) return;
        std::swap(layer_->rich_text.auto_style_rules[(size_t)row], layer_->rich_text.auto_style_rules[(size_t)row + 1]);
        invalidate_auto_text_styling();
        refresh_auto_rule_list(); lst_auto_style_rules_->setCurrentRow(row + 1); emit property_changed();
    });
    connect(lst_auto_style_rules_, &QListWidget::currentRowChanged, this, [this, load_rule_to_editor](int row){
        if (!layer_ || row < 0 || row >= (int)layer_->rich_text.auto_style_rules.size()) return;
        const auto &rule = layer_->rich_text.auto_style_rules[(size_t)row];
        load_rule_to_editor(rule);
    });
}

void PropertiesPanel::set_title(std::shared_ptr<Title> t)
{
    title_ = t;
}

void PropertiesPanel::set_active_text_edit_layer(const std::string &layer_id)
{
    if (active_text_edit_layer_id_ == layer_id) return;
    active_text_edit_layer_id_ = layer_id;
    load_values();
}

void PropertiesPanel::set_layer(std::shared_ptr<Layer> layer, double t)
{
    layer_    = layer;
    playhead_ = t;
    load_values();
}

void PropertiesPanel::update_playhead(double t)
{
    playhead_ = t;
    if (!layer_ || loading_values_)
        return;

    QScopedValueRollback<bool> loading_guard(loading_values_, true);
    const double lt = std::clamp(playhead_ - layer_->in_time, 0.0,
                                 std::max(0.0, layer_->out_time - layer_->in_time));
    const bool is_text = layer_->type == LayerType::Text;
    const bool is_clock = layer_->type == LayerType::Clock;
    const bool is_ticker = layer_->type == LayerType::Ticker;
    const bool is_text_like = is_text || is_clock || is_ticker;
    const bool is_image = layer_->type == LayerType::Image;
    const bool is_rect = layer_->type == LayerType::SolidRect || layer_->type == LayerType::Shape;

    auto set_double = [](QDoubleSpinBox *spin, double value) {
        if (!spin || !std::isfinite(value))
            return;
        if (std::abs(spin->value() - value) < 0.000001)
            return;
        QSignalBlocker blocker(spin);
        spin->setValue(value);
    };
    auto set_int = [](QSpinBox *spin, int value) {
        if (!spin)
            return;
        if (spin->value() == value)
            return;
        QSignalBlocker blocker(spin);
        spin->setValue(value);
    };
    auto set_color_button = [](QPushButton *button, uint32_t argb) {
        if (!button)
            return;
        if (button->property("buttonFillKind").toString() == QStringLiteral("color") &&
            button->property("argb").isValid() && button->property("argb").toUInt() == argb)
            return;
        style_color_button(button, argb);
        button->setProperty("buttonFillKind", QStringLiteral("color"));
        button->setProperty("argb", argb);
    };
    auto set_gradient_button = [](QPushButton *button, uint32_t start_argb,
                                  uint32_t end_argb, int gradient_type) {
        if (!button)
            return;
        if (button->property("buttonFillKind").toString() == QStringLiteral("gradient") &&
            button->property("gradientStartArgb").isValid() &&
            button->property("gradientStartArgb").toUInt() == start_argb &&
            button->property("gradientEndArgb").toUInt() == end_argb &&
            button->property("gradientType").toInt() == gradient_type)
            return;
        style_gradient_button(button, start_argb, end_argb, gradient_type);
        button->setProperty("buttonFillKind", QStringLiteral("gradient"));
        button->setProperty("gradientStartArgb", start_argb);
        button->setProperty("gradientEndArgb", end_argb);
        button->setProperty("gradientType", gradient_type);
    };
    auto set_kf_icon = [](QPushButton *button, bool active, bool has_keyframes) {
        if (!button)
            return;
        const bool outlined = has_keyframes && !active;
        if (button->property("active").isValid() &&
            button->property("active").toBool() == active &&
            button->property("outlined").toBool() == outlined)
            return;
        button->setIcon(keyframe_diamond_icon(active, outlined));
        button->setProperty("active", active);
        button->setProperty("outlined", outlined);
        button->style()->unpolish(button);
        button->style()->polish(button);
    };
    auto set_prop_kf_icon = [&](QPushButton *button, const AnimatedProperty &prop) {
        set_kf_icon(button, keyframe_at_time(prop, lt), prop.is_animated());
    };
    auto set_group_kf_icon = [&](QPushButton *button, std::initializer_list<const AnimatedProperty *> props) {
        set_kf_icon(button, any_keyframe_at_time(props, lt), any_keyframes(props));
    };
    auto set_vec_kf_icon = [&](QPushButton *button, const AnimatedVec2Property &prop) {
        set_kf_icon(button, keyframe_at_time(prop, lt), prop.is_animated());
    };

    const Vec2Value position = layer_->position.evaluate(lt);
    const Vec2Value scale = layer_->scale.evaluate(lt);
    set_double(spn_px_, position.x);
    set_double(spn_py_, position.y);
    set_double(spn_scale_x_, scale.x * 100.0);
    set_double(spn_scale_y_, scale.y * 100.0);
    set_double(spn_transform_size_w_, eval_box_width(*layer_, lt));
    set_double(spn_transform_size_h_, eval_box_height(*layer_, lt));
    set_double(spn_rot_, layer_->rotation.evaluate(lt));
    set_double(spn_opacity_, layer_->opacity.evaluate(lt));
    if (spn_appearance_opacity_)
        set_double(spn_appearance_opacity_, layer_->opacity.evaluate(lt) * 100.0);
    set_double(spn_origin_x_, eval_origin_x(*layer_, lt));
    set_double(spn_origin_y_, eval_origin_y(*layer_, lt));

    if (is_image) {
        double raw_w = eval_image_width(*layer_, lt);
        double raw_h = eval_image_height(*layer_, lt);
        if (raw_w <= 0.0 || raw_h <= 0.0) {
            const QSize intrinsic = editor_image_intrinsic_size(QString::fromStdString(layer_->image_path));
            if (intrinsic.isValid() && !intrinsic.isEmpty()) {
                raw_w = intrinsic.width();
                raw_h = intrinsic.height();
            }
        }
        const gsp::ImageDisplaySize display = gsp::calculate_image_display_size(
            layer_->image_box_mode, layer_->image_size_auto_fit,
            eval_box_width(*layer_, lt), eval_box_height(*layer_, lt), raw_w, raw_h);
        set_double(spn_layer_w_, display.width);
        set_double(spn_layer_h_, display.height);
    } else {
        set_double(spn_layer_w_, eval_box_width(*layer_, lt));
        set_double(spn_layer_h_, eval_box_height(*layer_, lt));
    }
    set_double(spn_image_box_w_, eval_box_width(*layer_, lt));
    set_double(spn_image_box_h_, eval_box_height(*layer_, lt));

    set_color_button(btn_text_color_, eval_text_color(*layer_, lt));
    set_color_button(btn_fill_color_, eval_fill_color(*layer_, lt));
    if (btn_appearance_fill_color_) {
        if (layer_->fill_type == 1)
            set_gradient_button(btn_appearance_fill_color_,
                                layer_->gradient_start_color,
                                layer_->gradient_end_color,
                                layer_->gradient_type);
        else
            set_color_button(btn_appearance_fill_color_,
                             is_text_like ? eval_text_color(*layer_, lt) : eval_fill_color(*layer_, lt));
        btn_appearance_fill_color_->setText(QString());
    }

    if (is_text_like) {
        set_int(spn_size_, (int)std::clamp(std::round(layer_->font_size_prop.evaluate(lt)), 1.0, 512.0));
        set_double(spn_char_tracking_, layer_->char_tracking_prop.evaluate(lt));
        set_double(spn_char_scale_x_, layer_->char_scale_x_prop.evaluate(lt) * 100.0);
        set_double(spn_char_scale_y_, layer_->char_scale_y_prop.evaluate(lt) * 100.0);
        set_double(spn_baseline_shift_, layer_->baseline_shift_prop.evaluate(lt));
        set_double(spn_paragraph_indent_left_, eval_paragraph_indent_left(*layer_, lt));
        set_double(spn_paragraph_indent_right_, eval_paragraph_indent_right(*layer_, lt));
        set_double(spn_paragraph_indent_first_line_, eval_paragraph_indent_first_line(*layer_, lt));
        set_double(spn_paragraph_space_before_, layer_->paragraph_space_before_prop.evaluate(lt));
        set_double(spn_paragraph_space_after_, layer_->paragraph_space_after_prop.evaluate(lt));

        if (lbl_text_fit_scale_ && layer_->text_overflow_mode == 2 && !is_ticker) {
            QFont preview_font = font_for_layer(*layer_);
            QRectF preview_rect(0, 0, eval_box_width(*layer_, lt), eval_box_height(*layer_, lt));
            double fit_scale = horizontal_fit_scale(preview_font, preview_rect,
                                                    display_text_for_style(*layer_), *layer_, lt);
            lbl_text_fit_scale_->setText(obsgs_tr("OBSTitles.ScalePercentFormat")
                                             .arg((int)std::round(fit_scale * 100.0)));
        }
    }

    set_vec_kf_icon(btn_kf_pos_x_, layer_->position);
    set_vec_kf_icon(btn_kf_pos_y_, layer_->position);
    set_vec_kf_icon(btn_kf_scale_x_, layer_->scale);
    set_vec_kf_icon(btn_kf_scale_y_, layer_->scale);
    set_vec_kf_icon(btn_kf_transform_size_, layer_->size);
    set_prop_kf_icon(btn_kf_rotation_, layer_->rotation);
    set_prop_kf_icon(btn_kf_opacity_, layer_->opacity);
    set_vec_kf_icon(btn_kf_origin_x_, layer_->origin_prop);
    set_vec_kf_icon(btn_kf_origin_y_, layer_->origin_prop);
    set_prop_kf_icon(btn_kf_paragraph_indent_left_, layer_->paragraph_indent_left_prop);
    set_prop_kf_icon(btn_kf_paragraph_indent_right_, layer_->paragraph_indent_right_prop);
    set_prop_kf_icon(btn_kf_paragraph_indent_first_line_, layer_->paragraph_indent_first_line_prop);
    set_prop_kf_icon(btn_kf_font_size_, layer_->font_size_prop);
    set_prop_kf_icon(btn_kf_char_scale_x_, layer_->char_scale_x_prop);
    set_prop_kf_icon(btn_kf_char_scale_y_, layer_->char_scale_y_prop);
    set_prop_kf_icon(btn_kf_char_tracking_, layer_->char_tracking_prop);
    set_prop_kf_icon(btn_kf_baseline_shift_, layer_->baseline_shift_prop);
    set_prop_kf_icon(btn_kf_paragraph_space_before_, layer_->paragraph_space_before_prop);
    set_prop_kf_icon(btn_kf_paragraph_space_after_, layer_->paragraph_space_after_prop);
    set_vec_kf_icon(btn_kf_width_, is_image ? layer_->image_size : layer_->size);
    set_vec_kf_icon(btn_kf_image_box_size_, layer_->size);
    set_group_kf_icon(btn_kf_text_color_, {&layer_->text_color_a, &layer_->text_color_r,
                                           &layer_->text_color_g, &layer_->text_color_b});
    set_group_kf_icon(btn_kf_fill_color_, {&layer_->fill_color_a, &layer_->fill_color_r,
                                           &layer_->fill_color_g, &layer_->fill_color_b});
    if (is_text_like)
        set_group_kf_icon(btn_kf_appearance_fill_, {&layer_->text_color_a, &layer_->text_color_r,
                                                    &layer_->text_color_g, &layer_->text_color_b});
    else if (is_rect)
        set_group_kf_icon(btn_kf_appearance_fill_, {&layer_->fill_color_a, &layer_->fill_color_r,
                                                    &layer_->fill_color_g, &layer_->fill_color_b});
}

void PropertiesPanel::load_values()
{
    loading_values_ = true;
    if (!layer_) {
        if (transform_box_) transform_box_->setVisible(false);
        if (appearance_box_) appearance_box_->setVisible(false);
        if (btn_transform_defaults_) btn_transform_defaults_->setEnabled(false);
        if (btn_anchor_grid_) { btn_anchor_grid_->setProperty("active_index", 4); btn_anchor_grid_->update(); }
        text_box_->setVisible(false);
        if (type_options_box_) type_options_box_->setVisible(false);
        if (paragraph_box_) paragraph_box_->setVisible(false);
        if (dynamic_text_box_) dynamic_text_box_->setVisible(false);
        if (auto_style_box_) auto_style_box_->setVisible(false);
        if (live_edit_box_) live_edit_box_->setVisible(false);
        if (bullets_box_) bullets_box_->setVisible(false);
        rect_box_->setVisible(false);
        if (btn_shape_defaults_) btn_shape_defaults_->setVisible(false);
        if (chk_size_lock_) chk_size_lock_->setVisible(false);
        if (row_shape_scale_options_) row_shape_scale_options_->setVisible(false);
        if (grp_shape_type_) {
            for (auto *button : grp_shape_type_->buttons())
                button->setVisible(false);
        }
        if (auto *shape_types_row = rect_box_ ? rect_box_->findChild<QWidget *>(QStringLiteral("OBSTitlesShapeTypeButtonsRow")) : nullptr)
            shape_types_row->setVisible(false);
        if (gradient_box_) gradient_box_->setVisible(false);
        image_box_->setVisible(false);
        if (image_box_size_box_) image_box_size_box_->setVisible(false);
        if (outline_box_) outline_box_->setVisible(false);
        if (shadow_box_) shadow_box_->setVisible(false);
        spn_px_->setValue(0.0);
        spn_py_->setValue(0.0);
        spn_rot_->setValue(0.0);
        spn_opacity_->setValue(1.0);
        spn_origin_x_->setValue(0.5);
        spn_origin_y_->setValue(0.5);
        if (chk_shape_scale_stroke_) chk_shape_scale_stroke_->setChecked(false);
        if (chk_shape_scale_corners_) chk_shape_scale_corners_->setChecked(false);
        txt_content_->clear();
        edit_image_path_->clear();
        set_image_preview_label(lbl_image_preview_, QString());
        if (cmb_image_scale_filter_) {
            QSignalBlocker block(cmb_image_scale_filter_);
            cmb_image_scale_filter_->setCurrentIndex(1);
        }
        if (cmb_image_box_mode_) {
            QSignalBlocker block(cmb_image_box_mode_);
            int mode_index = cmb_image_box_mode_->findData((int)ImageBoxMode::FitImageToBox);
            cmb_image_box_mode_->setCurrentIndex(mode_index >= 0 ? mode_index : 0);
        }
        if (chk_image_crop_when_outside_box_) {
            QSignalBlocker block(chk_image_crop_when_outside_box_);
            chk_image_crop_when_outside_box_->setChecked(false);
        }
        if (btn_image_anchor_grid_) { btn_image_anchor_grid_->setProperty("active_index", 4); btn_image_anchor_grid_->update(); }
        style_color_button(btn_text_color_, 0xFFFFFFFF);
        if (btn_text_color_) btn_text_color_->setEnabled(true);
        if (btn_kf_text_color_) btn_kf_text_color_->setEnabled(true);
        style_color_button(btn_fill_color_, 0xFF222222);
        if (cmb_fill_type_) cmb_fill_type_->setCurrentIndex(0);
        if (cmb_gradient_type_) cmb_gradient_type_->setCurrentIndex(0);
        if (cmb_gradient_spread_) cmb_gradient_spread_->setCurrentIndex(0);
        if (btn_gradient_start_color_) style_color_button(btn_gradient_start_color_, 0xFF4B6EA8);
        if (btn_gradient_end_color_) style_color_button(btn_gradient_end_color_, 0xFF1B1B1B);
        if (btn_appearance_fill_color_) { style_color_button(btn_appearance_fill_color_, 0xFF222222); btn_appearance_fill_color_->setText(QString()); }
        if (btn_appearance_stroke_color_) {
            style_color_button(btn_appearance_stroke_color_, 0xFF000000);
            btn_appearance_stroke_color_->setText(QString());
            btn_appearance_stroke_color_->setVisible(false);
        }
        if (btn_appearance_stroke_label_) btn_appearance_stroke_label_->setVisible(false);
        if (spn_appearance_stroke_width_) {
            spn_appearance_stroke_width_->setValue(0.0);
            spn_appearance_stroke_width_->setVisible(false);
        }
        if (spn_appearance_opacity_) spn_appearance_opacity_->setValue(100.0);
        if (spn_gradient_start_pos_) spn_gradient_start_pos_->setValue(0.0);
        if (spn_gradient_end_pos_) spn_gradient_end_pos_->setValue(1.0);
        if (spn_gradient_start_opacity_) spn_gradient_start_opacity_->setValue(1.0);
        if (spn_gradient_end_opacity_) spn_gradient_end_opacity_->setValue(1.0);
        if (spn_gradient_opacity_) spn_gradient_opacity_->setValue(1.0);
        if (spn_gradient_angle_) spn_gradient_angle_->setValue(0.0);
        if (spn_gradient_center_x_) spn_gradient_center_x_->setValue(0.5);
        if (spn_gradient_center_y_) spn_gradient_center_y_->setValue(0.5);
        if (spn_gradient_scale_) spn_gradient_scale_->setValue(1.0);
        if (spn_gradient_focal_x_) spn_gradient_focal_x_->setValue(0.5);
        if (spn_gradient_focal_y_) spn_gradient_focal_y_->setValue(0.5);
        spn_layer_w_->setValue(0.0);
        spn_layer_h_->setValue(0.0);
        if (spn_image_box_w_) spn_image_box_w_->setValue(0.0);
        if (spn_image_box_h_) spn_image_box_h_->setValue(0.0);
        if (spn_rect_corner_tl_) spn_rect_corner_tl_->setValue(0.0);
        if (spn_rect_corner_tr_) spn_rect_corner_tr_->setValue(0.0);
        if (spn_rect_corner_br_) spn_rect_corner_br_->setValue(0.0);
        if (spn_rect_corner_bl_) spn_rect_corner_bl_->setValue(0.0);
        if (spn_corner_bevel_roundness_) spn_corner_bevel_roundness_->setValue(100.0);
        spn_size_->setValue(72);
        if (cmb_font_style_) populate_font_style_combo(cmb_font_style_, cmb_font_->currentText(), obsgs_tr("OBSTitles.Regular"));
        chk_bold_->setChecked(false);
        chk_italic_->setChecked(false);
        if (chk_font_kerning_) chk_font_kerning_->setChecked(true);
        if (spn_text_leading_) spn_text_leading_->setValue(0.0);
        if (spn_char_tracking_) spn_char_tracking_->setValue(0.0);
        if (cmb_kerning_mode_) cmb_kerning_mode_->setCurrentIndex(0);
        if (spn_kerning_value_) spn_kerning_value_->setValue(0.0);
        if (spn_scale_x_) spn_scale_x_->setValue(100.0);
        if (spn_scale_y_) spn_scale_y_->setValue(100.0);
        if (spn_transform_size_w_) spn_transform_size_w_->setValue(0.0);
        if (spn_transform_size_h_) spn_transform_size_h_->setValue(0.0);
        if (chk_scale_lock_) chk_scale_lock_->setChecked(true);
        if (chk_transform_size_lock_) chk_transform_size_lock_->setChecked(true);
        if (chk_image_box_size_lock_) chk_image_box_size_lock_->setChecked(false);
        if (spn_char_scale_x_) spn_char_scale_x_->setValue(100.0);
        if (spn_char_scale_y_) spn_char_scale_y_->setValue(100.0);
        if (spn_baseline_shift_) spn_baseline_shift_->setValue(0.0);
        for (auto *b : {btn_all_caps_, btn_small_caps_, btn_superscript_, btn_subscript_, btn_underline_,
                        btn_strikethrough_, btn_ligatures_, btn_stylistic_alternates_, btn_fractions_, btn_opentype_features_})
            if (b) b->setChecked(false);
        if (cmb_text_style_) cmb_text_style_->setCurrentIndex(0);
        if (cmb_text_overflow_) cmb_text_overflow_->setCurrentIndex(0);
        if (spn_text_fit_min_scale_) spn_text_fit_min_scale_->setValue(0.5);
        if (lbl_text_fit_scale_) lbl_text_fit_scale_->setText(obsgs_tr("OBSTitles.Scale100"));
        if (chk_text_box_width_to_text_) chk_text_box_width_to_text_->setChecked(false);
        if (chk_text_box_height_to_text_) chk_text_box_height_to_text_->setChecked(false);
        if (spn_max_text_box_width_) { spn_max_text_box_width_->setValue(1920.0); spn_max_text_box_width_->setEnabled(false); }
        if (spn_max_text_box_height_) { spn_max_text_box_height_->setValue(1080.0); spn_max_text_box_height_->setEnabled(false); }
        if (grp_text_align_ && grp_text_align_->button(1)) grp_text_align_->button(1)->setChecked(true);
        if (grp_text_valign_ && grp_text_valign_->button(1)) grp_text_valign_->button(1)->setChecked(true);
        if (spn_paragraph_indent_left_) spn_paragraph_indent_left_->setValue(0.0);
        if (spn_paragraph_indent_right_) spn_paragraph_indent_right_->setValue(0.0);
        if (spn_paragraph_indent_first_line_) spn_paragraph_indent_first_line_->setValue(0.0);
        if (spn_paragraph_space_before_) spn_paragraph_space_before_->setValue(0.0);
        if (spn_paragraph_space_after_) spn_paragraph_space_after_->setValue(0.0);
        if (chk_paragraph_hyphenate_) chk_paragraph_hyphenate_->setChecked(false);
        if (cmb_anchor_) cmb_anchor_->setCurrentIndex(4);
        if (btn_anchor_grid_) { btn_anchor_grid_->setProperty("active_index", 4); btn_anchor_grid_->update(); }
        for (auto *b : {btn_kf_pos_x_, btn_kf_pos_y_, btn_kf_scale_x_, btn_kf_scale_y_,
                        btn_kf_rotation_, btn_kf_opacity_, btn_kf_origin_x_, btn_kf_origin_y_,
                        btn_kf_appearance_fill_, btn_kf_appearance_stroke_,
                        btn_kf_paragraph_indent_left_, btn_kf_paragraph_indent_right_, btn_kf_paragraph_indent_first_line_,
                        btn_kf_width_, btn_kf_transform_size_,
                        btn_kf_text_color_, btn_kf_fill_color_}) {
            if (!b) continue;
            b->setIcon(keyframe_diamond_icon(false));
            b->setProperty("active", false);
            b->setProperty("outlined", false);
            b->style()->unpolish(b);
            b->style()->polish(b);
        }
        loading_values_ = false;
        return;
    }

    const bool is_text = layer_->type == LayerType::Text;
    const bool is_clock = layer_->type == LayerType::Clock;
    const bool is_ticker = layer_->type == LayerType::Ticker;
    const bool is_text_like = is_text || is_clock || is_ticker;
    const bool is_rect = layer_->type == LayerType::SolidRect || layer_->type == LayerType::Shape;
    const bool is_image = layer_->type == LayerType::Image;
    const bool is_scene_mask_layer = layer_->use_as_scene_mask;
    const bool supports_outline = is_text_like || is_rect || is_image;
    if (transform_box_) transform_box_->setVisible(true);
    if (appearance_box_) appearance_box_->setVisible(true);
    if (btn_transform_defaults_) btn_transform_defaults_->setEnabled(true);
    text_box_->setVisible(is_text_like);
    if (type_options_box_) type_options_box_->setVisible(is_text_like);
    if (paragraph_box_) paragraph_box_->setVisible(is_text_like);
    if (dynamic_text_box_) dynamic_text_box_->setVisible(is_text_like);
    if (auto_style_box_) auto_style_box_->setVisible(is_text_like);
    if (live_edit_box_) live_edit_box_->setVisible(is_rect || is_text_like || is_image);
    if (bullets_box_) bullets_box_->setVisible(false);
    text_box_->setTitle("Character");
    if (row_text_color_) row_text_color_->setVisible(false);
    if (btn_kf_text_color_) btn_kf_text_color_->setVisible(false);
    if (auto *char_grid = qobject_cast<QGridLayout *>(text_box_->layout())) {
        const bool show_text_editor = !is_text;
        if (auto *label_item = char_grid->itemAtPosition(0, 0)) {
            if (auto *label = qobject_cast<QLabel *>(label_item->widget())) {
                label->setText(is_clock ? obsgs_tr("OBSTitles.DateTimeFormatLabel")
                                        : obsgs_tr("OBSTitles.TextLabel"));
                label->setVisible(show_text_editor);
            }
        }
        if (txt_content_) txt_content_->setVisible(show_text_editor);
        if (auto *label_item = char_grid->itemAtPosition(7, 0)) {
            if (auto *label = label_item->widget())
                label->setVisible(false);
        }
    }
    txt_content_->setPlaceholderText(is_clock ? "H:i:s" : obsgs_tr("OBSTitles.EnterTextPlaceholder"));
    if (cmb_auto_default_style_ && cmb_auto_rule_style_) {
        obsgsp::StylePresetLibrary library;
        const auto presets = library.presets(obsgsp::StylePresetKind::Text);
        auto fill_combo = [&](QComboBox *combo, bool include_none, const QString &selected) {
            QSignalBlocker blocker(combo);
            combo->clear();
            if (include_none) combo->addItem("None", QString());
            for (const auto &preset : presets)
                combo->addItem(preset.name + QStringLiteral(" — ") + preset.category, preset.id);
            const int idx = combo->findData(selected);
            if (idx >= 0) combo->setCurrentIndex(idx);
        };
        fill_combo(cmb_auto_default_style_, true, QString::fromStdString(layer_->rich_text.auto_default_style_preset_id));
        const QString first_rule = layer_->rich_text.auto_style_rules.empty()
            ? QString()
            : QString::fromStdString(layer_->rich_text.auto_style_rules.front().style_preset_id);
        fill_combo(cmb_auto_rule_style_, false, first_rule);
        if (chk_auto_style_enabled_) chk_auto_style_enabled_->setChecked(layer_->rich_text.auto_style_enabled);
        QSignalBlocker list_blocker(lst_auto_style_rules_);
        lst_auto_style_rules_->clear();
        int i = 1;
        for (const auto &rule : layer_->rich_text.auto_style_rules) {
            const QString preset_name = library.displayNameForId(QString::fromStdString(rule.style_preset_id));
            const QString display_name = rule.display_name.empty() ? preset_name : QString::fromStdString(rule.display_name);
            const QString rule_id = QString::fromStdString(rule.rule_id.empty() ? std::to_string(i) : rule.rule_id);
            const QString from = auto_style_marker_label(rule.start_condition, rule.start_offset, rule.start_custom_chars);
            const QString to = auto_style_marker_label(rule.end_condition, rule.end_offset ? rule.end_offset : rule.length, rule.end_custom_chars);
            auto *item = new QListWidgetItem(QStringLiteral("%1. %2  •  %3 → %4  •  %5%6")
                                             .arg(i++).arg(display_name).arg(from).arg(to)
                                             .arg(QString::fromStdString(rule.conflict_mode.empty() ? std::string("override_previous") : rule.conflict_mode))
                                             .arg(rule.enabled ? QString() : QStringLiteral("  [off]")),
                                             lst_auto_style_rules_);
            item->setData(Qt::UserRole, i - 2);
            item->setToolTip(QStringLiteral("Rule ID: %1\nPreset: %2\nRange: %3 → %4\nConflict mode: %5\n%6")
                             .arg(rule_id, preset_name, from, to,
                                  QString::fromStdString(rule.conflict_mode.empty() ? std::string("override_previous") : rule.conflict_mode),
                                  rule.enabled ? QStringLiteral("Enabled") : QStringLiteral("Disabled")));
        }
        if (!layer_->rich_text.auto_style_rules.empty()) {
            lst_auto_style_rules_->setCurrentRow(0);
            const auto &rule = layer_->rich_text.auto_style_rules.front();
            int sidx = cmb_auto_rule_start_condition_->findData(QString::fromStdString(rule.start_condition.empty() ? std::string("text_start") : rule.start_condition));
            int eidx = cmb_auto_rule_end_condition_->findData(QString::fromStdString(rule.end_condition.empty() ? std::string("character_index") : rule.end_condition));
            if (chk_auto_rule_enabled_) chk_auto_rule_enabled_->setChecked(rule.enabled);
            if (edt_auto_rule_name_) edt_auto_rule_name_->setText(QString::fromStdString(rule.display_name));
            if (sidx >= 0) cmb_auto_rule_start_condition_->setCurrentIndex(sidx);
            if (eidx >= 0) cmb_auto_rule_end_condition_->setCurrentIndex(eidx);
            spn_auto_rule_start_offset_->setValue((int)rule.start_offset);
            spn_auto_rule_end_offset_->setValue((int)(rule.end_offset ? rule.end_offset : rule.length));
            edt_auto_rule_start_chars_->setText(QString::fromStdString(rule.start_custom_chars));
            edt_auto_rule_end_chars_->setText(QString::fromStdString(rule.end_custom_chars));
            if (cmb_auto_rule_conflict_mode_) { int cidx = cmb_auto_rule_conflict_mode_->findData(QString::fromStdString(rule.conflict_mode.empty() ? std::string("override_previous") : rule.conflict_mode)); if (cidx >= 0) cmb_auto_rule_conflict_mode_->setCurrentIndex(cidx); }
            if (cmb_auto_rule_match_mode_) { int midx = cmb_auto_rule_match_mode_->findData(QString::fromStdString(rule.match_mode.empty() ? std::string("all_matches") : rule.match_mode)); if (midx >= 0) cmb_auto_rule_match_mode_->setCurrentIndex(midx); }
            if (edt_auto_rule_excludes_) edt_auto_rule_excludes_->setText(auto_style_join_ids(rule.excludes_rule_ids));
            if (chk_auto_rule_stop_processing_) chk_auto_rule_stop_processing_->setChecked(rule.stop_processing);
            if (chk_auto_rule_require_stop_match_) chk_auto_rule_require_stop_match_->setChecked(rule.require_stop_match);
            if (chk_auto_rule_include_start_marker_) chk_auto_rule_include_start_marker_->setChecked(rule.include_start_marker);
            if (chk_auto_rule_include_end_marker_) chk_auto_rule_include_end_marker_->setChecked(rule.include_end_marker);
            if (spn_auto_rule_chars_) spn_auto_rule_chars_->setValue((int)(rule.end_offset ? rule.end_offset : rule.length));
        } else {
            if (chk_auto_rule_enabled_) chk_auto_rule_enabled_->setChecked(true);
            if (edt_auto_rule_name_) edt_auto_rule_name_->clear();
            if (cmb_auto_rule_start_condition_) cmb_auto_rule_start_condition_->setCurrentIndex(0);
            if (cmb_auto_rule_end_condition_) cmb_auto_rule_end_condition_->setCurrentIndex(cmb_auto_rule_end_condition_->findData("character_index"));
            if (spn_auto_rule_start_offset_) spn_auto_rule_start_offset_->setValue(0);
            if (spn_auto_rule_end_offset_) spn_auto_rule_end_offset_->setValue(0);
            if (edt_auto_rule_start_chars_) edt_auto_rule_start_chars_->clear();
            if (edt_auto_rule_end_chars_) edt_auto_rule_end_chars_->clear();
            if (cmb_auto_rule_conflict_mode_) cmb_auto_rule_conflict_mode_->setCurrentIndex(0);
            if (cmb_auto_rule_match_mode_) cmb_auto_rule_match_mode_->setCurrentIndex(0);
            if (edt_auto_rule_excludes_) edt_auto_rule_excludes_->clear();
            if (chk_auto_rule_stop_processing_) chk_auto_rule_stop_processing_->setChecked(false);
            if (chk_auto_rule_require_stop_match_) chk_auto_rule_require_stop_match_->setChecked(true);
            if (chk_auto_rule_include_start_marker_) chk_auto_rule_include_start_marker_->setChecked(true);
            if (chk_auto_rule_include_end_marker_) chk_auto_rule_include_end_marker_->setChecked(false);
            if (spn_auto_rule_chars_) spn_auto_rule_chars_->setValue(0);
        }
    }
    if (spn_text_fit_min_scale_) spn_text_fit_min_scale_->setVisible(is_text_like && layer_->text_overflow_mode == 2 && !is_ticker);
    if (lbl_text_fit_scale_) lbl_text_fit_scale_->setVisible(is_text_like && layer_->text_overflow_mode == 2 && !is_ticker);
    if (auto *dynamic_form = qobject_cast<QFormLayout *>(dynamic_text_box_ ? dynamic_text_box_->layout() : nullptr)) {
        const bool show_ticker_fit = is_text_like && layer_->text_overflow_mode == 2 && !is_ticker;
        if (cmb_text_style_) {
            cmb_text_style_->setVisible(false);
            if (auto *label = dynamic_form->labelForField(cmb_text_style_)) label->setVisible(false);
        }
        if (auto *label = dynamic_form->labelForField(spn_text_fit_min_scale_))
            label->setVisible(show_ticker_fit);
        if (cmb_ticker_style_) {
            cmb_ticker_style_->setVisible(is_ticker);
            if (auto *label = dynamic_form->labelForField(cmb_ticker_style_)) label->setVisible(is_ticker);
        }
        if (spn_ticker_speed_) {
            spn_ticker_speed_->setVisible(is_ticker && layer_->ticker_style != 1);
            if (auto *label = dynamic_form->labelForField(spn_ticker_speed_)) label->setVisible(is_ticker && layer_->ticker_style != 1);
        }
        if (spn_ticker_line_hold_) {
            spn_ticker_line_hold_->setVisible(is_ticker && layer_->ticker_style == 1);
            if (auto *label = dynamic_form->labelForField(spn_ticker_line_hold_)) label->setVisible(is_ticker && layer_->ticker_style == 1);
        }
        if (cmb_ticker_direction_) {
            cmb_ticker_direction_->setVisible(is_ticker);
            if (auto *label = dynamic_form->labelForField(cmb_ticker_direction_)) label->setVisible(is_ticker);
        }
    }
    if (auto *live_form = qobject_cast<QFormLayout *>(live_edit_box_ ? live_edit_box_->layout() : nullptr)) {
        const bool show_scene_mask = is_rect || is_text_like;
        const bool show_expose_to_dock = is_text || is_ticker || is_image;
        const bool show_exposed_options = show_expose_to_dock && layer_->expose_text;
        const bool show_ignore_persistence = true;
        if (chk_scene_mask_) {
            chk_scene_mask_->setVisible(show_scene_mask);
            if (auto *label = live_form->labelForField(chk_scene_mask_))
                label->setVisible(show_scene_mask);
        }
        if (chk_expose_text_) {
            chk_expose_text_->setVisible(show_expose_to_dock);
            if (auto *label = live_form->labelForField(chk_expose_text_))
                label->setVisible(show_expose_to_dock);
        }
        if (chk_exposed_hide_if_empty_) {
            chk_exposed_hide_if_empty_->setVisible(show_exposed_options);
            chk_exposed_hide_if_empty_->setEnabled(show_exposed_options);
            if (auto *label = live_form->labelForField(chk_exposed_hide_if_empty_))
                label->setVisible(show_exposed_options);
        }
        if (chk_exposed_single_value_) {
            chk_exposed_single_value_->setVisible(show_exposed_options);
            chk_exposed_single_value_->setEnabled(show_exposed_options);
            if (auto *label = live_form->labelForField(chk_exposed_single_value_))
                label->setVisible(show_exposed_options);
        }
        if (chk_ignore_persistence_) {
            chk_ignore_persistence_->setVisible(show_ignore_persistence);
            if (auto *label = live_form->labelForField(chk_ignore_persistence_))
                label->setVisible(show_ignore_persistence);
        }
        if (live_edit_box_)
            live_edit_box_->setVisible(show_scene_mask || show_expose_to_dock || show_ignore_persistence);
    }
    rect_box_->setVisible(is_text_like || is_rect || is_image);
    rect_box_->setTitle(QString());
    if (auto *shape_title = rect_box_->findChild<QLabel *>(QStringLiteral("OBSTitlesShapePanelTitle"))) {
        shape_title->setText(is_rect ? obsgs_tr("OBSTitles.Shape")
                            : is_image ? obsgs_tr("OBSTitles.ImageSize")
                                       : (is_clock ? obsgs_tr("OBSTitles.ClockBox")
                                                   : (is_ticker ? obsgs_tr("OBSTitles.TickerBox")
                                                                : obsgs_tr("OBSTitles.TextBox"))));
    }
    const bool is_shape_layer = layer_->type == LayerType::Shape || layer_->type == LayerType::SolidRect;
    const ShapeType current_shape = layer_->type == LayerType::SolidRect ? ShapeType::RoundedRectangle : layer_->shape_type;
    const bool show_corner_radius = is_image ||
                                    (is_shape_layer &&
                                     (current_shape == ShapeType::Rectangle ||
                                      current_shape == ShapeType::RoundedRectangle));
    const bool show_star_controls = is_shape_layer && current_shape == ShapeType::Star;
    const bool show_polygon_controls = is_shape_layer && current_shape == ShapeType::Polygon;
    const bool show_roundness = is_shape_layer &&
                                current_shape != ShapeType::Ellipse &&
                                current_shape != ShapeType::Line &&
                                current_shape != ShapeType::Rectangle &&
                                current_shape != ShapeType::RoundedRectangle;
    const bool show_transform_scale = !is_shape_layer;
    const bool show_transform_size = is_shape_layer;
    const bool show_shape_panel_size = !is_shape_layer && (is_text_like || is_image);
    auto set_visible = [](QWidget *widget, bool visible) {
        if (widget) widget->setVisible(visible);
    };
    set_visible(btn_kf_scale_x_, show_transform_scale);
    set_visible(transform_scale_label_, show_transform_scale);
    set_visible(transform_scale_field_x_, show_transform_scale);
    set_visible(chk_scale_lock_, show_transform_scale);
    set_visible(transform_scale_field_y_, show_transform_scale);
    set_visible(btn_kf_transform_size_, show_transform_size);
    set_visible(transform_size_label_, show_transform_size);
    set_visible(transform_size_field_w_, show_transform_size);
    set_visible(chk_transform_size_lock_, show_transform_size);
    set_visible(transform_size_field_h_, show_transform_size);
    set_visible(row_shape_scale_options_, is_shape_layer || is_image);
    set_visible(chk_shape_scale_stroke_, is_shape_layer);
    set_visible(chk_shape_scale_corners_, is_shape_layer || is_image);
    set_visible(btn_kf_width_, show_shape_panel_size);
    set_visible(shape_size_label_, show_shape_panel_size);
    set_visible(shape_size_field_w_, show_shape_panel_size);
    set_visible(shape_size_field_h_, show_shape_panel_size);
    set_visible(chk_size_lock_, show_shape_panel_size && is_image);
    if (row_rect_corners_) row_rect_corners_->setVisible(show_corner_radius);
    if (chk_corner_lock_) chk_corner_lock_->setVisible(show_corner_radius);
    if (spn_corner_bevel_roundness_) spn_corner_bevel_roundness_->setVisible(show_corner_radius);
    if (grp_shape_type_) {
        for (auto *button : grp_shape_type_->buttons())
            button->setVisible(is_shape_layer);
    }
    if (auto *shape_types_row = rect_box_->findChild<QWidget *>(QStringLiteral("OBSTitlesShapeTypeButtonsRow")))
        shape_types_row->setVisible(is_shape_layer);
    if (btn_shape_defaults_) {
        btn_shape_defaults_->setVisible(is_shape_layer || is_image);
        btn_shape_defaults_->setToolTip(is_image ? obsgs_tr("OBSTitles.RestoreImageSizeDefaults")
                                                    : obsgs_tr("OBSTitles.RestoreShapeDefaults"));
    }
    if (spn_shape_points_) spn_shape_points_->setVisible(show_star_controls);
    if (spn_shape_sides_) spn_shape_sides_->setVisible(show_polygon_controls);
    if (spn_shape_inner_radius_) spn_shape_inner_radius_->setVisible(show_star_controls);
    if (spn_shape_outer_radius_) spn_shape_outer_radius_->setVisible(show_star_controls);
    if (spn_shape_roundness_) spn_shape_roundness_->setVisible(show_roundness);
    const bool supports_fill_type = is_rect || is_text_like;
    const bool solid_fill_active = is_rect && layer_->fill_type == 0;
    if (cmb_fill_type_) cmb_fill_type_->setVisible(false);
    if (btn_fill_color_) btn_fill_color_->setVisible(false);
    if (gradient_box_) gradient_box_->setVisible(false);
    const bool supports_text_box_auto_size = is_text || is_clock;
    if (chk_text_box_width_to_text_) chk_text_box_width_to_text_->setVisible(supports_text_box_auto_size);
    if (chk_text_box_height_to_text_) chk_text_box_height_to_text_->setVisible(supports_text_box_auto_size);
    if (spn_max_text_box_width_) spn_max_text_box_width_->setVisible(supports_text_box_auto_size);
    if (spn_max_text_box_height_) spn_max_text_box_height_->setVisible(supports_text_box_auto_size);
    btn_kf_text_color_->setVisible(false);
    const bool gradient_text_active = is_text_like && layer_->fill_type == 1;
    if (btn_text_color_) btn_text_color_->setEnabled(!gradient_text_active);
    if (btn_kf_text_color_) btn_kf_text_color_->setEnabled(!gradient_text_active);
    btn_kf_fill_color_->setVisible(false);
    if (row_fill_type_) row_fill_type_->setVisible(false);
    if (row_fill_color_) row_fill_color_->setVisible(false);
    const bool fill_controls_enabled = !is_scene_mask_layer;
    if (cmb_fill_type_) cmb_fill_type_->setEnabled(fill_controls_enabled);
    if (row_fill_type_) row_fill_type_->setEnabled(fill_controls_enabled);
    if (btn_fill_color_) btn_fill_color_->setEnabled(fill_controls_enabled);
    if (row_fill_color_) row_fill_color_->setEnabled(fill_controls_enabled);
    if (gradient_box_) gradient_box_->setEnabled(fill_controls_enabled);
    if (btn_kf_fill_color_) btn_kf_fill_color_->setEnabled(fill_controls_enabled);
    if (outline_box_) outline_box_->setVisible(false);
    if (background_gradient_box_) background_gradient_box_->setVisible(false);
    if (auto *form = rect_box_->findChild<QFormLayout *>()) {
        if (auto *label = form->labelForField(row_rect_corners_)) label->setVisible(show_corner_radius);
        if (auto *label = form->labelForField(spn_corner_bevel_roundness_)) label->setVisible(show_corner_radius);
        if (auto *label = form->labelForField(spn_shape_points_)) label->setVisible(show_star_controls);
        if (auto *label = form->labelForField(spn_shape_sides_)) label->setVisible(show_polygon_controls);
        if (auto *label = form->labelForField(spn_shape_inner_radius_)) label->setVisible(show_star_controls);
        if (auto *label = form->labelForField(spn_shape_outer_radius_)) label->setVisible(show_star_controls);
        if (auto *label = form->labelForField(spn_shape_roundness_)) label->setVisible(show_roundness);
        if (auto *label = form->labelForField(row_fill_type_))
            label->setVisible(false);
        if (auto *label = form->labelForField(row_fill_color_))
            label->setVisible(false);
        for (QWidget *field : std::initializer_list<QWidget *>{chk_text_box_width_to_text_, spn_max_text_box_width_,
                                                               chk_text_box_height_to_text_, spn_max_text_box_height_})
            if (auto *label = form->labelForField(field)) label->setVisible(supports_text_box_auto_size);
    }
    image_box_->setVisible(is_image);
    if (image_box_size_box_) image_box_size_box_->setVisible(is_image);
    if (shadow_box_) shadow_box_->setVisible(false);

    double lt = std::clamp(playhead_ - layer_->in_time, 0.0,
                           std::max(0.0, layer_->out_time - layer_->in_time));
    spn_px_->setValue(layer_->position.is_animated()
                      ? layer_->position.evaluate(lt).x
                      : layer_->position.static_value.x);
    spn_py_->setValue(layer_->position.is_animated()
                      ? layer_->position.evaluate(lt).y
                      : layer_->position.static_value.y);
    spn_scale_x_->setValue((layer_->scale.is_animated()
                            ? layer_->scale.evaluate(lt).x
                            : layer_->scale.static_value.x) * 100.0);
    spn_scale_y_->setValue((layer_->scale.is_animated()
                            ? layer_->scale.evaluate(lt).y
                            : layer_->scale.static_value.y) * 100.0);
    if (chk_scale_lock_) chk_scale_lock_->setChecked(layer_->scale_lock);
    if (spn_transform_size_w_) spn_transform_size_w_->setValue(eval_box_width(*layer_, lt));
    if (spn_transform_size_h_) spn_transform_size_h_->setValue(eval_box_height(*layer_, lt));
    if (chk_transform_size_lock_) chk_transform_size_lock_->setChecked(layer_->lock_aspect_ratio);
    if (chk_shape_scale_stroke_) chk_shape_scale_stroke_->setChecked(layer_->scale_stroke_with_shape);
    if (chk_shape_scale_corners_) chk_shape_scale_corners_->setChecked(layer_->scale_corners_with_shape);
    spn_rot_->setValue(layer_->rotation.is_animated()
                       ? layer_->rotation.evaluate(lt)
                       : layer_->rotation.static_value);
    spn_opacity_->setValue(layer_->opacity.is_animated()
                           ? layer_->opacity.evaluate(lt)
                           : layer_->opacity.static_value);
    if (spn_appearance_opacity_) spn_appearance_opacity_->setValue(spn_opacity_->value() * 100.0);
    const bool supports_fill_appearance = is_text_like || is_rect;
    if (btn_appearance_fill_color_) {
        if (layer_->fill_type == 1)
            style_gradient_button(btn_appearance_fill_color_,
                                  layer_->gradient_start_color,
                                  layer_->gradient_end_color,
                                  layer_->gradient_type);
        else
            style_color_button(btn_appearance_fill_color_,
                               is_text_like ? eval_text_color(*layer_, lt) : eval_fill_color(*layer_, lt));
        btn_appearance_fill_color_->setText(QString());
        btn_appearance_fill_color_->setEnabled(supports_fill_appearance && fill_controls_enabled);
    }
    if (btn_kf_appearance_fill_) btn_kf_appearance_fill_->setEnabled(supports_fill_appearance && fill_controls_enabled);
    if (btn_appearance_stroke_color_) {
        if (layer_->stroke_fill_type == 2)
            style_gradient_button(btn_appearance_stroke_color_,
                                  layer_->stroke_gradient_start_color,
                                  layer_->stroke_gradient_end_color,
                                  layer_->stroke_gradient_type);
        else
            style_color_button(btn_appearance_stroke_color_, layer_->stroke_color);
        btn_appearance_stroke_color_->setText(QString());
        btn_appearance_stroke_color_->setEnabled(supports_outline);
        btn_appearance_stroke_color_->setVisible(supports_outline);
    }
    if (btn_appearance_stroke_label_) btn_appearance_stroke_label_->setVisible(supports_outline);
    if (spn_appearance_stroke_width_) {
        spn_appearance_stroke_width_->setValue(layer_->stroke_width);
        spn_appearance_stroke_width_->setEnabled(supports_outline);
        spn_appearance_stroke_width_->setVisible(supports_outline);
    }
    if (btn_kf_appearance_stroke_) {
        btn_kf_appearance_stroke_->setEnabled(false);
        btn_kf_appearance_stroke_->setVisible(supports_outline);
    }
    spn_origin_x_->setValue(eval_origin_x(*layer_, lt));
    spn_origin_y_->setValue(eval_origin_y(*layer_, lt));
    const int anchor_index = anchor_index_from_layer(*layer_);
    cmb_anchor_->setCurrentIndex(anchor_index);
    if (btn_anchor_grid_) { btn_anchor_grid_->setProperty("active_index", anchor_index); btn_anchor_grid_->update(); }

    if (is_image) {
        double raw_w = eval_image_width(*layer_, lt);
        double raw_h = eval_image_height(*layer_, lt);
        if (raw_w <= 0.0 || raw_h <= 0.0) {
            const QSize intrinsic = editor_image_intrinsic_size(QString::fromStdString(layer_->image_path));
            if (intrinsic.isValid() && !intrinsic.isEmpty()) {
                raw_w = intrinsic.width();
                raw_h = intrinsic.height();
            }
        }
        const gsp::ImageDisplaySize display = gsp::calculate_image_display_size(
            layer_->image_box_mode, layer_->image_size_auto_fit,
            eval_box_width(*layer_, lt), eval_box_height(*layer_, lt), raw_w, raw_h);
        spn_layer_w_->setValue(display.width);
        spn_layer_h_->setValue(display.height);
    } else {
        spn_layer_w_->setValue(eval_box_width(*layer_, lt));
        spn_layer_h_->setValue(eval_box_height(*layer_, lt));
    }
    if (spn_image_box_w_) spn_image_box_w_->setValue(eval_box_width(*layer_, lt));
    if (spn_image_box_h_) spn_image_box_h_->setValue(eval_box_height(*layer_, lt));
    if (chk_size_lock_) chk_size_lock_->setChecked(layer_->lock_aspect_ratio);
    if (chk_image_box_size_lock_) chk_image_box_size_lock_->setChecked(layer_->image_box_lock_aspect_ratio);
    if (chk_corner_lock_) chk_corner_lock_->setChecked(layer_->corner_radius_locked);
    if (spn_rect_corner_tl_) spn_rect_corner_tl_->setValue(layer_->corner_radius_tl);
    if (spn_rect_corner_tr_) spn_rect_corner_tr_->setValue(layer_->corner_radius_tr);
    if (spn_rect_corner_br_) spn_rect_corner_br_->setValue(layer_->corner_radius_br);
    if (spn_rect_corner_bl_) spn_rect_corner_bl_->setValue(layer_->corner_radius_bl);
    if (spn_corner_bevel_roundness_) spn_corner_bevel_roundness_->setValue(layer_->corner_bevel_roundness);
    if (cmb_shape_type_) {
        int shape_idx = cmb_shape_type_->findData((int)current_shape);
        cmb_shape_type_->setCurrentIndex(shape_idx >= 0 ? shape_idx : 0);
    }
    if (grp_shape_type_) {
        if (auto *button = grp_shape_type_->button((int)current_shape))
            button->setChecked(true);
    }
    if (spn_shape_points_) spn_shape_points_->setValue(layer_->shape_points);
    if (spn_shape_sides_) spn_shape_sides_->setValue(layer_->shape_sides);
    if (spn_shape_inner_radius_) spn_shape_inner_radius_->setValue(layer_->shape_inner_radius);
    if (spn_shape_outer_radius_) spn_shape_outer_radius_->setValue(layer_->shape_outer_radius);
    if (spn_shape_roundness_) {
        const bool rectangle_roundness = current_shape == ShapeType::Rectangle ||
                                         current_shape == ShapeType::RoundedRectangle;
        double roundness = rectangle_roundness ? layer_->corner_radius : layer_->shape_roundness;
        if (current_shape == ShapeType::Path && !layer_->path_points.empty())
            roundness = layer_->path_points.front().corner_radius;
        spn_shape_roundness_->setValue(roundness);
    }
    edit_image_path_->setText(QString::fromStdString(layer_->image_path));
    set_image_preview_label(lbl_image_preview_, QString::fromStdString(layer_->image_path));
    if (cmb_image_scale_filter_) {
        QSignalBlocker block(cmb_image_scale_filter_);
        int filter_index = cmb_image_scale_filter_->findData((int)layer_->scale_filter);
        cmb_image_scale_filter_->setCurrentIndex(filter_index >= 0 ? filter_index : 1);
    }
    if (cmb_image_box_mode_) {
        QSignalBlocker block(cmb_image_box_mode_);
        int mode_index = cmb_image_box_mode_->findData((int)layer_->image_box_mode);
        cmb_image_box_mode_->setCurrentIndex(mode_index >= 0 ? mode_index : 0);
    }
    if (chk_image_crop_when_outside_box_) {
        QSignalBlocker block(chk_image_crop_when_outside_box_);
        chk_image_crop_when_outside_box_->setChecked(layer_->image_crop_when_outside_box);
    }
    const bool stretch_image_size = is_image && layer_->image_size_auto_fit &&
                                    layer_->image_box_mode == ImageBoxMode::StretchToFill;
    if (spn_layer_w_) spn_layer_w_->setEnabled(!stretch_image_size);
    if (spn_layer_h_) spn_layer_h_->setEnabled(!stretch_image_size);
    if (chk_size_lock_) chk_size_lock_->setEnabled(!stretch_image_size);
    if (btn_kf_width_) btn_kf_width_->setEnabled(!stretch_image_size);
    if (btn_image_anchor_grid_) {
        int image_anchor_x = layer_->image_anchor_x < 0.25f ? 0 : (layer_->image_anchor_x > 0.75f ? 2 : 1);
        int image_anchor_y = layer_->image_anchor_y < 0.25f ? 0 : (layer_->image_anchor_y > 0.75f ? 2 : 1);
        btn_image_anchor_grid_->setProperty("active_index", image_anchor_y * 3 + image_anchor_x);
        btn_image_anchor_grid_->update();
    }
    style_color_button(btn_text_color_, eval_text_color(*layer_, lt));
    style_color_button(btn_fill_color_, eval_fill_color(*layer_, lt));
    if (cmb_fill_type_) {
        int fill_idx = cmb_fill_type_->findData(layer_->fill_type);
        cmb_fill_type_->setCurrentIndex(fill_idx >= 0 ? fill_idx : 0);
    }
    if (cmb_gradient_type_) {
        int gradient_idx = cmb_gradient_type_->findData(layer_->gradient_type);
        cmb_gradient_type_->setCurrentIndex(gradient_idx >= 0 ? gradient_idx : 0);
    }
    if (cmb_gradient_spread_) {
        int spread_idx = cmb_gradient_spread_->findData(layer_->gradient_spread);
        cmb_gradient_spread_->setCurrentIndex(spread_idx >= 0 ? spread_idx : 0);
    }
    if (btn_gradient_start_color_) style_color_button(btn_gradient_start_color_, layer_->gradient_start_color);
    if (btn_gradient_end_color_) style_color_button(btn_gradient_end_color_, layer_->gradient_end_color);
    if (spn_gradient_start_pos_) spn_gradient_start_pos_->setValue(layer_->gradient_start_pos);
    if (spn_gradient_end_pos_) spn_gradient_end_pos_->setValue(layer_->gradient_end_pos);
    if (spn_gradient_start_opacity_) spn_gradient_start_opacity_->setValue(layer_->gradient_start_opacity);
    if (spn_gradient_end_opacity_) spn_gradient_end_opacity_->setValue(layer_->gradient_end_opacity);
    if (spn_gradient_opacity_) spn_gradient_opacity_->setValue(layer_->gradient_opacity);
    if (spn_gradient_angle_) spn_gradient_angle_->setValue(layer_->gradient_angle);
    if (spn_gradient_center_x_) spn_gradient_center_x_->setValue(layer_->gradient_center_x);
    if (spn_gradient_center_y_) spn_gradient_center_y_->setValue(layer_->gradient_center_y);
    if (spn_gradient_scale_) spn_gradient_scale_->setValue(layer_->gradient_scale);
    if (spn_gradient_focal_x_) spn_gradient_focal_x_->setValue(layer_->gradient_focal_x);
    if (spn_gradient_focal_y_) spn_gradient_focal_y_->setValue(layer_->gradient_focal_y);
    if (chk_text_box_width_to_text_) chk_text_box_width_to_text_->setChecked(layer_->text_box_width_to_text);
    if (chk_text_box_height_to_text_) chk_text_box_height_to_text_->setChecked(layer_->text_box_height_to_text);
    if (spn_max_text_box_width_) { spn_max_text_box_width_->setValue(layer_->max_text_box_width); spn_max_text_box_width_->setEnabled(true); }
    if (spn_max_text_box_height_) { spn_max_text_box_height_->setValue(layer_->max_text_box_height); spn_max_text_box_height_->setEnabled(true); }

    auto set_kf_icon = [](QPushButton *button, bool active, bool has_keyframes) {
        if (!button) return;
        const bool outlined = has_keyframes && !active;
        button->setIcon(keyframe_diamond_icon(active, outlined));
        button->setProperty("active", active);
        button->setProperty("outlined", outlined);
        button->style()->unpolish(button);
        button->style()->polish(button);
    };
    auto set_prop_kf_icon = [&](QPushButton *button, const AnimatedProperty &prop) {
        set_kf_icon(button, keyframe_at_time(prop, lt), prop.is_animated());
    };
    auto set_group_kf_icon = [&](QPushButton *button, std::initializer_list<const AnimatedProperty *> props) {
        set_kf_icon(button, any_keyframe_at_time(props, lt), any_keyframes(props));
    };
    auto set_vec_kf_icon = [&](QPushButton *button, const AnimatedVec2Property &prop) {
        set_kf_icon(button, keyframe_at_time(prop, lt), prop.is_animated());
    };
    set_vec_kf_icon(btn_kf_pos_x_, layer_->position);
    set_vec_kf_icon(btn_kf_pos_y_, layer_->position);
    set_vec_kf_icon(btn_kf_scale_x_, layer_->scale);
    set_vec_kf_icon(btn_kf_scale_y_, layer_->scale);
    set_vec_kf_icon(btn_kf_transform_size_, layer_->size);
    set_prop_kf_icon(btn_kf_rotation_, layer_->rotation);
    set_prop_kf_icon(btn_kf_opacity_, layer_->opacity);
    set_vec_kf_icon(btn_kf_origin_x_, layer_->origin_prop);
    set_vec_kf_icon(btn_kf_origin_y_, layer_->origin_prop);
    set_prop_kf_icon(btn_kf_paragraph_indent_left_, layer_->paragraph_indent_left_prop);
    set_prop_kf_icon(btn_kf_paragraph_indent_right_, layer_->paragraph_indent_right_prop);
    set_prop_kf_icon(btn_kf_paragraph_indent_first_line_, layer_->paragraph_indent_first_line_prop);
    set_prop_kf_icon(btn_kf_font_size_, layer_->font_size_prop);
    set_prop_kf_icon(btn_kf_char_scale_x_, layer_->char_scale_x_prop);
    set_prop_kf_icon(btn_kf_char_scale_y_, layer_->char_scale_y_prop);
    set_prop_kf_icon(btn_kf_char_tracking_, layer_->char_tracking_prop);
    set_prop_kf_icon(btn_kf_baseline_shift_, layer_->baseline_shift_prop);
    set_prop_kf_icon(btn_kf_paragraph_space_before_, layer_->paragraph_space_before_prop);
    set_prop_kf_icon(btn_kf_paragraph_space_after_, layer_->paragraph_space_after_prop);
    set_vec_kf_icon(btn_kf_width_, is_image ? layer_->image_size : layer_->size);
    set_vec_kf_icon(btn_kf_image_box_size_, layer_->size);
    set_group_kf_icon(btn_kf_text_color_, {&layer_->text_color_a, &layer_->text_color_r,
                                           &layer_->text_color_g, &layer_->text_color_b});
    set_group_kf_icon(btn_kf_fill_color_, {&layer_->fill_color_a, &layer_->fill_color_r,
                                           &layer_->fill_color_g, &layer_->fill_color_b});
    if (is_text_like)
        set_group_kf_icon(btn_kf_appearance_fill_, {&layer_->text_color_a, &layer_->text_color_r,
                                                    &layer_->text_color_g, &layer_->text_color_b});
    else
        set_group_kf_icon(btn_kf_appearance_fill_, {&layer_->fill_color_a, &layer_->fill_color_r,
                                                    &layer_->fill_color_g, &layer_->fill_color_b});
    set_kf_icon(btn_kf_appearance_stroke_, false, false);

    const QString panel_text = is_clock
        ? QString::fromStdString(layer_->clock_format)
        : QString::fromStdString((rich_text_document_ensure_canonical(*layer_), layer_->rich_text.plain_text));
    txt_content_->setPlainText(panel_text);
    int ticker_style_idx = cmb_ticker_style_->findData(layer_->ticker_style);
    cmb_ticker_style_->setCurrentIndex(ticker_style_idx >= 0 ? ticker_style_idx : 0);
    spn_ticker_speed_->setValue(layer_->ticker_speed);
    spn_ticker_line_hold_->setValue(layer_->ticker_line_hold);
    cmb_ticker_direction_->clear();
    if (layer_->ticker_style == 0) {
        cmb_ticker_direction_->addItem(obsgs_tr("OBSTitles.LeftToRight"), 0);
        cmb_ticker_direction_->addItem(obsgs_tr("OBSTitles.RightToLeft"), 1);
    } else {
        cmb_ticker_direction_->addItem(obsgs_tr("OBSTitles.TopToBottom"), 0);
        cmb_ticker_direction_->addItem(obsgs_tr("OBSTitles.BottomToTop"), 1);
    }
    int ticker_direction_idx = cmb_ticker_direction_->findData(layer_->ticker_direction);
    cmb_ticker_direction_->setCurrentIndex(ticker_direction_idx >= 0 ? ticker_direction_idx : 0);
    int fi = cmb_font_->findText(QString::fromStdString(layer_->font_family));
    if (fi >= 0) cmb_font_->setCurrentIndex(fi);
    populate_font_style_combo(cmb_font_style_, QString::fromStdString(layer_->font_family), QString::fromStdString(layer_->font_style));
    spn_size_->setValue((int)std::clamp(std::round(layer_->font_size_prop.is_animated() ? layer_->font_size_prop.evaluate(lt) : (double)layer_->font_size), 1.0, 512.0));
    chk_bold_->setChecked(layer_->font_bold);
    chk_italic_->setChecked(layer_->font_italic);
    if (chk_font_kerning_) chk_font_kerning_->setChecked(layer_->font_kerning);
    if (cmb_kerning_mode_) {
        int ki = cmb_kerning_mode_->findData(layer_->kerning_mode);
        cmb_kerning_mode_->setCurrentIndex(ki >= 0 ? ki : 0);
    }
    if (spn_kerning_value_) {
        spn_kerning_value_->setValue(layer_->manual_kerning);
        spn_kerning_value_->setEnabled(layer_->kerning_mode == 2);
    }
    if (spn_text_leading_) spn_text_leading_->setValue(layer_->text_leading);
    if (spn_char_tracking_) spn_char_tracking_->setValue(layer_->char_tracking_prop.is_animated() ? layer_->char_tracking_prop.evaluate(lt) : (double)layer_->char_tracking);
    if (spn_char_scale_x_) spn_char_scale_x_->setValue((layer_->char_scale_x_prop.is_animated() ? layer_->char_scale_x_prop.evaluate(lt) : (double)layer_->char_scale_x) * 100.0);
    if (spn_char_scale_y_) spn_char_scale_y_->setValue((layer_->char_scale_y_prop.is_animated() ? layer_->char_scale_y_prop.evaluate(lt) : (double)layer_->char_scale_y) * 100.0);
    if (spn_baseline_shift_) spn_baseline_shift_->setValue(layer_->baseline_shift_prop.is_animated() ? layer_->baseline_shift_prop.evaluate(lt) : (double)layer_->baseline_shift);
    if (btn_all_caps_) btn_all_caps_->setChecked(layer_->text_style == 1);
    if (btn_small_caps_) btn_small_caps_->setChecked(layer_->text_style == 2);
    if (btn_superscript_) btn_superscript_->setChecked(layer_->text_style == 3);
    if (btn_subscript_) btn_subscript_->setChecked(layer_->text_style == 4);
    if (btn_underline_) btn_underline_->setChecked(layer_->text_underline);
    const bool use_rich_char_summary = is_text_like;
    if (use_rich_char_summary) {
        const bool active = active_text_edit_layer_id_ == layer_->id;
        RichTextCharFormatSummary summary = summarize_rich_text_char_format(*layer_, active);
        const RichTextCharFormat &fmt = summary.format;
        int rich_fi = cmb_font_->findText(QString::fromStdString(fmt.font_family));
        if (rich_fi >= 0) cmb_font_->setCurrentIndex(rich_fi);
        populate_font_style_combo(cmb_font_style_, QString::fromStdString(fmt.font_family), QString::fromStdString(fmt.font_style));
        int rich_style_i = cmb_font_style_->findText(QString::fromStdString(fmt.font_style));
        if (rich_style_i >= 0) cmb_font_style_->setCurrentIndex(rich_style_i);
        spn_size_->setValue(fmt.font_size);
        chk_bold_->setChecked(fmt.bold);
        chk_italic_->setChecked(fmt.italic);
        if (chk_font_kerning_) chk_font_kerning_->setChecked(fmt.kerning);
        if (cmb_kerning_mode_) {
            int rich_kerning_i = cmb_kerning_mode_->findData(fmt.kerning_mode);
            cmb_kerning_mode_->setCurrentIndex(rich_kerning_i >= 0 ? rich_kerning_i : 0);
        }
        if (spn_kerning_value_) {
            spn_kerning_value_->setValue(fmt.manual_kerning);
            spn_kerning_value_->setEnabled(fmt.kerning_mode == 2);
        }
        if (spn_char_tracking_) spn_char_tracking_->setValue(fmt.tracking);
        if (spn_char_scale_x_) spn_char_scale_x_->setValue(fmt.scale_x * 100.0);
        if (spn_char_scale_y_) spn_char_scale_y_->setValue(fmt.scale_y * 100.0);
        if (spn_baseline_shift_) spn_baseline_shift_->setValue(fmt.baseline_shift);
        if (btn_underline_) btn_underline_->setChecked(fmt.underline);
        if (btn_strikethrough_) btn_strikethrough_->setChecked(fmt.strikethrough);
        if (btn_all_caps_) btn_all_caps_->setChecked(fmt.text_style == 1);
        if (btn_small_caps_) btn_small_caps_->setChecked(fmt.text_style == 2);
        if (btn_superscript_) btn_superscript_->setChecked(fmt.text_style == 3);
        if (btn_subscript_) btn_subscript_->setChecked(fmt.text_style == 4);
        if (btn_ligatures_) btn_ligatures_->setChecked(fmt.ligatures);
        if (btn_stylistic_alternates_) btn_stylistic_alternates_->setChecked(fmt.stylistic_alternates);
        if (btn_fractions_) btn_fractions_->setChecked(fmt.fractions);
        if (btn_opentype_features_) btn_opentype_features_->setChecked(fmt.opentype_features);
        int rich_text_style_i = cmb_text_style_->findData(fmt.text_style);
        cmb_text_style_->setCurrentIndex(rich_text_style_i >= 0 ? rich_text_style_i : 0);
        if (summary.mixed & RichTextCharFillColor) {
            style_color_button_mixed(btn_text_color_);
            style_color_button_mixed(btn_gradient_start_color_);
            style_color_button_mixed(btn_gradient_end_color_);
        } else {
            style_color_button(btn_text_color_, fmt.fill.color);
            if (cmb_fill_type_) {
                int rich_fill_i = cmb_fill_type_->findData(fmt.fill.type);
                cmb_fill_type_->setCurrentIndex(rich_fill_i >= 0 ? rich_fill_i : 0);
            }
            if (cmb_gradient_type_) {
                int rich_gradient_i = cmb_gradient_type_->findData(fmt.fill.gradient_type);
                cmb_gradient_type_->setCurrentIndex(rich_gradient_i >= 0 ? rich_gradient_i : 0);
            }
            if (cmb_gradient_spread_) {
                int rich_spread_i = cmb_gradient_spread_->findData(fmt.fill.gradient_spread);
                cmb_gradient_spread_->setCurrentIndex(rich_spread_i >= 0 ? rich_spread_i : 0);
            }
            if (btn_gradient_start_color_) style_color_button(btn_gradient_start_color_, fmt.fill.gradient_start_color);
            if (btn_gradient_end_color_) style_color_button(btn_gradient_end_color_, fmt.fill.gradient_end_color);
            if (spn_gradient_start_pos_) spn_gradient_start_pos_->setValue(fmt.fill.gradient_start_pos);
            if (spn_gradient_end_pos_) spn_gradient_end_pos_->setValue(fmt.fill.gradient_end_pos);
            if (spn_gradient_start_opacity_) spn_gradient_start_opacity_->setValue(fmt.fill.gradient_start_opacity);
            if (spn_gradient_end_opacity_) spn_gradient_end_opacity_->setValue(fmt.fill.gradient_end_opacity);
            if (spn_gradient_opacity_) spn_gradient_opacity_->setValue(fmt.fill.gradient_opacity);
            if (spn_gradient_angle_) spn_gradient_angle_->setValue(fmt.fill.gradient_angle);
            if (spn_gradient_center_x_) spn_gradient_center_x_->setValue(fmt.fill.gradient_center_x);
            if (spn_gradient_center_y_) spn_gradient_center_y_->setValue(fmt.fill.gradient_center_y);
            if (spn_gradient_scale_) spn_gradient_scale_->setValue(fmt.fill.gradient_scale);
            if (spn_gradient_focal_x_) spn_gradient_focal_x_->setValue(fmt.fill.gradient_focal_x);
            if (spn_gradient_focal_y_) spn_gradient_focal_y_->setValue(fmt.fill.gradient_focal_y);
        }

        set_combo_mixed(cmb_font_, summary.mixed & RichTextCharFontFamily);
        set_combo_mixed(cmb_font_style_, summary.mixed & (RichTextCharFontFamily | RichTextCharFontStyle | RichTextCharBold | RichTextCharItalic));
        set_spin_mixed(spn_size_, summary.mixed & RichTextCharFontSize);
        set_button_mixed(chk_bold_, summary.mixed & RichTextCharBold);
        set_button_mixed(chk_italic_, summary.mixed & RichTextCharItalic);
        set_button_mixed(chk_font_kerning_, summary.mixed & RichTextCharKerning);
        set_combo_mixed(cmb_kerning_mode_, summary.mixed & RichTextCharKerning);
        set_spin_mixed(spn_kerning_value_, summary.mixed & RichTextCharKerning);
        set_spin_mixed(spn_char_tracking_, summary.mixed & RichTextCharTracking);
        set_spin_mixed(spn_char_scale_x_, summary.mixed & RichTextCharScaleX);
        set_spin_mixed(spn_char_scale_y_, summary.mixed & RichTextCharScaleY);
        set_spin_mixed(spn_baseline_shift_, summary.mixed & RichTextCharBaselineShift);
        set_button_mixed(btn_underline_, summary.mixed & RichTextCharUnderline);
        set_button_mixed(btn_strikethrough_, summary.mixed & RichTextCharStrikethrough);
        set_button_mixed(btn_all_caps_, summary.mixed & RichTextCharTextStyle);
        set_button_mixed(btn_small_caps_, summary.mixed & RichTextCharTextStyle);
        set_button_mixed(btn_superscript_, summary.mixed & RichTextCharTextStyle);
        set_button_mixed(btn_subscript_, summary.mixed & RichTextCharTextStyle);
        set_button_mixed(btn_ligatures_, summary.mixed & RichTextCharLigatures);
        set_button_mixed(btn_stylistic_alternates_, summary.mixed & RichTextCharStylisticAlternates);
        set_button_mixed(btn_fractions_, summary.mixed & RichTextCharFractions);
        set_button_mixed(btn_opentype_features_, summary.mixed & RichTextCharOpenTypeFeatures);
        set_combo_mixed(cmb_text_style_, summary.mixed & RichTextCharTextStyle);
        set_combo_mixed(cmb_fill_type_, summary.mixed & RichTextCharFillColor);
        set_combo_mixed(cmb_gradient_type_, summary.mixed & RichTextCharFillColor);
        set_combo_mixed(cmb_gradient_spread_, summary.mixed & RichTextCharFillColor);
        set_spin_mixed(spn_gradient_start_pos_, summary.mixed & RichTextCharFillColor);
        set_spin_mixed(spn_gradient_end_pos_, summary.mixed & RichTextCharFillColor);
        set_spin_mixed(spn_gradient_start_opacity_, summary.mixed & RichTextCharFillColor);
        set_spin_mixed(spn_gradient_end_opacity_, summary.mixed & RichTextCharFillColor);
        set_spin_mixed(spn_gradient_opacity_, summary.mixed & RichTextCharFillColor);
        set_spin_mixed(spn_gradient_angle_, summary.mixed & RichTextCharFillColor);
        set_spin_mixed(spn_gradient_center_x_, summary.mixed & RichTextCharFillColor);
        set_spin_mixed(spn_gradient_center_y_, summary.mixed & RichTextCharFillColor);
        set_spin_mixed(spn_gradient_scale_, summary.mixed & RichTextCharFillColor);
        set_spin_mixed(spn_gradient_focal_x_, summary.mixed & RichTextCharFillColor);
        set_spin_mixed(spn_gradient_focal_y_, summary.mixed & RichTextCharFillColor);
    }
    if (!use_rich_char_summary) {
        if (btn_ligatures_) btn_ligatures_->setChecked(layer_->text_ligatures);
        if (btn_stylistic_alternates_) btn_stylistic_alternates_->setChecked(layer_->text_stylistic_alternates);
        if (btn_fractions_) btn_fractions_->setChecked(layer_->text_fractions);
        if (btn_opentype_features_) btn_opentype_features_->setChecked(layer_->text_opentype_features);
        int style_idx = cmb_text_style_->findData(layer_->text_style);
        cmb_text_style_->setCurrentIndex(style_idx >= 0 ? style_idx : 0);
    }
    int overflow_idx = cmb_text_overflow_->findData(layer_->text_overflow_mode);
    cmb_text_overflow_->setCurrentIndex(overflow_idx >= 0 ? overflow_idx : 0);
    spn_text_fit_min_scale_->setValue(layer_->text_fit_min_scale);
    bool is_fit = layer_->text_overflow_mode == 2 && !is_ticker;
    spn_text_fit_min_scale_->setVisible(is_fit);
    lbl_text_fit_scale_->setVisible(is_fit);
    if (auto *form = qobject_cast<QFormLayout *>(dynamic_text_box_ ? dynamic_text_box_->layout() : nullptr)) {
        if (auto *label = form->labelForField(spn_text_fit_min_scale_))
            label->setVisible(is_fit);
    }
    if (lbl_text_fit_scale_) {
        QFont preview_font = font_for_layer(*layer_);
        QRectF preview_rect(0, 0, eval_box_width(*layer_, lt), eval_box_height(*layer_, lt));
        double scale = horizontal_fit_scale(preview_font, preview_rect, display_text_for_style(*layer_), *layer_, lt);
        lbl_text_fit_scale_->setText(obsgs_tr("OBSTitles.ScalePercentFormat").arg((int)std::round(scale * 100.0)));
    }
    {
        QSignalBlocker block(chk_expose_text_);
        chk_expose_text_->setChecked(layer_->expose_text);
    }
    if (chk_exposed_hide_if_empty_) {
        QSignalBlocker block(chk_exposed_hide_if_empty_);
        chk_exposed_hide_if_empty_->setEnabled(layer_->expose_text);
        chk_exposed_hide_if_empty_->setChecked(layer_->exposed_hide_if_empty);
    }
    if (chk_exposed_single_value_) {
        QSignalBlocker block(chk_exposed_single_value_);
        chk_exposed_single_value_->setEnabled(layer_->expose_text);
        chk_exposed_single_value_->setChecked(layer_->exposed_single_value);
    }
    if (chk_ignore_persistence_) {
        const bool can_ignore_persistence = !layer_->expose_text;
        QSignalBlocker block(chk_ignore_persistence_);
        chk_ignore_persistence_->setEnabled(can_ignore_persistence);
        chk_ignore_persistence_->setChecked(can_ignore_persistence && layer_->ignore_persistence);
        chk_ignore_persistence_->setToolTip(can_ignore_persistence
            ? obsgs_tr("OBSTitles.IgnorePersistenceTooltip")
            : obsgs_tr("OBSTitles.IgnorePersistenceDisabledTooltip"));
    }
    if (chk_scene_mask_) chk_scene_mask_->setChecked(layer_->use_as_scene_mask);
    if (grp_text_align_) {
        QSignalBlocker block(grp_text_align_);
        if (auto *button = grp_text_align_->button(layer_->align_h))
            button->setChecked(true);
        else if (auto *fallback = grp_text_align_->button(1))
            fallback->setChecked(true);
    }
    if (grp_text_valign_) {
        QSignalBlocker block(grp_text_valign_);
        if (auto *button = grp_text_valign_->button(layer_->align_v))
            button->setChecked(true);
        else if (auto *fallback = grp_text_valign_->button(1))
            fallback->setChecked(true);
    }
    if (spn_paragraph_indent_left_) spn_paragraph_indent_left_->setValue(eval_paragraph_indent_left(*layer_, lt));
    if (spn_paragraph_indent_right_) spn_paragraph_indent_right_->setValue(eval_paragraph_indent_right(*layer_, lt));
    if (spn_paragraph_indent_first_line_) spn_paragraph_indent_first_line_->setValue(eval_paragraph_indent_first_line(*layer_, lt));
    if (spn_paragraph_space_before_) spn_paragraph_space_before_->setValue(layer_->paragraph_space_before_prop.is_animated() ? layer_->paragraph_space_before_prop.evaluate(lt) : (double)layer_->paragraph_space_before);
    if (spn_paragraph_space_after_) spn_paragraph_space_after_->setValue(layer_->paragraph_space_after_prop.is_animated() ? layer_->paragraph_space_after_prop.evaluate(lt) : (double)layer_->paragraph_space_after);
    if (chk_paragraph_hyphenate_) chk_paragraph_hyphenate_->setChecked(layer_->paragraph_hyphenate);

    QFontDatabase fdb;
    cmb_font_->setToolTip(fdb.families().contains(QString::fromStdString(layer_->font_family))
        ? QString()
        : obsgs_tr("OBSTitles.FontMissingWarningFormat").arg(QString::fromStdString(layer_->font_family)));

    loading_values_ = false;
}

void PropertiesPanel::remember_next_color_popup_position(const QPoint &global_pos)
{
    pending_color_popup_position_ = global_pos;
    pending_color_popup_position_valid_ = true;
}


void PropertiesPanel::open_foreground_color_selector()
{
    if (btn_appearance_fill_color_ && btn_appearance_fill_color_->isEnabled())
        btn_appearance_fill_color_->click();
}

void PropertiesPanel::open_background_color_selector()
{
    if (btn_appearance_stroke_color_ && btn_appearance_stroke_color_->isEnabled())
        btn_appearance_stroke_color_->click();
}

void PropertiesPanel::swap_foreground_background_colors()
{
    return;
}
