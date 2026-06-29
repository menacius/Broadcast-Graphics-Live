#include "style-presets.h"
#include "title-localization.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QAbstractItemView>
#include <QMenu>
#include <QPainter>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QUuid>
#include <QMessageBox>
#include <QPointer>
#include <QBrush>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QConicalGradient>
#include <algorithm>
#include <cmath>
#include <vector>

namespace obsbgs {
namespace {

struct PresetChangeListener {
    QPointer<QObject> context;
    std::function<void()> callback;
};

std::vector<PresetChangeListener> &presetChangeListeners()
{
    static std::vector<PresetChangeListener> listeners;
    return listeners;
}

QString kindToString(StylePresetKind kind)
{
    return kind == StylePresetKind::Text ? QStringLiteral("text") : QStringLiteral("gradient");
}

StylePresetKind stringToKind(const QString &value)
{
    return value == QStringLiteral("gradient") ? StylePresetKind::Gradient : StylePresetKind::Text;
}

int normalizedGradientType(int type)
{
    switch (std::clamp(type, 0, 4)) {
    case 1: return 1;
    case 2: return 2;
    case 4: return 1;
    case 0:
    case 3:
    default: return 0;
    }
}

int normalizedGradientSpread(int spread)
{
    return spread == 1 || spread == 2 ? spread : 0;
}

int gradientSpreadFromPayload(const QJsonObject &o, int fallback)
{
    if (!o.contains(QStringLiteral("gradientSpread")) && o.value(QStringLiteral("gradientType")).toInt(0) == 3)
        return 1;
    return normalizedGradientSpread(o.value(QStringLiteral("gradientSpread")).toInt(fallback));
}

QJsonObject gradientPayloadFromLayer(const Layer &layer)
{
    QJsonObject o;
    o[QStringLiteral("fillType")] = layer.fill_type;
    o[QStringLiteral("gradientType")] = layer.gradient_type;
    o[QStringLiteral("gradientSpread")] = layer.gradient_spread;
    o[QStringLiteral("startColor")] = QString::number(layer.gradient_start_color, 16);
    o[QStringLiteral("endColor")] = QString::number(layer.gradient_end_color, 16);
    o[QStringLiteral("startPos")] = layer.gradient_start_pos;
    o[QStringLiteral("endPos")] = layer.gradient_end_pos;
    o[QStringLiteral("startOpacity")] = layer.gradient_start_opacity;
    o[QStringLiteral("endOpacity")] = layer.gradient_end_opacity;
    o[QStringLiteral("opacity")] = layer.gradient_opacity;
    o[QStringLiteral("angle")] = layer.gradient_angle;
    o[QStringLiteral("centerX")] = layer.gradient_center_x;
    o[QStringLiteral("centerY")] = layer.gradient_center_y;
    o[QStringLiteral("scale")] = layer.gradient_scale;
    o[QStringLiteral("focalX")] = layer.gradient_focal_x;
    o[QStringLiteral("focalY")] = layer.gradient_focal_y;
    QJsonArray stops;
    for (const auto &stop : layer.gradient_stops) {
        QJsonObject s;
        s[QStringLiteral("color")] = QString::number(stop.color, 16);
        s[QStringLiteral("position")] = stop.position;
        s[QStringLiteral("opacity")] = stop.opacity;
        stops.append(s);
    }
    o[QStringLiteral("stops")] = stops;
    return o;
}

QJsonObject gradientPayloadFromFill(const RichTextFill &fill,
                                    const std::vector<GradientStop> &extra_stops)
{
    QJsonObject o;
    o[QStringLiteral("fillType")] = 1;
    o[QStringLiteral("gradientType")] = fill.gradient_type;
    o[QStringLiteral("gradientSpread")] = fill.gradient_spread;
    o[QStringLiteral("startColor")] = QString::number(fill.gradient_start_color, 16);
    o[QStringLiteral("endColor")] = QString::number(fill.gradient_end_color, 16);
    o[QStringLiteral("startPos")] = fill.gradient_start_pos;
    o[QStringLiteral("endPos")] = fill.gradient_end_pos;
    o[QStringLiteral("startOpacity")] = fill.gradient_start_opacity;
    o[QStringLiteral("endOpacity")] = fill.gradient_end_opacity;
    o[QStringLiteral("opacity")] = fill.gradient_opacity;
    o[QStringLiteral("angle")] = fill.gradient_angle;
    o[QStringLiteral("centerX")] = fill.gradient_center_x;
    o[QStringLiteral("centerY")] = fill.gradient_center_y;
    o[QStringLiteral("scale")] = fill.gradient_scale;
    o[QStringLiteral("focalX")] = fill.gradient_focal_x;
    o[QStringLiteral("focalY")] = fill.gradient_focal_y;
    QJsonArray stops;
    for (const auto &stop : extra_stops) {
        QJsonObject s;
        s[QStringLiteral("color")] = QString::number(stop.color, 16);
        s[QStringLiteral("position")] = stop.position;
        s[QStringLiteral("opacity")] = stop.opacity;
        stops.append(s);
    }
    o[QStringLiteral("stops")] = stops;
    return o;
}

static uint32_t parseArgb(const QJsonObject &o, const char *key, uint32_t fallback)
{
    bool ok = false;
    const uint value = o.value(QString::fromUtf8(key)).toString(QString::number(fallback, 16)).toUInt(&ok, 16);
    return ok ? value : fallback;
}

QColor colorFromArgb(uint32_t argb)
{
    return QColor((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff, (argb >> 24) & 0xff);
}

void applyGradientPayload(const QJsonObject &o, Layer &layer)
{
    layer.fill_type = o.value(QStringLiteral("fillType")).toInt(1);
    layer.gradient_spread = gradientSpreadFromPayload(o, layer.gradient_spread);
    layer.gradient_type = normalizedGradientType(o.value(QStringLiteral("gradientType")).toInt(layer.gradient_type));
    layer.gradient_start_color = parseArgb(o, "startColor", layer.gradient_start_color);
    layer.gradient_end_color = parseArgb(o, "endColor", layer.gradient_end_color);
    layer.gradient_start_pos = float(o.value(QStringLiteral("startPos")).toDouble(layer.gradient_start_pos));
    layer.gradient_end_pos = float(o.value(QStringLiteral("endPos")).toDouble(layer.gradient_end_pos));
    layer.gradient_start_opacity = float(o.value(QStringLiteral("startOpacity")).toDouble(layer.gradient_start_opacity));
    layer.gradient_end_opacity = float(o.value(QStringLiteral("endOpacity")).toDouble(layer.gradient_end_opacity));
    layer.gradient_opacity = float(o.value(QStringLiteral("opacity")).toDouble(layer.gradient_opacity));
    layer.gradient_angle = float(o.value(QStringLiteral("angle")).toDouble(layer.gradient_angle));
    layer.gradient_center_x = float(o.value(QStringLiteral("centerX")).toDouble(layer.gradient_center_x));
    layer.gradient_center_y = float(o.value(QStringLiteral("centerY")).toDouble(layer.gradient_center_y));
    layer.gradient_scale = float(o.value(QStringLiteral("scale")).toDouble(layer.gradient_scale));
    layer.gradient_focal_x = float(o.value(QStringLiteral("focalX")).toDouble(layer.gradient_focal_x));
    layer.gradient_focal_y = float(o.value(QStringLiteral("focalY")).toDouble(layer.gradient_focal_y));
    layer.gradient_stops.clear();
    const auto stops = o.value(QStringLiteral("stops")).toArray();
    for (const auto &entry : stops) {
        const auto s = entry.toObject();
        GradientStop stop;
        stop.color = parseArgb(s, "color", 0xffffffffu);
        stop.position = float(s.value(QStringLiteral("position")).toDouble(0.5));
        stop.opacity = float(s.value(QStringLiteral("opacity")).toDouble(1.0));
        layer.gradient_stops.push_back(stop);
    }
}

void fillFormatFromGradientPayload(const QJsonObject &o, RichTextCharFormat &format)
{
    format.fill.type = o.value(QStringLiteral("fillType")).toInt(1);
    format.fill.gradient_spread = gradientSpreadFromPayload(o, format.fill.gradient_spread);
    format.fill.gradient_type = normalizedGradientType(o.value(QStringLiteral("gradientType")).toInt(format.fill.gradient_type));
    format.fill.gradient_start_color = parseArgb(o, "startColor", format.fill.gradient_start_color);
    format.fill.gradient_end_color = parseArgb(o, "endColor", format.fill.gradient_end_color);
    format.fill.gradient_start_pos = float(o.value(QStringLiteral("startPos")).toDouble(format.fill.gradient_start_pos));
    format.fill.gradient_end_pos = float(o.value(QStringLiteral("endPos")).toDouble(format.fill.gradient_end_pos));
    format.fill.gradient_start_opacity = float(o.value(QStringLiteral("startOpacity")).toDouble(format.fill.gradient_start_opacity));
    format.fill.gradient_end_opacity = float(o.value(QStringLiteral("endOpacity")).toDouble(format.fill.gradient_end_opacity));
    format.fill.gradient_opacity = float(o.value(QStringLiteral("opacity")).toDouble(format.fill.gradient_opacity));
    format.fill.gradient_angle = float(o.value(QStringLiteral("angle")).toDouble(format.fill.gradient_angle));
    format.fill.gradient_center_x = float(o.value(QStringLiteral("centerX")).toDouble(format.fill.gradient_center_x));
    format.fill.gradient_center_y = float(o.value(QStringLiteral("centerY")).toDouble(format.fill.gradient_center_y));
    format.fill.gradient_scale = float(o.value(QStringLiteral("scale")).toDouble(format.fill.gradient_scale));
    format.fill.gradient_focal_x = float(o.value(QStringLiteral("focalX")).toDouble(format.fill.gradient_focal_x));
    format.fill.gradient_focal_y = float(o.value(QStringLiteral("focalY")).toDouble(format.fill.gradient_focal_y));
    format.fill.color = format.fill.type == 1 ? format.fill.gradient_start_color
                                               : parseArgb(o, "textColor", format.fill.color);
}

void fillFromGradientPayload(const QJsonObject &o,
                             RichTextFill &fill,
                             std::vector<GradientStop> *extra_stops)
{
    fill.type = 1;
    fill.gradient_spread = gradientSpreadFromPayload(o, fill.gradient_spread);
    fill.gradient_type = normalizedGradientType(o.value(QStringLiteral("gradientType")).toInt(fill.gradient_type));
    fill.gradient_start_color = parseArgb(o, "startColor", fill.gradient_start_color);
    fill.gradient_end_color = parseArgb(o, "endColor", fill.gradient_end_color);
    fill.gradient_start_pos = float(o.value(QStringLiteral("startPos")).toDouble(fill.gradient_start_pos));
    fill.gradient_end_pos = float(o.value(QStringLiteral("endPos")).toDouble(fill.gradient_end_pos));
    fill.gradient_start_opacity = float(o.value(QStringLiteral("startOpacity")).toDouble(fill.gradient_start_opacity));
    fill.gradient_end_opacity = float(o.value(QStringLiteral("endOpacity")).toDouble(fill.gradient_end_opacity));
    fill.gradient_opacity = float(o.value(QStringLiteral("opacity")).toDouble(fill.gradient_opacity));
    fill.gradient_angle = float(o.value(QStringLiteral("angle")).toDouble(fill.gradient_angle));
    fill.gradient_center_x = float(o.value(QStringLiteral("centerX")).toDouble(fill.gradient_center_x));
    fill.gradient_center_y = float(o.value(QStringLiteral("centerY")).toDouble(fill.gradient_center_y));
    fill.gradient_scale = float(o.value(QStringLiteral("scale")).toDouble(fill.gradient_scale));
    fill.gradient_focal_x = float(o.value(QStringLiteral("focalX")).toDouble(fill.gradient_focal_x));
    fill.gradient_focal_y = float(o.value(QStringLiteral("focalY")).toDouble(fill.gradient_focal_y));
    fill.color = fill.gradient_start_color;

    if (!extra_stops)
        return;
    extra_stops->clear();
    const auto stops = o.value(QStringLiteral("stops")).toArray();
    for (const auto &entry : stops) {
        const auto s = entry.toObject();
        GradientStop stop;
        stop.color = parseArgb(s, "color", 0xffffffffu);
        stop.position = float(s.value(QStringLiteral("position")).toDouble(0.5));
        stop.opacity = float(s.value(QStringLiteral("opacity")).toDouble(1.0));
        extra_stops->push_back(stop);
    }
}

QColor gradientStopColor(uint32_t argb, double stop_opacity, double overall_opacity)
{
    QColor color = colorFromArgb(argb);
    color.setAlphaF(std::clamp(color.alphaF() * stop_opacity * overall_opacity, 0.0, 1.0));
    return color;
}

void drawCheckerboard(QPainter &painter, const QRect &rect)
{
    constexpr int cell = 6;
    const QColor light(214, 214, 214);
    const QColor dark(164, 164, 164);
    for (int y = rect.top(); y <= rect.bottom(); y += cell) {
        for (int x = rect.left(); x <= rect.right(); x += cell) {
            const bool alternate = (((x - rect.left()) / cell) + ((y - rect.top()) / cell)) % 2;
            painter.fillRect(QRect(x, y, cell, cell).intersected(rect), alternate ? dark : light);
        }
    }
}
}

StylePresetLibrary::StylePresetLibrary()
{
    load();
}

QString StylePresetLibrary::storagePath() const
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.broadcast-graphics-live");
    QDir dir(base);
    dir.mkpath(QStringLiteral("style-presets"));
    return dir.filePath(QStringLiteral("style-presets/styles.json"));
}

bool StylePresetLibrary::load()
{
    presets_.clear();
    QFile file(storagePath());
    if (!file.exists()) {
        ensureDefaults();
        save();
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto doc = QJsonDocument::fromJson(file.readAll());
    const auto arr = doc.object().value(QStringLiteral("presets")).toArray();
    for (const auto &entry : arr) {
        const auto o = entry.toObject();
        StylePreset p;
        p.id = o.value(QStringLiteral("id")).toString();
        p.name = o.value(QStringLiteral("name")).toString();
        p.category = o.value(QStringLiteral("category")).toString(QStringLiteral("Default"));
        p.kind = stringToKind(o.value(QStringLiteral("kind")).toString());
        p.payload = o.value(QStringLiteral("payload")).toObject();
        if (!p.id.isEmpty() && !p.name.isEmpty()) presets_.append(p);
    }
    ensureDefaults();
    return true;
}

bool StylePresetLibrary::save() const
{
    QFile file(storagePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    QJsonArray arr;
    for (const auto &p : presets_) {
        QJsonObject o;
        o[QStringLiteral("id")] = p.id;
        o[QStringLiteral("name")] = p.name;
        o[QStringLiteral("category")] = p.category;
        o[QStringLiteral("kind")] = kindToString(p.kind);
        o[QStringLiteral("payload")] = p.payload;
        arr.append(o);
    }
    QJsonObject root;
    root[QStringLiteral("format")] = QStringLiteral("OBS-BGS Style Presets");
    root[QStringLiteral("version")] = 2;
    root[QStringLiteral("presets")] = arr;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

bool StylePresetLibrary::importFromFile(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    const auto arr = doc.object().value(QStringLiteral("presets")).toArray();
    for (const auto &entry : arr) {
        const auto o = entry.toObject();
        StylePreset p;
        p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        p.name = o.value(QStringLiteral("name")).toString();
        p.category = o.value(QStringLiteral("category")).toString(QStringLiteral("Imported"));
        p.kind = stringToKind(o.value(QStringLiteral("kind")).toString());
        p.payload = o.value(QStringLiteral("payload")).toObject();
        if (!p.name.isEmpty()) presets_.append(p);
    }
    const bool saved = save();
    if (saved) notifyChanged();
    return saved;
}

bool StylePresetLibrary::exportToFile(const QString &path, StylePresetKind kind, QString *error) const
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = file.errorString();
        return false;
    }
    QJsonArray arr;
    for (const auto &p : presets_) {
        if (p.kind != kind) continue;
        QJsonObject o;
        o[QStringLiteral("id")] = p.id;
        o[QStringLiteral("name")] = p.name;
        o[QStringLiteral("category")] = p.category;
        o[QStringLiteral("kind")] = kindToString(p.kind);
        o[QStringLiteral("payload")] = p.payload;
        arr.append(o);
    }
    QJsonObject root;
    root[QStringLiteral("format")] = QStringLiteral("OBS-BGS Style Presets");
    root[QStringLiteral("version")] = 2;
    root[QStringLiteral("presets")] = arr;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

QList<StylePreset> StylePresetLibrary::presets(StylePresetKind kind) const
{
    QList<StylePreset> out;
    for (const auto &p : presets_) if (p.kind == kind) out.append(p);
    return out;
}

bool StylePresetLibrary::findById(const QString &id, StylePreset *out) const
{
    for (const auto &preset : presets_) {
        if (preset.id == id) {
            if (out) *out = preset;
            return true;
        }
    }
    return false;
}

QString StylePresetLibrary::displayNameForId(const QString &id) const
{
    StylePreset preset;
    return findById(id, &preset) ? preset.name : QStringLiteral("Missing preset (%1)").arg(id);
}

QStringList StylePresetLibrary::categories(StylePresetKind kind) const
{
    QStringList out;
    for (const auto &p : presets_) if (p.kind == kind && !out.contains(p.category)) out.append(p.category);
    out.sort(Qt::CaseInsensitive);
    return out;
}

void StylePresetLibrary::upsert(const StylePreset &preset)
{
    for (auto &p : presets_) {
        if (p.id == preset.id) {
            p = preset;
            save();
            notifyChanged();
            return;
        }
    }
    presets_.append(preset);
    save();
    notifyChanged();
}

bool StylePresetLibrary::remove(const QString &id)
{
    for (int i = 0; i < presets_.size(); ++i) {
        if (presets_[i].id == id) {
            presets_.removeAt(i);
            save();
            notifyChanged();
            return true;
        }
    }
    return false;
}

StylePreset StylePresetLibrary::makeTextPreset(const Layer &layer, const QString &name, const QString &category)
{
    StylePreset p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.name = name;
    p.category = category.isEmpty() ? QStringLiteral("User") : category;
    p.kind = StylePresetKind::Text;
    QJsonObject o;
    o[QStringLiteral("fontFamily")] = QString::fromStdString(layer.font_family);
    o[QStringLiteral("fontStyle")] = QString::fromStdString(layer.font_style);
    o[QStringLiteral("fontSize")] = layer.font_size;
    o[QStringLiteral("bold")] = layer.font_bold;
    o[QStringLiteral("italic")] = layer.font_italic;
    o[QStringLiteral("underline")] = layer.text_underline;
    o[QStringLiteral("strike")] = layer.text_strikethrough;
    o[QStringLiteral("tracking")] = layer.char_tracking;
    o[QStringLiteral("scaleX")] = layer.char_scale_x;
    o[QStringLiteral("scaleY")] = layer.char_scale_y;
    o[QStringLiteral("baseline")] = layer.baseline_shift;
    o[QStringLiteral("leading")] = layer.text_leading;
    o[QStringLiteral("textColor")] = QString::number(layer.text_color, 16);
    o[QStringLiteral("alignH")] = layer.align_h;
    o[QStringLiteral("alignV")] = layer.align_v;
    o[QStringLiteral("paragraphBefore")] = layer.paragraph_space_before;
    o[QStringLiteral("paragraphAfter")] = layer.paragraph_space_after;
    o[QStringLiteral("paragraphLeft")] = layer.paragraph_indent_left;
    o[QStringLiteral("paragraphRight")] = layer.paragraph_indent_right;
    o[QStringLiteral("paragraphFirst")] = layer.paragraph_indent_first_line;
    o[QStringLiteral("fillType")] = layer.fill_type;
    o[QStringLiteral("gradient")] = gradientPayloadFromLayer(layer);
    p.payload = o;
    return p;
}

StylePreset StylePresetLibrary::makeGradientPreset(const Layer &layer, const QString &name, const QString &category)
{
    StylePreset p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.name = name;
    p.category = category.isEmpty() ? QStringLiteral("User") : category;
    p.kind = StylePresetKind::Gradient;
    p.payload = gradientPayloadFromLayer(layer);
    p.payload[QStringLiteral("fillType")] = 1;
    return p;
}

StylePreset StylePresetLibrary::makeGradientPreset(const RichTextFill &fill,
                                                   const std::vector<GradientStop> &extra_stops,
                                                   const QString &name,
                                                   const QString &category)
{
    StylePreset p;
    p.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    p.name = name;
    p.category = category.isEmpty() ? QStringLiteral("User") : category;
    p.kind = StylePresetKind::Gradient;
    p.payload = gradientPayloadFromFill(fill, extra_stops);
    return p;
}

bool StylePresetLibrary::applyTextPreset(const StylePreset &preset, Layer &layer)
{
    if (preset.kind != StylePresetKind::Text) return false;
    const auto o = preset.payload;
    layer.font_family = o.value(QStringLiteral("fontFamily")).toString(QString::fromStdString(layer.font_family)).toStdString();
    layer.font_style = o.value(QStringLiteral("fontStyle")).toString(QString::fromStdString(layer.font_style)).toStdString();
    layer.font_size = o.value(QStringLiteral("fontSize")).toInt(layer.font_size);
    layer.font_size_prop.static_value = layer.font_size; layer.font_size_prop.keyframes.clear();
    layer.font_bold = o.value(QStringLiteral("bold")).toBool(layer.font_bold);
    layer.font_italic = o.value(QStringLiteral("italic")).toBool(layer.font_italic);
    layer.text_underline = o.value(QStringLiteral("underline")).toBool(layer.text_underline);
    layer.text_strikethrough = o.value(QStringLiteral("strike")).toBool(layer.text_strikethrough);
    layer.char_tracking = float(o.value(QStringLiteral("tracking")).toDouble(layer.char_tracking));
    layer.char_tracking_prop.static_value = layer.char_tracking; layer.char_tracking_prop.keyframes.clear();
    layer.char_scale_x = float(o.value(QStringLiteral("scaleX")).toDouble(layer.char_scale_x));
    layer.char_scale_x_prop.static_value = layer.char_scale_x; layer.char_scale_x_prop.keyframes.clear();
    layer.char_scale_y = float(o.value(QStringLiteral("scaleY")).toDouble(layer.char_scale_y));
    layer.char_scale_y_prop.static_value = layer.char_scale_y; layer.char_scale_y_prop.keyframes.clear();
    layer.baseline_shift = float(o.value(QStringLiteral("baseline")).toDouble(layer.baseline_shift));
    layer.baseline_shift_prop.static_value = layer.baseline_shift; layer.baseline_shift_prop.keyframes.clear();
    layer.text_leading = float(o.value(QStringLiteral("leading")).toDouble(layer.text_leading));
    layer.text_color = parseArgb(o, "textColor", layer.text_color);
    layer.align_h = o.value(QStringLiteral("alignH")).toInt(layer.align_h);
    layer.align_v = std::clamp(o.value(QStringLiteral("alignV")).toInt(layer.align_v), 0, 3);
    layer.paragraph_space_before = float(o.value(QStringLiteral("paragraphBefore")).toDouble(layer.paragraph_space_before));
    layer.paragraph_space_before_prop.static_value = layer.paragraph_space_before; layer.paragraph_space_before_prop.keyframes.clear();
    layer.paragraph_space_after = float(o.value(QStringLiteral("paragraphAfter")).toDouble(layer.paragraph_space_after));
    layer.paragraph_space_after_prop.static_value = layer.paragraph_space_after; layer.paragraph_space_after_prop.keyframes.clear();
    layer.paragraph_indent_left = float(o.value(QStringLiteral("paragraphLeft")).toDouble(layer.paragraph_indent_left));
    layer.paragraph_indent_left_prop.static_value = layer.paragraph_indent_left; layer.paragraph_indent_left_prop.keyframes.clear();
    layer.paragraph_indent_right = float(o.value(QStringLiteral("paragraphRight")).toDouble(layer.paragraph_indent_right));
    layer.paragraph_indent_right_prop.static_value = layer.paragraph_indent_right; layer.paragraph_indent_right_prop.keyframes.clear();
    layer.paragraph_indent_first_line = float(o.value(QStringLiteral("paragraphFirst")).toDouble(layer.paragraph_indent_first_line));
    layer.paragraph_indent_first_line_prop.static_value = layer.paragraph_indent_first_line; layer.paragraph_indent_first_line_prop.keyframes.clear();
    if (o.contains(QStringLiteral("gradient"))) applyGradientPayload(o.value(QStringLiteral("gradient")).toObject(), layer);
    return true;
}

bool StylePresetLibrary::applyGradientPreset(const StylePreset &preset, Layer &layer)
{
    if (preset.kind != StylePresetKind::Gradient) return false;
    applyGradientPayload(preset.payload, layer);
    layer.fill_type = 1;
    if (layer.type == LayerType::Text || layer.type == LayerType::Clock || layer.type == LayerType::Ticker)
        layer.text_color = layer.gradient_start_color;
    return true;
}

bool StylePresetLibrary::gradientPresetToFill(const StylePreset &preset,
                                              RichTextFill &fill,
                                              std::vector<GradientStop> *extra_stops)
{
    if (preset.kind != StylePresetKind::Gradient)
        return false;
    fillFromGradientPayload(preset.payload, fill, extra_stops);
    return true;
}

bool StylePresetLibrary::isBuiltIn(const StylePreset &preset)
{
    return preset.category.compare(QStringLiteral("Built-in"), Qt::CaseInsensitive) == 0;
}

QString StylePresetLibrary::gradientDescription(const StylePreset &preset)
{
    if (preset.kind != StylePresetKind::Gradient)
        return preset.name;
    const int type = normalizedGradientType(preset.payload.value(QStringLiteral("gradientType")).toInt(0));
    QString type_name = bgl_tr("OBSTitles.LinearGradient");
    if (type == 1) type_name = bgl_tr("OBSTitles.RadialGradient");
    else if (type == 2) type_name = bgl_tr("OBSTitles.ConicalGradient");
    const int stop_count = 2 + preset.payload.value(QStringLiteral("stops")).toArray().size();
    return QStringLiteral("%1 — %2\n%3 · %4 stops")
        .arg(preset.name, preset.category, type_name)
        .arg(stop_count);
}

void StylePresetLibrary::subscribe(QObject *context, std::function<void()> callback)
{
    if (!context || !callback)
        return;
    auto &listeners = presetChangeListeners();
    listeners.erase(std::remove_if(listeners.begin(), listeners.end(), [](const PresetChangeListener &listener) {
        return listener.context.isNull();
    }), listeners.end());
    listeners.push_back({context, std::move(callback)});
}

void StylePresetLibrary::notifyChanged()
{
    auto &listeners = presetChangeListeners();
    for (auto it = listeners.begin(); it != listeners.end();) {
        if (it->context.isNull()) {
            it = listeners.erase(it);
            continue;
        }
        if (it->callback)
            it->callback();
        ++it;
    }
}


bool StylePresetLibrary::textPresetToCharFormat(const StylePreset &preset, RichTextCharFormat &format)
{
    if (preset.kind != StylePresetKind::Text) return false;
    const auto o = preset.payload;
    format.font_family = o.value(QStringLiteral("fontFamily")).toString(QString::fromStdString(format.font_family)).toStdString();
    format.font_style = o.value(QStringLiteral("fontStyle")).toString(QString::fromStdString(format.font_style)).toStdString();
    format.font_size = o.value(QStringLiteral("fontSize")).toInt(format.font_size);
    format.bold = o.value(QStringLiteral("bold")).toBool(format.bold);
    format.italic = o.value(QStringLiteral("italic")).toBool(format.italic);
    format.underline = o.value(QStringLiteral("underline")).toBool(format.underline);
    format.strikethrough = o.value(QStringLiteral("strike")).toBool(format.strikethrough);
    format.tracking = float(o.value(QStringLiteral("tracking")).toDouble(format.tracking));
    format.scale_x = float(o.value(QStringLiteral("scaleX")).toDouble(format.scale_x));
    format.scale_y = float(o.value(QStringLiteral("scaleY")).toDouble(format.scale_y));
    format.baseline_shift = float(o.value(QStringLiteral("baseline")).toDouble(format.baseline_shift));
    format.fill.color = parseArgb(o, "textColor", format.fill.color);
    format.fill.type = o.value(QStringLiteral("fillType")).toInt(format.fill.type);
    if (o.contains(QStringLiteral("gradient")))
        fillFormatFromGradientPayload(o.value(QStringLiteral("gradient")).toObject(), format);
    return true;
}

bool StylePresetLibrary::gradientPresetToCharFormat(const StylePreset &preset, RichTextCharFormat &format)
{
    if (preset.kind != StylePresetKind::Gradient) return false;
    fillFormatFromGradientPayload(preset.payload, format);
    return true;
}

uint32_t StylePresetLibrary::textPresetCharMask()
{
    return RichTextCharFontFamily | RichTextCharFontStyle | RichTextCharFontSize |
           RichTextCharBold | RichTextCharItalic | RichTextCharUnderline |
           RichTextCharStrikethrough | RichTextCharTracking | RichTextCharScaleX |
           RichTextCharScaleY | RichTextCharBaselineShift | RichTextCharFillColor;
}

uint32_t StylePresetLibrary::gradientPresetCharMask()
{
    return RichTextCharFillColor;
}

QPixmap StylePresetLibrary::thumbnail(const StylePreset &preset, const QSize &size)
{
    QPixmap pix(size);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing, true);
    QRect r = pix.rect().adjusted(2, 2, -2, -2);
    p.setPen(QColor(60, 60, 60));
    if (preset.kind == StylePresetKind::Gradient) {
        drawCheckerboard(p, r);
        const int type = normalizedGradientType(preset.payload.value(QStringLiteral("gradientType")).toInt(0));
        const double opacity = std::clamp(preset.payload.value(QStringLiteral("opacity")).toDouble(1.0), 0.0, 1.0);
        const double angle = preset.payload.value(QStringLiteral("angle")).toDouble(0.0);
        const QPointF center(r.left() + std::clamp(preset.payload.value(QStringLiteral("centerX")).toDouble(0.5), 0.0, 1.0) * r.width(),
                             r.top() + std::clamp(preset.payload.value(QStringLiteral("centerY")).toDouble(0.5), 0.0, 1.0) * r.height());
        QGradientStops gradient_stops;
        gradient_stops.append(QGradientStop(
            std::clamp(preset.payload.value(QStringLiteral("startPos")).toDouble(0.0), 0.0, 1.0),
            gradientStopColor(parseArgb(preset.payload, "startColor", 0xff4b6ea8),
                              preset.payload.value(QStringLiteral("startOpacity")).toDouble(1.0), opacity)));
        const auto stops = preset.payload.value(QStringLiteral("stops")).toArray();
        for (const auto &entry : stops) {
            const auto s = entry.toObject();
            gradient_stops.append(QGradientStop(
                std::clamp(s.value(QStringLiteral("position")).toDouble(0.5), 0.0, 1.0),
                gradientStopColor(parseArgb(s, "color", 0xffffffff),
                                  s.value(QStringLiteral("opacity")).toDouble(1.0), opacity)));
        }
        gradient_stops.append(QGradientStop(
            std::clamp(preset.payload.value(QStringLiteral("endPos")).toDouble(1.0), 0.0, 1.0),
            gradientStopColor(parseArgb(preset.payload, "endColor", 0xff1b1b1b),
                              preset.payload.value(QStringLiteral("endOpacity")).toDouble(1.0), opacity)));
        std::sort(gradient_stops.begin(), gradient_stops.end(), [](const QGradientStop &a, const QGradientStop &b) {
            return a.first < b.first;
        });

        const auto spread = [&]() {
            switch (gradientSpreadFromPayload(preset.payload, 0)) {
            case 1: return QGradient::ReflectSpread;
            case 2: return QGradient::RepeatSpread;
            default: return QGradient::PadSpread;
            }
        }();
        QBrush gradient_brush;
        if (type == 1) {
            const double scale = std::max(0.05, preset.payload.value(QStringLiteral("scale")).toDouble(1.0));
            QRadialGradient gradient(center, std::max(1.0, 0.55 * std::min(r.width(), r.height()) * scale));
            gradient.setSpread(spread);
            gradient.setStops(gradient_stops);
            gradient_brush = QBrush(gradient);
        } else if (type == 2) {
            QConicalGradient gradient(center, -angle);
            gradient.setSpread(spread);
            gradient.setStops(gradient_stops);
            gradient_brush = QBrush(gradient);
        } else {
            constexpr double kPi = 3.14159265358979323846;
            const double radians = angle * kPi / 180.0;
            const QPointF direction(std::cos(radians), std::sin(radians));
            const double half = 0.5 * std::hypot(r.width(), r.height());
            QLinearGradient gradient(center - direction * half, center + direction * half);
            gradient.setSpread(spread);
            gradient.setStops(gradient_stops);
            gradient_brush = QBrush(gradient);
        }
        p.setBrush(gradient_brush);
        p.drawRoundedRect(r, 3, 3);
    } else {
        const QColor c = colorFromArgb(parseArgb(preset.payload, "textColor", 0xffffffff));
        p.setBrush(QColor(35, 35, 35));
        p.drawRoundedRect(r, 5, 5);
        QFont f(preset.payload.value(QStringLiteral("fontFamily")).toString(QStringLiteral("Arial")), 22);
        f.setBold(preset.payload.value(QStringLiteral("bold")).toBool(false));
        f.setItalic(preset.payload.value(QStringLiteral("italic")).toBool(false));
        p.setFont(f);
        p.setPen(c);
        p.drawText(r, Qt::AlignCenter, QStringLiteral("Aa"));
    }
    return pix;
}

void StylePresetLibrary::ensureDefaults()
{
    bool hasText = false, hasGradient = false;
    for (const auto &p : presets_) {
        hasText |= p.kind == StylePresetKind::Text;
        hasGradient |= p.kind == StylePresetKind::Gradient;
    }
    if (!hasText) {
        Layer l;
        l.font_family = "Arial";
        l.font_size = 64;
        l.font_bold = true;
        l.text_color = 0xffffffff;
        presets_.append(makeTextPreset(l, QStringLiteral("Clean Broadcast"), QStringLiteral("Built-in")));
        l.font_italic = true;
        l.char_tracking = 40.0f;
        l.text_color = 0xff00a1b9;
        presets_.append(makeTextPreset(l, QStringLiteral("Editorial Accent"), QStringLiteral("Built-in")));
    }
    if (!hasGradient) {
        Layer l;
        l.fill_type = 1;
        l.gradient_start_color = 0xff00a1b9;
        l.gradient_end_color = 0xff0b2530;
        presets_.append(makeGradientPreset(l, QStringLiteral("Cyan News Bar"), QStringLiteral("Built-in")));
        l.gradient_start_color = 0xfff15b24;
        l.gradient_end_color = 0xff1b1b1b;
        presets_.append(makeGradientPreset(l, QStringLiteral("Orange Alert"), QStringLiteral("Built-in")));
    }
}

StylePresetPanel::StylePresetPanel(StylePresetKind kind, QWidget *parent)
    : QWidget(parent), kind_(kind)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *top = new QHBoxLayout;
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(bgl_tr("OBSTitles.SearchStyles"));
    category_filter_ = new QComboBox(this);
    category_filter_->setEditable(false);
    top->addWidget(search_, 1);
    top->addWidget(category_filter_);
    layout->addLayout(top);

    list_ = new QListWidget(this);
    list_->setViewMode(QListView::IconMode);
    list_->setResizeMode(QListView::Adjust);
    list_->setMovement(QListView::Static);
    list_->setSpacing(kind_ == StylePresetKind::Gradient ? 4 : 2);
    list_->setIconSize(kind_ == StylePresetKind::Gradient ? QSize(32, 32) : QSize(96, 48));
    list_->setGridSize(kind_ == StylePresetKind::Gradient ? QSize(42, 42) : QSize(132, 92));
    list_->setUniformItemSizes(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(list_, 1);

    auto *buttons = new QHBoxLayout;
    add_button_ = new QToolButton(this); add_button_->setText(QStringLiteral("+"));
    add_button_->setToolTip(kind_ == StylePresetKind::Gradient
                                ? bgl_tr("OBSTitles.SaveGradientPreset")
                                : bgl_tr("OBSTitles.SaveStylePreset"));
    apply_button_ = new QToolButton(this); apply_button_->setText(QStringLiteral("✓")); apply_button_->setToolTip(bgl_tr("OBSTitles.ApplyStylePreset"));
    delete_button_ = new QToolButton(this); delete_button_->setText(QStringLiteral("−")); delete_button_->setToolTip(bgl_tr("OBSTitles.DeleteStylePreset"));
    import_button_ = new QToolButton(this); import_button_->setText(QStringLiteral("Import")); import_button_->setToolTip(bgl_tr("OBSTitles.ImportStylePresets"));
    export_button_ = new QToolButton(this); export_button_->setText(QStringLiteral("Export")); export_button_->setToolTip(bgl_tr("OBSTitles.ExportStylePresets"));
    buttons->addWidget(add_button_);
    buttons->addWidget(apply_button_);
    buttons->addWidget(delete_button_);
    buttons->addStretch(1);
    buttons->addWidget(import_button_);
    buttons->addWidget(export_button_);
    layout->addLayout(buttons);

    connect(search_, &QLineEdit::textChanged, this, &StylePresetPanel::refreshList);
    connect(category_filter_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &StylePresetPanel::refreshList);
    if (kind_ == StylePresetKind::Gradient) {
        connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *) {
            applySelectedPreset();
        });
        apply_button_->hide();
    } else {
        connect(list_, &QListWidget::itemDoubleClicked, this, [this]() {
            applySelectedPreset();
        });
    }
    connect(list_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *, QListWidgetItem *) {
                const StylePreset *preset = selectedPreset();
                delete_button_->setEnabled(preset && !StylePresetLibrary::isBuiltIn(*preset));
                apply_button_->setEnabled(preset != nullptr);
            });
    connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        auto *item = list_->itemAt(pos);
        if (!item)
            return;
        list_->setCurrentItem(item);
        const StylePreset *preset = selectedPreset();
        if (!preset || StylePresetLibrary::isBuiltIn(*preset))
            return;
        QMenu menu(list_);
        QAction *remove_action = menu.addAction(bgl_tr("OBSTitles.DeleteStylePreset"));
        if (menu.exec(list_->viewport()->mapToGlobal(pos)) == remove_action)
            deleteSelectedPreset();
    });
    connect(add_button_, &QToolButton::clicked, this, &StylePresetPanel::addCurrentAsPreset);
    connect(apply_button_, &QToolButton::clicked, this, &StylePresetPanel::applySelectedPreset);
    connect(delete_button_, &QToolButton::clicked, this, &StylePresetPanel::deleteSelectedPreset);
    connect(import_button_, &QToolButton::clicked, this, &StylePresetPanel::importPresets);
    connect(export_button_, &QToolButton::clicked, this, &StylePresetPanel::exportPresets);

    rebuildCategoryFilter();
    refreshList();
    delete_button_->setEnabled(false);
    apply_button_->setEnabled(false);
    StylePresetLibrary::subscribe(this, [this]() {
        reload();
    });
}

void StylePresetPanel::setCreatePresetCallback(std::function<StylePreset(const QString &, const QString &)> callback)
{
    create_callback_ = std::move(callback);
}

void StylePresetPanel::setApplyPresetCallback(std::function<void(const StylePreset &)> callback)
{
    apply_callback_ = std::move(callback);
}

void StylePresetPanel::reload()
{
    library_.load();
    rebuildCategoryFilter();
    refreshList();
}

void StylePresetPanel::rebuildCategoryFilter()
{
    const QString current = category_filter_->currentData().toString();
    category_filter_->blockSignals(true);
    category_filter_->clear();
    category_filter_->addItem(bgl_tr("OBSTitles.AllCategories"), QString());
    for (const auto &category : library_.categories(kind_)) category_filter_->addItem(category, category);
    const int idx = category_filter_->findData(current);
    if (idx >= 0) category_filter_->setCurrentIndex(idx);
    category_filter_->blockSignals(false);
}

void StylePresetPanel::refreshList()
{
    const QString needle = search_->text().trimmed();
    const QString category = category_filter_->currentData().toString();
    list_->clear();
    for (const auto &preset : library_.presets(kind_)) {
        if (!category.isEmpty() && preset.category != category) continue;
        if (!needle.isEmpty() && !preset.name.contains(needle, Qt::CaseInsensitive) && !preset.category.contains(needle, Qt::CaseInsensitive)) continue;
        const bool gradient = kind_ == StylePresetKind::Gradient;
        const QSize thumbnail_size = gradient ? QSize(32, 32) : QSize(96, 48);
        const QString label = gradient ? QString() : preset.name + QStringLiteral("\n") + preset.category;
        auto *item = new QListWidgetItem(QIcon(StylePresetLibrary::thumbnail(preset, thumbnail_size)), label, list_);
        item->setData(Qt::UserRole, preset.id);
        item->setToolTip(gradient ? StylePresetLibrary::gradientDescription(preset)
                                  : preset.name + QStringLiteral(" — ") + preset.category);
        if (gradient)
            item->setSizeHint(QSize(42, 42));
    }
}

StylePreset *StylePresetPanel::selectedPreset()
{
    auto *item = list_->currentItem();
    if (!item) return nullptr;
    const QString id = item->data(Qt::UserRole).toString();
    static StylePreset selected;
    for (const auto &preset : library_.presets(kind_)) {
        if (preset.id == id) {
            selected = preset;
            return &selected;
        }
    }
    return nullptr;
}

const StylePreset *StylePresetPanel::selectedPreset() const
{
    return const_cast<StylePresetPanel *>(this)->selectedPreset();
}

void StylePresetPanel::addCurrentAsPreset()
{
    if (!create_callback_) return;
    bool ok = false;
    const QString dialog_title = kind_ == StylePresetKind::Gradient
        ? bgl_tr("OBSTitles.SaveGradientPreset")
        : bgl_tr("OBSTitles.SaveStylePreset");
    const QString name = QInputDialog::getText(this, dialog_title, bgl_tr("OBSTitles.StylePresetName"), QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    const QString category = QInputDialog::getText(this, bgl_tr("OBSTitles.StylePresetCategory"), bgl_tr("OBSTitles.StylePresetCategory"), QLineEdit::Normal, QStringLiteral("User"), &ok);
    if (!ok) return;
    library_.upsert(create_callback_(name.trimmed(), category.trimmed().isEmpty() ? QStringLiteral("User") : category.trimmed()));
    rebuildCategoryFilter();
    refreshList();
}

void StylePresetPanel::applySelectedPreset()
{
    const StylePreset *preset = selectedPreset();
    if (preset && apply_callback_) apply_callback_(*preset);
}

void StylePresetPanel::deleteSelectedPreset()
{
    const StylePreset *preset = selectedPreset();
    if (!preset) return;
    if (StylePresetLibrary::isBuiltIn(*preset)) return;
    if (QMessageBox::question(this, bgl_tr("OBSTitles.DeleteStylePreset"), bgl_tr("OBSTitles.DeleteStylePresetConfirm")) != QMessageBox::Yes) return;
    library_.remove(preset->id);
    rebuildCategoryFilter();
    refreshList();
}

void StylePresetPanel::importPresets()
{
    const QString path = QFileDialog::getOpenFileName(this, bgl_tr("OBSTitles.ImportStylePresets"), QString(), QStringLiteral("OBS BGS Style Presets (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!library_.importFromFile(path, &error)) QMessageBox::warning(this, bgl_tr("OBSTitles.ImportStylePresets"), error);
    rebuildCategoryFilter();
    refreshList();
}

void StylePresetPanel::exportPresets()
{
    const QString path = QFileDialog::getSaveFileName(this, bgl_tr("OBSTitles.ExportStylePresets"), QStringLiteral("style-presets.json"), QStringLiteral("OBS BGS Style Presets (*.json)"));
    if (path.isEmpty()) return;
    QString error;
    if (!library_.exportToFile(path, kind_, &error)) QMessageBox::warning(this, bgl_tr("OBSTitles.ExportStylePresets"), error);
}

} // namespace obsbgs
