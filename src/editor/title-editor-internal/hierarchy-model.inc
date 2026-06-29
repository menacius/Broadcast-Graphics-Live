static void set_animated_y(AnimatedVec2Property &prop, double time, double value)
{
    Vec2Value next = prop.evaluate(time);
    next.y = value;
    set_animated_value(prop, time, next);
}

static void set_color_channels_at(Layer &layer, bool text, double time, uint32_t argb)
{
    auto &a = text ? layer.text_color_a : layer.fill_color_a;
    auto &r = text ? layer.text_color_r : layer.fill_color_r;
    auto &g = text ? layer.text_color_g : layer.fill_color_g;
    auto &b = text ? layer.text_color_b : layer.fill_color_b;
    set_animated_value(a, time, (argb >> 24) & 0xFF);
    set_animated_value(r, time, (argb >> 16) & 0xFF);
    set_animated_value(g, time, (argb >> 8) & 0xFF);
    set_animated_value(b, time, argb & 0xFF);
}

static void set_shadow_color_channels_at(Layer &layer, double time, uint32_t argb)
{
    set_animated_value(layer.shadow_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(layer.shadow_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(layer.shadow_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(layer.shadow_color_b, time, argb & 0xFF);
}

static void set_background_color_channels_at(Layer &layer, double time, uint32_t argb)
{
    set_animated_value(layer.background_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(layer.background_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(layer.background_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(layer.background_color_b, time, argb & 0xFF);
}

static void set_background_stroke_color_channels_at(Layer &layer, double time, uint32_t argb)
{
    set_animated_value(layer.background_stroke_color_a, time, (argb >> 24) & 0xFF);
    set_animated_value(layer.background_stroke_color_r, time, (argb >> 16) & 0xFF);
    set_animated_value(layer.background_stroke_color_g, time, (argb >> 8) & 0xFF);
    set_animated_value(layer.background_stroke_color_b, time, argb & 0xFF);
}

static bool keyframe_at_time(const AnimatedProperty &prop, double time)
{
    constexpr double kEpsilon = 1.0 / 240.0;
    for (const auto &kf : prop.keyframes)
        if (std::abs(kf.time - time) <= kEpsilon) return true;
    return false;
}

static bool keyframe_at_time(const AnimatedVec2Property &prop, double time)
{
    constexpr double kEpsilon = 1.0 / 240.0;
    for (const auto &kf : prop.keyframes)
        if (std::abs(kf.time - time) <= kEpsilon) return true;
    return false;
}

static void remove_keyframe_at(AnimatedProperty &prop, double time)
{
    constexpr double kEpsilon = 1.0 / 240.0;
    prop.keyframes.erase(
        std::remove_if(prop.keyframes.begin(), prop.keyframes.end(),
                       [&](const Keyframe &kf) { return std::abs(kf.time - time) <= kEpsilon; }),
        prop.keyframes.end());
}

static void remove_keyframe_at(AnimatedVec2Property &prop, double time)
{
    constexpr double kEpsilon = 1.0 / 240.0;
    prop.keyframes.erase(
        std::remove_if(prop.keyframes.begin(), prop.keyframes.end(),
                       [&](const VectorKeyframe &kf) { return std::abs(kf.time - time) <= kEpsilon; }),
        prop.keyframes.end());
}

static void toggle_keyframe(AnimatedProperty &prop, double time, double value)
{
    if (keyframe_at_time(prop, time))
        remove_keyframe_at(prop, time);
    else
        add_or_replace_keyframe(prop, time, value);
}

static void toggle_keyframe(AnimatedVec2Property &prop, double time, Vec2Value value)
{
    if (keyframe_at_time(prop, time))
        remove_keyframe_at(prop, time);
    else
        add_or_replace_keyframe(prop, time, value);
}

static bool any_keyframe_at_time(std::initializer_list<const AnimatedProperty *> props, double time)
{
    for (const auto *prop : props)
        if (prop && keyframe_at_time(*prop, time)) return true;
    return false;
}

static bool any_keyframes(std::initializer_list<const AnimatedProperty *> props)
{
    for (const auto *prop : props)
        if (prop && prop->is_animated()) return true;
    return false;
}

static QStringList font_styles_for_family(const QString &family)
{
    QFontDatabase fdb;
    QStringList styles = fdb.styles(family);
    if (styles.isEmpty()) {
        styles << bgl_tr("OBSTitles.Regular");
    } else {
        styles.removeDuplicates();
        styles.sort(Qt::CaseInsensitive);
        int regular = styles.indexOf(bgl_tr("OBSTitles.Regular"));
        if (regular > 0) {
            QString item = styles.takeAt(regular);
            styles.prepend(item);
        }
    }
    return styles;
}

static void populate_font_style_combo(QComboBox *combo, const QString &family, const QString &preferred)
{
    if (!combo) return;
    QSignalBlocker blocker(combo);
    combo->clear();
    QStringList styles = font_styles_for_family(family);
    for (const QString &style : styles)
        combo->addItem(style, style);
    int idx = preferred.isEmpty() ? -1 : combo->findText(preferred);
    if (idx < 0) idx = combo->findText(bgl_tr("OBSTitles.Regular"));
    combo->setCurrentIndex(idx >= 0 ? idx : 0);
}

static void style_color_button(QPushButton *button, uint32_t argb)
{
    if (!button) return;
    QColor c = color_from_argb(argb);
    button->setText(c.name(QColor::HexArgb));
    button->setToolTip(QString());
    button->setStyleSheet(QString(
        "QPushButton{color:%1;background:%2;border:1px solid #555;"
        "border-radius:3px;padding:3px 8px;}")
        .arg(c.lightness() < 128 ? "#fff" : "#000")
        .arg(c.name(QColor::HexArgb)));
}

static void style_gradient_button(QPushButton *button, uint32_t start_argb, uint32_t end_argb, int gradient_type)
{
    if (!button) return;
    QColor start = color_from_argb(start_argb);
    QColor end = color_from_argb(end_argb);
    button->setText(QString());
    const int type = normalized_gradient_type(gradient_type);
    const bool radial = type == 1;
    QString type_name = bgl_tr("OBSTitles.LinearGradient");
    if (type == 1) type_name = bgl_tr("OBSTitles.RadialGradient");
    else if (type == 2) type_name = bgl_tr("OBSTitles.ConicalGradient");
    button->setToolTip(type_name);
    const QString fill = radial
        ? QStringLiteral("qradialgradient(cx:0.5,cy:0.5,radius:0.65,fx:0.5,fy:0.5,stop:0 %1,stop:1 %2)")
              .arg(start.name(QColor::HexArgb), end.name(QColor::HexArgb))
        : QStringLiteral("qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 %1,stop:1 %2)")
              .arg(start.name(QColor::HexArgb), end.name(QColor::HexArgb));
    button->setStyleSheet(QStringLiteral(
        "QPushButton{background:%1;border:1px solid #555;border-radius:3px;padding:3px 8px;}").arg(fill));
}

static void style_color_button_mixed(QPushButton *button)
{
    if (!button) return;
    button->setText(bgl_tr("OBSTitles.M"));
    button->setToolTip(bgl_tr("OBSTitles.MixedValues"));
    button->setStyleSheet(
        "QPushButton{color:#f2c94c;background:#252525;border:1px dashed #8a7635;"
        "border-radius:3px;padding:3px 8px;font-weight:700;}");
}

static void remember_mixed_tooltip(QWidget *widget)
{
    if (!widget || widget->property("_bgs_mixed_original_tooltip").isValid())
        return;
    widget->setProperty("_bgs_mixed_original_tooltip", widget->toolTip());
}

static void set_mixed_tooltip(QWidget *widget, bool mixed)
{
    if (!widget) return;
    remember_mixed_tooltip(widget);
    widget->setToolTip(mixed
        ? bgl_tr("OBSTitles.MixedValues")
        : widget->property("_bgs_mixed_original_tooltip").toString());
}

static void set_mixed_style_suffix(QWidget *widget, bool mixed,
                                   const QString &suffix)
{
    if (!widget) return;
    if (!widget->property("_bgs_mixed_base_stylesheet").isValid())
        widget->setProperty("_bgs_mixed_base_stylesheet", widget->styleSheet());
    const QString base = widget->property("_bgs_mixed_base_stylesheet").toString();
    widget->setStyleSheet(mixed ? base + suffix : base);
}

static QIcon mixed_tinted_icon(const QIcon &source)
{
    if (source.isNull()) return source;
    QIcon result;
    const QColor mixed_color(QStringLiteral("#f2c94c"));
    const QList<QSize> sizes{QSize(12, 12), QSize(14, 14), QSize(16, 16),
                             QSize(20, 20), QSize(24, 24), QSize(32, 32)};
    for (const QSize &size : sizes) {
        QPixmap pixmap = source.pixmap(size);
        if (pixmap.isNull()) continue;
        QPainter painter(&pixmap);
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), mixed_color);
        painter.end();
        result.addPixmap(pixmap);
    }
    return result;
}

static void set_spin_mixed(QAbstractSpinBox *spin, bool mixed)
{
    if (!spin) return;
    if (auto *edit = spin->findChild<QLineEdit *>()) {
        if (!edit->property("_bgs_mixed_original_alignment").isValid())
            edit->setProperty("_bgs_mixed_original_alignment", (int)edit->alignment());
        if (mixed) {
            edit->setText(bgl_tr("OBSTitles.M"));
            edit->setAlignment(Qt::AlignCenter);
        } else {
            edit->setAlignment((Qt::Alignment)edit->property(
                "_bgs_mixed_original_alignment").toInt());
        }
        set_mixed_style_suffix(edit, mixed,
            QStringLiteral("QLineEdit{color:#f2c94c;font-weight:700;}"));
    }
    set_mixed_tooltip(spin, mixed);
}

static void set_combo_mixed(QComboBox *combo, bool mixed)
{
    if (!combo) return;
    if (!combo->property("_bgs_mixed_original_placeholder").isValid())
        combo->setProperty("_bgs_mixed_original_placeholder", combo->placeholderText());
    if (combo->isEditable() && combo->lineEdit() &&
        !combo->lineEdit()->property("_bgs_mixed_original_alignment").isValid()) {
        combo->lineEdit()->setProperty("_bgs_mixed_original_alignment",
                                       (int)combo->lineEdit()->alignment());
    }
    if (mixed) {
        combo->setCurrentIndex(-1);
        combo->setPlaceholderText(bgl_tr("OBSTitles.M"));
        if (combo->isEditable() && combo->lineEdit()) {
            combo->lineEdit()->setText(bgl_tr("OBSTitles.M"));
            combo->lineEdit()->setAlignment(Qt::AlignCenter);
        }
    } else {
        combo->setPlaceholderText(combo->property(
            "_bgs_mixed_original_placeholder").toString());
        if (combo->isEditable() && combo->lineEdit())
            combo->lineEdit()->setAlignment((Qt::Alignment)combo->lineEdit()->property(
                "_bgs_mixed_original_alignment").toInt());
    }
    set_mixed_style_suffix(combo, mixed,
        QStringLiteral("QComboBox{color:#f2c94c;font-weight:700;}"
                       "QComboBox QLineEdit{color:#f2c94c;font-weight:700;}"));
    set_mixed_tooltip(combo, mixed);
}

static void set_button_mixed(QAbstractButton *button, bool mixed)
{
    if (!button) return;
    if (!button->property("_bgs_mixed_original_icon").isValid())
        button->setProperty("_bgs_mixed_original_icon", button->icon());
    if (mixed) {
        button->setChecked(false);
        const QIcon original = button->property("_bgs_mixed_original_icon").value<QIcon>();
        if (!original.isNull())
            button->setIcon(mixed_tinted_icon(original));
    } else {
        button->setIcon(button->property("_bgs_mixed_original_icon").value<QIcon>());
    }
    set_mixed_style_suffix(button, mixed,
        QStringLiteral("QAbstractButton{color:#f2c94c;font-weight:700;}"));
    set_mixed_tooltip(button, mixed);
}

static void set_button_group_mixed(QButtonGroup *group, bool mixed)
{
    if (!group) return;
    const bool was_exclusive = group->exclusive();
    if (mixed && was_exclusive)
        group->setExclusive(false);
    for (QAbstractButton *button : group->buttons())
        set_button_mixed(button, mixed);
    if (mixed && was_exclusive)
        group->setExclusive(true);
}

static QColor keyframe_color(EasingType)
{
    return C_KF_DOT;
}

static QPainterPath keyframe_shape_path(EasingType easing, const QPointF &center, qreal radius)
{
    const qreal x = center.x();
    const qreal y = center.y();
    QPainterPath path;

    switch (easing) {
    case EasingType::Linear:
        path.moveTo(x, y - radius);
        path.lineTo(x + radius, y);
        path.lineTo(x, y + radius);
        path.lineTo(x - radius, y);
        path.closeSubpath();
        break;
    case EasingType::EaseIn:
        path.moveTo(x - radius, y);
        path.lineTo(x, y - radius);
        path.lineTo(x + radius, y - radius);
        path.lineTo(x + radius, y + radius);
        path.lineTo(x, y + radius);
        path.closeSubpath();
        break;
    case EasingType::EaseOut:
        path.moveTo(x + radius, y);
        path.lineTo(x, y - radius);
        path.lineTo(x - radius, y - radius);
        path.lineTo(x - radius, y + radius);
        path.lineTo(x, y + radius);
        path.closeSubpath();
        break;
    case EasingType::EaseInOut:
        path.moveTo(x - radius, y - radius);
        path.lineTo(x + radius, y - radius);
        path.lineTo(x - radius, y + radius);
        path.lineTo(x + radius, y + radius);
        path.closeSubpath();
        break;
    case EasingType::Bezier:
        path.addEllipse(center, radius, radius);
        break;
    case EasingType::Hold:
        path.addRect(QRectF(x - radius, y - radius, radius * 2.0, radius * 2.0));
        break;
    default:
        path.moveTo(x, y - radius);
        path.lineTo(x + radius, y);
        path.lineTo(x, y + radius);
        path.lineTo(x - radius, y);
        path.closeSubpath();
        break;
    }

    return path;
}

static void draw_keyframe_marker(QPainter &painter, const QPointF &center, EasingType easing,
                                 qreal radius, const QColor &fill,
                                 const QColor &stroke, qreal stroke_width)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(fill);
    painter.setPen(QPen(stroke, stroke_width, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
    painter.drawPath(keyframe_shape_path(easing, center, radius));

    if (easing == EasingType::Bezier) {
        painter.setPen(QPen(stroke, std::max<qreal>(1.0, stroke_width - 0.5)));
        painter.drawLine(QPointF(center.x() - radius - 3.0, center.y()),
                         QPointF(center.x() - radius, center.y()));
        painter.drawLine(QPointF(center.x() + radius, center.y()),
                         QPointF(center.x() + radius + 3.0, center.y()));
    }

    painter.restore();
}

static QString easing_label(EasingType easing)
{
    switch (easing) {
    case EasingType::Linear: return bgl_tr("OBSTitles.Linear");
    case EasingType::EaseIn: return bgl_tr("OBSTitles.EaseIn");
    case EasingType::EaseOut: return bgl_tr("OBSTitles.EaseOut");
    case EasingType::EaseInOut: return bgl_tr("OBSTitles.EasyEase");
    case EasingType::Bezier: return bgl_tr("OBSTitles.CustomBezier");
    case EasingType::Hold: return bgl_tr("OBSTitles.Hold");
    default: return bgl_tr("OBSTitles.Linear");
    }
}

struct TimelinePropertyRef {
    AnimatedProperty *scalar = nullptr;
    AnimatedVec2Property *vector = nullptr;
    LayerEffect *extension_effect = nullptr;
    QString extension_path;
    std::string extension_name;
    QJsonValue extension_fallback;
    EasingType extension_default_easing = EasingType::Linear;
    std::vector<AnimatedProperty *> scalar_group;

    AnimatedProperty *group_representative() const
    {
        for (AnimatedProperty *property : scalar_group)
            if (property && property->is_animated()) return property;
        return scalar_group.empty() ? nullptr : scalar_group.front();
    }
    explicit operator bool() const {
        return scalar || vector || extension_effect || !scalar_group.empty();
    }
    bool is_extension() const { return extension_effect != nullptr; }
    std::string name() const {
        if (!extension_name.empty()) return extension_name;
        return vector ? vector->name : scalar ? scalar->name : std::string();
    }
    bool is_animated() const {
        if (is_extension())
            return !bgs::effects::animation::track_keys(*extension_effect, extension_path).isEmpty();
        if (!scalar_group.empty())
            return std::any_of(scalar_group.begin(), scalar_group.end(),
                [](const AnimatedProperty *property) {
                    return property && property->is_animated();
                });
        return vector ? vector->is_animated() : scalar && scalar->is_animated();
    }
    size_t keyframe_count() const {
        if (is_extension())
            return static_cast<size_t>(bgs::effects::animation::track_keys(*extension_effect, extension_path).size());
        if (AnimatedProperty *property = group_representative())
            return property->keyframes.size();
        return vector ? vector->keyframes.size() : scalar ? scalar->keyframes.size() : 0;
    }
    double keyframe_time(size_t index) const {
        if (is_extension())
            return bgs::effects::animation::track_keys(*extension_effect, extension_path)
                .at(static_cast<qsizetype>(index)).toObject()
                .value(QStringLiteral("time")).toDouble();
        if (AnimatedProperty *property = group_representative())
            return property->keyframes[index].time;
        return vector ? vector->keyframes[index].time : scalar->keyframes[index].time;
    }
    EasingType keyframe_easing(size_t index) const {
        if (is_extension())
            return bgs::effects::animation::easing_from_key(
                bgs::effects::animation::track_keys(*extension_effect, extension_path)
                    .at(static_cast<qsizetype>(index)).toObject());
        if (AnimatedProperty *property = group_representative())
            return property->keyframes[index].easing;
        return vector ? vector->keyframes[index].easing : scalar->keyframes[index].easing;
    }
    Keyframe scalar_keyframe(size_t index) const {
        if (AnimatedProperty *property = group_representative())
            return property->keyframes[index];
        return scalar ? scalar->keyframes[index] : Keyframe{};
    }
    std::vector<Keyframe> scalar_group_keyframes(size_t index) const
    {
        std::vector<Keyframe> result;
        if (scalar_group.empty()) return result;
        const double time = keyframe_time(index);
        result.reserve(scalar_group.size());
        for (AnimatedProperty *property : scalar_group) {
            Keyframe keyframe;
            bool found = false;
            if (property) {
                for (const Keyframe &candidate : property->keyframes) {
                    if (std::abs(candidate.time - time) < 0.0001) {
                        keyframe = candidate;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    keyframe.time = time;
                    keyframe.value = property->evaluate(time);
                    keyframe.easing = keyframe_easing(index);
                }
            }
            result.push_back(keyframe);
        }
        return result;
    }
    VectorKeyframe vector_keyframe(size_t index) const { return vector ? vector->keyframes[index] : VectorKeyframe{}; }
    QJsonObject extension_keyframe(size_t index) const {
        return is_extension()
            ? bgs::effects::animation::track_keys(*extension_effect, extension_path)
                  .at(static_cast<qsizetype>(index)).toObject()
            : QJsonObject{};
    }
    bool is_vector() const { return vector != nullptr; }
    bool is_scalar_group() const { return !scalar_group.empty(); }
    void push_keyframe(const Keyframe &keyframe)
    {
        if (scalar) scalar->keyframes.push_back(keyframe);
    }
    void push_keyframe(const VectorKeyframe &keyframe)
    {
        if (vector) vector->keyframes.push_back(keyframe);
    }
    void push_keyframes(const std::vector<Keyframe> &keyframes)
    {
        if (scalar_group.empty()) return;
        const size_t count = std::min(scalar_group.size(), keyframes.size());
        for (size_t i = 0; i < count; ++i)
            if (scalar_group[i]) scalar_group[i]->keyframes.push_back(keyframes[i]);
    }
    void push_keyframe(const QJsonObject &keyframe)
    {
        if (!is_extension()) return;
        QJsonArray keys = bgs::effects::animation::track_keys(*extension_effect, extension_path);
        keys.append(keyframe);
        bgs::effects::animation::write_track_keys(*extension_effect, extension_path, keys);
    }
    void set_keyframe_time(size_t index, double time)
    {
        if (is_extension()) {
            QJsonArray keys = bgs::effects::animation::track_keys(*extension_effect, extension_path);
            QJsonObject key = keys.at(static_cast<qsizetype>(index)).toObject();
            key.insert(QStringLiteral("time"), time);
            keys.replace(static_cast<qsizetype>(index), key);
            bgs::effects::animation::write_track_keys(*extension_effect, extension_path, keys);
        } else if (!scalar_group.empty()) {
            const double old_time = keyframe_time(index);
            for (AnimatedProperty *property : scalar_group) {
                if (!property) continue;
                for (Keyframe &keyframe : property->keyframes) {
                    if (std::abs(keyframe.time - old_time) < 0.0001) {
                        keyframe.time = time;
                        break;
                    }
                }
            }
        } else if (vector) vector->keyframes[index].time = time;
        else if (scalar) scalar->keyframes[index].time = time;
    }
    void erase_keyframe(size_t index)
    {
        if (is_extension()) {
            QJsonArray keys = bgs::effects::animation::track_keys(*extension_effect, extension_path);
            keys.removeAt(static_cast<qsizetype>(index));
            bgs::effects::animation::write_track_keys(*extension_effect, extension_path, keys);
        } else if (!scalar_group.empty()) {
            const double time = keyframe_time(index);
            for (AnimatedProperty *property : scalar_group) {
                if (!property) continue;
                property->keyframes.erase(
                    std::remove_if(property->keyframes.begin(), property->keyframes.end(),
                        [time](const Keyframe &keyframe) {
                            return std::abs(keyframe.time - time) < 0.0001;
                        }),
                    property->keyframes.end());
            }
        } else if (vector) vector->keyframes.erase(vector->keyframes.begin() + (ptrdiff_t)index);
        else if (scalar) scalar->keyframes.erase(scalar->keyframes.begin() + (ptrdiff_t)index);
    }
    void clear_keyframes()
    {
        if (is_extension())
            bgs::effects::animation::write_track_keys(*extension_effect, extension_path, QJsonArray{});
        else if (!scalar_group.empty()) {
            for (AnimatedProperty *property : scalar_group)
                if (property) property->keyframes.clear();
        } else if (vector) vector->keyframes.clear();
        else if (scalar) scalar->keyframes.clear();
    }
    void sort_keyframes()
    {
        if (is_extension()) {
            bgs::effects::animation::write_track_keys(
                *extension_effect, extension_path,
                bgs::effects::animation::track_keys(*extension_effect, extension_path));
        } else if (!scalar_group.empty()) {
            for (AnimatedProperty *property : scalar_group) {
                if (!property) continue;
                std::sort(property->keyframes.begin(), property->keyframes.end(),
                          [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
            }
        } else if (vector) {
            std::sort(vector->keyframes.begin(), vector->keyframes.end(),
                      [](const VectorKeyframe &a, const VectorKeyframe &b) { return a.time < b.time; });
        } else if (scalar) {
            std::sort(scalar->keyframes.begin(), scalar->keyframes.end(),
                      [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; });
        }
    }
    void apply_easing(size_t index, EasingType easing)
    {
        if (is_extension()) {
            QJsonArray keys = bgs::effects::animation::track_keys(*extension_effect, extension_path);
            QJsonObject key = keys.at(static_cast<qsizetype>(index)).toObject();
            bgs::effects::animation::set_key_easing(key, easing);
            keys.replace(static_cast<qsizetype>(index), key);
            bgs::effects::animation::write_track_keys(*extension_effect, extension_path, keys);
        } else if (!scalar_group.empty()) {
            const double time = keyframe_time(index);
            for (AnimatedProperty *property : scalar_group) {
                if (!property) continue;
                for (Keyframe &keyframe : property->keyframes) {
                    if (std::abs(keyframe.time - time) < 0.0001) {
                        apply_easing_preset(keyframe, easing);
                        break;
                    }
                }
            }
        } else if (vector) apply_easing_preset(vector->keyframes[index], easing);
        else if (scalar) apply_easing_preset(scalar->keyframes[index], easing);
    }
    void toggle_at(double time)
    {
        if (is_extension()) {
            const QJsonValue fallback = extension_fallback.isUndefined()
                ? bgs::effects::animation::state_path_value(*extension_effect, extension_path)
                : extension_fallback;
            const QJsonValue value = bgs::effects::animation::evaluate_track(
                *extension_effect, extension_path, time, fallback);
            bgs::effects::animation::toggle_keyframe(
                *extension_effect, extension_path, time, value,
                extension_default_easing);
        } else if (!scalar_group.empty()) {
            const bool has_key = std::any_of(
                scalar_group.begin(), scalar_group.end(), [time](AnimatedProperty *property) {
                    return property && std::any_of(property->keyframes.begin(), property->keyframes.end(),
                        [time](const Keyframe &keyframe) {
                            return std::abs(keyframe.time - time) < 0.0001;
                        });
                });
            for (AnimatedProperty *property : scalar_group) {
                if (!property) continue;
                if (has_key) {
                    property->keyframes.erase(
                        std::remove_if(property->keyframes.begin(), property->keyframes.end(),
                            [time](const Keyframe &keyframe) {
                                return std::abs(keyframe.time - time) < 0.0001;
                            }),
                        property->keyframes.end());
                } else {
                    toggle_keyframe(*property, time, property->evaluate(time));
                }
            }
        } else if (vector) {
            toggle_keyframe(*vector, time, vector->evaluate(time));
        } else if (scalar) {
            toggle_keyframe(*scalar, time, scalar->evaluate(time));
        }
    }
};

static std::vector<TimelinePropertyRef> timeline_properties(Layer &layer)
{
    std::vector<TimelinePropertyRef> props {
        {nullptr, &layer.position},
        {nullptr, &layer.scale},
        {&layer.rotation, nullptr}, {&layer.opacity, nullptr},
        {nullptr, &layer.size},
        {nullptr, &layer.origin_prop},
        {&layer.paragraph_indent_left_prop, nullptr}, {&layer.paragraph_indent_right_prop, nullptr},
        {&layer.paragraph_indent_first_line_prop, nullptr},
        {&layer.ticker_completion_prop, nullptr},
        {&layer.font_size_prop, nullptr}, {&layer.char_scale_x_prop, nullptr}, {&layer.char_scale_y_prop, nullptr},
        {&layer.char_tracking_prop, nullptr}, {&layer.baseline_shift_prop, nullptr},
        {&layer.paragraph_space_before_prop, nullptr}, {&layer.paragraph_space_after_prop, nullptr},
        {&layer.text_color_a, nullptr}, {&layer.text_color_r, nullptr},
        {&layer.text_color_g, nullptr}, {&layer.text_color_b, nullptr},
        {&layer.fill_color_a, nullptr}, {&layer.fill_color_r, nullptr},
        {&layer.fill_color_g, nullptr}, {&layer.fill_color_b, nullptr}
    };

    auto add_effect_props = [&](LayerEffect &effect, size_t effect_index) {
        QString effect_title = effect_type_name(effect.type);
        QString slug = effect_title;
        slug.replace(QStringLiteral(" "), QStringLiteral("_"));
        slug = slug.toLower();
        auto encoded_name = [&](const QString &suffix) {
            return QStringLiteral("__effect__%1__%2__%3")
                .arg(static_cast<qulonglong>(effect_index))
                .arg(slug).arg(suffix).toStdString();
        };
        auto name = [&](AnimatedProperty &prop, const char *suffix) {
            props.push_back({&prop, nullptr, nullptr, QString(),
                             encoded_name(QString::fromLatin1(suffix)), QJsonValue()});
        };
        auto name_group = [&](const char *suffix,
                              std::initializer_list<AnimatedProperty *> group) {
            props.push_back({nullptr, nullptr, nullptr, QString(),
                             encoded_name(QString::fromLatin1(suffix)), QJsonValue(),
                             EasingType::Linear,
                             std::vector<AnimatedProperty *>(group)});
        };
        name(effect.enabled_prop, "enabled");
        switch (effect.type) {
        case LayerEffectType::BackgroundColor:
            name(effect.opacity_prop, "opacity");
            name(effect.gradient_start_pos_prop, "gradient_start_pos");
            name(effect.gradient_end_pos_prop, "gradient_end_pos");
            name(effect.gradient_start_opacity_prop, "gradient_start_opacity");
            name(effect.gradient_end_opacity_prop, "gradient_end_opacity");
            name(effect.gradient_angle_prop, "gradient_angle");
            name(effect.gradient_center_x_prop, "gradient_center_x");
            name(effect.gradient_center_y_prop, "gradient_center_y");
            name(effect.gradient_scale_prop, "gradient_scale");
            name(effect.gradient_focal_x_prop, "gradient_focal_x");
            name(effect.gradient_focal_y_prop, "gradient_focal_y");
            name(effect.gradient_opacity_prop, "gradient_opacity");
            name_group("gradient_start_color", {&effect.gradient_start_color_a,
                &effect.gradient_start_color_r, &effect.gradient_start_color_g,
                &effect.gradient_start_color_b});
            name_group("gradient_end_color", {&effect.gradient_end_color_a,
                &effect.gradient_end_color_r, &effect.gradient_end_color_g,
                &effect.gradient_end_color_b});
            name(effect.padding_left_prop, "padding_left");
            name(effect.padding_right_prop, "padding_right");
            name(effect.padding_top_prop, "padding_top");
            name(effect.padding_bottom_prop, "padding_bottom");
            name(effect.corner_radius_tl_prop, "corner_tl");
            name(effect.corner_radius_tr_prop, "corner_tr");
            name(effect.corner_radius_br_prop, "corner_br");
            name(effect.corner_radius_bl_prop, "corner_bl");
            name(effect.stroke_width_prop, "stroke_width");
            name(effect.stroke_opacity_prop, "stroke_opacity");
            name_group("color", {&effect.color_a, &effect.color_r,
                &effect.color_g, &effect.color_b});
            name_group("stroke_color", {&effect.stroke_color_a,
                &effect.stroke_color_r, &effect.stroke_color_g,
                &effect.stroke_color_b});
            break;
        case LayerEffectType::Outline:
            name(effect.size_prop, "width");
            name(effect.opacity_prop, "opacity");
            name_group("color", {&effect.color_a, &effect.color_r,
                &effect.color_g, &effect.color_b});
            break;
        case LayerEffectType::DropShadow:
            name(effect.opacity_prop, "opacity");
            name(effect.distance_prop, "distance");
            name(effect.angle_prop, "angle");
            name(effect.size_prop, "blur");
            name(effect.spread_prop, "spread");
            name_group("color", {&effect.color_a, &effect.color_r,
                &effect.color_g, &effect.color_b});
            break;
        case LayerEffectType::InnerShadow:
            name(effect.opacity_prop, "opacity");
            name(effect.distance_prop, "distance");
            name(effect.angle_prop, "angle");
            name(effect.size_prop, "size");
            name_group("color", {&effect.color_a, &effect.color_r,
                &effect.color_g, &effect.color_b});
            break;
        case LayerEffectType::BrightnessContrast:
            name(effect.brightness_prop, "brightness");
            name(effect.contrast_prop, "contrast");
            break;
        case LayerEffectType::Saturation:
            name(effect.saturation_prop, "saturation");
            break;
        case LayerEffectType::LongShadow:
            name(effect.opacity_prop, "opacity");
            name(effect.distance_prop, "length");
            name(effect.angle_prop, "angle");
            name(effect.falloff_prop, "falloff");
            name(effect.size_prop, "blur");
            name_group("color", {&effect.color_a, &effect.color_r,
                &effect.color_g, &effect.color_b});
            break;
        case LayerEffectType::LensFlare:
            name(effect.opacity_prop, "opacity");
            name(effect.amount_prop, "amount");
            name(effect.scale_prop, "scale");
            name(effect.size_prop, "size");
            name(effect.spread_prop, "spread");
            name(effect.falloff_prop, "falloff");
            name(effect.angle_prop, "angle");
            name(effect.center_x_prop, "center_x");
            name(effect.center_y_prop, "center_y");
            name(effect.complexity_prop, "ghosts");
            name_group("color", {&effect.color_a, &effect.color_r,
                &effect.color_g, &effect.color_b});
            name_group("secondary_color", {&effect.secondary_color_a,
                &effect.secondary_color_r, &effect.secondary_color_g,
                &effect.secondary_color_b});
            break;
        case LayerEffectType::Vignette:
            name(effect.opacity_prop, "opacity");
            name(effect.amount_prop, "amount");
            name(effect.scale_prop, "scale");
            name(effect.softness_prop, "softness");
            name(effect.roundness_prop, "roundness");
            name(effect.center_x_prop, "center_x");
            name(effect.center_y_prop, "center_y");
            name_group("color", {&effect.color_a, &effect.color_r,
                &effect.color_g, &effect.color_b});
            break;
        case LayerEffectType::Noise:
            name(effect.opacity_prop, "opacity");
            name(effect.amount_prop, "amount");
            name(effect.scale_prop, "scale");
            name(effect.softness_prop, "softness");
            name(effect.complexity_prop, "complexity");
            name(effect.evolution_prop, "evolution");
            name(effect.speed_prop, "speed");
            break;
        case LayerEffectType::RoughenEdges:
            name(effect.opacity_prop, "opacity");
            name(effect.amount_prop, "amount");
            name(effect.scale_prop, "scale");
            name(effect.softness_prop, "softness");
            name(effect.complexity_prop, "complexity");
            name(effect.evolution_prop, "evolution");
            break;
        case LayerEffectType::ColorOverlay:
            name(effect.opacity_prop, "opacity");
            name_group("color", {&effect.color_a, &effect.color_r,
                &effect.color_g, &effect.color_b});
            break;
        case LayerEffectType::Glow:
        case LayerEffectType::InnerGlow:
            name(effect.opacity_prop, "opacity");
            name(effect.size_prop, "size");
            name_group("color", {&effect.color_a, &effect.color_r,
                &effect.color_g, &effect.color_b});
            break;
        case LayerEffectType::Blur:
        case LayerEffectType::MotionBlur:
            name(effect.opacity_prop, "amount");
            name(effect.size_prop, effect.type == LayerEffectType::MotionBlur
                ? "shutter_angle" : "size");
            break;
        case LayerEffectType::Bloom:
            name(effect.opacity_prop, "opacity");
            name(effect.spread_prop, "threshold");
            name(effect.size_prop, "size");
            name(effect.falloff_prop, "intensity");
            name_group("color", {&effect.color_a, &effect.color_r,
                &effect.color_g, &effect.color_b});
            break;
        case LayerEffectType::Emboss:
            name(effect.size_prop, "depth");
            name(effect.distance_prop, "height");
            name(effect.angle_prop, "angle");
            name(effect.spread_prop, "softness");
            name(effect.opacity_prop, "opacity");
            break;
        case LayerEffectType::FourColorGradient:
            break;
        default:
            break;
        }

        auto &catalog = BglEffectExtensionCatalog::instance();
        if (catalog.effects().empty()) catalog.reload();
        const auto *definition = effect.extension_id.empty()
            ? nullptr : catalog.find(QString::fromStdString(effect.extension_id));
        const bool schema_driven = definition &&
            (!definition->builtIn || effect.type == LayerEffectType::FourColorGradient);
        QSet<QString> extension_paths;
        if (schema_driven) {
            for (auto it = definition->parameterSchema.begin();
                 it != definition->parameterSchema.end(); ++it) {
                const QJsonObject meta = it.value().toObject();
                if (!meta.value(QStringLiteral("animatable")).toBool(false)) continue;
                extension_paths.insert(it.key());
                const QJsonValue fallback = bgs::effects::animation::state_path_value(effect, it.key()).isUndefined()
                    ? meta.value(QStringLiteral("default"))
                    : bgs::effects::animation::state_path_value(effect, it.key());
                const QString declared_type = meta.value(QStringLiteral("type")).toString();
                const EasingType default_easing =
                    (declared_type == QStringLiteral("bool") ||
                     declared_type == QStringLiteral("enum"))
                        ? EasingType::Hold : EasingType::Linear;
                props.push_back({nullptr, nullptr, &effect, it.key(),
                    encoded_name(it.key()), fallback, default_easing});
            }
        }
        const QJsonObject existing_tracks = bgs::effects::animation::tracks_object(effect);
        for (auto it = existing_tracks.begin(); it != existing_tracks.end(); ++it) {
            if (it.value().toArray().isEmpty() || extension_paths.contains(it.key())) continue;
            const QJsonArray track = it.value().toArray();
            const EasingType default_easing = track.isEmpty()
                ? EasingType::Linear
                : bgs::effects::animation::easing_from_key(track.at(0).toObject());
            props.push_back({nullptr, nullptr, &effect, it.key(),
                encoded_name(it.key()),
                bgs::effects::animation::state_path_value(effect, it.key()),
                default_easing});
        }
    };
    for (size_t index = 0; index < layer.effects.size(); ++index)
        add_effect_props(layer.effects[index], index);
    return props;
}

static QString property_label(const std::string &name)
{
    QString qname = QString::fromStdString(name);
    QString encoded_effect_slug;
    QString encoded_effect_suffix;
    if (qname.startsWith(QStringLiteral("__effect__"))) {
        const QStringList parts = qname.split(QStringLiteral("__"), Qt::KeepEmptyParts);
        if (parts.size() >= 5) {
            encoded_effect_slug = parts.at(3);
            encoded_effect_suffix = parts.mid(4).join(QStringLiteral("__"));
            qname = encoded_effect_slug + QStringLiteral("_") + encoded_effect_suffix;
        }
    }
    auto effect_label = [&](const QString &prefix, const QString &label) -> QString {
        QString title = prefix;
        title.replace(QStringLiteral("_"), QStringLiteral(" "));
        for (int i = 0; i < title.size(); ++i)
            if (i == 0 || title.at(i - 1).isSpace())
                title[i] = title.at(i).toUpper();
        return title + QStringLiteral(" ") + label;
    };
    if (qname.startsWith(QStringLiteral("background_color_"))) {
        QString suffix = qname.mid(QStringLiteral("background_color_").size());
        if (suffix == QStringLiteral("enabled")) return effect_label(bgl_tr("OBSTitles.BackgroundColor"), bgl_tr("OBSTitles.Enabled"));
        if (suffix == QStringLiteral("opacity")) return effect_label(bgl_tr("OBSTitles.BackgroundColor"), bgl_tr("OBSTitles.Opacity"));
        if (suffix.startsWith(QStringLiteral("padding_"))) return effect_label(bgl_tr("OBSTitles.BackgroundColor"), suffix.replace(QStringLiteral("_"), QStringLiteral(" ")));
        if (suffix.startsWith(QStringLiteral("corner_"))) return effect_label(bgl_tr("OBSTitles.BackgroundColor"), suffix.replace(QStringLiteral("_"), QStringLiteral(" ")));
        if (suffix.startsWith(QStringLiteral("stroke_"))) return effect_label(bgl_tr("OBSTitles.BackgroundColor"), suffix.replace(QStringLiteral("_"), QStringLiteral(" ")));
        if (suffix.startsWith(QStringLiteral("color_"))) return effect_label(bgl_tr("OBSTitles.BackgroundColor"), bgl_tr("OBSTitles.Color"));
    }
    if (qname.startsWith(QStringLiteral("outline_"))) {
        QString suffix = qname.mid(QStringLiteral("outline_").size());
        if (suffix == QStringLiteral("enabled")) return effect_label(bgl_tr("OBSTitles.Outline"), bgl_tr("OBSTitles.Enabled"));
        if (suffix == QStringLiteral("width")) return effect_label(bgl_tr("OBSTitles.Outline"), bgl_tr("OBSTitles.Width"));
        if (suffix == QStringLiteral("opacity")) return effect_label(bgl_tr("OBSTitles.Outline"), bgl_tr("OBSTitles.Opacity"));
        if (suffix.startsWith(QStringLiteral("color_"))) return effect_label(bgl_tr("OBSTitles.Outline"), bgl_tr("OBSTitles.Color"));
    }
    for (const QString &prefix : {QStringLiteral("drop_shadow_"), QStringLiteral("long_shadow_"), QStringLiteral("inner_shadow_")}) {
        if (!qname.startsWith(prefix)) continue;
        QString suffix = qname.mid(prefix.size()).replace(QStringLiteral("_"), QStringLiteral(" "));
        QString title = prefix.left(prefix.size() - 1);
        title.replace(QStringLiteral("_"), QStringLiteral(" "));
        return effect_label(title, suffix);
    }
    if (name == "position") return bgl_tr("OBSTitles.Position");
    if (name == "scale") return bgl_tr("OBSTitles.Scale");
    if (name == "size") return bgl_tr("OBSTitles.Size");
    if (name == "origin") return bgl_tr("OBSTitles.Origin");
    if (name == "paragraph_indent_left") return bgl_tr("OBSTitles.ParagraphIndentLeft");
    if (name == "paragraph_indent_right") return bgl_tr("OBSTitles.ParagraphIndentRight");
    if (name == "paragraph_indent_first_line") return bgl_tr("OBSTitles.ParagraphIndentFirstLine");
    if (name == "font_size") return bgl_tr("OBSTitles.Size");
    if (name == "char_scale_x") return bgl_tr("OBSTitles.HScale");
    if (name == "char_scale_y") return bgl_tr("OBSTitles.VScale");
    if (name == "char_tracking") return bgl_tr("OBSTitles.Tracking");
    if (name == "baseline_shift") return bgl_tr("OBSTitles.Baseline");
    if (name == "paragraph_space_before") return bgl_tr("OBSTitles.ParagraphSpaceBefore");
    if (name == "paragraph_space_after") return bgl_tr("OBSTitles.ParagraphSpaceAfter");
    if (name == "text_color_a" || name == "text_color_r" ||
        name == "text_color_g" || name == "text_color_b") return bgl_tr("OBSTitles.TextColor");
    if (name == "fill_color_a" || name == "fill_color_r" ||
        name == "fill_color_g" || name == "fill_color_b") return bgl_tr("OBSTitles.FillColor");
    if (name == "shadow_color_a" || name == "shadow_color_r" ||
        name == "shadow_color_g" || name == "shadow_color_b") return bgl_tr("OBSTitles.ShadowColor");
    if (name == "background_color_a" || name == "background_color_r" ||
        name == "background_color_g" || name == "background_color_b") return bgl_tr("OBSTitles.BackgroundColor");
    if (name == "background_enabled") return bgl_tr("OBSTitles.EnableColorBackground");
    if (name == "background_opacity") return bgl_tr("OBSTitles.BackgroundOpacityLabel");
    if (name == "background_padding_x") return bgl_tr("OBSTitles.BackgroundHorizontalPaddingLabel");
    if (name == "background_padding_y") return bgl_tr("OBSTitles.BackgroundVerticalPaddingLabel");
    if (name == "background_padding_left") return bgl_tr("OBSTitles.LeftPadding");
    if (name == "background_padding_right") return bgl_tr("OBSTitles.RightPadding");
    if (name == "background_padding_top") return bgl_tr("OBSTitles.TopPadding");
    if (name == "background_padding_bottom") return bgl_tr("OBSTitles.BottomPadding");
    if (name == "background_corner_radius") return bgl_tr("OBSTitles.BackgroundCornerLabel");
    if (name == "background_corner_radius_tl") return bgl_tr("OBSTitles.CornerTL");
    if (name == "background_corner_radius_tr") return bgl_tr("OBSTitles.CornerTR");
    if (name == "background_corner_radius_br") return bgl_tr("OBSTitles.CornerBR");
    if (name == "background_corner_radius_bl") return bgl_tr("OBSTitles.CornerBL");
    if (name == "background_stroke_width") return bgl_tr("OBSTitles.BackgroundStrokeWidth");
    if (name == "background_stroke_opacity") return bgl_tr("OBSTitles.BackgroundStrokeOpacity");
    if (name == "background_stroke_color_a" || name == "background_stroke_color_r" || name == "background_stroke_color_g" || name == "background_stroke_color_b") return bgl_tr("OBSTitles.BackgroundStroke");
    if (name == "shadow_enabled") return bgl_tr("OBSTitles.ShadowEnable");
    if (name == "shadow_opacity") return bgl_tr("OBSTitles.ShadowOpacity");
    if (name == "shadow_distance") return bgl_tr("OBSTitles.ShadowDistance");
    if (name == "shadow_angle") return bgl_tr("OBSTitles.ShadowAngle");
    if (name == "shadow_blur") return bgl_tr("OBSTitles.ShadowBlur");
    if (name == "shadow_spread") return bgl_tr("OBSTitles.ShadowSpread");
    if (name == "rotation") return bgl_tr("OBSTitles.Rotation");
    if (name == "opacity") return bgl_tr("OBSTitles.Opacity");
    if (!encoded_effect_slug.isEmpty()) {
        QString suffix = encoded_effect_suffix;
        if (suffix.startsWith(QStringLiteral("elements."))) {
            const QStringList element_parts = suffix.split(QLatin1Char('.'));
            if (element_parts.size() == 3)
                suffix = QStringLiteral("Element %1 %2")
                    .arg(element_parts.at(1).toInt() + 1)
                    .arg(element_parts.at(2));
        }
        suffix.replace(QLatin1Char('.'), QLatin1Char(' '));
        suffix.replace(QLatin1Char('_'), QLatin1Char(' '));
        return effect_label(encoded_effect_slug, suffix);
    }
    return QString::fromStdString(name);
}

static QString property_value_text(const AnimatedProperty &prop, const Layer &layer)
{
    double value = prop.static_value;
    if (prop.name == "pos_x")
        return QString("%1,%2").arg(layer.position.static_value.x, 0, 'f', 1)
                                .arg(layer.position.static_value.y, 0, 'f', 1);
    if (prop.name == "scale_x")
        return QString("%1,%2%").arg(layer.scale.static_value.x * 100.0, 0, 'f', 1)
                                 .arg(layer.scale.static_value.y * 100.0, 0, 'f', 1);
    if (prop.name == "box_width")
        return QString("%1 × %2").arg(layer.size.static_value.x, 0, 'f', 0)
                                  .arg(layer.size.static_value.y, 0, 'f', 0);
    if (prop.name == "origin_x")
        return QString("%1,%2").arg(layer.origin_prop.static_value.x, 0, 'f', 2)
                                .arg(layer.origin_prop.static_value.y, 0, 'f', 2);
    if (prop.name == "char_scale_x" || prop.name == "char_scale_y") value *= 100.0;
    if (prop.name == "opacity" || prop.name == "shadow_opacity" || prop.name == "background_opacity") value *= 100.0;
    if (prop.name == "shadow_enabled" || prop.name == "background_enabled") return value >= 0.5 ? bgl_tr("OBSTitles.On") : bgl_tr("OBSTitles.Off");
    return QString::number(value, 'f', (prop.name == "opacity" || prop.name == "shadow_opacity" || prop.name == "background_opacity") ? 1 : 2);
}

static QString property_value_text(const TimelinePropertyRef &prop, const Layer &layer)
{
    if (!prop.vector)
        return prop.scalar ? property_value_text(*prop.scalar, layer) : QString();

    if (prop.name() == "position")
        return QString("%1,%2").arg(layer.position.static_value.x, 0, 'f', 1)
                                .arg(layer.position.static_value.y, 0, 'f', 1);
    if (prop.name() == "scale")
        return QString("%1,%2%").arg(layer.scale.static_value.x * 100.0, 0, 'f', 1)
                                 .arg(layer.scale.static_value.y * 100.0, 0, 'f', 1);
    if (prop.name() == "size")
        return QString("%1 x %2").arg(layer.size.static_value.x, 0, 'f', 0)
                                  .arg(layer.size.static_value.y, 0, 'f', 0);
    if (prop.name() == "origin")
        return QString("%1,%2").arg(layer.origin_prop.static_value.x, 0, 'f', 2)
                                .arg(layer.origin_prop.static_value.y, 0, 'f', 2);
    return QString::fromStdString(prop.name());
}

struct LayerHierarchyRow {
    std::shared_ptr<Layer> layer;
    int depth = 0;
};

static bool layer_has_collapsed_group_ancestor(const std::shared_ptr<Title> &title,
                                               const Layer &layer)
{
    if (!title) return false;
    std::string parent_id = layer.parent_id;
    std::set<std::string> visited;
    while (!parent_id.empty() && visited.insert(parent_id).second) {
        auto parent = title->find_layer(parent_id);
        if (!parent || !layer_type_is_container(parent->type))
            break;
        if (parent->group_collapsed)
            return true;
        parent_id = parent->parent_id;
    }
    return false;
}

/* Shared hierarchy model for the Layers panel and timeline.  The source model
 * remains bottom-to-top; this helper returns the visible UI order (top-to-bottom)
 * and hides descendants of collapsed groups.  Malformed legacy/cyclic rows are
 * still surfaced, but never by bypassing an intentional collapsed ancestor. */
static std::vector<LayerHierarchyRow> visible_layer_hierarchy_rows(
    const std::shared_ptr<Title> &title)
{
    std::vector<LayerHierarchyRow> rows;
    if (!title) return rows;

    std::map<std::string, std::vector<std::shared_ptr<Layer>>> children;
    auto valid_group_parent = [&](const std::string &parent_id) {
        auto parent = title->find_layer(parent_id);
        return parent && layer_type_is_container(parent->type);
    };
    for (auto it = title->layers.rbegin(); it != title->layers.rend(); ++it) {
        const auto &layer = *it;
        if (layer && !layer->parent_id.empty() && valid_group_parent(layer->parent_id))
            children[layer->parent_id].push_back(layer);
    }

    std::set<std::string> emitted;
    std::function<void(const std::shared_ptr<Layer> &, int)> append;
    append = [&](const std::shared_ptr<Layer> &layer, int depth) {
        if (!layer || !emitted.insert(layer->id).second)
            return;
        rows.push_back({layer, depth});
        if (layer->type == LayerType::Asset ||
            layer->type != LayerType::Group || layer->group_collapsed)
            return;
        const auto found = children.find(layer->id);
        if (found == children.end())
            return;
        for (const auto &child : found->second)
            append(child, depth + 1);
    };

    for (auto it = title->layers.rbegin(); it != title->layers.rend(); ++it) {
        const auto &layer = *it;
        if (!layer) continue;
        if (layer->parent_id.empty() || !valid_group_parent(layer->parent_id))
            append(layer, 0);
    }

    for (auto it = title->layers.rbegin(); it != title->layers.rend(); ++it) {
        const auto &layer = *it;
        if (!layer || emitted.find(layer->id) != emitted.end())
            continue;
        if (layer_has_collapsed_group_ancestor(title, *layer))
            continue;
        append(layer, 0);
    }
    return rows;
}

static int group_descendant_object_count(const std::shared_ptr<Title> &title,
                                         const std::string &group_id)
{
    if (!title || group_id.empty()) return 0;
    int count = 0;
    for (const auto &layer : title->layers) {
        if (!layer || layer_type_is_container(layer->type))
            continue;
        std::string parent_id = layer->parent_id;
        std::set<std::string> visited;
        while (!parent_id.empty() && visited.insert(parent_id).second) {
            if (parent_id == group_id) {
                ++count;
                break;
            }
            auto parent = title->find_layer(parent_id);
            if (!parent || !layer_type_is_container(parent->type))
                break;
            parent_id = parent->parent_id;
        }
    }
    return count;
}

struct TimelineRow {
    std::shared_ptr<Layer> layer;
    TimelinePropertyRef prop;
    bool is_property = false;
};

static std::vector<TimelineRow> timeline_rows(const std::shared_ptr<Title> &title)
{
    std::vector<TimelineRow> rows;
    if (!title) return rows;
    for (const auto &hierarchy_row : visible_layer_hierarchy_rows(title)) {
        auto layer = hierarchy_row.layer;
        if (!layer) continue;
        rows.push_back({layer, TimelinePropertyRef{}, false});
        const bool show_properties = layer->properties_expanded ||
            (layer->type == LayerType::Group && !layer->group_collapsed);
        if (!show_properties)
            continue;
        std::set<std::string> seen;
        for (auto prop : timeline_properties(*layer)) {
            if (!prop.is_animated()) continue;
            QString label = property_label(prop.name());
            QString identity = label;
            const QString encoded = QString::fromStdString(prop.name());
            if (encoded.startsWith(QStringLiteral("__effect__"))) {
                const QStringList parts = encoded.split(QStringLiteral("__"));
                if (parts.size() >= 3)
                    identity.prepend(QStringLiteral("effect-%1:").arg(parts.at(2)));
            }
            if (seen.insert(identity.toStdString()).second)
                rows.push_back({layer, prop, true});
        }
    }
    return rows;
}

/* ══════════════════════════════════════════════════════════════════
 *  TitleEditor
 * ══════════════════════════════════════════════════════════════════ */
