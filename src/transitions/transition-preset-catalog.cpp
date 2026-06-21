#include "transition-preset-catalog.h"

#include "effect-preset-catalog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>

#include <algorithm>
#include <cmath>

namespace gsp::transitions {
namespace {

constexpr qint64 kMaxPresetFileBytes = 1024 * 1024;

QString normalized_id(QString value)
{
    value = value.trimmed().toLower();
    value.replace(QLatin1Char('_'), QLatin1Char('-'));
    value.replace(QLatin1Char(' '), QLatin1Char('-'));
    return value;
}

bool path_is_within(const QString &path, const QString &root)
{
    if (path.isEmpty() || root.isEmpty())
        return false;
    QFileInfo path_info(path);
    QFileInfo root_info(root);
    QString normalized_path = path_info.canonicalFilePath();
    QString normalized_root = root_info.canonicalFilePath();
    if (normalized_path.isEmpty()) normalized_path = path_info.absoluteFilePath();
    if (normalized_root.isEmpty()) normalized_root = root_info.absoluteFilePath();
    normalized_path = QDir::fromNativeSeparators(QDir::cleanPath(normalized_path));
    normalized_root = QDir::fromNativeSeparators(QDir::cleanPath(normalized_root));
#ifdef Q_OS_WIN
    normalized_path = normalized_path.toLower();
    normalized_root = normalized_root.toLower();
#endif
    return normalized_path == normalized_root ||
           normalized_path.startsWith(normalized_root + QLatin1Char('/'));
}

bool paths_equal(const QString &first, const QString &second)
{
    QFileInfo first_info(first);
    QFileInfo second_info(second);
    QString normalized_first = first_info.canonicalFilePath();
    QString normalized_second = second_info.canonicalFilePath();
    if (normalized_first.isEmpty()) normalized_first = first_info.absoluteFilePath();
    if (normalized_second.isEmpty()) normalized_second = second_info.absoluteFilePath();
    normalized_first = QDir::fromNativeSeparators(QDir::cleanPath(normalized_first));
    normalized_second = QDir::fromNativeSeparators(QDir::cleanPath(normalized_second));
#ifdef Q_OS_WIN
    return normalized_first.compare(normalized_second, Qt::CaseInsensitive) == 0;
#else
    return normalized_first == normalized_second;
#endif
}

QStringList category_path_from_json(const QJsonValue &value)
{
    QStringList parts;
    if (value.isString()) {
        QString path = value.toString().trimmed();
        path.replace(QLatin1Char('\\'), QLatin1Char('/'));
        parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    } else if (value.isArray()) {
        const QJsonArray array = value.toArray();
        for (const QJsonValue &part : array) {
            if (!part.isString())
                return {};
            parts.push_back(part.toString());
        }
    }

    if (parts.size() > 16)
        return {};
    for (QString &part : parts) {
        part = part.trimmed();
        if (part.isEmpty() || part.size() > 128 ||
            part == QStringLiteral(".") || part == QStringLiteral("..") ||
            part.contains(QLatin1Char('/')) || part.contains(QLatin1Char('\\')))
            return {};
        for (const QChar ch : part) {
            if (ch.category() == QChar::Other_Control)
                return {};
        }
    }
    return parts;
}

bool valid_transition_category_path(const QStringList &parts, bool text_transition)
{
    if (parts.size() < 2 ||
        parts.at(0).compare(QStringLiteral("Transitions"), Qt::CaseInsensitive) != 0)
        return false;
    const QString expected_kind = text_transition
        ? QStringLiteral("Text") : QStringLiteral("General");
    return parts.at(1).compare(expected_kind, Qt::CaseInsensitive) == 0;
}

void set_error(QString *error_message, const QString &message)
{
    if (error_message)
        *error_message = message;
}

} // namespace

QString transition_type_id(LayerTransitionType type)
{
    switch (type) {
    case LayerTransitionType::Dissolve: return QStringLiteral("dissolve");
    case LayerTransitionType::Opacity: return QStringLiteral("opacity");
    case LayerTransitionType::OpacityBlur: return QStringLiteral("opacity-blur");
    case LayerTransitionType::Scale: return QStringLiteral("scale");
    case LayerTransitionType::Wipe: return QStringLiteral("wipe");
    case LayerTransitionType::Slide: return QStringLiteral("slide");
    case LayerTransitionType::ZoomBlur: return QStringLiteral("zoom-blur");
    case LayerTransitionType::TextFade: return QStringLiteral("text-fade");
    case LayerTransitionType::TextSlide: return QStringLiteral("text-slide");
    case LayerTransitionType::TextScale: return QStringLiteral("text-scale");
    case LayerTransitionType::TextBlur: return QStringLiteral("text-blur");
    case LayerTransitionType::TextWipe: return QStringLiteral("text-wipe");
    case LayerTransitionType::BlurSlide: return QStringLiteral("blur-slide");
    case LayerTransitionType::TextBlurSlide: return QStringLiteral("text-blur-slide");
    default: return QStringLiteral("dissolve");
    }
}

bool transition_type_from_id(const QString &id, LayerTransitionType *type)
{
    if (!type) return false;
    const QString value = normalized_id(id);
    if (value == QStringLiteral("dissolve") || value == QStringLiteral("cross-dissolve")) *type = LayerTransitionType::Dissolve;
    else if (value == QStringLiteral("opacity") || value == QStringLiteral("fade")) *type = LayerTransitionType::Opacity;
    else if (value == QStringLiteral("opacity-blur") || value == QStringLiteral("fade-blur")) *type = LayerTransitionType::OpacityBlur;
    else if (value == QStringLiteral("scale")) *type = LayerTransitionType::Scale;
    else if (value == QStringLiteral("wipe")) *type = LayerTransitionType::Wipe;
    else if (value == QStringLiteral("slide") || value == QStringLiteral("push")) *type = LayerTransitionType::Slide;
    else if (value == QStringLiteral("zoom-blur")) *type = LayerTransitionType::ZoomBlur;
    else if (value == QStringLiteral("text-fade") || value == QStringLiteral("fade-text")) *type = LayerTransitionType::TextFade;
    else if (value == QStringLiteral("text-slide") || value == QStringLiteral("slide-text")) *type = LayerTransitionType::TextSlide;
    else if (value == QStringLiteral("text-scale") || value == QStringLiteral("scale-text")) *type = LayerTransitionType::TextScale;
    else if (value == QStringLiteral("text-blur") || value == QStringLiteral("blur-text")) *type = LayerTransitionType::TextBlur;
    else if (value == QStringLiteral("text-wipe") || value == QStringLiteral("wipe-text")) *type = LayerTransitionType::TextWipe;
    else if (value == QStringLiteral("blur-slide") || value == QStringLiteral("slide-blur")) *type = LayerTransitionType::BlurSlide;
    else if (value == QStringLiteral("text-blur-slide") || value == QStringLiteral("blur-slide-text")) *type = LayerTransitionType::TextBlurSlide;
    else return false;
    return true;
}

QString transition_unit_id(LayerTransitionUnit unit)
{
    switch (unit) {
    case LayerTransitionUnit::Word: return QStringLiteral("word");
    case LayerTransitionUnit::Sentence: return QStringLiteral("sentence");
    case LayerTransitionUnit::Character:
    default: return QStringLiteral("character");
    }
}

bool transition_unit_from_id(const QString &id, LayerTransitionUnit *unit)
{
    if (!unit) return false;
    const QString value = normalized_id(id);
    if (value == QStringLiteral("character") || value == QStringLiteral("characters") || value == QStringLiteral("char"))
        *unit = LayerTransitionUnit::Character;
    else if (value == QStringLiteral("word") || value == QStringLiteral("words"))
        *unit = LayerTransitionUnit::Word;
    else if (value == QStringLiteral("sentence") || value == QStringLiteral("sentences") || value == QStringLiteral("line"))
        *unit = LayerTransitionUnit::Sentence;
    else
        return false;
    return true;
}

QString transition_direction_id(LayerTransitionDirection direction)
{
    switch (direction) {
    case LayerTransitionDirection::Left: return QStringLiteral("left");
    case LayerTransitionDirection::Right: return QStringLiteral("right");
    case LayerTransitionDirection::Up: return QStringLiteral("up");
    case LayerTransitionDirection::Down: return QStringLiteral("down");
    case LayerTransitionDirection::None:
    default: return QStringLiteral("none");
    }
}

bool transition_direction_from_id(const QString &id, LayerTransitionDirection *direction)
{
    if (!direction) return false;
    const QString value = normalized_id(id);
    if (value.isEmpty() || value == QStringLiteral("none")) *direction = LayerTransitionDirection::None;
    else if (value == QStringLiteral("left")) *direction = LayerTransitionDirection::Left;
    else if (value == QStringLiteral("right")) *direction = LayerTransitionDirection::Right;
    else if (value == QStringLiteral("up") || value == QStringLiteral("top")) *direction = LayerTransitionDirection::Up;
    else if (value == QStringLiteral("down") || value == QStringLiteral("bottom")) *direction = LayerTransitionDirection::Down;
    else return false;
    return true;
}

QString transition_easing_id(EasingType easing)
{
    switch (easing) {
    case EasingType::Linear: return QStringLiteral("linear");
    case EasingType::EaseIn: return QStringLiteral("ease-in");
    case EasingType::EaseOut: return QStringLiteral("ease-out");
    case EasingType::Hold: return QStringLiteral("hold");
    case EasingType::Bezier: return QStringLiteral("bezier");
    case EasingType::EaseInOut:
    default: return QStringLiteral("ease-in-out");
    }
}

bool transition_easing_from_id(const QString &id, EasingType *easing)
{
    if (!easing) return false;
    const QString value = normalized_id(id);
    if (value == QStringLiteral("linear")) *easing = EasingType::Linear;
    else if (value == QStringLiteral("ease-in")) *easing = EasingType::EaseIn;
    else if (value == QStringLiteral("ease-out")) *easing = EasingType::EaseOut;
    else if (value == QStringLiteral("hold")) *easing = EasingType::Hold;
    else if (value == QStringLiteral("bezier")) *easing = EasingType::Bezier;
    else if (value == QStringLiteral("ease-in-out") || value == QStringLiteral("ease")) *easing = EasingType::EaseInOut;
    else return false;
    return true;
}

bool is_transition_preset_path(const QString &file_path)
{
    const QString suffix = QStringLiteral(".") + QFileInfo(file_path).suffix().toLower();
    return suffix == QString::fromUtf8(kTextTransitionExtension) ||
           suffix == QString::fromUtf8(kGeneralTransitionExtension);
}

bool is_transition_file_in_library(const QString &file_path)
{
    const QString root = gsp::effects::effect_presets_root_path();
    if (root.isEmpty())
        return false;
    const QFileInfo info(file_path);
    return path_is_within(file_path, root) && paths_equal(info.absolutePath(), root);
}

bool load_transition_preset_file(const QString &file_path,
                                 TransitionPresetDescriptor *descriptor,
                                 QString *error_message)
{
    if (!descriptor) {
        set_error(error_message, QStringLiteral("Missing transition descriptor output."));
        return false;
    }
    const QFileInfo info(file_path);
    if (!info.exists() || !info.isFile() || !info.isReadable() || !is_transition_preset_path(file_path)) {
        set_error(error_message, QStringLiteral("The transition preset file is not readable or has an unsupported extension."));
        return false;
    }
    if (!is_transition_file_in_library(file_path)) {
        set_error(error_message, QStringLiteral("Transition files must be stored directly in the Effects & Presets library folder."));
        return false;
    }
    if (info.size() < 0 || info.size() > kMaxPresetFileBytes) {
        set_error(error_message, QStringLiteral("The transition preset file is too large."));
        return false;
    }

    const QString suffix = QStringLiteral(".") + info.suffix().toLower();
    const bool text_transition = suffix == QString::fromUtf8(kTextTransitionExtension);

    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        set_error(error_message, file.errorString());
        return false;
    }
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(file.read(kMaxPresetFileBytes + 1), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        set_error(error_message, parse_error.errorString());
        return false;
    }

    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("version")).toInt(1) != 1) {
        set_error(error_message, QStringLiteral("Unsupported transition preset version."));
        return false;
    }
    const QString expected_format = text_transition
        ? QStringLiteral("obs-graphics-studio-pro-text-transition")
        : QStringLiteral("obs-graphics-studio-pro-general-transition");
    if (object.value(QStringLiteral("format")).toString() != expected_format) {
        set_error(error_message, QStringLiteral("The file format does not match its transition extension."));
        return false;
    }

    QStringList category_path = category_path_from_json(object.value(QStringLiteral("category")));
    if (category_path.isEmpty()) {
        category_path = {
            QStringLiteral("Transitions"),
            text_transition ? QStringLiteral("Text") : QStringLiteral("General")
        };
    }
    if (!valid_transition_category_path(category_path, text_transition)) {
        set_error(error_message, text_transition
            ? QStringLiteral("Text transition categories must begin with Transitions/Text.")
            : QStringLiteral("General transition categories must begin with Transitions/General."));
        return false;
    }

    LayerTransition transition;
    transition.kind = text_transition ? LayerTransitionKind::Text : LayerTransitionKind::General;
    const QString type_id = object.value(QStringLiteral("type")).toString();
    if (!transition_type_from_id(type_id, &transition.type)) {
        set_error(error_message, QStringLiteral("Unknown transition type: %1").arg(type_id));
        return false;
    }
    const bool type_is_text = layer_transition_type_is_text(transition.type);
    if (type_is_text != text_transition) {
        set_error(error_message, QStringLiteral("Text and general transition types cannot be mixed between file formats."));
        return false;
    }

    QString preset_id = object.value(QStringLiteral("id")).toString().trimmed().left(128);
    if (preset_id.isEmpty()) preset_id = info.completeBaseName().left(128);
    QString display_name = object.value(QStringLiteral("name")).toString(info.completeBaseName()).trimmed().left(256);
    if (display_name.isEmpty()) display_name = info.completeBaseName().left(256);
    transition.preset_id = preset_id.toStdString();
    transition.display_name = display_name.toStdString();
    auto finite_clamp = [&object](const char *key, double fallback, double minimum, double maximum) {
        const QJsonValue value = object.value(QString::fromUtf8(key));
        const double number = value.isDouble() ? value.toDouble(fallback) : fallback;
        return std::isfinite(number) ? std::clamp(number, minimum, maximum) : fallback;
    };
    transition.duration = finite_clamp("duration", 0.5, 1.0 / 240.0, 120.0);
    transition.blur_amount = finite_clamp("blur", 18.0, 0.0, 256.0);
    transition.scale_from = finite_clamp("scaleFrom", 0.82, -10.0, 10.0);
    transition.offset = finite_clamp("offset", 80.0, 0.0, 10000.0);
    transition.stagger = finite_clamp("stagger", 0.35, 0.0, 0.95);
    transition.softness = finite_clamp("softness", 0.0, 0.0, 1.0);
    transition.reverse_order = object.value(QStringLiteral("reverseOrder")).toBool(false);
    transition_unit_from_id(object.value(QStringLiteral("unit")).toString(QStringLiteral("character")), &transition.unit);
    transition_direction_from_id(object.value(QStringLiteral("direction")).toString(QStringLiteral("none")), &transition.direction);
    transition_easing_from_id(object.value(QStringLiteral("easing")).toString(QStringLiteral("ease-in-out")), &transition.easing);

    descriptor->file_path = info.absoluteFilePath();
    descriptor->id = QString::fromStdString(transition.preset_id);
    descriptor->display_name = QString::fromStdString(transition.display_name);
    descriptor->category_path = category_path;
    descriptor->transition = transition;
    return true;
}

QByteArray encode_transition_preset_mime(const QString &file_path)
{
    return QFileInfo(file_path).absoluteFilePath().toUtf8();
}

QString transition_preset_path_from_mime(const QMimeData *mime_data)
{
    if (!mime_data || !mime_data->hasFormat(QString::fromUtf8(kTransitionPresetMimeType)))
        return {};
    const QByteArray payload = mime_data->data(QString::fromUtf8(kTransitionPresetMimeType));
    if (payload.isEmpty() || payload.size() > 32768)
        return {};
    return QString::fromUtf8(payload).trimmed();
}

bool mime_has_transition_preset(const QMimeData *mime_data)
{
    const QString path = transition_preset_path_from_mime(mime_data);
    return !path.isEmpty() && is_transition_preset_path(path) && is_transition_file_in_library(path);
}

} // namespace gsp::transitions
