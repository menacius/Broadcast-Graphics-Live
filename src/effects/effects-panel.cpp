#include "title-editor-internal.h"
#include "effect-preset-catalog.h"
#include "extensions/effect-extension-catalog.h"

#include <QHash>
#include <QAbstractItemModel>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QMetaType>
#include <QScopedValueRollback>
#include <cmath>
#include <algorithm>
#include <functional>
#include <utility>

namespace {

class EffectsStackListWidget final : public QListWidget {
public:
    using QListWidget::QListWidget;
    std::function<bool(const QString &)> external_drop_handler;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event && bgs::effects::mime_has_effect_preset(event->mimeData())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        QListWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (event && bgs::effects::mime_has_effect_preset(event->mimeData())) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return;
        }
        QListWidget::dragMoveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (event && bgs::effects::mime_has_effect_preset(event->mimeData())) {
            const QString path = bgs::effects::effect_preset_path_from_mime(event->mimeData());
            if (external_drop_handler && external_drop_handler(path)) {
                event->setDropAction(Qt::CopyAction);
                event->accept();
            } else {
                // This is an external preset payload, not an internal list
                // reorder. Never pass a failed preset drop to QListWidget,
                // which may try to interpret it as model data.
                event->ignore();
            }
            return;
        }
        QListWidget::dropEvent(event);
    }
};

} // namespace


static QString effect_display_name(const LayerEffect &effect)
{
    if (!effect.extension_id.empty()) {
        auto &catalog = BglEffectExtensionCatalog::instance();
        if (catalog.effects().empty()) catalog.reload();
        if (const auto *definition = catalog.find(QString::fromStdString(effect.extension_id)))
            return definition->displayName;
        return QString::fromStdString(effect.extension_id);
    }
    return effect_type_name(effect.type);
}

static QString bgl_effects_panel_style()
{
    const QPalette pal = qApp->palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor window_text = pal.color(QPalette::WindowText);
    const QColor base = pal.color(QPalette::Base);
    const QColor text = pal.color(QPalette::Text);
    const QColor button = pal.color(QPalette::Button);
    const QColor button_text = pal.color(QPalette::ButtonText);
    const QColor mid = pal.color(QPalette::Mid);
    const QColor highlight = pal.color(QPalette::Highlight);
    const QColor highlighted_text = pal.color(QPalette::HighlightedText);
    const QColor alternate = pal.color(QPalette::AlternateBase);
    const QColor hover = button.lightness() < 128 ? button.lighter(122) : button.darker(108);
    const QColor disabled = window_text.lightness() < 128 ? window_text.lighter(160) : window_text.darker(160);

    QString css = QStringLiteral(
        "QWidget#BroadcastGraphicsLiveEffectsPanel{background:@window@;color:@windowText@;}"
        "QLabel{color:@windowText@;background:transparent;}"
        "QListWidget{background:@base@;border:1px solid @mid@;color:@text@;alternate-background-color:@alternate@;}"
        "QListWidget::item{padding:4px;}"
        "QListWidget::item:selected{background:@highlight@;color:@highlightedText@;}"
        "QToolButton{color:@buttonText@;background:@button@;border:1px solid @mid@;border-radius:2px;padding:2px;}"
        "QToolButton:hover{background:@hover@;border-color:@mid@;}"
        "QToolButton:pressed{background:@highlight@;color:@highlightedText@;border-color:@highlight@;}"
        "QToolButton:checked{background:@highlight@;color:@highlightedText@;border-color:@highlight@;}"
        "QScrollArea{background:@window@;border:none;}"
        "QWidget#BroadcastGraphicsLiveEffectsSettingsContainer{background:@window@;}"
        "QMenu{color:@windowText@;background:@window@;border:1px solid @mid@;}"
        "QMenu::item{padding:5px 22px;}"
        "QMenu::item:selected{background:@highlight@;color:@highlightedText@;}"
        "QMenu::item:disabled{color:@disabled@;}");
    css.replace(QStringLiteral("@window@"), window.name(QColor::HexRgb));
    css.replace(QStringLiteral("@windowText@"), window_text.name(QColor::HexRgb));
    css.replace(QStringLiteral("@base@"), base.name(QColor::HexRgb));
    css.replace(QStringLiteral("@text@"), text.name(QColor::HexRgb));
    css.replace(QStringLiteral("@button@"), button.name(QColor::HexRgb));
    css.replace(QStringLiteral("@buttonText@"), button_text.name(QColor::HexRgb));
    css.replace(QStringLiteral("@mid@"), mid.name(QColor::HexRgb));
    css.replace(QStringLiteral("@highlight@"), highlight.name(QColor::HexRgb));
    css.replace(QStringLiteral("@highlightedText@"), highlighted_text.name(QColor::HexRgb));
    css.replace(QStringLiteral("@alternate@"), alternate.name(QColor::HexRgb));
    css.replace(QStringLiteral("@hover@"), hover.name(QColor::HexRgb));
    css.replace(QStringLiteral("@disabled@"), disabled.name(QColor::HexRgb));
    return css;
}

static QString bgl_theme_control_style()
{
    const QPalette pal = qApp->palette();
    return QStringLiteral(
        "QDoubleSpinBox,QSpinBox,QComboBox,QLineEdit,QTextEdit{color:%1;background:%2;"
        "border:1px solid %3;border-radius:2px;padding:1px 3px;selection-background-color:%4;}"
        "QDoubleSpinBox:focus,QSpinBox:focus,QComboBox:focus,QLineEdit:focus,QTextEdit:focus{border-color:%4;}")
        .arg(pal.color(QPalette::Text).name(QColor::HexRgb), pal.color(QPalette::Base).name(QColor::HexRgb),
             pal.color(QPalette::Mid).name(QColor::HexRgb), pal.color(QPalette::Highlight).name(QColor::HexRgb));
}


static uint32_t color_button_argb(QPushButton *button)
{
    return button ? button->property("argb").toUInt() : 0xFFFFFFFF;
}

static void set_color_button_argb(QPushButton *button, uint32_t argb)
{
    if (!button) return;
    button->setProperty("argb", argb);
    QColor c = color_from_argb(argb);
    button->setText(c.name(QColor::HexArgb).toUpper());
    const QColor border = qApp->palette().color(QPalette::Mid);
    const QColor text = c.lightness() < 128 ? QColor(Qt::white) : QColor(Qt::black);
    button->setStyleSheet(QStringLiteral("QPushButton{color:%1;background:%2;border:1px solid %3;border-radius:3px;padding:3px 8px;}")
                          .arg(text.name(QColor::HexRgb), c.name(QColor::HexRgb), border.name(QColor::HexRgb)));
}

static uint32_t panel_eval_effect_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.color_a, (effect.effect_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.color_r, (effect.effect_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.color_g, (effect.effect_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.color_b, effect.effect_color & 0xFF, t);
}

static uint32_t panel_eval_effect_stroke_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.stroke_color_a, (effect.effect_stroke_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.stroke_color_r, (effect.effect_stroke_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.stroke_color_g, (effect.effect_stroke_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.stroke_color_b, effect.effect_stroke_color & 0xFF, t);
}

static uint32_t panel_eval_gradient_start_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.gradient_start_color_a, (effect.effect_gradient_start_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.gradient_start_color_r, (effect.effect_gradient_start_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.gradient_start_color_g, (effect.effect_gradient_start_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.gradient_start_color_b, effect.effect_gradient_start_color & 0xFF, t);
}

static uint32_t panel_eval_gradient_end_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.gradient_end_color_a, (effect.effect_gradient_end_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.gradient_end_color_r, (effect.effect_gradient_end_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.gradient_end_color_g, (effect.effect_gradient_end_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.gradient_end_color_b, effect.effect_gradient_end_color & 0xFF, t);
}

static void set_gradient_start_color_channels_at(LayerEffect &effect, double time, uint32_t argb)
{
    set_animated_value(effect.gradient_start_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(effect.gradient_start_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(effect.gradient_start_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(effect.gradient_start_color_b, time, argb & 0xFF);
}

static void set_gradient_end_color_channels_at(LayerEffect &effect, double time, uint32_t argb)
{
    set_animated_value(effect.gradient_end_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(effect.gradient_end_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(effect.gradient_end_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(effect.gradient_end_color_b, time, argb & 0xFF);
}

static double panel_eval_effect_property(const AnimatedProperty &prop, double fallback, double t)
{
    return prop.is_animated() ? prop.evaluate(t) : fallback;
}

static void set_effect_color_channels_at(LayerEffect &effect, double time, uint32_t argb)
{
    set_animated_value(effect.color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(effect.color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(effect.color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(effect.color_b, time, argb & 0xFF);
}

static void set_effect_stroke_color_channels_at(LayerEffect &effect, double time, uint32_t argb)
{
    set_animated_value(effect.stroke_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(effect.stroke_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(effect.stroke_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(effect.stroke_color_b, time, argb & 0xFF);
}

static uint32_t panel_eval_effect_secondary_color(const LayerEffect &effect, double t)
{
    return ((uint32_t)eval_channel(effect.secondary_color_a, (effect.effect_secondary_color >> 24) & 0xFF, t) << 24) |
           ((uint32_t)eval_channel(effect.secondary_color_r, (effect.effect_secondary_color >> 16) & 0xFF, t) << 16) |
           ((uint32_t)eval_channel(effect.secondary_color_g, (effect.effect_secondary_color >> 8) & 0xFF, t) << 8) |
           (uint32_t)eval_channel(effect.secondary_color_b, effect.effect_secondary_color & 0xFF, t);
}

static void set_effect_secondary_color_channels_at(LayerEffect &effect, double time, uint32_t argb)
{
    set_animated_value(effect.secondary_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(effect.secondary_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(effect.secondary_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(effect.secondary_color_b, time, argb & 0xFF);
}

static bool effect_property_has_keyframe_at(const AnimatedProperty &property, double time)
{
    return std::any_of(property.keyframes.begin(), property.keyframes.end(),
                       [time](const Keyframe &keyframe) {
                           return std::abs(keyframe.time - time) < 0.0001;
                       });
}

static bool extension_track_has_keyframe_at(const LayerEffect &effect,
                                            const QString &path,
                                            double time)
{
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(effect.extension_keyframes_json));
    const QJsonArray keys = doc.isObject()
        ? doc.object().value(path).toArray() : QJsonArray{};
    for (const QJsonValue &value : keys) {
        if (std::abs(value.toObject().value(QStringLiteral("time")).toDouble() -
                     time) < 0.0001)
            return true;
    }
    return false;
}

static uint32_t extension_json_color_to_argb(const QJsonValue &value,
                                              uint32_t fallback = 0xFFFFFFFFu)
{
    if (value.isString()) {
        const QColor color(value.toString());
        return color.isValid() ? argb_from_color(color) : fallback;
    }
    const QJsonArray array = value.toArray();
    if (array.size() < 3)
        return fallback;
    QColor color;
    color.setRgbF(std::clamp(array.at(0).toDouble(1.0), 0.0, 1.0),
                  std::clamp(array.at(1).toDouble(1.0), 0.0, 1.0),
                  std::clamp(array.at(2).toDouble(1.0), 0.0, 1.0),
                  std::clamp(array.size() > 3 ? array.at(3).toDouble(1.0) : 1.0,
                             0.0, 1.0));
    return argb_from_color(color);
}

static QJsonArray extension_argb_to_json_color(uint32_t argb)
{
    const QColor color = color_from_argb(argb);
    return QJsonArray{color.redF(), color.greenF(), color.blueF(), color.alphaF()};
}

static QJsonValue extension_state_path_value(const LayerEffect &effect,
                                               const QString &path)
{
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(effect.extension_parameters_json));
    if (!doc.isObject())
        return QJsonValue();
    const QJsonObject root = doc.object();
    const QStringList parts = path.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (parts.size() == 1)
        return root.value(parts.front());
    if (parts.size() == 3 && parts.front() == QStringLiteral("elements")) {
        bool ok = false;
        const int index = parts.at(1).toInt(&ok);
        const QJsonArray elements = root.value(QStringLiteral("elements")).toArray();
        if (ok && index >= 0 && index < elements.size())
            return elements.at(index).toObject().value(parts.at(2));
    }
    return QJsonValue();
}

static QJsonValue evaluate_extension_track(const LayerEffect &effect,
                                           const QString &path,
                                           double time,
                                           const QJsonValue &fallback)
{
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray::fromStdString(effect.extension_keyframes_json));
    const QJsonArray keys = doc.isObject()
        ? doc.object().value(path).toArray() : QJsonArray{};
    if (keys.isEmpty())
        return fallback;

    QJsonObject left = keys.first().toObject();
    QJsonObject right = keys.last().toObject();
    for (const QJsonValue &value : keys) {
        const QJsonObject key = value.toObject();
        const double key_time = key.value(QStringLiteral("time")).toDouble();
        if (key_time <= time)
            left = key;
        if (key_time >= time) {
            right = key;
            break;
        }
    }
    const double left_time = left.value(QStringLiteral("time")).toDouble();
    const double right_time = right.value(QStringLiteral("time")).toDouble();
    double mix = right_time > left_time
        ? (time - left_time) / (right_time - left_time) : 0.0;
    mix = std::clamp(mix, 0.0, 1.0);
    if (left.value(QStringLiteral("interpolation")).toString() ==
        QStringLiteral("hold"))
        mix = 0.0;

    const QJsonValue a = left.value(QStringLiteral("value"));
    const QJsonValue b = right.value(QStringLiteral("value"));
    if (a.isDouble() && b.isDouble())
        return a.toDouble() + (b.toDouble() - a.toDouble()) * mix;
    if (a.isArray() && b.isArray()) {
        const QJsonArray aa = a.toArray();
        const QJsonArray bb = b.toArray();
        QJsonArray out;
        const qsizetype count = std::min(aa.size(), bb.size());
        for (qsizetype index = 0; index < count; ++index) {
            out.append(aa.at(index).toDouble() +
                       (bb.at(index).toDouble() - aa.at(index).toDouble()) * mix);
        }
        return out;
    }
    return mix < 1.0 ? a : b;
}

EffectsPanel::EffectsPanel(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("BroadcastGraphicsLiveEffectsPanel"));
    setAcceptDrops(true);
    setStyleSheet(bgl_effects_panel_style());

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *hint = new QLabel(bgl_tr("OBSTitles.EffectStack"), this);
    QFont hint_font = hint->font();
    hint_font.setBold(true);
    hint->setFont(hint_font);
    layout->addWidget(hint);

    auto *effect_stack_list = new EffectsStackListWidget(this);
    effect_stack_list->external_drop_handler = [this](const QString &file_path) {
        return add_effect_from_preset_file(file_path);
    };
    effect_list_ = effect_stack_list;
    effect_list_->setObjectName(QStringLiteral("BroadcastGraphicsLiveEffectsList"));
    effect_list_->setSelectionMode(QAbstractItemView::SingleSelection);
    effect_list_->setAlternatingRowColors(true);
    effect_list_->setDragEnabled(true);
    effect_list_->setAcceptDrops(true);
    effect_list_->setDragDropMode(QAbstractItemView::InternalMove);
    effect_list_->setDefaultDropAction(Qt::MoveAction);
    effect_list_->setDropIndicatorShown(true);
    
    layout->addWidget(effect_list_, 1);

    auto *button_bar = new QWidget(this);
    button_bar->setObjectName(QStringLiteral("BroadcastGraphicsLiveEffectsButtonBar"));
    auto *button_layout = new QHBoxLayout(button_bar);
    button_layout->setContentsMargins(0, 0, 0, 0);
    button_layout->setSpacing(4);

    auto add_button = [button_bar, button_layout](const char *icon, const QString &tip) {
        auto *button = new QToolButton(button_bar);
        button->setIcon(obs_icon(icon));
        button->setIconSize(QSize(16, 16));
        button->setToolTip(tip);
        button->setAutoRaise(true);
        button_layout->addWidget(button);
        return button;
    };

    auto *btn_add = add_button("add.svg", bgl_tr("OBSTitles.AddEffect"));
    btn_remove_ = add_button("delete.svg", bgl_tr("OBSTitles.RemoveEffect"));
    btn_duplicate_ = add_button("duplicate.svg", bgl_tr("OBSTitles.DuplicateEffect"));
    btn_move_up_ = add_button("move-up.svg", bgl_tr("OBSTitles.MoveEffectUp"));
    btn_move_down_ = add_button("move-down.svg", bgl_tr("OBSTitles.MoveEffectDown"));
    btn_respect_masks_ = add_button("timeline-mask.svg", bgl_tr("OBSTitles.ApplyEffectStackAfterMask"));
    btn_respect_masks_->setCheckable(true);
    btn_respect_masks_->setToolTip(bgl_tr("OBSTitles.ApplyEffectStackAfterMaskTooltip"));
    button_layout->addStretch(1);
    layout->addWidget(button_bar);

    auto *settings_scroll = new QScrollArea(this);
    settings_scroll->setWidgetResizable(true);
    
    settings_container_ = new QWidget(settings_scroll);
    settings_container_->setObjectName(QStringLiteral("BroadcastGraphicsLiveEffectsSettingsContainer"));
    settings_layout_ = new QVBoxLayout(settings_container_);
    settings_layout_->setContentsMargins(0, 6, 0, 0);
    settings_layout_->setSpacing(6);
    settings_scroll->setWidget(settings_container_);
    layout->addWidget(settings_scroll, 2);

    connect(effect_list_, &QListWidget::currentRowChanged, this, [this](int row) {
        selected_index_ = row;
        load_settings();
        emit extension_canvas_handles_changed(extension_canvas_handles());
    });

    connect(effect_list_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        if (loading_values_ || !layer_) return;
        int row = effect_list_->row(item);
        if (row < 0 || row >= (int)layer_->effects.size()) return;
        layer_->effects[row].enabled = item->checkState() == Qt::Checked;
        set_animated_value(layer_->effects[row].enabled_prop, std::clamp(playhead_ - layer_->in_time, 0.0,
                           std::max(0.0, layer_->out_time - layer_->in_time)),
                           layer_->effects[row].enabled ? 1.0 : 0.0);
        emit_effect_changed();
    });

    connect(effect_list_->model(), &QAbstractItemModel::rowsMoved, this,
            [this](const QModelIndex &, int, int, const QModelIndex &, int) {
                apply_effect_list_order_from_items();
            });

    connect(btn_add, &QToolButton::clicked, this, [this, btn_add]() {
        if (!layer_) return;
        QMenu menu(btn_add);
        
        auto &extension_catalog = BglEffectExtensionCatalog::instance();
        if (extension_catalog.effects().empty())
            extension_catalog.reload();
        QHash<QString, QMenu *> category_menus;
        const auto menu_for_category = [&](const QString &category) -> QMenu * {
            const QString normalized = category.isEmpty() ? tr("Extensions") : category;
            if (category_menus.contains(normalized)) return category_menus.value(normalized);
            QMenu *parent_menu = &menu;
            QString accumulated;
            for (const QString &part : normalized.split('/', Qt::SkipEmptyParts)) {
                accumulated += (accumulated.isEmpty() ? QString() : QStringLiteral("/")) + part;
                if (!category_menus.contains(accumulated))
                    category_menus.insert(accumulated, parent_menu->addMenu(part));
                parent_menu = category_menus.value(accumulated);
            }
            category_menus.insert(normalized, parent_menu);
            return parent_menu;
        };
        for (const auto &definition : extension_catalog.effects()) {
            QMenu *target = menu_for_category(definition.category);
            QAction *action = target->addAction(definition.displayName);
            action->setData(QStringLiteral("effect:") + definition.id);
            action->setToolTip(definition.providerId + QStringLiteral(" ") + definition.providerVersion);
        }
        QAction *chosen = menu.exec(btn_add->mapToGlobal(QPoint(0, btn_add->height())));
        if (!chosen) return;
        LayerEffect effect;
        const QString stable_id = chosen->data().toString().mid(QStringLiteral("effect:").size());
        if (const auto *definition = extension_catalog.find(stable_id)) {
            if (definition->builtIn) {
                effect = bgs::effects::make_default_layer_effect(definition->builtInType);
            } else {
                effect = bgs::effects::make_default_layer_effect(LayerEffectType::BackgroundColor);
                effect.extension_id = stable_id.toStdString();
                effect.extension_parameters_json = QJsonDocument(definition->defaults)
                    .toJson(QJsonDocument::Compact).toStdString();
                effect.extension_schema_version = definition->schemaVersion;
            }
        } else {
            return;
        }
        layer_->effects.push_back(effect);
        selected_index_ = (int)layer_->effects.size() - 1;
        rebuild_stack();
        emit_effect_changed();
    });

    connect(btn_remove_, &QToolButton::clicked, this, [this]() {
        if (!layer_ || selected_index_ < 0 || selected_index_ >= (int)layer_->effects.size()) return;
        layer_->effects.erase(layer_->effects.begin() + selected_index_);
        if (selected_index_ >= (int)layer_->effects.size()) selected_index_ = (int)layer_->effects.size() - 1;
        sync_legacy_enabled_flags();
        rebuild_stack();
        emit_effect_changed();
    });

    connect(btn_duplicate_, &QToolButton::clicked, this, [this]() {
        if (!layer_ || selected_index_ < 0 || selected_index_ >= (int)layer_->effects.size()) return;
        layer_->effects.insert(layer_->effects.begin() + selected_index_ + 1, layer_->effects[selected_index_]);
        ++selected_index_;
        sync_legacy_enabled_flags();
        rebuild_stack();
        emit_effect_changed();
    });

    connect(btn_move_up_, &QToolButton::clicked, this, [this]() {
        if (!layer_ || selected_index_ <= 0 || selected_index_ >= (int)layer_->effects.size()) return;
        const int target = selected_index_ - 1;
        std::swap(layer_->effects[(size_t)selected_index_], layer_->effects[(size_t)target]);
        selected_index_ = target;
        rebuild_stack();
        emit_effect_changed();
    });

    connect(btn_move_down_, &QToolButton::clicked, this, [this]() {
        if (!layer_ || selected_index_ < 0 || selected_index_ + 1 >= (int)layer_->effects.size()) return;
        const int target = selected_index_ + 1;
        std::swap(layer_->effects[(size_t)selected_index_], layer_->effects[(size_t)target]);
        selected_index_ = target;
        rebuild_stack();
        emit_effect_changed();
    });

    connect(btn_respect_masks_, &QToolButton::toggled, this, [this](bool checked) {
        if (loading_values_ || !layer_) return;
        layer_->effect_stack_respects_masks = checked;
        emit_effect_changed();
    });

    rebuild_stack();
}


void EffectsPanel::dragEnterEvent(QDragEnterEvent *event)
{
    if (event && bgs::effects::mime_has_effect_preset(event->mimeData())) {
        event->setDropAction(Qt::CopyAction);
        event->accept();
        return;
    }
    QWidget::dragEnterEvent(event);
}

void EffectsPanel::dragMoveEvent(QDragMoveEvent *event)
{
    if (event && bgs::effects::mime_has_effect_preset(event->mimeData())) {
        if (layer_ && !layer_->locked) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        } else {
            event->ignore();
        }
        return;
    }
    QWidget::dragMoveEvent(event);
}

void EffectsPanel::dropEvent(QDropEvent *event)
{
    if (event && bgs::effects::mime_has_effect_preset(event->mimeData())) {
        const QString path = bgs::effects::effect_preset_path_from_mime(event->mimeData());
        if (add_effect_from_preset_file(path)) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
        } else {
            event->ignore();
        }
        return;
    }
    QWidget::dropEvent(event);
}

bool EffectsPanel::add_effect_from_preset_file(const QString &file_path)
{
    if (!layer_ || layer_->locked)
        return false;

    bgs::effects::EffectPresetDescriptor descriptor;
    if (!bgs::effects::load_effect_preset_file(file_path, &descriptor))
        return false;

    layer_->effects.push_back(descriptor.effect);
    selected_index_ = static_cast<int>(layer_->effects.size()) - 1;
    rebuild_stack();
    emit_effect_changed();
    return true;
}

void EffectsPanel::set_layer(std::shared_ptr<Layer> layer, double playhead)
{
    const bool same_layer = layer_ && layer && layer_.get() == layer.get();
    const int previous_effect_count = layer_ ? (int)layer_->effects.size() : -1;
    layer_ = layer;
    playhead_ = playhead;
    selected_index_ = layer_ && !layer_->effects.empty() ? std::clamp(selected_index_, 0, (int)layer_->effects.size() - 1) : -1;
    if (same_layer && previous_effect_count == (layer_ ? (int)layer_->effects.size() : -1) &&
        (settings_editor_has_focus() || numeric_label_dragging_)) {
        return;
    }
    rebuild_stack();
    emit extension_canvas_handles_changed(extension_canvas_handles());
}

LayerEffect *EffectsPanel::selected_effect()
{
    if (!layer_ || selected_index_ < 0 || selected_index_ >= (int)layer_->effects.size()) return nullptr;
    return &layer_->effects[selected_index_];
}

const LayerEffect *EffectsPanel::selected_effect() const
{
    if (!layer_ || selected_index_ < 0 || selected_index_ >= (int)layer_->effects.size()) return nullptr;
    return &layer_->effects[selected_index_];
}

void EffectsPanel::sync_legacy_enabled_flags()
{
    /* Effect stack is the only source of truth. Legacy layer flags are kept
     * only for old project import/export fallback and are not synchronized.
     */
}

QJsonArray EffectsPanel::extension_canvas_handles() const
{
    const LayerEffect *effect = selected_effect();
    if (!effect || effect->extension_id.empty()) return {};
    auto &catalog = BglEffectExtensionCatalog::instance();
    if (catalog.effects().empty()) catalog.reload();
    const auto *definition = catalog.find(QString::fromStdString(effect->extension_id));
    if (!definition || definition->canvasHandles.isEmpty()) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(effect->extension_parameters_json));
    const QJsonObject state = doc.isObject() ? doc.object() : definition->defaults;
    QJsonArray result;
    for (const QJsonValue &value : definition->canvasHandles) {
        QJsonObject handle = value.toObject();
        const QString path = handle.value(QStringLiteral("path")).toString();
        const QJsonValue position = state.value(path);
        if (path.isEmpty() || !position.isArray() || position.toArray().size() < 2) continue;
        handle.insert(QStringLiteral("value"), position);
        result.append(handle);
    }
    return result;
}

void EffectsPanel::set_extension_canvas_handle_position(const QString &path,
                                                         const QPointF &normalized_position,
                                                         bool final_change)
{
    LayerEffect *effect = selected_effect();
    if (!effect || path.isEmpty()) return;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(effect->extension_parameters_json));
    QJsonObject state = doc.isObject() ? doc.object() : QJsonObject{};
    const QJsonArray encoded{std::clamp(normalized_position.x(), 0.0, 1.0),
                             std::clamp(normalized_position.y(), 0.0, 1.0)};
    state.insert(path, encoded);
    effect->extension_parameters_json = QJsonDocument(state).toJson(QJsonDocument::Compact).toStdString();

    const double lt = layer_ ? std::clamp(playhead_ - layer_->in_time, 0.0,
                                          std::max(0.0, layer_->out_time - layer_->in_time)) : 0.0;
    QJsonDocument tracksDoc = QJsonDocument::fromJson(QByteArray::fromStdString(effect->extension_keyframes_json));
    QJsonObject tracks = tracksDoc.isObject() ? tracksDoc.object() : QJsonObject{};
    QJsonArray keys = tracks.value(path).toArray();
    for (int i = 0; i < keys.size(); ++i) {
        QJsonObject key = keys.at(i).toObject();
        if (std::abs(key.value(QStringLiteral("time")).toDouble() - lt) < 0.0001) {
            key.insert(QStringLiteral("value"), encoded);
            keys.replace(i, key);
            break;
        }
    }
    tracks.insert(path, keys);
    effect->extension_keyframes_json = QJsonDocument(tracks).toJson(QJsonDocument::Compact).toStdString();
    sync_legacy_enabled_flags();
    emit property_changed(final_change);
    emit extension_canvas_handles_changed(extension_canvas_handles());
}

void EffectsPanel::emit_effect_changed()
{
    sync_legacy_enabled_flags();
    emit property_changed(!numeric_label_dragging_);
    emit extension_canvas_handles_changed(extension_canvas_handles());
}

bool EffectsPanel::settings_editor_has_focus() const
{
    QWidget *focus = qApp ? qApp->focusWidget() : nullptr;
    return focus && settings_container_ &&
           (focus == settings_container_ || settings_container_->isAncestorOf(focus));
}

void EffectsPanel::update_playhead(double playhead)
{
    playhead_ = playhead;
    update_bound_controls();
}

double EffectsPanel::current_local_time() const
{
    if (!layer_)
        return 0.0;
    return std::clamp(playhead_ - layer_->in_time, 0.0,
                      std::max(0.0, layer_->out_time - layer_->in_time));
}

void EffectsPanel::update_bound_controls()
{
    if (!layer_ || loading_values_ || numeric_label_dragging_)
        return;

    const double lt = std::clamp(playhead_ - layer_->in_time, 0.0,
                                 std::max(0.0, layer_->out_time - layer_->in_time));

    if (effect_list_) {
        QSignalBlocker blocker(effect_list_);
        const int count = std::min(effect_list_->count(), (int)layer_->effects.size());
        for (int row = 0; row < count; ++row) {
            if (QListWidgetItem *item = effect_list_->item(row)) {
                const bool enabled = eval_effect_enabled(layer_->effects[(size_t)row], lt);
                const Qt::CheckState state = enabled ? Qt::Checked : Qt::Unchecked;
                if (item->checkState() != state)
                    item->setCheckState(state);
            }
        }
    }

    const LayerEffect *effect = selected_effect();
    if (!effect)
        return;

    QScopedValueRollback<bool> loading_guard(loading_values_, true);
    for (const auto &binding : numeric_bindings_) {
        if (!binding.spin || !binding.value)
            continue;
        const double value = binding.value(*effect, lt);
        if (!std::isfinite(value) || std::abs(binding.spin->value() - value) < 0.000001)
            continue;
        QSignalBlocker blocker(binding.spin);
        binding.spin->setValue(value);
    }

    for (const auto &binding : color_bindings_) {
        if (!binding.button || !binding.value)
            continue;
        const uint32_t argb = binding.value(*effect, lt);
        if (binding.button->property("argb").toUInt() != argb)
            set_color_button_argb(binding.button, argb);
    }
    for (const auto &binding : bool_bindings_) {
        if (!binding.checkbox || !binding.value)
            continue;
        const bool value = binding.value(*effect, lt);
        if (binding.checkbox->isChecked() != value) {
            QSignalBlocker blocker(binding.checkbox);
            binding.checkbox->setChecked(value);
        }
    }
    for (const auto &binding : combo_bindings_) {
        if (!binding.combo || !binding.value)
            continue;
        const QVariant value = binding.value(*effect, lt);
        int index = binding.combo->findData(value);
        if (index < 0 && binding.combo->count() > 0)
            index = 0;
        if (index >= 0 && binding.combo->currentIndex() != index) {
            QSignalBlocker blocker(binding.combo);
            binding.combo->setCurrentIndex(index);
        }
    }
    for (const auto &binding : keyframe_bindings_) {
        if (!binding.button || !binding.has_keyframe)
            continue;
        const bool keyed = binding.has_keyframe(*effect, lt);
        binding.button->setText(keyed ? QStringLiteral("◆") : QStringLiteral("◇"));
        binding.button->setProperty("keyframed", keyed);
    }
}

void EffectsPanel::apply_effect_list_order_from_items()
{
    if (loading_values_ || !layer_ || !effect_list_)
        return;
    const int count = (int)layer_->effects.size();
    if (effect_list_->count() != count || count <= 1)
        return;

    std::vector<LayerEffect> reordered;
    reordered.reserve((size_t)count);
    std::vector<bool> used((size_t)count, false);
    for (int row = 0; row < count; ++row) {
        QListWidgetItem *item = effect_list_->item(row);
        const int source_index = item ? item->data(Qt::UserRole).toInt() : -1;
        if (source_index < 0 || source_index >= count || used[(size_t)source_index])
            return;
        reordered.push_back(layer_->effects[(size_t)source_index]);
        used[(size_t)source_index] = true;
    }

    layer_->effects = std::move(reordered);
    selected_index_ = std::clamp(effect_list_->currentRow(), 0, count - 1);

    QSignalBlocker blocker(effect_list_);
    for (int row = 0; row < count; ++row) {
        if (QListWidgetItem *item = effect_list_->item(row))
            item->setData(Qt::UserRole, row);
    }

    sync_legacy_enabled_flags();
    emit_effect_changed();
    load_settings();
}

void EffectsPanel::rebuild_stack()
{
    loading_values_ = true;
    effect_list_->clear();
    if (!layer_) {
        effect_list_->addItem(bgl_tr("OBSTitles.SelectLayerEditEffects"));
        if (auto *item = effect_list_->item(0)) item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        selected_index_ = -1;
    } else if (layer_->effects.empty()) {
        effect_list_->addItem(bgl_tr("OBSTitles.NoEffectsAdded"));
        if (auto *item = effect_list_->item(0)) item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        selected_index_ = -1;
    } else {
        for (int i = 0; i < (int)layer_->effects.size(); ++i) {
            const auto &effect = layer_->effects[i];
            auto *item = new QListWidgetItem(effect_display_name(effect));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable |
                           Qt::ItemIsEnabled | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled);
            item->setData(Qt::UserRole, i);
            item->setCheckState(effect.enabled ? Qt::Checked : Qt::Unchecked);
            effect_list_->addItem(item);
        }
        selected_index_ = std::clamp(selected_index_, 0, (int)layer_->effects.size() - 1);
        effect_list_->setCurrentRow(selected_index_);
    }
    loading_values_ = false;

    const bool has_selection = selected_effect() != nullptr;
    if (btn_remove_) btn_remove_->setEnabled(has_selection);
    if (btn_duplicate_) btn_duplicate_->setEnabled(has_selection);
    if (btn_move_up_) btn_move_up_->setEnabled(has_selection && selected_index_ > 0);
    if (btn_move_down_) btn_move_down_->setEnabled(has_selection && layer_ && selected_index_ + 1 < (int)layer_->effects.size());
    if (btn_respect_masks_) {
        QSignalBlocker blocker(btn_respect_masks_);
        btn_respect_masks_->setEnabled(layer_ != nullptr);
        btn_respect_masks_->setChecked(layer_ && layer_->effect_stack_respects_masks);
    }

    load_settings();
}

void EffectsPanel::build_settings()
{
    numeric_bindings_.clear();
    color_bindings_.clear();
    bool_bindings_.clear();
    combo_bindings_.clear();
    keyframe_bindings_.clear();
    while (QLayoutItem *item = settings_layout_->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
}

void EffectsPanel::load_settings()
{
    build_settings();
    if (!layer_ || !selected_effect()) {
        auto *label = new QLabel(layer_ ? bgl_tr("OBSTitles.AddEffectSettingsHint") : bgl_tr("OBSTitles.SelectLayerEditEffectsHint"), settings_container_);
        label->setWordWrap(true);
        
        settings_layout_->addWidget(label);
        settings_layout_->addStretch(1);
        return;
    }

    loading_values_ = true;
    const double lt = std::clamp(playhead_ - layer_->in_time, 0.0, std::max(0.0, layer_->out_time - layer_->in_time));
    auto *box = new QGroupBox(effect_display_name(*selected_effect()), settings_container_);
    {
        const QPalette pal = qApp->palette();
        const QColor section_bg = pal.color(QPalette::Window).lightness() < 128
                                    ? pal.color(QPalette::Window).lighter(112)
                                    : pal.color(QPalette::Window).darker(104);
        const QColor hover = pal.color(QPalette::Button).lightness() < 128
                             ? pal.color(QPalette::Button).lighter(122)
                             : pal.color(QPalette::Button).darker(108);
        QString group_css = QStringLiteral(
            "QGroupBox{color:@windowText@;background:@section@;border:1px solid @mid@;margin-top:8px;padding-top:14px;}"
            "QGroupBox::title{subcontrol-origin:margin;left:8px;padding:0 3px;}"
            "QDoubleSpinBox,QComboBox{color:@text@;background:@base@;border:1px solid @mid@;border-radius:2px;padding:1px 3px;}"
            "QDoubleSpinBox:focus,QComboBox:focus{border-color:@highlight@;}"
            "QPushButton{color:@buttonText@;background:@button@;border:1px solid @mid@;border-radius:3px;padding:3px 8px;}"
            "QPushButton:hover{background:@hover@;border-color:@mid@;}"
            "QPushButton:pressed{background:@highlight@;color:@highlightedText@;border-color:@highlight@;}");
        group_css.replace(QStringLiteral("@windowText@"), pal.color(QPalette::WindowText).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@section@"), section_bg.name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@mid@"), pal.color(QPalette::Mid).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@text@"), pal.color(QPalette::Text).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@base@"), pal.color(QPalette::Base).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@highlight@"), pal.color(QPalette::Highlight).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@buttonText@"), pal.color(QPalette::ButtonText).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@button@"), pal.color(QPalette::Button).name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@hover@"), hover.name(QColor::HexRgb));
        group_css.replace(QStringLiteral("@highlightedText@"), pal.color(QPalette::HighlightedText).name(QColor::HexRgb));
        box->setStyleSheet(group_css);
    }
    auto *form = new QFormLayout(box);
    form->setContentsMargins(8, 8, 8, 8);
    form->setSpacing(6);
    auto spin = [box](double min, double max, double step) { auto *s = new QDoubleSpinBox(box); s->setRange(min, max); s->setSingleStep(step); s->setFixedHeight(22); s->setStyleSheet(bgl_theme_control_style()); return s; };
    auto combo = [box]() { auto *c = new QComboBox(box); c->setFixedHeight(22); c->setStyleSheet(bgl_theme_control_style()); return c; };
    auto color_button = [this, box](uint32_t argb, auto setter) {
        auto *button = new QPushButton(box);
        set_color_button_argb(button, argb);
        connect(button, &QPushButton::clicked, this, [this, button, setter]() {
            QColor picked = bgl_pick_color(color_from_argb(color_button_argb(button)), this, bgl_tr("OBSTitles.ChooseColor"));
            if (!picked.isValid()) return;
            uint32_t argb = argb_from_color(picked);
            set_color_button_argb(button, argb);
            setter(argb);
            emit_effect_changed();
        });
        return button;
    };
    auto bind_numeric = [this](QDoubleSpinBox *spin,
                               std::function<double(const LayerEffect &, double)> value) {
        if (spin && value)
            numeric_bindings_.push_back({spin, std::move(value)});
    };
    auto bind_color = [this](QPushButton *button,
                             std::function<uint32_t(const LayerEffect &, double)> value) {
        if (button && value)
            color_bindings_.push_back({button, std::move(value)});
    };

    auto wrap_scalar_keyframe = [this, box, lt](QWidget *field,
            AnimatedProperty LayerEffect::*property,
            std::function<double()> current_value = {}) -> QWidget * {
        if (!field || !property)
            return field;
        auto *row = new QWidget(box);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        layout->addWidget(field, 1);
        auto *button = new QPushButton(QStringLiteral("◇"), row);
        button->setFixedSize(26, 22);
        button->setToolTip(tr("Toggle keyframe at the current timeline position"));
        layout->addWidget(button);
        keyframe_bindings_.push_back({button,
            [property](const LayerEffect &effect, double time) {
                return effect_property_has_keyframe_at(effect.*property, time);
            }});
        connect(button, &QPushButton::clicked, this,
                [this, property, current_value, field]() {
            LayerEffect *effect = selected_effect();
            if (!effect || !layer_)
                return;
            const double time = std::clamp(playhead_ - layer_->in_time, 0.0,
                std::max(0.0, layer_->out_time - layer_->in_time));
            AnimatedProperty &animated = effect->*property;
            if (effect_property_has_keyframe_at(animated, time)) {
                remove_keyframe_at(animated, time);
            } else {
                double value = animated.evaluate(time);
                if (current_value)
                    value = current_value();
                else if (auto *double_spin = qobject_cast<QDoubleSpinBox *>(field))
                    value = double_spin->value();
                else if (auto *int_spin = qobject_cast<QSpinBox *>(field))
                    value = int_spin->value();
                add_or_replace_keyframe(animated, time, value);
            }
            emit_effect_changed();
            update_bound_controls();
        });
        return row;
    };

    auto wrap_color_keyframe = [this, box, lt](QPushButton *field,
            AnimatedProperty LayerEffect::*a,
            AnimatedProperty LayerEffect::*r,
            AnimatedProperty LayerEffect::*g,
            AnimatedProperty LayerEffect::*b) -> QWidget * {
        if (!field || !a || !r || !g || !b)
            return field;
        auto *row = new QWidget(box);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        layout->addWidget(field, 1);
        auto *button = new QPushButton(QStringLiteral("◇"), row);
        button->setFixedSize(26, 22);
        button->setToolTip(tr("Toggle color keyframe at the current timeline position"));
        layout->addWidget(button);
        keyframe_bindings_.push_back({button,
            [a, r, g, b](const LayerEffect &effect, double time) {
                return effect_property_has_keyframe_at(effect.*a, time) ||
                       effect_property_has_keyframe_at(effect.*r, time) ||
                       effect_property_has_keyframe_at(effect.*g, time) ||
                       effect_property_has_keyframe_at(effect.*b, time);
            }});
        connect(button, &QPushButton::clicked, this,
                [this, field, a, r, g, b]() {
            LayerEffect *effect = selected_effect();
            if (!effect || !layer_)
                return;
            const double time = std::clamp(playhead_ - layer_->in_time, 0.0,
                std::max(0.0, layer_->out_time - layer_->in_time));
            const bool keyed = effect_property_has_keyframe_at(effect->*a, time) ||
                               effect_property_has_keyframe_at(effect->*r, time) ||
                               effect_property_has_keyframe_at(effect->*g, time) ||
                               effect_property_has_keyframe_at(effect->*b, time);
            if (keyed) {
                remove_keyframe_at(effect->*a, time);
                remove_keyframe_at(effect->*r, time);
                remove_keyframe_at(effect->*g, time);
                remove_keyframe_at(effect->*b, time);
            } else {
                const uint32_t argb = field->property("argb").toUInt();
                add_or_replace_keyframe(effect->*a, time, (argb >> 24) & 0xFF);
                add_or_replace_keyframe(effect->*r, time, (argb >> 16) & 0xFF);
                add_or_replace_keyframe(effect->*g, time, (argb >> 8) & 0xFF);
                add_or_replace_keyframe(effect->*b, time, argb & 0xFF);
            }
            emit_effect_changed();
            update_bound_controls();
        });
        return row;
    };

    auto add_effect_row = [this, box, form](const QString &label_text, QWidget *field) {
        if (label_text.isEmpty()) {
            form->addRow(label_text, field);
            return;
        }
        auto *label = new NumericDragLabel(label_text, field, box,
                                           [this]() {
                                                if (loading_values_) return;
                                                numeric_label_dragging_ = true;
                                            },
                                           [this]() {
                                                if (loading_values_) return;
                                                numeric_label_dragging_ = false;
                                                emit property_changed(true);
                                           });
        label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->addRow(label, field);
    };

    auto *effect_enabled = new QCheckBox(box);
    effect_enabled->setChecked(eval_effect_enabled(*selected_effect(), lt));
    bool_bindings_.push_back({effect_enabled,
        [](const LayerEffect &effect, double time) {
            return eval_effect_enabled(effect, time);
        }});
    add_effect_row(bgl_tr("OBSTitles.Enabled"),
                   wrap_scalar_keyframe(effect_enabled,
                                        &LayerEffect::enabled_prop,
                                        [effect_enabled]() {
                                            return effect_enabled->isChecked() ? 1.0 : 0.0;
                                        }));
    connect(effect_enabled, &QCheckBox::toggled, this, [this](bool enabled) {
        if (loading_values_ || !selected_effect())
            return;
        selected_effect()->enabled = enabled;
        set_animated_value(selected_effect()->enabled_prop,
                           current_local_time(), enabled ? 1.0 : 0.0);
        emit_effect_changed();
    });

    const BglEffectExtensionDefinition *extension_definition = nullptr;
    if (!selected_effect()->extension_id.empty()) {
        auto &catalog = BglEffectExtensionCatalog::instance();
        if (catalog.effects().empty()) catalog.reload();
        extension_definition = catalog.find(QString::fromStdString(selected_effect()->extension_id));
    }
    if (extension_definition && !extension_definition->builtIn) {
        LayerEffect *effect = selected_effect();
        QJsonDocument state_doc = QJsonDocument::fromJson(QByteArray::fromStdString(effect->extension_parameters_json));
        QJsonObject state = state_doc.isObject() ? state_doc.object() : extension_definition->defaults;
        auto persist_state = [this, effect](const QJsonObject &value) {
            effect->extension_parameters_json = QJsonDocument(value).toJson(QJsonDocument::Compact).toStdString();
            emit_effect_changed();
        };

        const QJsonArray preset_items = extension_definition->presetIndex.value(QStringLiteral("items")).toArray();
        if (!preset_items.isEmpty()) {
            auto *preset = combo();
            preset->addItem(tr("Custom"), QString());
            for (const auto &value : preset_items) {
                const QJsonObject item = value.toObject();
                preset->addItem(item.value(QStringLiteral("name")).toString(), item.value(QStringLiteral("file")).toString());
            }
            add_effect_row(tr("Preset"), preset);
            connect(preset, QOverload<int>::of(&QComboBox::activated), this,
                    [this, preset, extensionBasePath = extension_definition->basePath](int index) {
                if (loading_values_ || index <= 0 || !selected_effect()) return;
                const QString relative = preset->itemData(index).toString();
                const QString presetPath = QFileInfo(relative).isAbsolute()
                    ? relative : QDir(extensionBasePath).filePath(relative);
                QFile file(presetPath);
                if (!file.open(QIODevice::ReadOnly)) return;
                const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
                if (!doc.isObject()) return;
                const QJsonObject presetObject = doc.object();
                const QJsonObject parameters = presetObject.value(QStringLiteral("parameters")).isObject()
                    ? presetObject.value(QStringLiteral("parameters")).toObject()
                    : presetObject;
                selected_effect()->extension_parameters_json = QJsonDocument(parameters).toJson(QJsonDocument::Compact).toStdString();
                emit_effect_changed();
                load_settings();
            });
        }

        for (auto it = extension_definition->parameterSchema.begin(); it != extension_definition->parameterSchema.end(); ++it) {
            const QString key = it.key();
            const QJsonObject meta = it.value().toObject();
            const QString type = meta.value(QStringLiteral("type")).toString();
            const QString label = meta.value(QStringLiteral("label")).toString(key);
            if (type == QStringLiteral("float") || type == QStringLiteral("int")) {
                auto *field = spin(meta.value(QStringLiteral("min")).toDouble(-100000.0),
                                   meta.value(QStringLiteral("max")).toDouble(100000.0),
                                   meta.value(QStringLiteral("step")).toDouble(type == QStringLiteral("int") ? 1.0 : 0.01));
                field->setDecimals(type == QStringLiteral("int") ? 0 : 3);
                const QJsonValue numericFallback = state.value(key).isUndefined()
                    ? meta.value(QStringLiteral("default")) : state.value(key);
                field->setValue(evaluate_extension_track(*effect, key, lt, numericFallback).toDouble(numericFallback.toDouble()));
                bind_numeric(field, [key, numericFallback](const LayerEffect &current, double time) {
                    return evaluate_extension_track(current, key, time, numericFallback).toDouble(numericFallback.toDouble());
                });
                QWidget *valueWidget = field;
                QPushButton *keyframeButton = nullptr;
                if (meta.value(QStringLiteral("animatable")).toBool(false)) {
                    auto *row = new QWidget(box);
                    auto *layout = new QHBoxLayout(row);
                    layout->setContentsMargins(0, 0, 0, 0);
                    layout->setSpacing(4);
                    layout->addWidget(field, 1);
                    keyframeButton = new QPushButton(QStringLiteral("◇"), row);
                    keyframeButton->setFixedWidth(26);
                    keyframeButton->setToolTip(tr("Toggle extension keyframe at the current timeline position"));
                    layout->addWidget(keyframeButton);
                    keyframe_bindings_.push_back({keyframeButton,
                        [key](const LayerEffect &current, double time) {
                            return extension_track_has_keyframe_at(current, key, time);
                        }});
                    valueWidget = row;
                }
                add_effect_row(label, valueWidget);
                connect(field, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                        [this, key, type](double value) {
                    if (loading_values_ || !selected_effect()) return;
                    const double currentTime = layer_
                        ? std::clamp(playhead_ - layer_->in_time, 0.0,
                                     std::max(0.0, layer_->out_time - layer_->in_time))
                        : 0.0;
                    const QJsonValue encoded = type == QStringLiteral("int") ? QJsonValue(static_cast<int>(value)) : QJsonValue(value);
                    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(selected_effect()->extension_parameters_json));
                    QJsonObject object = doc.isObject() ? doc.object() : QJsonObject{};
                    object.insert(key, encoded);
                    selected_effect()->extension_parameters_json = QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
                    QJsonDocument tracksDoc = QJsonDocument::fromJson(QByteArray::fromStdString(selected_effect()->extension_keyframes_json));
                    QJsonObject tracks = tracksDoc.isObject() ? tracksDoc.object() : QJsonObject{};
                    QJsonArray keys = tracks.value(key).toArray();
                    for (int i = 0; i < keys.size(); ++i) {
                        QJsonObject k = keys.at(i).toObject();
                        if (std::abs(k.value(QStringLiteral("time")).toDouble() - currentTime) < 0.0001) {
                            k.insert(QStringLiteral("value"), encoded); keys.replace(i, k); break;
                        }
                    }
                    tracks.insert(key, keys);
                    selected_effect()->extension_keyframes_json = QJsonDocument(tracks).toJson(QJsonDocument::Compact).toStdString();
                    emit_effect_changed();
                });
                if (keyframeButton) connect(keyframeButton, &QPushButton::clicked, this, [this, key, field, type]() {
                    if (!selected_effect()) return;
                    const double currentTime = layer_
                        ? std::clamp(playhead_ - layer_->in_time, 0.0,
                                     std::max(0.0, layer_->out_time - layer_->in_time))
                        : 0.0;
                    QJsonDocument tracksDoc = QJsonDocument::fromJson(QByteArray::fromStdString(selected_effect()->extension_keyframes_json));
                    QJsonObject tracks = tracksDoc.isObject() ? tracksDoc.object() : QJsonObject{};
                    QJsonArray keys = tracks.value(key).toArray();
                    int existing = -1;
                    for (int i = 0; i < keys.size(); ++i)
                        if (std::abs(keys.at(i).toObject().value(QStringLiteral("time")).toDouble() - currentTime) < 0.0001) { existing = i; break; }
                    if (existing >= 0) keys.removeAt(existing);
                    else {
                        QJsonObject k;
                        k.insert(QStringLiteral("time"), currentTime);
                        k.insert(QStringLiteral("value"), type == QStringLiteral("int") ? QJsonValue(static_cast<int>(field->value())) : QJsonValue(field->value()));
                        k.insert(QStringLiteral("interpolation"), QStringLiteral("linear"));
                        int insertAt = keys.size();
                        for (int i = 0; i < keys.size(); ++i) {
                            if (keys.at(i).toObject().value(QStringLiteral("time")).toDouble() > currentTime) { insertAt = i; break; }
                        }
                        keys.insert(insertAt, k);
                    }
                    tracks.insert(key, keys);
                    selected_effect()->extension_keyframes_json = QJsonDocument(tracks).toJson(QJsonDocument::Compact).toStdString();
                    emit_effect_changed();
                    update_bound_controls();
                });
            } else if (type == QStringLiteral("point")) {
                const QJsonArray current = state.value(key).toArray();
                auto *row = new QWidget(box);
                auto *layout = new QHBoxLayout(row);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->setSpacing(4);
                auto *x = spin(meta.value(QStringLiteral("minX")).toDouble(0.0),
                               meta.value(QStringLiteral("maxX")).toDouble(1.0),
                               meta.value(QStringLiteral("step")).toDouble(0.01));
                auto *y = spin(meta.value(QStringLiteral("minY")).toDouble(0.0),
                               meta.value(QStringLiteral("maxY")).toDouble(1.0),
                               meta.value(QStringLiteral("step")).toDouble(0.01));
                x->setPrefix(QStringLiteral("X "));
                y->setPrefix(QStringLiteral("Y "));
                const QJsonArray evaluatedPoint = evaluate_extension_track(
                    *effect, key, lt, current).toArray();
                x->setValue(evaluatedPoint.size() > 0 ? evaluatedPoint.at(0).toDouble(0.5) : 0.5);
                y->setValue(evaluatedPoint.size() > 1 ? evaluatedPoint.at(1).toDouble(0.5) : 0.5);
                bind_numeric(x, [key, current](const LayerEffect &active, double time) {
                    const QJsonArray value = evaluate_extension_track(active, key, time, current).toArray();
                    return value.size() > 0 ? value.at(0).toDouble(0.5) : 0.5;
                });
                bind_numeric(y, [key, current](const LayerEffect &active, double time) {
                    const QJsonArray value = evaluate_extension_track(active, key, time, current).toArray();
                    return value.size() > 1 ? value.at(1).toDouble(0.5) : 0.5;
                });
                layout->addWidget(x);
                layout->addWidget(y);
                QPushButton *keyframeButton = nullptr;
                if (meta.value(QStringLiteral("animatable")).toBool(false)) {
                    keyframeButton = new QPushButton(QStringLiteral("◇"), row);
                    keyframeButton->setFixedWidth(26);
                    keyframeButton->setToolTip(tr("Toggle extension keyframe at the current timeline position"));
                    layout->addWidget(keyframeButton);
                    keyframe_bindings_.push_back({keyframeButton,
                        [key](const LayerEffect &active, double time) {
                            return extension_track_has_keyframe_at(active, key, time);
                        }});
                }
                add_effect_row(label, row);
                auto writePoint = [this, key, x, y]() {
                    if (loading_values_ || !selected_effect()) return;
                    const double currentTime = layer_
                        ? std::clamp(playhead_ - layer_->in_time, 0.0,
                                     std::max(0.0, layer_->out_time - layer_->in_time))
                        : 0.0;
                    QJsonArray encoded{x->value(), y->value()};
                    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(selected_effect()->extension_parameters_json));
                    QJsonObject object = doc.isObject() ? doc.object() : QJsonObject{};
                    object.insert(key, encoded);
                    selected_effect()->extension_parameters_json = QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
                    QJsonDocument tracksDoc = QJsonDocument::fromJson(QByteArray::fromStdString(selected_effect()->extension_keyframes_json));
                    QJsonObject tracks = tracksDoc.isObject() ? tracksDoc.object() : QJsonObject{};
                    QJsonArray keys = tracks.value(key).toArray();
                    for (int i = 0; i < keys.size(); ++i) {
                        QJsonObject k = keys.at(i).toObject();
                        if (std::abs(k.value(QStringLiteral("time")).toDouble() - currentTime) < 0.0001) {
                            k.insert(QStringLiteral("value"), encoded);
                            keys.replace(i, k);
                            break;
                        }
                    }
                    tracks.insert(key, keys);
                    selected_effect()->extension_keyframes_json = QJsonDocument(tracks).toJson(QJsonDocument::Compact).toStdString();
                    emit_effect_changed();
                };
                connect(x, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [writePoint](double) { writePoint(); });
                connect(y, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [writePoint](double) { writePoint(); });
                if (keyframeButton) connect(keyframeButton, &QPushButton::clicked, this, [this, key, x, y]() {
                    if (!selected_effect()) return;
                    const double currentTime = layer_
                        ? std::clamp(playhead_ - layer_->in_time, 0.0,
                                     std::max(0.0, layer_->out_time - layer_->in_time))
                        : 0.0;
                    QJsonDocument tracksDoc = QJsonDocument::fromJson(QByteArray::fromStdString(selected_effect()->extension_keyframes_json));
                    QJsonObject tracks = tracksDoc.isObject() ? tracksDoc.object() : QJsonObject{};
                    QJsonArray keys = tracks.value(key).toArray();
                    int existing = -1;
                    for (int i = 0; i < keys.size(); ++i)
                        if (std::abs(keys.at(i).toObject().value(QStringLiteral("time")).toDouble() - currentTime) < 0.0001) { existing = i; break; }
                    if (existing >= 0) keys.removeAt(existing);
                    else {
                        QJsonObject k;
                        k.insert(QStringLiteral("time"), currentTime);
                        k.insert(QStringLiteral("value"), QJsonArray{x->value(), y->value()});
                        k.insert(QStringLiteral("interpolation"), QStringLiteral("linear"));
                        int insertAt = keys.size();
                        for (int i = 0; i < keys.size(); ++i) {
                            if (keys.at(i).toObject().value(QStringLiteral("time")).toDouble() > currentTime) { insertAt = i; break; }
                        }
                        keys.insert(insertAt, k);
                    }
                    tracks.insert(key, keys);
                    selected_effect()->extension_keyframes_json = QJsonDocument(tracks).toJson(QJsonDocument::Compact).toStdString();
                    emit_effect_changed();
                    update_bound_controls();
                });
            } else if (type == QStringLiteral("color")) {
                const QJsonValue fallback = state.value(key).isUndefined()
                    ? meta.value(QStringLiteral("default")) : state.value(key);
                const QJsonValue evaluated = evaluate_extension_track(
                    *effect, key, lt, fallback);
                auto *field = color_button(extension_json_color_to_argb(evaluated),
                    [this, key](uint32_t argb) {
                        if (!selected_effect()) return;
                        const QJsonArray encoded = extension_argb_to_json_color(argb);
                        QJsonDocument stateDoc = QJsonDocument::fromJson(
                            QByteArray::fromStdString(selected_effect()->extension_parameters_json));
                        QJsonObject stateObject = stateDoc.isObject()
                            ? stateDoc.object() : QJsonObject{};
                        stateObject.insert(key, encoded);
                        selected_effect()->extension_parameters_json = QJsonDocument(stateObject)
                            .toJson(QJsonDocument::Compact).toStdString();
                        const double time = current_local_time();
                        QJsonDocument tracksDoc = QJsonDocument::fromJson(
                            QByteArray::fromStdString(selected_effect()->extension_keyframes_json));
                        QJsonObject tracks = tracksDoc.isObject()
                            ? tracksDoc.object() : QJsonObject{};
                        QJsonArray keys = tracks.value(key).toArray();
                        for (int i = 0; i < keys.size(); ++i) {
                            QJsonObject keyframe = keys.at(i).toObject();
                            if (std::abs(keyframe.value(QStringLiteral("time")).toDouble() - time) < 0.0001) {
                                keyframe.insert(QStringLiteral("value"), encoded);
                                keys.replace(i, keyframe);
                                tracks.insert(key, keys);
                                selected_effect()->extension_keyframes_json = QJsonDocument(tracks)
                                    .toJson(QJsonDocument::Compact).toStdString();
                                break;
                            }
                        }
                    });
                bind_color(field, [key, fallback](const LayerEffect &active, double time) {
                    return extension_json_color_to_argb(
                        evaluate_extension_track(active, key, time, fallback));
                });
                QWidget *valueWidget = field;
                if (meta.value(QStringLiteral("animatable")).toBool(false)) {
                    auto *row = new QWidget(box);
                    auto *layout = new QHBoxLayout(row);
                    layout->setContentsMargins(0, 0, 0, 0);
                    layout->setSpacing(4);
                    layout->addWidget(field, 1);
                    auto *button = new QPushButton(QStringLiteral("◇"), row);
                    button->setFixedSize(26, 22);
                    button->setToolTip(tr("Toggle extension color keyframe at the current timeline position"));
                    layout->addWidget(button);
                    keyframe_bindings_.push_back({button,
                        [key](const LayerEffect &active, double time) {
                            return extension_track_has_keyframe_at(active, key, time);
                        }});
                    connect(button, &QPushButton::clicked, this,
                            [this, key, field]() {
                        LayerEffect *active = selected_effect();
                        if (!active) return;
                        const double time = current_local_time();
                        QJsonDocument doc = QJsonDocument::fromJson(
                            QByteArray::fromStdString(active->extension_keyframes_json));
                        QJsonObject tracks = doc.isObject() ? doc.object() : QJsonObject{};
                        QJsonArray keys = tracks.value(key).toArray();
                        int found = -1;
                        for (int i = 0; i < keys.size(); ++i) {
                            if (std::abs(keys.at(i).toObject().value(QStringLiteral("time")).toDouble() - time) < 0.0001) {
                                found = i; break;
                            }
                        }
                        if (found >= 0) keys.removeAt(found);
                        else {
                            QJsonObject keyframe{{QStringLiteral("time"), time},
                                                 {QStringLiteral("value"), extension_argb_to_json_color(color_button_argb(field))},
                                                 {QStringLiteral("interpolation"), QStringLiteral("linear")}};
                            int insertAt = keys.size();
                            for (int i = 0; i < keys.size(); ++i) {
                                if (keys.at(i).toObject().value(QStringLiteral("time")).toDouble() > time) {
                                    insertAt = i; break;
                                }
                            }
                            keys.insert(insertAt, keyframe);
                        }
                        tracks.insert(key, keys);
                        active->extension_keyframes_json = QJsonDocument(tracks)
                            .toJson(QJsonDocument::Compact).toStdString();
                        emit_effect_changed();
                        update_bound_controls();
                    });
                    valueWidget = row;
                }
                add_effect_row(label, valueWidget);
            } else if (type == QStringLiteral("bool")) {
                const QJsonValue fallback = state.value(key).isUndefined()
                    ? meta.value(QStringLiteral("default")) : state.value(key);
                auto *field = new QCheckBox(box);
                field->setChecked(evaluate_extension_track(
                    *effect, key, lt, fallback).toBool(fallback.toBool()));
                bool_bindings_.push_back({field,
                    [key, fallback](const LayerEffect &active, double time) {
                        return evaluate_extension_track(active, key, time, fallback)
                            .toBool(fallback.toBool());
                    }});

                QWidget *valueWidget = field;
                QPushButton *keyframeButton = nullptr;
                if (meta.value(QStringLiteral("animatable")).toBool(false)) {
                    auto *row = new QWidget(box);
                    auto *layout = new QHBoxLayout(row);
                    layout->setContentsMargins(0, 0, 0, 0);
                    layout->setSpacing(4);
                    layout->addWidget(field, 1);
                    keyframeButton = new QPushButton(QStringLiteral("◇"), row);
                    keyframeButton->setFixedSize(26, 22);
                    keyframeButton->setToolTip(
                        tr("Toggle extension hold keyframe at the current timeline position"));
                    layout->addWidget(keyframeButton);
                    keyframe_bindings_.push_back({keyframeButton,
                        [key](const LayerEffect &active, double time) {
                            return extension_track_has_keyframe_at(active, key, time);
                        }});
                    valueWidget = row;
                }
                add_effect_row(label, valueWidget);

                connect(field, &QCheckBox::toggled, this,
                        [this, key](bool value) {
                    if (loading_values_ || !selected_effect()) return;
                    LayerEffect *active = selected_effect();
                    QJsonDocument stateDoc = QJsonDocument::fromJson(
                        QByteArray::fromStdString(active->extension_parameters_json));
                    QJsonObject stateObject = stateDoc.isObject()
                        ? stateDoc.object() : QJsonObject{};
                    stateObject.insert(key, value);
                    active->extension_parameters_json = QJsonDocument(stateObject)
                        .toJson(QJsonDocument::Compact).toStdString();

                    const double time = current_local_time();
                    QJsonDocument tracksDoc = QJsonDocument::fromJson(
                        QByteArray::fromStdString(active->extension_keyframes_json));
                    QJsonObject tracks = tracksDoc.isObject()
                        ? tracksDoc.object() : QJsonObject{};
                    QJsonArray keys = tracks.value(key).toArray();
                    for (int i = 0; i < keys.size(); ++i) {
                        QJsonObject keyframe = keys.at(i).toObject();
                        if (std::abs(keyframe.value(QStringLiteral("time")).toDouble() - time) < 0.0001) {
                            keyframe.insert(QStringLiteral("value"), value);
                            keyframe.insert(QStringLiteral("interpolation"), QStringLiteral("hold"));
                            keys.replace(i, keyframe);
                            break;
                        }
                    }
                    tracks.insert(key, keys);
                    active->extension_keyframes_json = QJsonDocument(tracks)
                        .toJson(QJsonDocument::Compact).toStdString();
                    emit_effect_changed();
                });

                if (keyframeButton) {
                    connect(keyframeButton, &QPushButton::clicked, this,
                            [this, key, field]() {
                        LayerEffect *active = selected_effect();
                        if (!active) return;
                        const double time = current_local_time();
                        QJsonDocument doc = QJsonDocument::fromJson(
                            QByteArray::fromStdString(active->extension_keyframes_json));
                        QJsonObject tracks = doc.isObject() ? doc.object() : QJsonObject{};
                        QJsonArray keys = tracks.value(key).toArray();
                        int found = -1;
                        for (int i = 0; i < keys.size(); ++i) {
                            if (std::abs(keys.at(i).toObject()
                                             .value(QStringLiteral("time")).toDouble() - time) < 0.0001) {
                                found = i;
                                break;
                            }
                        }
                        if (found >= 0) {
                            keys.removeAt(found);
                        } else {
                            QJsonObject keyframe{
                                {QStringLiteral("time"), time},
                                {QStringLiteral("value"), field->isChecked()},
                                {QStringLiteral("interpolation"), QStringLiteral("hold")}};
                            int insertAt = keys.size();
                            for (int i = 0; i < keys.size(); ++i) {
                                if (keys.at(i).toObject()
                                        .value(QStringLiteral("time")).toDouble() > time) {
                                    insertAt = i;
                                    break;
                                }
                            }
                            keys.insert(insertAt, keyframe);
                        }
                        tracks.insert(key, keys);
                        active->extension_keyframes_json = QJsonDocument(tracks)
                            .toJson(QJsonDocument::Compact).toStdString();
                        emit_effect_changed();
                        update_bound_controls();
                    });
                }
            } else if (type == QStringLiteral("enum")) {
                const QJsonValue fallback = state.value(key).isUndefined()
                    ? meta.value(QStringLiteral("default")) : state.value(key);
                auto *field = combo();
                for (const QJsonValue &option : meta.value(QStringLiteral("options")).toArray()) {
                    if (option.isObject()) {
                        const QJsonObject object = option.toObject();
                        field->addItem(object.value(QStringLiteral("label")).toString(),
                                       object.value(QStringLiteral("value")).toVariant());
                    } else {
                        field->addItem(option.toString(), option.toVariant());
                    }
                }
                const QVariant evaluated = evaluate_extension_track(
                    *effect, key, lt, fallback).toVariant();
                int initialIndex = field->findData(evaluated);
                if (initialIndex < 0 && field->count() > 0)
                    initialIndex = 0;
                field->setCurrentIndex(initialIndex);
                combo_bindings_.push_back({field,
                    [key, fallback](const LayerEffect &active, double time) {
                        return evaluate_extension_track(active, key, time, fallback).toVariant();
                    }});

                QWidget *valueWidget = field;
                QPushButton *keyframeButton = nullptr;
                if (meta.value(QStringLiteral("animatable")).toBool(false)) {
                    auto *row = new QWidget(box);
                    auto *layout = new QHBoxLayout(row);
                    layout->setContentsMargins(0, 0, 0, 0);
                    layout->setSpacing(4);
                    layout->addWidget(field, 1);
                    keyframeButton = new QPushButton(QStringLiteral("◇"), row);
                    keyframeButton->setFixedSize(26, 22);
                    keyframeButton->setToolTip(
                        tr("Toggle extension hold keyframe at the current timeline position"));
                    layout->addWidget(keyframeButton);
                    keyframe_bindings_.push_back({keyframeButton,
                        [key](const LayerEffect &active, double time) {
                            return extension_track_has_keyframe_at(active, key, time);
                        }});
                    valueWidget = row;
                }
                add_effect_row(label, valueWidget);

                connect(field, QOverload<int>::of(&QComboBox::activated), this,
                        [this, field, key](int) {
                    if (loading_values_ || !selected_effect()) return;
                    LayerEffect *active = selected_effect();
                    const QJsonValue encoded = QJsonValue::fromVariant(field->currentData());
                    QJsonDocument stateDoc = QJsonDocument::fromJson(
                        QByteArray::fromStdString(active->extension_parameters_json));
                    QJsonObject stateObject = stateDoc.isObject()
                        ? stateDoc.object() : QJsonObject{};
                    stateObject.insert(key, encoded);
                    active->extension_parameters_json = QJsonDocument(stateObject)
                        .toJson(QJsonDocument::Compact).toStdString();

                    const double time = current_local_time();
                    QJsonDocument tracksDoc = QJsonDocument::fromJson(
                        QByteArray::fromStdString(active->extension_keyframes_json));
                    QJsonObject tracks = tracksDoc.isObject()
                        ? tracksDoc.object() : QJsonObject{};
                    QJsonArray keys = tracks.value(key).toArray();
                    for (int i = 0; i < keys.size(); ++i) {
                        QJsonObject keyframe = keys.at(i).toObject();
                        if (std::abs(keyframe.value(QStringLiteral("time")).toDouble() - time) < 0.0001) {
                            keyframe.insert(QStringLiteral("value"), encoded);
                            keyframe.insert(QStringLiteral("interpolation"), QStringLiteral("hold"));
                            keys.replace(i, keyframe);
                            break;
                        }
                    }
                    tracks.insert(key, keys);
                    active->extension_keyframes_json = QJsonDocument(tracks)
                        .toJson(QJsonDocument::Compact).toStdString();
                    emit_effect_changed();
                });

                if (keyframeButton) {
                    connect(keyframeButton, &QPushButton::clicked, this,
                            [this, key, field]() {
                        LayerEffect *active = selected_effect();
                        if (!active || field->currentIndex() < 0) return;
                        const double time = current_local_time();
                        QJsonDocument doc = QJsonDocument::fromJson(
                            QByteArray::fromStdString(active->extension_keyframes_json));
                        QJsonObject tracks = doc.isObject() ? doc.object() : QJsonObject{};
                        QJsonArray keys = tracks.value(key).toArray();
                        int found = -1;
                        for (int i = 0; i < keys.size(); ++i) {
                            if (std::abs(keys.at(i).toObject()
                                             .value(QStringLiteral("time")).toDouble() - time) < 0.0001) {
                                found = i;
                                break;
                            }
                        }
                        if (found >= 0) {
                            keys.removeAt(found);
                        } else {
                            QJsonObject keyframe{
                                {QStringLiteral("time"), time},
                                {QStringLiteral("value"), QJsonValue::fromVariant(field->currentData())},
                                {QStringLiteral("interpolation"), QStringLiteral("hold")}};
                            int insertAt = keys.size();
                            for (int i = 0; i < keys.size(); ++i) {
                                if (keys.at(i).toObject()
                                        .value(QStringLiteral("time")).toDouble() > time) {
                                    insertAt = i;
                                    break;
                                }
                            }
                            keys.insert(insertAt, keyframe);
                        }
                        tracks.insert(key, keys);
                        active->extension_keyframes_json = QJsonDocument(tracks)
                            .toJson(QJsonDocument::Compact).toStdString();
                        emit_effect_changed();
                        update_bound_controls();
                    });
                }
            }
        }

        if (extension_definition->capabilities.value(QStringLiteral("compoundGraph")).toBool()) {
            auto *studio = new QWidget(box);
            auto *studioLayout = new QVBoxLayout(studio);
            studioLayout->setContentsMargins(0, 0, 0, 0);
            studioLayout->setSpacing(6);

            auto *hint = new QLabel(tr("Build the flare from optical elements. Select an element to edit it; drag the main Light Position above to animate the complete flare."), studio);
            hint->setWordWrap(true);
            studioLayout->addWidget(hint);

            auto *elementsList = new QListWidget(studio);
            elementsList->setMinimumHeight(145);
            elementsList->setSelectionMode(QAbstractItemView::SingleSelection);
            studioLayout->addWidget(elementsList);

            auto *toolbar = new QWidget(studio);
            auto *toolbarLayout = new QHBoxLayout(toolbar);
            toolbarLayout->setContentsMargins(0, 0, 0, 0);
            toolbarLayout->setSpacing(4);
            auto *addElement = new QPushButton(tr("Add"), toolbar);
            auto *duplicateElement = new QPushButton(tr("Duplicate"), toolbar);
            auto *removeElement = new QPushButton(tr("Remove"), toolbar);
            auto *moveUp = new QPushButton(QStringLiteral("↑"), toolbar);
            auto *moveDown = new QPushButton(QStringLiteral("↓"), toolbar);
            toolbarLayout->addWidget(addElement);
            toolbarLayout->addWidget(duplicateElement);
            toolbarLayout->addWidget(removeElement);
            toolbarLayout->addStretch(1);
            toolbarLayout->addWidget(moveUp);
            toolbarLayout->addWidget(moveDown);
            studioLayout->addWidget(toolbar);

            auto *properties = new QGroupBox(tr("Selected element"), studio);
            auto *propertiesForm = new QFormLayout(properties);
            propertiesForm->setContentsMargins(8, 8, 8, 8);
            propertiesForm->setSpacing(5);
            auto *elementType = combo();
            elementType->addItem(tr("Glow / Disc"), 0);
            elementType->addItem(tr("Ring"), 1);
            elementType->addItem(tr("Polygon Ghost"), 2);
            elementType->addItem(tr("Anamorphic Streak"), 3);
            elementType->addItem(tr("Soft Iris"), 4);
            auto *elementPosition = spin(-2.0, 2.0, 0.01);
            auto *elementSize = spin(0.001, 2.0, 0.01);
            auto *elementOpacity = spin(0.0, 5.0, 0.01);
            auto *elementSoftness = spin(0.0, 2.0, 0.01);
            auto *elementAspect = spin(0.005, 20.0, 0.01);
            auto *elementRotation = spin(-360.0, 360.0, 1.0);
            auto *elementColor = new QPushButton(properties);
            elementColor->setFixedHeight(22);
            auto element_keyframe_row = [this, elementsList, properties](
                    QWidget *field, const QString &property,
                    std::function<QJsonValue()> value) -> QWidget * {
                auto *row = new QWidget(properties);
                auto *layout = new QHBoxLayout(row);
                layout->setContentsMargins(0, 0, 0, 0);
                layout->setSpacing(4);
                layout->addWidget(field, 1);
                auto *button = new QPushButton(QStringLiteral("◇"), row);
                button->setFixedSize(26, 22);
                button->setToolTip(tr("Toggle element keyframe at the current timeline position"));
                layout->addWidget(button);
                keyframe_bindings_.push_back({button,
                    [elementsList, property](const LayerEffect &effect, double time) {
                        const int index = elementsList->currentRow();
                        return index >= 0 && extension_track_has_keyframe_at(
                            effect, QStringLiteral("elements.%1.%2").arg(index).arg(property), time);
                    }});
                connect(button, &QPushButton::clicked, this,
                        [this, elementsList, property, value]() {
                    LayerEffect *effect = selected_effect();
                    const int index = elementsList->currentRow();
                    if (!effect || index < 0) return;
                    const double time = current_local_time();
                    const QString path = QStringLiteral("elements.%1.%2").arg(index).arg(property);
                    const QJsonDocument doc = QJsonDocument::fromJson(
                        QByteArray::fromStdString(effect->extension_keyframes_json));
                    QJsonObject tracks = doc.isObject() ? doc.object() : QJsonObject{};
                    QJsonArray keys = tracks.value(path).toArray();
                    int found = -1;
                    for (int i = 0; i < keys.size(); ++i) {
                        if (std::abs(keys.at(i).toObject().value(QStringLiteral("time")).toDouble() - time) < 0.0001) {
                            found = i; break;
                        }
                    }
                    if (found >= 0) keys.removeAt(found);
                    else {
                        QJsonObject keyframe{{QStringLiteral("time"), time},
                                             {QStringLiteral("value"), value()},
                                             {QStringLiteral("interpolation"), QStringLiteral("linear")}};
                        int insert_at = keys.size();
                        for (int i = 0; i < keys.size(); ++i) {
                            if (keys.at(i).toObject().value(QStringLiteral("time")).toDouble() > time) {
                                insert_at = i; break;
                            }
                        }
                        keys.insert(insert_at, keyframe);
                    }
                    tracks.insert(path, keys);
                    effect->extension_keyframes_json = QJsonDocument(tracks).toJson(QJsonDocument::Compact).toStdString();
                    emit_effect_changed();
                    update_bound_controls();
                });
                return row;
            };
            auto element_animated_value = [elementsList](
                    const LayerEffect &effect, const QString &property,
                    double time, double fallback) {
                const int index = elementsList->currentRow();
                if (index < 0) return fallback;
                const QString path = QStringLiteral("elements.%1.%2").arg(index).arg(property);
                const QJsonValue base = extension_state_path_value(effect, path);
                return evaluate_extension_track(effect, path, time, base).toDouble(fallback);
            };
            bind_numeric(elementPosition, [element_animated_value](const LayerEffect &effect, double time) {
                return element_animated_value(effect, QStringLiteral("position"), time, 0.0);
            });
            bind_numeric(elementSize, [element_animated_value](const LayerEffect &effect, double time) {
                return element_animated_value(effect, QStringLiteral("size"), time, 0.1);
            });
            bind_numeric(elementOpacity, [element_animated_value](const LayerEffect &effect, double time) {
                return element_animated_value(effect, QStringLiteral("opacity"), time, 1.0);
            });
            bind_numeric(elementSoftness, [element_animated_value](const LayerEffect &effect, double time) {
                return element_animated_value(effect, QStringLiteral("softness"), time, 0.25);
            });
            bind_numeric(elementAspect, [element_animated_value](const LayerEffect &effect, double time) {
                return element_animated_value(effect, QStringLiteral("aspect"), time, 1.0);
            });
            bind_numeric(elementRotation, [element_animated_value](const LayerEffect &effect, double time) {
                return element_animated_value(effect, QStringLiteral("rotation"), time, 0.0);
            });
            bind_color(elementColor, [elementsList](const LayerEffect &effect, double time) {
                const int index = elementsList->currentRow();
                if (index < 0) return uint32_t{0xFFFFFFFFu};
                const QString path = QStringLiteral("elements.%1.color").arg(index);
                const QJsonValue base = extension_state_path_value(effect, path);
                const QJsonArray value = evaluate_extension_track(effect, path, time, base).toArray();
                QColor color;
                color.setRgbF(value.size() > 0 ? value.at(0).toDouble(1.0) : 1.0,
                              value.size() > 1 ? value.at(1).toDouble(1.0) : 1.0,
                              value.size() > 2 ? value.at(2).toDouble(1.0) : 1.0,
                              value.size() > 3 ? value.at(3).toDouble(1.0) : 1.0);
                return argb_from_color(color);
            });
            propertiesForm->addRow(tr("Type"), elementType);
            propertiesForm->addRow(tr("Axis Position"), element_keyframe_row(elementPosition, QStringLiteral("position"), [elementPosition]() { return QJsonValue(elementPosition->value()); }));
            propertiesForm->addRow(tr("Size"), element_keyframe_row(elementSize, QStringLiteral("size"), [elementSize]() { return QJsonValue(elementSize->value()); }));
            propertiesForm->addRow(tr("Brightness"), element_keyframe_row(elementOpacity, QStringLiteral("opacity"), [elementOpacity]() { return QJsonValue(elementOpacity->value()); }));
            propertiesForm->addRow(tr("Softness"), element_keyframe_row(elementSoftness, QStringLiteral("softness"), [elementSoftness]() { return QJsonValue(elementSoftness->value()); }));
            propertiesForm->addRow(tr("Aspect"), element_keyframe_row(elementAspect, QStringLiteral("aspect"), [elementAspect]() { return QJsonValue(elementAspect->value()); }));
            propertiesForm->addRow(tr("Rotation"), element_keyframe_row(elementRotation, QStringLiteral("rotation"), [elementRotation]() { return QJsonValue(elementRotation->value()); }));
            propertiesForm->addRow(tr("Color"), element_keyframe_row(elementColor, QStringLiteral("color"), [elementColor]() {
                const QColor color = color_from_argb(color_button_argb(elementColor));
                return QJsonValue(QJsonArray{color.redF(), color.greenF(), color.blueF(), color.alphaF()});
            }));
            studioLayout->addWidget(properties);
            add_effect_row(tr("Flare Designer"), studio);

            auto readState = [this]() {
                if (!selected_effect()) return QJsonObject{};
                const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(selected_effect()->extension_parameters_json));
                return doc.isObject() ? doc.object() : QJsonObject{};
            };
            auto writeState = [this](const QJsonObject &object) {
                if (!selected_effect()) return;
                selected_effect()->extension_parameters_json = QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
                emit_effect_changed();
            };
            auto elementName = [this](const QJsonObject &element, int index) {
                static const char *names[] = {"Glow / Disc", "Ring", "Polygon Ghost", "Anamorphic Streak", "Soft Iris"};
                const int type = std::clamp(element.value(QStringLiteral("type")).toInt(), 0, 4);
                return tr("%1. %2").arg(index + 1).arg(tr(names[type]));
            };
            auto rebuildList = [elementsList, readState, elementName](int preferredRow) {
                const QJsonArray elements = readState().value(QStringLiteral("elements")).toArray();
                QSignalBlocker blocker(elementsList);
                elementsList->clear();
                for (int i = 0; i < elements.size(); ++i)
                    elementsList->addItem(elementName(elements.at(i).toObject(), i));
                if (!elements.isEmpty()) {
                    const int lastRow = static_cast<int>(elements.size()) - 1;
                    elementsList->setCurrentRow(std::clamp(preferredRow, 0, lastRow));
                }
            };
            auto loadElement = [=](int row) {
                const QJsonArray elements = readState().value(QStringLiteral("elements")).toArray();
                const bool valid = row >= 0 && row < elements.size();
                properties->setEnabled(valid);
                if (!valid) return;
                const QJsonObject element = elements.at(row).toObject();
                QSignalBlocker b0(elementType), b1(elementPosition), b2(elementSize), b3(elementOpacity), b4(elementSoftness), b5(elementAspect), b6(elementRotation);
                elementType->setCurrentIndex(std::max(0, elementType->findData(element.value(QStringLiteral("type")).toInt(0))));
                elementPosition->setValue(element.value(QStringLiteral("position")).toDouble(0.0));
                elementSize->setValue(element.value(QStringLiteral("size")).toDouble(0.1));
                elementOpacity->setValue(element.value(QStringLiteral("opacity")).toDouble(1.0));
                elementSoftness->setValue(element.value(QStringLiteral("softness")).toDouble(0.25));
                elementAspect->setValue(element.value(QStringLiteral("aspect")).toDouble(1.0));
                elementRotation->setValue(element.value(QStringLiteral("rotation")).toDouble(0.0));
                const QJsonArray color = element.value(QStringLiteral("color")).toArray();
                QColor qcolor;
                qcolor.setRgbF(color.size() > 0 ? color.at(0).toDouble(1.0) : 1.0,
                               color.size() > 1 ? color.at(1).toDouble(1.0) : 1.0,
                               color.size() > 2 ? color.at(2).toDouble(1.0) : 1.0,
                               color.size() > 3 ? color.at(3).toDouble(1.0) : 1.0);
                set_color_button_argb(elementColor, argb_from_color(qcolor));
                update_bound_controls();
            };
            auto updateElement = [=](const QString &key, const QJsonValue &value) {
                const int row = elementsList->currentRow();
                QJsonObject object = readState();
                QJsonArray elements = object.value(QStringLiteral("elements")).toArray();
                if (row < 0 || row >= elements.size()) return;
                QJsonObject element = elements.at(row).toObject();
                element.insert(key, value);
                elements.replace(row, element);
                object.insert(QStringLiteral("elements"), elements);
                writeState(object);
                if (LayerEffect *effect = selected_effect()) {
                    const double time = current_local_time();
                    const QString path = QStringLiteral("elements.%1.%2").arg(row).arg(key);
                    const QJsonDocument doc = QJsonDocument::fromJson(
                        QByteArray::fromStdString(effect->extension_keyframes_json));
                    QJsonObject tracks = doc.isObject() ? doc.object() : QJsonObject{};
                    QJsonArray keys = tracks.value(path).toArray();
                    for (int i = 0; i < keys.size(); ++i) {
                        QJsonObject keyframe = keys.at(i).toObject();
                        if (std::abs(keyframe.value(QStringLiteral("time")).toDouble() - time) < 0.0001) {
                            keyframe.insert(QStringLiteral("value"), value);
                            keys.replace(i, keyframe);
                            tracks.insert(path, keys);
                            effect->extension_keyframes_json = QJsonDocument(tracks).toJson(QJsonDocument::Compact).toStdString();
                            break;
                        }
                    }
                }
                rebuildList(row);
                update_bound_controls();
            };

            connect(elementsList, &QListWidget::currentRowChanged, this, loadElement);
            connect(elementType, QOverload<int>::of(&QComboBox::activated), this, [=](int) { updateElement(QStringLiteral("type"), elementType->currentData().toInt()); });
            connect(elementPosition, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double v) { if (!loading_values_) updateElement(QStringLiteral("position"), v); });
            connect(elementSize, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double v) { if (!loading_values_) updateElement(QStringLiteral("size"), v); });
            connect(elementOpacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double v) { if (!loading_values_) updateElement(QStringLiteral("opacity"), v); });
            connect(elementSoftness, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double v) { if (!loading_values_) updateElement(QStringLiteral("softness"), v); });
            connect(elementAspect, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double v) { if (!loading_values_) updateElement(QStringLiteral("aspect"), v); });
            connect(elementRotation, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [=](double v) { if (!loading_values_) updateElement(QStringLiteral("rotation"), v); });
            connect(elementColor, &QPushButton::clicked, this, [=]() {
                const QColor current = color_from_argb(color_button_argb(elementColor));
                const QColor picked = bgl_pick_color(current, this, tr("Element color"));
                if (!picked.isValid()) return;
                set_color_button_argb(elementColor, argb_from_color(picked));
                QJsonArray encoded; encoded.append(picked.redF()); encoded.append(picked.greenF()); encoded.append(picked.blueF()); encoded.append(picked.alphaF());
                updateElement(QStringLiteral("color"), encoded);
            });
            connect(addElement, &QPushButton::clicked, this, [=]() {
                QJsonObject object = readState();
                QJsonArray elements = object.value(QStringLiteral("elements")).toArray();
                if (elements.size() >= 16) return;
                QJsonObject element{{QStringLiteral("type"), 0}, {QStringLiteral("position"), 0.0}, {QStringLiteral("size"), 0.1}, {QStringLiteral("opacity"), 1.0}, {QStringLiteral("softness"), 0.35}, {QStringLiteral("aspect"), 1.0}, {QStringLiteral("rotation"), 0.0}};
                element.insert(QStringLiteral("color"), QJsonArray{1.0, 0.8, 0.55, 1.0});
                elements.append(element); object.insert(QStringLiteral("elements"), elements); writeState(object); rebuildList(elements.size() - 1);
            });
            connect(duplicateElement, &QPushButton::clicked, this, [=]() {
                QJsonObject object = readState(); QJsonArray elements = object.value(QStringLiteral("elements")).toArray(); const int row = elementsList->currentRow();
                if (row < 0 || row >= elements.size() || elements.size() >= 16) return;
                elements.insert(row + 1, elements.at(row)); object.insert(QStringLiteral("elements"), elements); writeState(object); rebuildList(row + 1);
            });
            connect(removeElement, &QPushButton::clicked, this, [=]() {
                QJsonObject object = readState(); QJsonArray elements = object.value(QStringLiteral("elements")).toArray(); const int row = elementsList->currentRow();
                if (row < 0 || row >= elements.size()) return;
                elements.removeAt(row);
                object.insert(QStringLiteral("elements"), elements);
                writeState(object);
                const int lastRow = static_cast<int>(elements.size()) - 1;
                rebuildList(std::min(row, lastRow));
            });
            auto moveElement = [=](int delta) {
                QJsonObject object = readState(); QJsonArray elements = object.value(QStringLiteral("elements")).toArray(); const int row = elementsList->currentRow(); const int target = row + delta;
                if (row < 0 || target < 0 || target >= elements.size()) return; const QJsonValue value = elements.at(row); elements.removeAt(row); elements.insert(target, value); object.insert(QStringLiteral("elements"), elements); writeState(object); rebuildList(target);
            };
            connect(moveUp, &QPushButton::clicked, this, [=]() { moveElement(-1); });
            connect(moveDown, &QPushButton::clicked, this, [=]() { moveElement(1); });
            rebuildList(0);
            loadElement(elementsList->currentRow());
        }
    } else if (selected_effect()->type == LayerEffectType::BackgroundColor) {
        LayerEffect *effect = selected_effect();
        auto section_label = [box](const QString &text) {
            auto *label = new QLabel(text, box);
            QFont f = label->font();
            f.setBold(true);
            label->setFont(f);
            return label;
        };
        auto *fill = combo();
        fill->addItem(bgl_tr("OBSTitles.Solid"), 0);
        fill->addItem(bgl_tr("OBSTitles.Gradient"), 1);
        fill->setCurrentIndex(fill->findData(effect->effect_fill_type));
        auto *fill_color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){
            if (!selected_effect()) return;
            selected_effect()->effect_color = argb;
            set_effect_color_channels_at(*selected_effect(), current_local_time(), argb);
        });
        auto *opacity = spin(0.0, 1.0, 0.05);
        opacity->setDecimals(2);
        opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);

        auto *stroke_color = color_button(panel_eval_effect_stroke_color(*effect, lt), [this, lt](uint32_t argb){
            if (!selected_effect()) return;
            selected_effect()->effect_stroke_color = argb;
            set_effect_stroke_color_channels_at(*selected_effect(), current_local_time(), argb);
        });
        auto *stroke_width = spin(0.0, 1000.0, 1.0);
        stroke_width->setValue(effect->stroke_width_prop.is_animated() ? effect->stroke_width_prop.evaluate(lt) : effect->effect_stroke_width);
        auto *stroke_opacity = spin(0.0, 1.0, 0.05);
        stroke_opacity->setDecimals(2);
        stroke_opacity->setValue(effect->stroke_opacity_prop.is_animated() ? effect->stroke_opacity_prop.evaluate(lt) : effect->effect_stroke_opacity);

        auto *pad_left = spin(-1000.0, 1000.0, 1.0); pad_left->setValue(effect->padding_left_prop.is_animated() ? effect->padding_left_prop.evaluate(lt) : effect->effect_padding_left);
        auto *pad_right = spin(-1000.0, 1000.0, 1.0); pad_right->setValue(effect->padding_right_prop.is_animated() ? effect->padding_right_prop.evaluate(lt) : effect->effect_padding_right);
        auto *pad_top = spin(-1000.0, 1000.0, 1.0); pad_top->setValue(effect->padding_top_prop.is_animated() ? effect->padding_top_prop.evaluate(lt) : effect->effect_padding_top);
        auto *pad_bottom = spin(-1000.0, 1000.0, 1.0); pad_bottom->setValue(effect->padding_bottom_prop.is_animated() ? effect->padding_bottom_prop.evaluate(lt) : effect->effect_padding_bottom);

        auto *corner_tl = spin(0.0, 1000.0, 1.0); corner_tl->setValue(effect->corner_radius_tl_prop.is_animated() ? effect->corner_radius_tl_prop.evaluate(lt) : effect->effect_corner_radius_tl);
        auto *corner_tr = spin(0.0, 1000.0, 1.0); corner_tr->setValue(effect->corner_radius_tr_prop.is_animated() ? effect->corner_radius_tr_prop.evaluate(lt) : effect->effect_corner_radius_tr);
        auto *corner_br = spin(0.0, 1000.0, 1.0); corner_br->setValue(effect->corner_radius_br_prop.is_animated() ? effect->corner_radius_br_prop.evaluate(lt) : effect->effect_corner_radius_br);
        auto *corner_bl = spin(0.0, 1000.0, 1.0); corner_bl->setValue(effect->corner_radius_bl_prop.is_animated() ? effect->corner_radius_bl_prop.evaluate(lt) : effect->effect_corner_radius_bl);
        auto *corner_row = new QWidget(box);
        auto *corner_grid = new QGridLayout(corner_row);
        corner_grid->setContentsMargins(0, 0, 0, 0);
        corner_grid->setHorizontalSpacing(6);
        corner_grid->setVerticalSpacing(4);
        corner_grid->addWidget(new QLabel(bgl_tr("OBSTitles.TL"), corner_row), 0, 0);
        corner_grid->addWidget(wrap_scalar_keyframe(corner_tl, &LayerEffect::corner_radius_tl_prop), 0, 1);
        corner_grid->addWidget(new QLabel(bgl_tr("OBSTitles.TR"), corner_row), 0, 2);
        corner_grid->addWidget(wrap_scalar_keyframe(corner_tr, &LayerEffect::corner_radius_tr_prop), 0, 3);
        corner_grid->addWidget(new QLabel(bgl_tr("OBSTitles.BL"), corner_row), 1, 0);
        corner_grid->addWidget(wrap_scalar_keyframe(corner_bl, &LayerEffect::corner_radius_bl_prop), 1, 1);
        corner_grid->addWidget(new QLabel(bgl_tr("OBSTitles.BR"), corner_row), 1, 2);
        corner_grid->addWidget(wrap_scalar_keyframe(corner_br, &LayerEffect::corner_radius_br_prop), 1, 3);

        auto *corner_type_row = new QWidget(box);
        auto *corner_type_layout = new QHBoxLayout(corner_type_row);
        corner_type_layout->setContentsMargins(0, 0, 0, 0);
        corner_type_layout->setSpacing(4);
        auto *corner_group = new QButtonGroup(corner_type_row);
        corner_group->setExclusive(true);
        auto make_corner_button = [corner_type_row, corner_group, corner_type_layout](CornerType type, const char *text, const QString &tip) {
            auto *button = new QToolButton(corner_type_row);
            button->setText(QString::fromLatin1(text));
            button->setToolTip(tip);
            button->setCheckable(true);
            button->setFixedSize(26, 22);
            button->setStyleSheet(bgl_theme_control_style());
            corner_group->addButton(button, (int)type);
            corner_type_layout->addWidget(button);
        };
        make_corner_button(CornerType::Round, "R", bgl_tr("OBSTitles.Round"));
        make_corner_button(CornerType::Straight, "B", bgl_tr("OBSTitles.BevelStraight"));
        make_corner_button(CornerType::Concave, "I", bgl_tr("OBSTitles.InverseConcave"));
        make_corner_button(CornerType::Cutout, "C", bgl_tr("OBSTitles.InsetCutout"));
        corner_type_layout->addStretch(1);
        if (auto *button = corner_group->button(effect->effect_corner_type))
            button->setChecked(true);

        auto *grad_type = combo();
        grad_type->addItem(bgl_tr("OBSTitles.LinearGradient"), 0);
        grad_type->addItem(bgl_tr("OBSTitles.RadialGradient"), 1);
        grad_type->addItem(bgl_tr("OBSTitles.ConicalGradient"), 2);
        grad_type->setCurrentIndex(std::max(0, grad_type->findData(effect->effect_gradient_type)));
        auto *grad_spread = combo();
        grad_spread->addItem(bgl_tr("OBSTitles.No"), 0);
        grad_spread->addItem(bgl_tr("OBSTitles.Repeat"), 2);
        grad_spread->addItem(bgl_tr("OBSTitles.Reflect"), 1);
        grad_spread->setCurrentIndex(std::max(0, grad_spread->findData(effect->effect_gradient_spread)));
        auto *grad_start = color_button(panel_eval_gradient_start_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_gradient_start_color = argb; set_gradient_start_color_channels_at(*selected_effect(), current_local_time(), argb); } });
        auto *grad_end = color_button(panel_eval_gradient_end_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_gradient_end_color = argb; set_gradient_end_color_channels_at(*selected_effect(), current_local_time(), argb); } });
        auto *grad_start_pos = spin(0.0, 1.0, 0.01); grad_start_pos->setDecimals(3); grad_start_pos->setValue(panel_eval_effect_property(effect->gradient_start_pos_prop, effect->effect_gradient_start_pos, lt));
        auto *grad_end_pos = spin(0.0, 1.0, 0.01); grad_end_pos->setDecimals(3); grad_end_pos->setValue(panel_eval_effect_property(effect->gradient_end_pos_prop, effect->effect_gradient_end_pos, lt));
        auto *grad_start_opacity = spin(0.0, 1.0, 0.01); grad_start_opacity->setDecimals(3); grad_start_opacity->setValue(panel_eval_effect_property(effect->gradient_start_opacity_prop, effect->effect_gradient_start_opacity, lt));
        auto *grad_end_opacity = spin(0.0, 1.0, 0.01); grad_end_opacity->setDecimals(3); grad_end_opacity->setValue(panel_eval_effect_property(effect->gradient_end_opacity_prop, effect->effect_gradient_end_opacity, lt));
        auto *grad_opacity = spin(0.0, 1.0, 0.01); grad_opacity->setDecimals(3); grad_opacity->setValue(panel_eval_effect_property(effect->gradient_opacity_prop, effect->effect_gradient_opacity, lt));
        auto *grad_angle = spin(-360.0, 360.0, 1.0); grad_angle->setValue(panel_eval_effect_property(effect->gradient_angle_prop, effect->effect_gradient_angle, lt));
        auto *grad_center_x = spin(-100.0, 100.0, 0.01); grad_center_x->setDecimals(3); grad_center_x->setValue(panel_eval_effect_property(effect->gradient_center_x_prop, effect->effect_gradient_center_x, lt));
        auto *grad_center_y = spin(-100.0, 100.0, 0.01); grad_center_y->setDecimals(3); grad_center_y->setValue(panel_eval_effect_property(effect->gradient_center_y_prop, effect->effect_gradient_center_y, lt));
        auto *grad_scale = spin(0.01, 100.0, 0.01); grad_scale->setDecimals(3); grad_scale->setValue(panel_eval_effect_property(effect->gradient_scale_prop, effect->effect_gradient_scale, lt));
        auto *grad_focal_x = spin(-100.0, 100.0, 0.01); grad_focal_x->setDecimals(3); grad_focal_x->setValue(panel_eval_effect_property(effect->gradient_focal_x_prop, effect->effect_gradient_focal_x, lt));
        auto *grad_focal_y = spin(-100.0, 100.0, 0.01); grad_focal_y->setDecimals(3); grad_focal_y->setValue(panel_eval_effect_property(effect->gradient_focal_y_prop, effect->effect_gradient_focal_y, lt));

        bind_color(fill_color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_color(effect, t);
        });
        bind_color(stroke_color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_stroke_color(effect, t);
        });
        bind_color(grad_start, [](const LayerEffect &effect, double t) {
            return panel_eval_gradient_start_color(effect, t);
        });
        bind_color(grad_end, [](const LayerEffect &effect, double t) {
            return panel_eval_gradient_end_color(effect, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(stroke_width, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.stroke_width_prop, effect.effect_stroke_width, t);
        });
        bind_numeric(stroke_opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.stroke_opacity_prop, effect.effect_stroke_opacity, t);
        });
        bind_numeric(pad_left, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.padding_left_prop, effect.effect_padding_left, t);
        });
        bind_numeric(pad_right, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.padding_right_prop, effect.effect_padding_right, t);
        });
        bind_numeric(pad_top, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.padding_top_prop, effect.effect_padding_top, t);
        });
        bind_numeric(pad_bottom, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.padding_bottom_prop, effect.effect_padding_bottom, t);
        });
        bind_numeric(corner_tl, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.corner_radius_tl_prop, effect.effect_corner_radius_tl, t);
        });
        bind_numeric(corner_tr, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.corner_radius_tr_prop, effect.effect_corner_radius_tr, t);
        });
        bind_numeric(corner_br, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.corner_radius_br_prop, effect.effect_corner_radius_br, t);
        });
        bind_numeric(corner_bl, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.corner_radius_bl_prop, effect.effect_corner_radius_bl, t);
        });
        bind_numeric(grad_start_pos, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.gradient_start_pos_prop, effect.effect_gradient_start_pos, t);
        });
        bind_numeric(grad_end_pos, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.gradient_end_pos_prop, effect.effect_gradient_end_pos, t);
        });
        bind_numeric(grad_start_opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.gradient_start_opacity_prop, effect.effect_gradient_start_opacity, t);
        });
        bind_numeric(grad_end_opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.gradient_end_opacity_prop, effect.effect_gradient_end_opacity, t);
        });
        bind_numeric(grad_opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.gradient_opacity_prop, effect.effect_gradient_opacity, t);
        });
        bind_numeric(grad_angle, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.gradient_angle_prop, effect.effect_gradient_angle, t);
        });
        bind_numeric(grad_center_x, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.gradient_center_x_prop, effect.effect_gradient_center_x, t);
        });
        bind_numeric(grad_center_y, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.gradient_center_y_prop, effect.effect_gradient_center_y, t);
        });
        bind_numeric(grad_scale, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.gradient_scale_prop, effect.effect_gradient_scale, t);
        });
        bind_numeric(grad_focal_x, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.gradient_focal_x_prop, effect.effect_gradient_focal_x, t);
        });
        bind_numeric(grad_focal_y, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.gradient_focal_y_prop, effect.effect_gradient_focal_y, t);
        });

        form->addRow(section_label(bgl_tr("OBSTitles.Appearance")));
        add_effect_row(bgl_tr("OBSTitles.Fill"), fill);
        add_effect_row(bgl_tr("OBSTitles.FillColor"), wrap_color_keyframe(fill_color, &LayerEffect::color_a, &LayerEffect::color_r, &LayerEffect::color_g, &LayerEffect::color_b));
        add_effect_row(bgl_tr("OBSTitles.StrokeColor"), wrap_color_keyframe(stroke_color, &LayerEffect::stroke_color_a, &LayerEffect::stroke_color_r, &LayerEffect::stroke_color_g, &LayerEffect::stroke_color_b));
        add_effect_row(bgl_tr("OBSTitles.StrokeWidth"), wrap_scalar_keyframe(stroke_width, &LayerEffect::stroke_width_prop));
        add_effect_row(bgl_tr("OBSTitles.StrokeOpacity"), wrap_scalar_keyframe(stroke_opacity, &LayerEffect::stroke_opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), wrap_scalar_keyframe(opacity, &LayerEffect::opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.GradientTypeLabel"), grad_type);
        add_effect_row(bgl_tr("OBSTitles.SpreadLabel"), grad_spread);
        add_effect_row(bgl_tr("OBSTitles.StartColorLabel"), wrap_color_keyframe(grad_start, &LayerEffect::gradient_start_color_a, &LayerEffect::gradient_start_color_r, &LayerEffect::gradient_start_color_g, &LayerEffect::gradient_start_color_b));
        add_effect_row(bgl_tr("OBSTitles.EndColorLabel"), wrap_color_keyframe(grad_end, &LayerEffect::gradient_end_color_a, &LayerEffect::gradient_end_color_r, &LayerEffect::gradient_end_color_g, &LayerEffect::gradient_end_color_b));
        add_effect_row(bgl_tr("OBSTitles.StartStopLabel"), wrap_scalar_keyframe(grad_start_pos, &LayerEffect::gradient_start_pos_prop));
        add_effect_row(bgl_tr("OBSTitles.EndStopLabel"), wrap_scalar_keyframe(grad_end_pos, &LayerEffect::gradient_end_pos_prop));
        add_effect_row(bgl_tr("OBSTitles.StartOpacityLabel"), wrap_scalar_keyframe(grad_start_opacity, &LayerEffect::gradient_start_opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.EndOpacityLabel"), wrap_scalar_keyframe(grad_end_opacity, &LayerEffect::gradient_end_opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.GradientOpacityLabel"), wrap_scalar_keyframe(grad_opacity, &LayerEffect::gradient_opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.AngleLabel"), wrap_scalar_keyframe(grad_angle, &LayerEffect::gradient_angle_prop));
        add_effect_row(bgl_tr("OBSTitles.CenterXLabel"), wrap_scalar_keyframe(grad_center_x, &LayerEffect::gradient_center_x_prop));
        add_effect_row(bgl_tr("OBSTitles.CenterYLabel"), wrap_scalar_keyframe(grad_center_y, &LayerEffect::gradient_center_y_prop));
        add_effect_row(bgl_tr("OBSTitles.ScaleLabel"), wrap_scalar_keyframe(grad_scale, &LayerEffect::gradient_scale_prop));
        add_effect_row(bgl_tr("OBSTitles.FocalXLabel"), wrap_scalar_keyframe(grad_focal_x, &LayerEffect::gradient_focal_x_prop));
        add_effect_row(bgl_tr("OBSTitles.FocalYLabel"), wrap_scalar_keyframe(grad_focal_y, &LayerEffect::gradient_focal_y_prop));
        form->addRow(section_label(bgl_tr("OBSTitles.Padding")));
        add_effect_row(bgl_tr("OBSTitles.LeftPadding"), wrap_scalar_keyframe(pad_left, &LayerEffect::padding_left_prop));
        add_effect_row(bgl_tr("OBSTitles.RightPadding"), wrap_scalar_keyframe(pad_right, &LayerEffect::padding_right_prop));
        add_effect_row(bgl_tr("OBSTitles.TopPadding"), wrap_scalar_keyframe(pad_top, &LayerEffect::padding_top_prop));
        add_effect_row(bgl_tr("OBSTitles.BottomPadding"), wrap_scalar_keyframe(pad_bottom, &LayerEffect::padding_bottom_prop));
        form->addRow(section_label(bgl_tr("OBSTitles.Corners")));
        add_effect_row(bgl_tr("OBSTitles.CornerInitials"), corner_row);
        add_effect_row(bgl_tr("OBSTitles.CornerType"), corner_type_row);

        connect(fill, QOverload<int>::of(&QComboBox::activated), this, [this, fill](int){ if (selected_effect()) { selected_effect()->effect_fill_type = fill->currentData().toInt(); emit_effect_changed(); }});
        connect(grad_type, QOverload<int>::of(&QComboBox::activated), this, [this, grad_type](int){ if (selected_effect()) { selected_effect()->effect_gradient_type = grad_type->currentData().toInt(); emit_effect_changed(); }});
        connect(grad_spread, QOverload<int>::of(&QComboBox::activated), this, [this, grad_spread](int){ if (selected_effect()) { selected_effect()->effect_gradient_spread = grad_spread->currentData().toInt(); emit_effect_changed(); }});
        connect(grad_start_pos, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_gradient_start_pos = (float)v; set_animated_value(selected_effect()->gradient_start_pos_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(grad_end_pos, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_gradient_end_pos = (float)v; set_animated_value(selected_effect()->gradient_end_pos_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(grad_start_opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_gradient_start_opacity = (float)v; set_animated_value(selected_effect()->gradient_start_opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(grad_end_opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_gradient_end_opacity = (float)v; set_animated_value(selected_effect()->gradient_end_opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(grad_opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_gradient_opacity = (float)v; set_animated_value(selected_effect()->gradient_opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(grad_angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_gradient_angle = (float)v; set_animated_value(selected_effect()->gradient_angle_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(grad_center_x, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_gradient_center_x = (float)v; set_animated_value(selected_effect()->gradient_center_x_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(grad_center_y, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_gradient_center_y = (float)v; set_animated_value(selected_effect()->gradient_center_y_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(grad_scale, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_gradient_scale = (float)v; set_animated_value(selected_effect()->gradient_scale_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(grad_focal_x, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_gradient_focal_x = (float)v; set_animated_value(selected_effect()->gradient_focal_x_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(grad_focal_y, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_gradient_focal_y = (float)v; set_animated_value(selected_effect()->gradient_focal_y_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = v; set_animated_value(selected_effect()->opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(stroke_width, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_stroke_width = v; set_animated_value(selected_effect()->stroke_width_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(stroke_opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_stroke_opacity = v; set_animated_value(selected_effect()->stroke_opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(pad_left, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_padding_left = v; set_animated_value(selected_effect()->padding_left_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(pad_right, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_padding_right = v; set_animated_value(selected_effect()->padding_right_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(pad_top, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_padding_top = v; set_animated_value(selected_effect()->padding_top_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(pad_bottom, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_padding_bottom = v; set_animated_value(selected_effect()->padding_bottom_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(corner_tl, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_corner_radius_tl = v; set_animated_value(selected_effect()->corner_radius_tl_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(corner_tr, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_corner_radius_tr = v; set_animated_value(selected_effect()->corner_radius_tr_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(corner_br, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_corner_radius_br = v; set_animated_value(selected_effect()->corner_radius_br_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(corner_bl, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_corner_radius_bl = v; set_animated_value(selected_effect()->corner_radius_bl_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(corner_group, &QButtonGroup::idClicked, this, [this](int id){
            if (!loading_values_ && selected_effect()) {
                selected_effect()->effect_corner_type = std::clamp(id, 0, 3);
                emit_effect_changed();
            }
        });
    } else if (selected_effect()->type == LayerEffectType::Outline) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){
            if (!selected_effect()) return;
            selected_effect()->effect_color = argb;
            set_effect_color_channels_at(*selected_effect(), current_local_time(), argb);
        });
        auto *width = spin(0.0, 200.0, 1.0); width->setValue(effect->size_prop.is_animated() ? effect->size_prop.evaluate(lt) : effect->effect_size);
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);
        auto *join = combo(); join->addItem(bgl_tr("OBSTitles.Miter"), 0); join->addItem(bgl_tr("OBSTitles.Round"), 1); join->addItem(bgl_tr("OBSTitles.Bevel"), 2); join->setCurrentIndex(join->findData(effect->effect_join_style));
        auto *position = combo(); position->addItem(bgl_tr("OBSTitles.Back"), 0); position->addItem(bgl_tr("OBSTitles.Front"), 1); position->setCurrentIndex(effect->effect_on_front ? 1 : 0);
        auto *aa = new QCheckBox(bgl_tr("OBSTitles.AntialiasOutline"), box); aa->setChecked(effect->effect_antialias);
        bind_color(color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_color(effect, t);
        });
        bind_numeric(width, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), wrap_color_keyframe(color, &LayerEffect::color_a, &LayerEffect::color_r, &LayerEffect::color_g, &LayerEffect::color_b));
        add_effect_row(bgl_tr("OBSTitles.ThicknessLabel"), wrap_scalar_keyframe(width, &LayerEffect::size_prop));
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), wrap_scalar_keyframe(opacity, &LayerEffect::opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.JoinLabel"), join);
        add_effect_row(bgl_tr("OBSTitles.PositionLabelIndented"), position);
        add_effect_row(QString(), aa);
        connect(width, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = v; set_animated_value(selected_effect()->size_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = v; set_animated_value(selected_effect()->opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(join, QOverload<int>::of(&QComboBox::activated), this, [this, join](int){ if (selected_effect()) { selected_effect()->effect_join_style = join->currentData().toInt(); emit_effect_changed(); }});
        connect(position, QOverload<int>::of(&QComboBox::activated), this, [this, position](int){ if (selected_effect()) { selected_effect()->effect_on_front = position->currentData().toInt() == 1; emit_effect_changed(); }});
        connect(aa, &QCheckBox::toggled, this, [this](bool v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_antialias = v; emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::DropShadow) {
        LayerEffect *effect = selected_effect();
        auto *preset = combo(); preset->addItems({bgl_tr("OBSTitles.Custom"), bgl_tr("OBSTitles.Soft"), bgl_tr("OBSTitles.Medium"), bgl_tr("OBSTitles.Strong"), bgl_tr("OBSTitles.Broadcast")});
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color = argb; set_effect_color_channels_at(*selected_effect(), current_local_time(), argb); } });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);
        auto *dist = spin(0.0, 4096.0, 1.0); dist->setValue(effect->distance_prop.is_animated() ? effect->distance_prop.evaluate(lt) : effect->effect_distance);
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(effect->angle_prop.is_animated() ? effect->angle_prop.evaluate(lt) : effect->effect_angle);
        auto *blur = spin(0.0, 512.0, 1.0); blur->setValue(effect->size_prop.is_animated() ? effect->size_prop.evaluate(lt) : effect->effect_size);
        auto *spread = spin(0.0, 512.0, 1.0); spread->setValue(effect->spread_prop.is_animated() ? effect->spread_prop.evaluate(lt) : effect->effect_spread);
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_color(color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_color(effect, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(dist, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.distance_prop, effect.effect_distance, t);
        });
        bind_numeric(angle, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.angle_prop, effect.effect_angle, t);
        });
        bind_numeric(blur, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        bind_numeric(spread, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.spread_prop, effect.effect_spread, t);
        });
        add_effect_row(bgl_tr("OBSTitles.PresetLabel"), preset);
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), wrap_color_keyframe(color, &LayerEffect::color_a, &LayerEffect::color_r, &LayerEffect::color_g, &LayerEffect::color_b));
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), wrap_scalar_keyframe(opacity, &LayerEffect::opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.DistanceLabel"), wrap_scalar_keyframe(dist, &LayerEffect::distance_prop));
        add_effect_row(bgl_tr("OBSTitles.AngleLabel"), wrap_scalar_keyframe(angle, &LayerEffect::angle_prop));
        add_effect_row(bgl_tr("OBSTitles.BlurTypeLabel"), blur_type);
        add_effect_row(bgl_tr("OBSTitles.BlurLabel"), wrap_scalar_keyframe(blur, &LayerEffect::size_prop));
        add_effect_row(bgl_tr("OBSTitles.SpreadLabel"), wrap_scalar_keyframe(spread, &LayerEffect::spread_prop));
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; set_animated_value(selected_effect()->opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(dist, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_distance = (float)v; set_animated_value(selected_effect()->distance_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_angle = (float)v; set_animated_value(selected_effect()->angle_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(blur, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; set_animated_value(selected_effect()->size_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(spread, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_spread = (float)v; set_animated_value(selected_effect()->spread_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(preset, QOverload<int>::of(&QComboBox::activated), this, [this, preset]() {
            auto *effect = selected_effect();
            if (!effect) return;
            switch (preset->currentIndex()) {
            case 1: effect->effect_opacity = 0.35f; effect->effect_distance = 4.0f; effect->effect_size = 10.0f; effect->effect_spread = 0.0f; break;
            case 2: effect->effect_opacity = 0.55f; effect->effect_distance = 8.0f; effect->effect_size = 8.0f; effect->effect_spread = 0.0f; break;
            case 3: effect->effect_opacity = 0.75f; effect->effect_distance = 12.0f; effect->effect_size = 6.0f; effect->effect_spread = 0.0f; break;
            case 4: effect->effect_opacity = 0.6f; effect->effect_distance = 6.0f; effect->effect_size = 3.0f; effect->effect_spread = 2.0f; break;
            default: return;
            }
            const double t = layer_ ? std::clamp(playhead_ - layer_->in_time, 0.0, std::max(0.0, layer_->out_time - layer_->in_time)) : 0.0;
            set_animated_value(effect->opacity_prop, t, effect->effect_opacity);
            set_animated_value(effect->distance_prop, t, effect->effect_distance);
            set_animated_value(effect->size_prop, t, effect->effect_size);
            set_animated_value(effect->spread_prop, t, effect->effect_spread);
            emit_effect_changed();
            load_settings();
        });
    } else if (selected_effect()->type == LayerEffectType::BrightnessContrast) {
        LayerEffect *effect = selected_effect();
        auto *brightness = spin(-1.0, 1.0, 0.01); brightness->setDecimals(2); brightness->setValue(panel_eval_effect_property(effect->brightness_prop, effect->brightness, lt));
        auto *contrast = spin(0.0, 4.0, 0.05); contrast->setDecimals(2); contrast->setValue(panel_eval_effect_property(effect->contrast_prop, effect->contrast, lt));
        bind_numeric(brightness, [](const LayerEffect &effect, double t) { return panel_eval_effect_property(effect.brightness_prop, effect.brightness, t); });
        bind_numeric(contrast, [](const LayerEffect &effect, double t) { return panel_eval_effect_property(effect.contrast_prop, effect.contrast, t); });
        add_effect_row(bgl_tr("OBSTitles.BrightnessLabel"), wrap_scalar_keyframe(brightness, &LayerEffect::brightness_prop));
        add_effect_row(bgl_tr("OBSTitles.ContrastLabel"), wrap_scalar_keyframe(contrast, &LayerEffect::contrast_prop));
        connect(brightness, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->brightness = (float)v; set_animated_value(selected_effect()->brightness_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(contrast, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->contrast = (float)v; set_animated_value(selected_effect()->contrast_prop, current_local_time(), v); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::Saturation) {
        LayerEffect *effect = selected_effect();
        auto *saturation = spin(0.0, 4.0, 0.05); saturation->setDecimals(2); saturation->setValue(panel_eval_effect_property(effect->saturation_prop, effect->saturation, lt));
        bind_numeric(saturation, [](const LayerEffect &effect, double t) { return panel_eval_effect_property(effect.saturation_prop, effect.saturation, t); });
        add_effect_row(bgl_tr("OBSTitles.SaturationLabel"), wrap_scalar_keyframe(saturation, &LayerEffect::saturation_prop));
        connect(saturation, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->saturation = (float)v; set_animated_value(selected_effect()->saturation_prop, current_local_time(), v); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::ColorOverlay) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color = argb; selected_effect()->tint_color = argb; set_effect_color_channels_at(*selected_effect(), current_local_time(), argb); } });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(panel_eval_effect_property(effect->opacity_prop, effect->effect_opacity, lt));
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        add_effect_row(bgl_tr("OBSTitles.ColorOverlayColorLabel"), wrap_color_keyframe(color, &LayerEffect::color_a, &LayerEffect::color_r, &LayerEffect::color_g, &LayerEffect::color_b));
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), wrap_scalar_keyframe(opacity, &LayerEffect::opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; selected_effect()->tint_amount = (float)v; set_animated_value(selected_effect()->opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::Glow || selected_effect()->type == LayerEffectType::InnerGlow) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color = argb; set_effect_color_channels_at(*selected_effect(), current_local_time(), argb); } });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(panel_eval_effect_property(effect->opacity_prop, effect->effect_opacity, lt));
        auto *size = spin(0.0, 512.0, 1.0); size->setValue(panel_eval_effect_property(effect->size_prop, effect->effect_size, lt));
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(size, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), wrap_color_keyframe(color, &LayerEffect::color_a, &LayerEffect::color_r, &LayerEffect::color_g, &LayerEffect::color_b));
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), wrap_scalar_keyframe(opacity, &LayerEffect::opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.SizeRadiusLabel"), wrap_scalar_keyframe(size, &LayerEffect::size_prop));
        add_effect_row(bgl_tr("OBSTitles.BlurTypeLabel"), blur_type);
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; set_animated_value(selected_effect()->opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(size, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; set_animated_value(selected_effect()->size_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::Blur) {
        LayerEffect *effect = selected_effect();
        auto *amount = spin(0.0, 1.0, 0.05); amount->setDecimals(2); amount->setValue(panel_eval_effect_property(effect->opacity_prop, effect->effect_opacity, lt));
        auto *size = spin(0.0, 512.0, 1.0); size->setValue(panel_eval_effect_property(effect->size_prop, effect->effect_size, lt));
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        bind_numeric(amount, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(size, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        add_effect_row(bgl_tr("OBSTitles.AmountLabel"), wrap_scalar_keyframe(amount, &LayerEffect::opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.SizeRadiusLabel"), wrap_scalar_keyframe(size, &LayerEffect::size_prop));
        add_effect_row(bgl_tr("OBSTitles.BlurTypeLabel"), blur_type);
        connect(amount, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; set_animated_value(selected_effect()->opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(size, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; set_animated_value(selected_effect()->size_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::MotionBlur) {
        LayerEffect *effect = selected_effect();
        auto *amount = spin(0.0, 1.0, 0.05); amount->setDecimals(2); amount->setValue(panel_eval_effect_property(effect->opacity_prop, effect->effect_opacity, lt));
        auto *shutter = spin(0.0, 720.0, 5.0); shutter->setValue(panel_eval_effect_property(effect->size_prop, effect->effect_size, lt));
        auto *samples = new QSpinBox(box); samples->setRange(2, 64); samples->setValue(effect->effect_samples); samples->setFixedHeight(22);
        auto *centered = new QCheckBox(bgl_tr("OBSTitles.MotionBlurCentered"), box); centered->setChecked(effect->effect_centered);
        bind_numeric(amount, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(shutter, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        add_effect_row(bgl_tr("OBSTitles.AmountLabel"), wrap_scalar_keyframe(amount, &LayerEffect::opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.ShutterAngleLabel"), wrap_scalar_keyframe(shutter, &LayerEffect::size_prop));
        add_effect_row(bgl_tr("OBSTitles.SamplesLabel"), samples);
        add_effect_row(QString(), centered);
        connect(amount, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; set_animated_value(selected_effect()->opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(shutter, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; set_animated_value(selected_effect()->size_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(samples, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_samples = v; emit_effect_changed(); }});
        connect(centered, &QCheckBox::toggled, this, [this](bool v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_centered = v; emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::Bloom) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color = argb; set_effect_color_channels_at(*selected_effect(), current_local_time(), argb); emit_effect_changed(); } });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);
        auto *threshold = spin(0.0, 1.0, 0.01); threshold->setDecimals(2); threshold->setValue(effect->spread_prop.is_animated() ? effect->spread_prop.evaluate(lt) : effect->effect_spread);
        auto *radius = spin(0.0, 512.0, 1.0); radius->setValue(effect->size_prop.is_animated() ? effect->size_prop.evaluate(lt) : effect->effect_size);
        auto *intensity = spin(0.0, 8.0, 0.1); intensity->setDecimals(2); intensity->setValue(effect->falloff_prop.is_animated() ? effect->falloff_prop.evaluate(lt) : effect->effect_falloff);
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_color(color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_color(effect, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(threshold, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.spread_prop, effect.effect_spread, t);
        });
        bind_numeric(radius, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        bind_numeric(intensity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.falloff_prop, effect.effect_falloff, t);
        });
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), wrap_color_keyframe(color, &LayerEffect::color_a, &LayerEffect::color_r, &LayerEffect::color_g, &LayerEffect::color_b));
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), wrap_scalar_keyframe(opacity, &LayerEffect::opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.ThresholdLabel"), wrap_scalar_keyframe(threshold, &LayerEffect::spread_prop));
        add_effect_row(bgl_tr("OBSTitles.SizeRadiusLabel"), wrap_scalar_keyframe(radius, &LayerEffect::size_prop));
        add_effect_row(bgl_tr("OBSTitles.IntensityLabel"), wrap_scalar_keyframe(intensity, &LayerEffect::falloff_prop));
        add_effect_row(bgl_tr("OBSTitles.BlurTypeLabel"), blur_type);
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity=(float)v; set_animated_value(selected_effect()->opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(threshold, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_spread=(float)v; set_animated_value(selected_effect()->spread_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(radius, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size=(float)v; set_animated_value(selected_effect()->size_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(intensity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_falloff=(float)v; set_animated_value(selected_effect()->falloff_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type=blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode=(EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::LensFlare) {
        LayerEffect *effect = selected_effect();
        auto *profile = combo();
        profile->addItem(QStringLiteral("Classic 35mm"), 0);
        profile->addItem(QStringLiteral("Anamorphic Blue"), 1);
        profile->addItem(QStringLiteral("Cinematic Warm"), 2);
        profile->addItem(QStringLiteral("Modern Sci-Fi"), 3);
        profile->addItem(QStringLiteral("Subtle Natural"), 4);
        profile->setCurrentIndex(profile->findData(effect->effect_profile));
        auto *primary = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb) {
            if (!selected_effect()) return;
            selected_effect()->effect_color = argb;
            set_effect_color_channels_at(*selected_effect(), current_local_time(), argb);
        });
        auto *secondary = color_button(panel_eval_effect_secondary_color(*effect, lt), [this, lt](uint32_t argb) {
            if (!selected_effect()) return;
            selected_effect()->effect_secondary_color = argb;
            set_effect_secondary_color_channels_at(*selected_effect(), current_local_time(), argb);
        });
        auto *amount = spin(0.0, 10.0, 0.05); amount->setValue(panel_eval_effect_property(effect->amount_prop, effect->effect_amount, lt));
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setValue(panel_eval_effect_property(effect->opacity_prop, effect->effect_opacity, lt));
        auto *scale = spin(0.01, 20.0, 0.05); scale->setValue(panel_eval_effect_property(effect->scale_prop, effect->effect_scale, lt));
        auto *radius = spin(0.001, 4.0, 0.01); radius->setValue(panel_eval_effect_property(effect->size_prop, effect->effect_size, lt));
        auto *spread = spin(0.0, 4.0, 0.05); spread->setValue(panel_eval_effect_property(effect->spread_prop, effect->effect_spread, lt));
        auto *falloff = spin(0.01, 16.0, 0.1); falloff->setValue(panel_eval_effect_property(effect->falloff_prop, effect->effect_falloff, lt));
        auto *angle = spin(-360.0, 360.0, 1.0); angle->setValue(panel_eval_effect_property(effect->angle_prop, effect->effect_angle, lt));
        auto *center_x = spin(-4.0, 4.0, 0.01); center_x->setValue(panel_eval_effect_property(effect->center_x_prop, effect->effect_center_x, lt));
        auto *center_y = spin(-4.0, 4.0, 0.01); center_y->setValue(panel_eval_effect_property(effect->center_y_prop, effect->effect_center_y, lt));
        auto *ghosts = spin(2.0, 12.0, 1.0);
        ghosts->setDecimals(0);
        ghosts->setValue(panel_eval_effect_property(
            effect->complexity_prop, effect->effect_complexity, lt));
        bind_color(primary, [](const LayerEffect &e, double t) { return panel_eval_effect_color(e, t); });
        bind_color(secondary, [](const LayerEffect &e, double t) { return panel_eval_effect_secondary_color(e, t); });
        bind_numeric(amount, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.amount_prop, e.effect_amount, t); });
        bind_numeric(opacity, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.opacity_prop, e.effect_opacity, t); });
        bind_numeric(scale, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.scale_prop, e.effect_scale, t); });
        bind_numeric(radius, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.size_prop, e.effect_size, t); });
        bind_numeric(spread, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.spread_prop, e.effect_spread, t); });
        bind_numeric(falloff, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.falloff_prop, e.effect_falloff, t); });
        bind_numeric(angle, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.angle_prop, e.effect_angle, t); });
        bind_numeric(center_x, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.center_x_prop, e.effect_center_x, t); });
        bind_numeric(center_y, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.center_y_prop, e.effect_center_y, t); });
        bind_numeric(ghosts, [](const LayerEffect &e, double t){ return panel_eval_effect_property(e.complexity_prop, e.effect_complexity, t); });
        add_effect_row(bgl_tr("OBSTitles.EffectProfile"), profile);
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), wrap_color_keyframe(primary, &LayerEffect::color_a, &LayerEffect::color_r, &LayerEffect::color_g, &LayerEffect::color_b));
        add_effect_row(bgl_tr("OBSTitles.SecondaryColor"), wrap_color_keyframe(secondary, &LayerEffect::secondary_color_a, &LayerEffect::secondary_color_r, &LayerEffect::secondary_color_g, &LayerEffect::secondary_color_b));
        add_effect_row(bgl_tr("OBSTitles.Amount"), wrap_scalar_keyframe(amount, &LayerEffect::amount_prop));
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), wrap_scalar_keyframe(opacity, &LayerEffect::opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.Scale"), wrap_scalar_keyframe(scale, &LayerEffect::scale_prop));
        add_effect_row(bgl_tr("OBSTitles.SizeRadiusLabel"), wrap_scalar_keyframe(radius, &LayerEffect::size_prop));
        add_effect_row(bgl_tr("OBSTitles.SpreadLabel"), wrap_scalar_keyframe(spread, &LayerEffect::spread_prop));
        add_effect_row(bgl_tr("OBSTitles.FalloffLabel"), wrap_scalar_keyframe(falloff, &LayerEffect::falloff_prop));
        add_effect_row(bgl_tr("OBSTitles.AngleLabel"), wrap_scalar_keyframe(angle, &LayerEffect::angle_prop));
        add_effect_row(bgl_tr("OBSTitles.CenterX"), wrap_scalar_keyframe(center_x, &LayerEffect::center_x_prop));
        add_effect_row(bgl_tr("OBSTitles.CenterY"), wrap_scalar_keyframe(center_y, &LayerEffect::center_y_prop));
        add_effect_row(bgl_tr("OBSTitles.Ghosts"), wrap_scalar_keyframe(ghosts, &LayerEffect::complexity_prop));
        connect(profile, QOverload<int>::of(&QComboBox::activated), this, [this, profile](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_profile=profile->currentData().toInt(); emit_effect_changed(); }});
        connect(ghosts, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, lt](double v) {
                    if (!loading_values_ && selected_effect()) {
                        selected_effect()->effect_complexity = (float)v;
                        selected_effect()->effect_samples = (int)std::round(v);
                        set_animated_value(selected_effect()->complexity_prop, current_local_time(), v);
                        emit_effect_changed();
                    }
                });
        const auto bind_value = [this](QDoubleSpinBox *w, float LayerEffect::*field, AnimatedProperty LayerEffect::*prop) {
            connect(w, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, field, prop](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->*field=(float)v; set_animated_value(selected_effect()->*prop, current_local_time(), v); emit_effect_changed(); }});
        };
        bind_value(amount, &LayerEffect::effect_amount, &LayerEffect::amount_prop);
        bind_value(opacity, &LayerEffect::effect_opacity, &LayerEffect::opacity_prop);
        bind_value(scale, &LayerEffect::effect_scale, &LayerEffect::scale_prop);
        bind_value(radius, &LayerEffect::effect_size, &LayerEffect::size_prop);
        bind_value(spread, &LayerEffect::effect_spread, &LayerEffect::spread_prop);
        bind_value(falloff, &LayerEffect::effect_falloff, &LayerEffect::falloff_prop);
        bind_value(angle, &LayerEffect::effect_angle, &LayerEffect::angle_prop);
        bind_value(center_x, &LayerEffect::effect_center_x, &LayerEffect::center_x_prop);
        bind_value(center_y, &LayerEffect::effect_center_y, &LayerEffect::center_y_prop);
    } else if (selected_effect()->type == LayerEffectType::Vignette) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color=argb; set_effect_color_channels_at(*selected_effect(), current_local_time(), argb); }});
        auto *amount = spin(0.0, 2.0, 0.02); amount->setValue(panel_eval_effect_property(effect->amount_prop, effect->effect_amount, lt));
        auto *scale = spin(0.01, 4.0, 0.02); scale->setValue(panel_eval_effect_property(effect->scale_prop, effect->effect_scale, lt));
        auto *soft = spin(0.0, 1.0, 0.01); soft->setValue(panel_eval_effect_property(effect->softness_prop, effect->effect_softness, lt));
        auto *round = spin(-1.0, 1.0, 0.02); round->setValue(panel_eval_effect_property(effect->roundness_prop, effect->effect_roundness, lt));
        auto *cx = spin(-4.0, 4.0, 0.01); cx->setValue(panel_eval_effect_property(effect->center_x_prop, effect->effect_center_x, lt));
        auto *cy = spin(-4.0, 4.0, 0.01); cy->setValue(panel_eval_effect_property(effect->center_y_prop, effect->effect_center_y, lt));
        auto *invert = new QCheckBox(bgl_tr("OBSTitles.Invert"), box); invert->setChecked(effect->effect_invert);
        bind_color(color, [](const LayerEffect &e,double t){ return panel_eval_effect_color(e,t); });
        const auto init_bind=[&](QDoubleSpinBox *w, const AnimatedProperty LayerEffect::*prop, const float LayerEffect::*field){ bind_numeric(w,[prop,field](const LayerEffect&e,double t){return panel_eval_effect_property(e.*prop,e.*field,t);});};
        init_bind(amount,&LayerEffect::amount_prop,&LayerEffect::effect_amount); init_bind(scale,&LayerEffect::scale_prop,&LayerEffect::effect_scale); init_bind(soft,&LayerEffect::softness_prop,&LayerEffect::effect_softness); init_bind(round,&LayerEffect::roundness_prop,&LayerEffect::effect_roundness); init_bind(cx,&LayerEffect::center_x_prop,&LayerEffect::effect_center_x); init_bind(cy,&LayerEffect::center_y_prop,&LayerEffect::effect_center_y);
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), wrap_color_keyframe(color, &LayerEffect::color_a, &LayerEffect::color_r, &LayerEffect::color_g, &LayerEffect::color_b)); add_effect_row(bgl_tr("OBSTitles.Amount"), wrap_scalar_keyframe(amount, &LayerEffect::amount_prop)); add_effect_row(bgl_tr("OBSTitles.Scale"), wrap_scalar_keyframe(scale, &LayerEffect::scale_prop)); add_effect_row(bgl_tr("OBSTitles.SoftnessLabel"), wrap_scalar_keyframe(soft, &LayerEffect::softness_prop)); add_effect_row(bgl_tr("OBSTitles.Roundness"), wrap_scalar_keyframe(round, &LayerEffect::roundness_prop)); add_effect_row(bgl_tr("OBSTitles.CenterX"), wrap_scalar_keyframe(cx, &LayerEffect::center_x_prop)); add_effect_row(bgl_tr("OBSTitles.CenterY"), wrap_scalar_keyframe(cy, &LayerEffect::center_y_prop)); add_effect_row(QString(),invert);
        const auto bind_value=[this](QDoubleSpinBox*w,float LayerEffect::*f,AnimatedProperty LayerEffect::*p){connect(w,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this,f,p](double v){if(!loading_values_&&selected_effect()){selected_effect()->*f=(float)v;set_animated_value(selected_effect()->*p,current_local_time(),v);emit_effect_changed();}});};
        bind_value(amount,&LayerEffect::effect_amount,&LayerEffect::amount_prop); bind_value(scale,&LayerEffect::effect_scale,&LayerEffect::scale_prop); bind_value(soft,&LayerEffect::effect_softness,&LayerEffect::softness_prop); bind_value(round,&LayerEffect::effect_roundness,&LayerEffect::roundness_prop); bind_value(cx,&LayerEffect::effect_center_x,&LayerEffect::center_x_prop); bind_value(cy,&LayerEffect::effect_center_y,&LayerEffect::center_y_prop);
        connect(invert,&QCheckBox::toggled,this,[this](bool v){if(!loading_values_&&selected_effect()){selected_effect()->effect_invert=v;emit_effect_changed();}});
    } else if (selected_effect()->type == LayerEffectType::Noise || selected_effect()->type == LayerEffectType::RoughenEdges) {
        LayerEffect *effect = selected_effect();
        const bool noise = effect->type == LayerEffectType::Noise;
        QComboBox *profile = nullptr;
        if (noise) { profile=combo(); profile->addItem(QStringLiteral("Uniform"),0); profile->addItem(QStringLiteral("Smooth"),1); profile->addItem(QStringLiteral("Gaussian"),2); profile->addItem(QStringLiteral("Film Grain"),3); profile->addItem(QStringLiteral("Digital Speckle"),4); profile->addItem(QStringLiteral("Organic Grain"),5); profile->setCurrentIndex(profile->findData(effect->effect_profile)); add_effect_row(bgl_tr("OBSTitles.EffectProfile"),profile); }
        auto *opacity=spin(0.0,1.0,0.01); opacity->setValue(panel_eval_effect_property(effect->opacity_prop,effect->effect_opacity,lt));
        auto *amount=spin(0.0,4.0,0.01); amount->setValue(panel_eval_effect_property(effect->amount_prop,effect->effect_amount,lt));
        auto *scale=spin(0.001,1000.0,0.1); scale->setValue(panel_eval_effect_property(effect->scale_prop,effect->effect_scale,lt));
        auto *soft=spin(0.0,1.0,0.01); soft->setValue(panel_eval_effect_property(effect->softness_prop,effect->effect_softness,lt));
        auto *complexity=spin(1.0,12.0,0.25); complexity->setValue(panel_eval_effect_property(effect->complexity_prop,effect->effect_complexity,lt));
        auto *evolution=spin(-100000.0,100000.0,1.0); evolution->setValue(panel_eval_effect_property(effect->evolution_prop,effect->effect_evolution,lt));
        auto *seed=new QSpinBox(box); seed->setRange(0,1000000); seed->setValue(effect->effect_seed);
        QDoubleSpinBox *speed=nullptr; QCheckBox *animated=nullptr; QCheckBox *mono=nullptr;
        if(noise){speed=spin(-100.0,100.0,0.1);speed->setValue(panel_eval_effect_property(effect->speed_prop,effect->effect_speed,lt));animated=new QCheckBox(bgl_tr("OBSTitles.Animated"),box);animated->setChecked(effect->effect_animated);mono=new QCheckBox(bgl_tr("OBSTitles.Monochrome"),box);mono->setChecked(effect->effect_monochrome);}
        auto *invert=new QCheckBox(bgl_tr("OBSTitles.Invert"),box);invert->setChecked(effect->effect_invert);
        const auto init_bind=[&](QDoubleSpinBox*w,const AnimatedProperty LayerEffect::*p,const float LayerEffect::*f){if(w)bind_numeric(w,[p,f](const LayerEffect&e,double t){return panel_eval_effect_property(e.*p,e.*f,t);});};
        init_bind(opacity,&LayerEffect::opacity_prop,&LayerEffect::effect_opacity);init_bind(amount,&LayerEffect::amount_prop,&LayerEffect::effect_amount);init_bind(scale,&LayerEffect::scale_prop,&LayerEffect::effect_scale);init_bind(soft,&LayerEffect::softness_prop,&LayerEffect::effect_softness);init_bind(complexity,&LayerEffect::complexity_prop,&LayerEffect::effect_complexity);init_bind(evolution,&LayerEffect::evolution_prop,&LayerEffect::effect_evolution);init_bind(speed,&LayerEffect::speed_prop,&LayerEffect::effect_speed);
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), wrap_scalar_keyframe(opacity, &LayerEffect::opacity_prop));add_effect_row(bgl_tr("OBSTitles.Amount"), wrap_scalar_keyframe(amount, &LayerEffect::amount_prop));add_effect_row(bgl_tr("OBSTitles.Scale"), wrap_scalar_keyframe(scale, &LayerEffect::scale_prop));add_effect_row(bgl_tr("OBSTitles.SoftnessLabel"), wrap_scalar_keyframe(soft, &LayerEffect::softness_prop));add_effect_row(bgl_tr("OBSTitles.Complexity"), wrap_scalar_keyframe(complexity, &LayerEffect::complexity_prop));add_effect_row(bgl_tr("OBSTitles.Evolution"), wrap_scalar_keyframe(evolution, &LayerEffect::evolution_prop));if(speed)add_effect_row(bgl_tr("OBSTitles.Speed"), wrap_scalar_keyframe(speed, &LayerEffect::speed_prop));add_effect_row(bgl_tr("OBSTitles.Seed"),seed);if(animated)add_effect_row(QString(),animated);if(mono)add_effect_row(QString(),mono);add_effect_row(QString(),invert);
        const auto bind_value=[this](QDoubleSpinBox*w,float LayerEffect::*f,AnimatedProperty LayerEffect::*p){if(w)connect(w,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this,f,p](double v){if(!loading_values_&&selected_effect()){selected_effect()->*f=(float)v;set_animated_value(selected_effect()->*p,current_local_time(),v);emit_effect_changed();}});};
        bind_value(opacity,&LayerEffect::effect_opacity,&LayerEffect::opacity_prop);bind_value(amount,&LayerEffect::effect_amount,&LayerEffect::amount_prop);bind_value(scale,&LayerEffect::effect_scale,&LayerEffect::scale_prop);bind_value(soft,&LayerEffect::effect_softness,&LayerEffect::softness_prop);bind_value(complexity,&LayerEffect::effect_complexity,&LayerEffect::complexity_prop);bind_value(evolution,&LayerEffect::effect_evolution,&LayerEffect::evolution_prop);bind_value(speed,&LayerEffect::effect_speed,&LayerEffect::speed_prop);
        if(profile)connect(profile,QOverload<int>::of(&QComboBox::activated),this,[this,profile](int){if(!loading_values_&&selected_effect()){selected_effect()->effect_profile=profile->currentData().toInt();emit_effect_changed();}});
        connect(seed,QOverload<int>::of(&QSpinBox::valueChanged),this,[this](int v){if(!loading_values_&&selected_effect()){selected_effect()->effect_seed=v;emit_effect_changed();}});
        if(animated)connect(animated,&QCheckBox::toggled,this,[this](bool v){if(!loading_values_&&selected_effect()){selected_effect()->effect_animated=v;emit_effect_changed();}});
        if(mono)connect(mono,&QCheckBox::toggled,this,[this](bool v){if(!loading_values_&&selected_effect()){selected_effect()->effect_monochrome=v;emit_effect_changed();}});
        connect(invert,&QCheckBox::toggled,this,[this](bool v){if(!loading_values_&&selected_effect()){selected_effect()->effect_invert=v;emit_effect_changed();}});
    } else if (selected_effect()->type == LayerEffectType::Emboss) {
        LayerEffect *effect = selected_effect();
        auto *depth = spin(0.1, 32.0, 0.1); depth->setDecimals(2); depth->setValue(effect->size_prop.is_animated() ? effect->size_prop.evaluate(lt) : effect->effect_size);
        auto *height = spin(0.1, 32.0, 0.1); height->setDecimals(2); height->setValue(effect->distance_prop.is_animated() ? effect->distance_prop.evaluate(lt) : effect->effect_distance);
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(effect->angle_prop.is_animated() ? effect->angle_prop.evaluate(lt) : effect->effect_angle);
        auto *softness = spin(0.0, 16.0, 0.1); softness->setDecimals(2); softness->setValue(effect->spread_prop.is_animated() ? effect->spread_prop.evaluate(lt) : effect->effect_spread);
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_numeric(depth, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        bind_numeric(height, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.distance_prop, effect.effect_distance, t);
        });
        bind_numeric(angle, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.angle_prop, effect.effect_angle, t);
        });
        bind_numeric(softness, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.spread_prop, effect.effect_spread, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        add_effect_row(bgl_tr("OBSTitles.DepthLabel"), wrap_scalar_keyframe(depth, &LayerEffect::size_prop));
        add_effect_row(bgl_tr("OBSTitles.HeightLabel"), wrap_scalar_keyframe(height, &LayerEffect::distance_prop));
        add_effect_row(bgl_tr("OBSTitles.AngleLabel"), wrap_scalar_keyframe(angle, &LayerEffect::angle_prop));
        add_effect_row(bgl_tr("OBSTitles.SoftnessLabel"), wrap_scalar_keyframe(softness, &LayerEffect::spread_prop));
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), wrap_scalar_keyframe(opacity, &LayerEffect::opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(depth, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size=(float)v; set_animated_value(selected_effect()->size_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(height, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_distance=(float)v; set_animated_value(selected_effect()->distance_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_angle=(float)v; set_animated_value(selected_effect()->angle_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(softness, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_spread=(float)v; set_animated_value(selected_effect()->spread_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity=(float)v; set_animated_value(selected_effect()->opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode=(EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::InnerShadow) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){
            if (selected_effect()) {
                selected_effect()->effect_color = argb;
                set_effect_color_channels_at(*selected_effect(), current_local_time(), argb);
            }
        });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(panel_eval_effect_property(effect->opacity_prop, effect->effect_opacity, lt));
        auto *dist = spin(0.0, 4096.0, 1.0); dist->setValue(panel_eval_effect_property(effect->distance_prop, effect->effect_distance, lt));
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(panel_eval_effect_property(effect->angle_prop, effect->effect_angle, lt));
        auto *size = spin(0.0, 512.0, 1.0); size->setValue(panel_eval_effect_property(effect->size_prop, effect->effect_size, lt));
        auto *blur_type = combo(); add_shadow_blur_items(blur_type); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_color(color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_color(effect, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(dist, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.distance_prop, effect.effect_distance, t);
        });
        bind_numeric(angle, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.angle_prop, effect.effect_angle, t);
        });
        bind_numeric(size, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        add_effect_row(bgl_tr("OBSTitles.ColorLabel"), wrap_color_keyframe(color, &LayerEffect::color_a, &LayerEffect::color_r, &LayerEffect::color_g, &LayerEffect::color_b));
        add_effect_row(bgl_tr("OBSTitles.OpacityLabel"), wrap_scalar_keyframe(opacity, &LayerEffect::opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.DistanceLabel"), wrap_scalar_keyframe(dist, &LayerEffect::distance_prop));
        add_effect_row(bgl_tr("OBSTitles.AngleLabel"), wrap_scalar_keyframe(angle, &LayerEffect::angle_prop));
        add_effect_row(bgl_tr("OBSTitles.SizeRadiusLabel"), wrap_scalar_keyframe(size, &LayerEffect::size_prop));
        add_effect_row(bgl_tr("OBSTitles.BlurTypeLabel"), blur_type);
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; set_animated_value(selected_effect()->opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(dist, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_distance = (float)v; set_animated_value(selected_effect()->distance_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_angle = (float)v; set_animated_value(selected_effect()->angle_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(size, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; set_animated_value(selected_effect()->size_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    } else if (selected_effect()->type == LayerEffectType::LongShadow) {
        LayerEffect *effect = selected_effect();
        auto *color = color_button(panel_eval_effect_color(*effect, lt), [this, lt](uint32_t argb){ if (selected_effect()) { selected_effect()->effect_color = argb; set_effect_color_channels_at(*selected_effect(), current_local_time(), argb); } });
        auto *opacity = spin(0.0, 1.0, 0.05); opacity->setDecimals(2); opacity->setValue(effect->opacity_prop.is_animated() ? effect->opacity_prop.evaluate(lt) : effect->effect_opacity);
        auto *length = spin(0.0, 4096.0, 5.0); length->setValue(effect->distance_prop.is_animated() ? effect->distance_prop.evaluate(lt) : effect->effect_distance);
        auto *angle = spin(-360.0, 360.0, 5.0); angle->setValue(effect->angle_prop.is_animated() ? effect->angle_prop.evaluate(lt) : effect->effect_angle);
        auto *falloff = spin(0.0, 8.0, 0.1); falloff->setDecimals(2); falloff->setValue(effect->falloff_prop.is_animated() ? effect->falloff_prop.evaluate(lt) : effect->effect_falloff);
        auto *blur_type = combo(); blur_type->addItem(bgl_tr("OBSTitles.NoBlur"), (int)LongShadowBlurType::None); blur_type->addItem(bgl_tr("OBSTitles.BoxBlur"), (int)LongShadowBlurType::Box); blur_type->addItem(bgl_tr("OBSTitles.GaussianBlur"), (int)LongShadowBlurType::Gaussian); blur_type->addItem(bgl_tr("OBSTitles.StackFastBlur"), (int)LongShadowBlurType::StackFast); blur_type->setCurrentIndex(blur_type->findData(effect->effect_blur_type));
        auto *blur = spin(0.0, 512.0, 1.0); blur->setValue(effect->size_prop.is_animated() ? effect->size_prop.evaluate(lt) : effect->effect_size);
        auto *blend = combo(); add_blend_mode_items(blend); blend->setCurrentIndex(blend->findData((int)effect->blend_mode));
        bind_color(color, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_color(effect, t);
        });
        bind_numeric(opacity, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.opacity_prop, effect.effect_opacity, t);
        });
        bind_numeric(length, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.distance_prop, effect.effect_distance, t);
        });
        bind_numeric(angle, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.angle_prop, effect.effect_angle, t);
        });
        bind_numeric(falloff, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.falloff_prop, effect.effect_falloff, t);
        });
        bind_numeric(blur, [](const LayerEffect &effect, double t) {
            return panel_eval_effect_property(effect.size_prop, effect.effect_size, t);
        });
        add_effect_row(bgl_tr("OBSTitles.LongShadowColor"), wrap_color_keyframe(color, &LayerEffect::color_a, &LayerEffect::color_r, &LayerEffect::color_g, &LayerEffect::color_b));
        add_effect_row(bgl_tr("OBSTitles.LongShadowOpacity"), wrap_scalar_keyframe(opacity, &LayerEffect::opacity_prop));
        add_effect_row(bgl_tr("OBSTitles.LongShadowLength"), wrap_scalar_keyframe(length, &LayerEffect::distance_prop));
        add_effect_row(bgl_tr("OBSTitles.LongShadowAngle"), wrap_scalar_keyframe(angle, &LayerEffect::angle_prop));
        add_effect_row(bgl_tr("OBSTitles.LongShadowFalloff"), wrap_scalar_keyframe(falloff, &LayerEffect::falloff_prop));
        add_effect_row(bgl_tr("OBSTitles.LongShadowBlurType"), blur_type);
        add_effect_row(bgl_tr("OBSTitles.LongShadowBlur"), wrap_scalar_keyframe(blur, &LayerEffect::size_prop));
        add_effect_row(bgl_tr("OBSTitles.BlendingModeLabel"), blend);
        connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_opacity = (float)v; set_animated_value(selected_effect()->opacity_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(length, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_distance = (float)v; set_animated_value(selected_effect()->distance_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(angle, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_angle = (float)v; set_animated_value(selected_effect()->angle_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(falloff, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_falloff = (float)v; set_animated_value(selected_effect()->falloff_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(blur_type, QOverload<int>::of(&QComboBox::activated), this, [this, blur_type](int){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_blur_type = blur_type->currentData().toInt(); emit_effect_changed(); }});
        connect(blur, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, lt](double v){ if (!loading_values_ && selected_effect()) { selected_effect()->effect_size = (float)v; set_animated_value(selected_effect()->size_prop, current_local_time(), v); emit_effect_changed(); }});
        connect(blend, QOverload<int>::of(&QComboBox::activated), this, [this, blend](int){ if (!loading_values_ && selected_effect()) { selected_effect()->blend_mode = (EffectBlendMode)blend->currentData().toInt(); emit_effect_changed(); }});
    }
    if (LayerEffect *effect = selected_effect()) {
        const bool outside_capable = effect->type == LayerEffectType::DropShadow ||
                                     effect->type == LayerEffectType::LongShadow ||
                                     effect->type == LayerEffectType::Glow;
        if (outside_capable) {
            auto *outside = new QCheckBox(bgl_tr("OBSTitles.EffectOutsideHardAlpha"), box);
            auto *outside_invert = new QCheckBox(bgl_tr("OBSTitles.EffectMaskInvert"), box);
            outside->setChecked(effect->effect_outside_hard_alpha);
            outside_invert->setChecked(effect->effect_outside_hard_alpha_invert);
            outside_invert->setEnabled(effect->effect_outside_hard_alpha);
            add_effect_row(QString(), outside);
            add_effect_row(QString(), outside_invert);
            connect(outside, &QCheckBox::toggled, this, [this, outside_invert](bool value) {
                outside_invert->setEnabled(value);
                if (!loading_values_ && selected_effect()) {
                    selected_effect()->effect_outside_hard_alpha = value;
                    emit_effect_changed();
                }
            });
            connect(outside_invert, &QCheckBox::toggled, this, [this](bool value) {
                if (!loading_values_ && selected_effect()) {
                    selected_effect()->effect_outside_hard_alpha_invert = value;
                    emit_effect_changed();
                }
            });
        }
        const bool backdrop_capable = effect->type != LayerEffectType::BackgroundColor &&
                                      effect->type != LayerEffectType::Outline &&
                                      effect->type != LayerEffectType::DropShadow &&
                                      effect->type != LayerEffectType::LongShadow &&
                                      effect->type != LayerEffectType::Glow &&
                                      effect->type != LayerEffectType::InnerGlow &&
                                      effect->type != LayerEffectType::InnerShadow &&
                                      effect->type != LayerEffectType::MotionBlur;
        if (backdrop_capable) {
            auto *behind = new QCheckBox(bgl_tr("OBSTitles.EffectAffectLayersBehind"), box);
            auto *behind_invert = new QCheckBox(bgl_tr("OBSTitles.EffectMaskInvert"), box);
            behind->setChecked(effect->affect_layers_behind);
            behind_invert->setChecked(effect->affect_layers_behind_invert);
            behind_invert->setEnabled(effect->affect_layers_behind);
            add_effect_row(QString(), behind);
            add_effect_row(QString(), behind_invert);
            connect(behind, &QCheckBox::toggled, this, [this, behind_invert](bool value) {
                behind_invert->setEnabled(value);
                if (!loading_values_ && selected_effect()) {
                    selected_effect()->affect_layers_behind = value;
                    emit_effect_changed();
                }
            });
            connect(behind_invert, &QCheckBox::toggled, this, [this](bool value) {
                if (!loading_values_ && selected_effect()) {
                    selected_effect()->affect_layers_behind_invert = value;
                    emit_effect_changed();
                }
            });
        }
    }
    settings_layout_->addWidget(box);
    settings_layout_->addStretch(1);
    loading_values_ = false;
}
