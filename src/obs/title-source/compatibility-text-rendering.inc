            const int relative_start = overlap_start - block.position();
            const int relative_end = overlap_end - block.position();
            const qreal x1 = line.cursorToX(relative_start);
            const qreal x2 = line.cursorToX(relative_end);
            const QRectF line_bounds(
                block_rect.left() + std::min(x1, x2),
                block_rect.top() + line.y(),
                std::max<qreal>(1.0, std::abs(x2 - x1)),
                std::max<qreal>(1.0, line.height()));
            local_bounds = has_bounds ? local_bounds.united(line_bounds) : line_bounds;
            has_bounds = true;
        }
    }
    if (!has_bounds)
        return {};

    const QSizeF document_size = doc.size();
    const double horizontal_scale = rich_text_horizontal_fit_scale(layer, text_rect, document_size);
    const QPointF origin = rich_text_document_origin(layer, text_rect, document_size, t, horizontal_scale);
    const qreal scaled_left = local_bounds.left() * horizontal_scale;
    const qreal scaled_width = local_bounds.width() * horizontal_scale;
    local_bounds.setRect(scaled_left, local_bounds.top(),
                         scaled_width, local_bounds.height());
    return local_bounds.translated(origin);
}

static QRect text_transition_unit_render_bounds(const QTextDocument &shaped_document,
                                                const Layer &layer,
                                                const QRectF &text_rect,
                                                double t,
                                                const TextTransitionUnitRange &range,
                                                const ShadowRenderParams &shadow_params,
                                                bool render_shadow,
                                                const QRect &image_bounds)
{
    QRectF bounds = rich_text_document_range_bounds(
        shaped_document, layer, text_rect, t, range);
    if (bounds.isEmpty())
        bounds = text_rect;

    // QTextLine bounds describe advances, not all italic/outline/shadow pixel
    // overhang. Expand from the actual line height, then include the complete
    // shadow envelope. This remains much smaller than a full layer surface.
    double pad = std::max(8.0, bounds.height() * 0.5) +
                 std::max(std::max(0.0, eval_outline_width(layer, t)),
                          max_rich_text_stroke_width(layer, t)) * 2.0 + 3.0;
    if (render_shadow) {
        const double drop_extent = std::max(std::abs(shadow_params.dx),
                                            std::abs(shadow_params.dy)) +
                                   shadow_params.blur * 3.0 +
                                   std::max(0.0, shadow_params.spread);
        const double long_extent = std::max(0.0, shadow_params.long_length) +
                                   std::max(0.0, shadow_params.long_blur) * 3.0;
        pad += std::max(drop_extent, long_extent);
    }

    QRect result = bounds.adjusted(-pad, -pad, pad, pad).toAlignedRect();
    result = result.intersected(image_bounds);
    return result;
}

static QPointF rich_text_transition_unit_kerning_offset(const QTextDocument &shaped_document,
                                                        const QTextDocument &isolated_document,
                                                        const Layer &layer,
                                                        const QRectF &text_rect,
                                                        double t,
                                                        const TextTransitionUnitRange &range)
{
    // Hiding the surrounding characters creates QText format-run boundaries.
    // Qt can no longer apply kerning across those boundaries, even though the
    // hidden characters still reserve their normal advance. Compare the caret
    // position in the fully shaped document with the isolated document and
    // move the independently rendered unit back to its original shaped origin.
    const QPointF shaped = rich_text_document_cursor_position(
        shaped_document, layer, text_rect, t, range.start);
    const QPointF isolated = rich_text_document_cursor_position(
        isolated_document, layer, text_rect, t, range.start);
    return shaped - isolated;
}

static void clear_global_outline_on_local_stroke_ranges(
    QTextDocument &document, const Layer &layer, double t,
    const TextTransitionUnitRange *visible_range)
{
    const RichTextDocument model = rich_text_model_for_source_time(layer, t);
    const QString qplain = document.toPlainText();
    const SourceQtUtf8PositionMap positions(qplain);
    const int visible_start = visible_range ? std::max(0, visible_range->start) : 0;
    const int visible_end = visible_range
        ? visible_start + std::max(0, visible_range->length)
        : std::max(0, document.characterCount() - 1);
    for (const RichTextRange &range : model.ranges) {
        if ((range.mask & RichTextCharStroke) == 0 || range.length == 0)
            continue;
        const int start = std::max(
            visible_start, positions.qpos_from_byte(range.start));
        const int end = std::min(
            visible_end, positions.qpos_from_byte(range.start + range.length));
        if (end <= start)
            continue;
        QTextCursor cursor(&document);
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        QTextCharFormat no_global_outline;
        no_global_outline.setTextOutline(QPen(Qt::NoPen));
        cursor.mergeCharFormat(no_global_outline);
    }
}

static void draw_rich_text_document(QPainter &painter, const Layer &layer, const QFont &font,
                                    const QRectF &text_rect, double t,
                                    const QPen *text_outline = nullptr,
                                    bool outline_only = false,
                                    bool alpha_mask_only = false,
                                    const TextTransitionUnitRange *visible_range = nullptr)
{
    auto doc = rich_text_document_for_layer(layer, font, text_rect, t, visible_range);
    if (text_outline || alpha_mask_only) {
        QTextCursor cursor(doc.get());
        if (visible_range) {
            const int text_length = std::max(0, doc->characterCount() - 1);
            const int start = std::clamp(visible_range->start, 0, text_length);
            const int end = std::clamp(start + std::max(0, visible_range->length), start, text_length);
            cursor.setPosition(start);
            cursor.setPosition(end, QTextCursor::KeepAnchor);
        } else {
            cursor.select(QTextCursor::Document);
        }
        QTextCharFormat format;
        if (text_outline)
            format.setTextOutline(*text_outline);
        if (outline_only)
            format.setForeground(QBrush(Qt::transparent));
        else if (alpha_mask_only) {
            // A stroke-alignment mask must not inherit inline fill colours,
            // gradients or opacity.  Use one opaque white glyph silhouette so
            // DestinationIn/DestinationOut isolate the exact inner/outer half.
            format.setForeground(QBrush(Qt::white));
            format.setTextOutline(QPen(Qt::NoPen));
        }
        cursor.mergeCharFormat(format);
        if (text_outline)
            clear_global_outline_on_local_stroke_ranges(*doc, layer, t, visible_range);
    }
    QSizeF doc_size = doc->size();
    const double scale = rich_text_horizontal_fit_scale(layer, text_rect, doc_size);
    const QPointF origin = rich_text_document_origin(layer, text_rect, doc_size, t, scale);
    if (std::abs(scale - 1.0) > 0.0001) {
        painter.save();
        painter.translate(origin);
        painter.scale(scale, 1.0);
        doc->drawContents(&painter, QRectF(QPointF(0, 0), doc_size));
        painter.restore();
        return;
    }
    painter.save();
    painter.translate(origin);
    doc->drawContents(&painter, QRectF(QPointF(0, 0), doc_size));
    painter.restore();
}

static void draw_prepared_rich_text_document(QPainter &painter,
                                                const QTextDocument &source_document,
                                                const Layer &layer,
                                                const QRectF &text_rect,
                                                double t,
                                                const TextTransitionUnitRange &visible_range,
                                                const QPen *text_outline = nullptr,
                                                bool outline_only = false,
                                                bool alpha_mask_only = false)
{
    std::unique_ptr<QTextDocument> modified_document;
    QTextDocument *document = const_cast<QTextDocument *>(&source_document);
    if (text_outline || alpha_mask_only) {
        modified_document.reset(source_document.clone());
        document = modified_document.get();
        const int text_length = std::max(0, document->characterCount() - 1);
        const int start = std::clamp(visible_range.start, 0, text_length);
        const int end = std::clamp(start + std::max(0, visible_range.length), start, text_length);
        QTextCursor cursor(document);
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        QTextCharFormat format;
        if (text_outline)
            format.setTextOutline(*text_outline);
        if (outline_only)
            format.setForeground(QBrush(Qt::transparent));
        else if (alpha_mask_only) {
            format.setForeground(QBrush(Qt::white));
            format.setTextOutline(QPen(Qt::NoPen));
        }
        cursor.mergeCharFormat(format);
        if (text_outline)
            clear_global_outline_on_local_stroke_ranges(*document, layer, t, &visible_range);
    }

    const QSizeF document_size = document->size();
    const double scale = rich_text_horizontal_fit_scale(layer, text_rect, document_size);
    const QPointF origin = rich_text_document_origin(layer, text_rect, document_size, t, scale);
    if (std::abs(scale - 1.0) > 0.0001) {
        painter.save();
        painter.translate(origin);
        painter.scale(scale, 1.0);
        document->drawContents(&painter, QRectF(QPointF(0, 0), document_size));
        painter.restore();
        return;
    }

    painter.save();
    painter.translate(origin);
    document->drawContents(&painter, QRectF(QPointF(0, 0), document_size));
    painter.restore();
}


struct RichTextStrokeRenderGroup {
    bool on_front = false;
    int alignment = 0;
    bool antialias = true;
};

static bool same_rich_text_stroke_group(const RichTextStrokeRenderGroup &a,
                                        const RichTextStrokeRenderGroup &b)
{
    return a.on_front == b.on_front && a.alignment == b.alignment &&
           a.antialias == b.antialias;
}

static std::vector<RichTextStrokeRenderGroup>
rich_text_stroke_render_groups(const RichTextDocument &model)
{
    std::vector<RichTextStrokeRenderGroup> groups;
    for (const TextLayoutPaintRun &run : text_layout_paint_runs(model)) {
        const RichTextStroke &stroke = run.style.stroke;
        if (!stroke.enabled || stroke.width <= 0.0001f)
            continue;
        RichTextStrokeRenderGroup group;
        group.on_front = stroke.on_front;
        group.alignment = std::clamp(stroke.alignment, 0, 2);
        group.antialias = stroke.antialias;
        if (std::none_of(groups.begin(), groups.end(),
                         [&](const RichTextStrokeRenderGroup &existing) {
                             return same_rich_text_stroke_group(existing, group);
                         }))
            groups.push_back(group);
    }
    return groups;
}

static void draw_positioned_rich_text_document(QPainter &painter,
                                                QTextDocument &document,
                                                const Layer &layer,
                                                const QRectF &text_rect,
                                                double t)
{
    const QSizeF document_size = document.size();
    const double scale = rich_text_horizontal_fit_scale(
        layer, text_rect, document_size);
    const QPointF origin = rich_text_document_origin(
        layer, text_rect, document_size, t, scale);
    painter.save();
    painter.translate(origin);
    if (std::abs(scale - 1.0) > 0.0001)
        painter.scale(scale, 1.0);
    document.drawContents(&painter,
                          QRectF(QPointF(0, 0), document_size));
    painter.restore();
}

static std::unique_ptr<QTextDocument>
rich_text_fill_only_document(const QTextDocument &source)
{
    std::unique_ptr<QTextDocument> document(source.clone());
    QTextCursor cursor(document.get());
    cursor.select(QTextCursor::Document);
    QTextCharFormat format;
    format.setTextOutline(QPen(Qt::NoPen));
    cursor.mergeCharFormat(format);
    return document;
}

static std::unique_ptr<QTextDocument> rich_text_stroke_group_document(
    const QTextDocument &source, const RichTextDocument &model,
    const QRectF &text_rect, const RichTextStrokeRenderGroup &group,
    bool glyph_mask_only, const TextTransitionUnitRange *visible_range)
{
    std::unique_ptr<QTextDocument> document(source.clone());
    QTextCursor all(document.get());
    all.select(QTextCursor::Document);
    QTextCharFormat hidden;
    hidden.setForeground(QBrush(Qt::transparent));
    hidden.setTextOutline(QPen(Qt::NoPen));
    hidden.setFontUnderline(false);
    hidden.setFontStrikeOut(false);
    all.mergeCharFormat(hidden);

    const QString qplain = document->toPlainText();
    const SourceQtUtf8PositionMap positions(qplain);
    const int text_length = std::max(0, document->characterCount() - 1);
    const int visible_start = visible_range
        ? std::clamp(visible_range->start, 0, text_length)
        : 0;
    const int visible_end = visible_range
        ? std::clamp(visible_start + std::max(0, visible_range->length),
                     visible_start, text_length)
        : text_length;

    for (const TextLayoutPaintRun &run : text_layout_paint_runs(model)) {
        const RichTextStroke &stroke = run.style.stroke;
        if (!stroke.enabled || stroke.width <= 0.0001f ||
            stroke.on_front != group.on_front ||
            std::clamp(stroke.alignment, 0, 2) != group.alignment ||
            stroke.antialias != group.antialias)
            continue;

        int start = positions.qpos_from_byte(run.byte_start);
        int end = positions.qpos_from_byte(run.byte_start + run.byte_length);
        start = std::max(start, visible_start);
        end = std::min(end, visible_end);
        if (end <= start)
            continue;

        QTextCursor cursor(document.get());
        cursor.setPosition(std::clamp(start, 0, text_length));
        cursor.setPosition(std::clamp(end, 0, text_length),
                           QTextCursor::KeepAnchor);
        QTextCharFormat format;
        format.setFontUnderline(false);
        format.setFontStrikeOut(false);
        if (glyph_mask_only) {
            format.setForeground(QBrush(Qt::white));
            format.setTextOutline(QPen(Qt::NoPen));
        } else {
            format.setForeground(QBrush(Qt::transparent));
            format.setTextOutline(rich_text_stroke_pen(stroke, text_rect));
        }
        cursor.mergeCharFormat(format);
    }
    return document;
}

static QImage render_rich_text_stroke_group(
    const QTextDocument &source, const RichTextDocument &model,
    const Layer &layer, const QRectF &text_rect, double t,
    const QSize &surface_size, const QPointF &surface_origin,
    const RichTextStrokeRenderGroup &group,
    const TextTransitionUnitRange *visible_range = nullptr)
{
    if (surface_size.isEmpty())
        return {};

    QImage stroke_layer(surface_size,
                        QImage::Format_ARGB32_Premultiplied);
    stroke_layer.fill(Qt::transparent);
    {
        std::unique_ptr<QTextDocument> stroke_document =
            rich_text_stroke_group_document(
                source, model, text_rect, group, false, visible_range);
        QPainter stroke_painter(&stroke_layer);
        stroke_painter.setRenderHint(QPainter::Antialiasing,
                                     group.antialias);
        stroke_painter.setRenderHint(QPainter::TextAntialiasing,
                                     group.antialias);
        stroke_painter.translate(-surface_origin.x(), -surface_origin.y());
        draw_positioned_rich_text_document(
            stroke_painter, *stroke_document, layer, text_rect, t);
    }

    if (group.alignment != 1) {
        QImage glyph_mask(surface_size,
                          QImage::Format_ARGB32_Premultiplied);
        glyph_mask.fill(Qt::transparent);
        {
            std::unique_ptr<QTextDocument> mask_document =
                rich_text_stroke_group_document(
                    source, model, text_rect, group, true, visible_range);
            QPainter mask_painter(&glyph_mask);
            mask_painter.setRenderHint(QPainter::Antialiasing,
                                       group.antialias);
            mask_painter.setRenderHint(QPainter::TextAntialiasing,
                                       group.antialias);
            mask_painter.translate(-surface_origin.x(), -surface_origin.y());
            draw_positioned_rich_text_document(
                mask_painter, *mask_document, layer, text_rect, t);
        }

        QPainter isolate(&stroke_layer);
        isolate.setCompositionMode(
            group.alignment == 0
                ? QPainter::CompositionMode_DestinationOut
                : QPainter::CompositionMode_DestinationIn);
        isolate.drawImage(QPointF(0.0, 0.0), glyph_mask);
    }
    return stroke_layer;
}

static const LayerTransition *active_text_layer_transition(const Layer &layer, double title_time)
{
    const LayerTransition *active = nullptr;
    double lowest_progress = 2.0;
    for (const auto &transition : layer.transitions) {
        if (!transition.enabled || transition.kind != LayerTransitionKind::Text)
            continue;
        const bool within = transition.edge == LayerTransitionEdge::In
            ? title_time <= layer.in_time + transition.duration
            : title_time >= layer.out_time - transition.duration;
        if (!within)
            continue;
        const double progress = layer_transition_progress(
            transition, layer.in_time, layer.out_time, title_time);
        if (progress < lowest_progress) {
            lowest_progress = progress;
            active = &transition;
        }
    }
    return active;
}

static QVector<TextTransitionUnitRange> text_transition_unit_ranges(
    const QString &text, LayerTransitionUnit unit)
{
    QVector<TextTransitionUnitRange> ranges;

    auto append_range = [&](int start, int length) {
        if (length <= 0 || start < 0 || start >= static_cast<int>(text.size()))
            return;
        const int clamped_length = std::min(length, static_cast<int>(text.size()) - start);
        if (clamped_length <= 0 || text.mid(start, clamped_length).trimmed().isEmpty())
            return;
        ranges.push_back(TextTransitionUnitRange{start, clamped_length});
    };

    if (unit == LayerTransitionUnit::Character) {
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
        finder.toStart();
        int start = finder.position();
        while (start >= 0) {
            const int end = finder.toNextBoundary();
            if (end < 0)
                break;
            append_range(start, end - start);
            start = end;
        }
        return ranges;
    }

    if (unit == LayerTransitionUnit::Word) {
        static const QRegularExpression word_pattern(QStringLiteral("\\S+"));
        QRegularExpressionMatchIterator matches = word_pattern.globalMatch(text);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            append_range(static_cast<int>(match.capturedStart()),
                         static_cast<int>(match.capturedLength()));
        }
        return ranges;
    }

    static const QRegularExpression sentence_pattern(
        QStringLiteral("[^.!?\\n]+[.!?]*"));
    QRegularExpressionMatchIterator matches = sentence_pattern.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        append_range(static_cast<int>(match.capturedStart()),
                     static_cast<int>(match.capturedLength()));
    }
    if (ranges.isEmpty())
        append_range(0, static_cast<int>(text.size()));
    return ranges;
}

static QVector<QRectF> text_transition_unit_rects(const QString &text,
                                                   const QFont &font,
                                                   const QRectF &text_rect,
                                                   const Layer &layer,
                                                   LayerTransitionUnit unit)
{
    QVector<QRectF> result;
    const QFontMetricsF metrics(font);
    const QStringList lines = text.split(QLatin1Char('\n'));
    const qreal line_height = std::max<qreal>(1.0, metrics.height());
    const qreal total_height = line_height * std::max(1, static_cast<int>(lines.size()));
    qreal y = text_rect.top();
    if (layer.align_v == 1)
        y = text_rect.center().y() - total_height / 2.0;
    else if (layer.align_v == 2)
        y = text_rect.bottom() - total_height;

    for (const QString &line : lines) {
        const qreal line_width = metrics.horizontalAdvance(line);
        qreal line_x = text_rect.left();
        if (layer.align_h == 1)
            line_x = text_rect.center().x() - line_width / 2.0;
        else if (layer.align_h == 2)
            line_x = text_rect.right() - line_width;

        auto append_range = [&](int start, int length) {
            if (length <= 0)
                return;
            const QString fragment = line.mid(start, length);
            if (fragment.trimmed().isEmpty())
                return;
            const qreal x = line_x + metrics.horizontalAdvance(line.left(start));
            const qreal width = std::max<qreal>(1.0, metrics.horizontalAdvance(fragment));
            result.push_back(QRectF(x, y, width, line_height));
        };

        if (unit == LayerTransitionUnit::Character) {
            for (int i = 0; i < static_cast<int>(line.size()); ++i)
                append_range(i, 1);
        } else if (unit == LayerTransitionUnit::Word) {
            static const QRegularExpression word_pattern(QStringLiteral("\\S+"));
            QRegularExpressionMatchIterator matches = word_pattern.globalMatch(line);
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                append_range(static_cast<int>(match.capturedStart()),
                             static_cast<int>(match.capturedLength()));
            }
        } else {
            static const QRegularExpression sentence_pattern(
                QStringLiteral("[^.!?]+[.!?]*"));
            QRegularExpressionMatchIterator matches = sentence_pattern.globalMatch(line);
            bool matched = false;
            while (matches.hasNext()) {
                const QRegularExpressionMatch match = matches.next();
                append_range(static_cast<int>(match.capturedStart()),
                             static_cast<int>(match.capturedLength()));
                matched = true;
            }
            if (!matched)
                append_range(0, static_cast<int>(line.size()));
        }
        y += line_height;
    }
    return result;
}

static QImage blurred_transition_image(const QImage &source, int radius)
{
    if (source.isNull() || radius <= 0)
        return source;
    QImage blurred = source.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const int width = blurred.width();
    const int height = blurred.height();
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y)
        std::memcpy(pixels.data() + static_cast<size_t>(y) * width * 4,
                    blurred.constScanLine(y), static_cast<size_t>(width) * 4);
    box_blur_pixels(pixels, width, height, std::clamp(radius, 1, 64));
    for (int y = 0; y < height; ++y)
        std::memcpy(blurred.scanLine(y),
                    pixels.data() + static_cast<size_t>(y) * width * 4,
                    static_cast<size_t>(width) * 4);
    return blurred;
}

static int animated_text_blur_radius(double maximum_radius, double unit_progress)
{
    const int max_radius = std::clamp(
        static_cast<int>(std::lround(std::max(0.0, maximum_radius))), 0, 64);
    if (max_radius <= 0)
        return 0;

    // A small blur pyramid avoids recalculating dozens of nearly identical
    // radii per glyph while still making the blur visibly contract to zero.
    constexpr int kBlurRadiusSteps = 12;
    const double hidden = std::clamp(1.0 - unit_progress, 0.0, 1.0);
    const int step = std::clamp(
        static_cast<int>(std::lround(hidden * kBlurRadiusSteps)),
        0, kBlurRadiusSteps);
    return std::clamp(
        static_cast<int>(std::lround(
            static_cast<double>(max_radius) * step / kBlurRadiusSteps)),
        0, max_radius);
}

static QRect transition_image_alpha_bounds(const QImage &image)
{
    if (image.isNull())
        return QRect();

    int min_x = image.width();
    int min_y = image.height();
    int max_x = -1;
    int max_y = -1;
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(line[x]) == 0)
                continue;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
        }
    }
    if (max_x < min_x || max_y < min_y)
        return QRect();
    return QRect(QPoint(min_x, min_y), QPoint(max_x, max_y));
}

static QImage render_rich_text_transition_unit(const QTextDocument &isolated_document,
                                               const Layer &layer,
                                               double t,
                                               const QFont &font,
                                               const QRectF &text_rect,
                                               const QRect &render_bounds,
                                               const ShadowRenderParams &shadow_params,
                                               bool render_shadow,
                                               const TextTransitionUnitRange &range)
{
    if (render_bounds.isEmpty())
        return {};

    QImage unit_image(render_bounds.size(), QImage::Format_ARGB32_Premultiplied);
    unit_image.fill(Qt::transparent);

    QPainter painter(&unit_image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);
    painter.translate(-render_bounds.x(), -render_bounds.y());

    const double text_outline_clip_pad =
        std::max(std::max(0.0, eval_outline_width(layer, t)),
                 max_rich_text_stroke_width(layer, t)) + 2.0;
    painter.save();
    painter.setClipRect(text_rect.adjusted(-text_outline_clip_pad,
                                           -text_outline_clip_pad,
                                           text_outline_clip_pad,
                                           text_outline_clip_pad));

    if (render_shadow) {
        QImage shadow_mask(render_bounds.size(),
                           QImage::Format_ARGB32_Premultiplied);
        shadow_mask.fill(Qt::transparent);
        QPainter mask_painter(&shadow_mask);
        mask_painter.setRenderHint(QPainter::Antialiasing, true);
        mask_painter.setRenderHint(QPainter::TextAntialiasing, true);
        mask_painter.setPen(Qt::NoPen);
        mask_painter.setBrush(Qt::white);
        mask_painter.translate(-render_bounds.x(), -render_bounds.y());
        draw_prepared_rich_text_document(mask_painter, isolated_document,
                                         layer, text_rect, t, range);
        mask_painter.end();
        const QString shape_key =
            QStringLiteral("text-transition|%1,%2,%3x%4|%5")
                .arg(render_bounds.x()).arg(render_bounds.y())
                .arg(render_bounds.width()).arg(render_bounds.height())
                .arg(image_content_hash(shadow_mask));
        CachedShadowImage shadow = build_shadow_image(
            layer, shadow_params, shape_key, shadow_mask);
        painter.save();
        painter.setClipping(false);
        painter.drawImage(QPointF(render_bounds.topLeft()) + shadow.origin,
                          shadow.image);
        painter.restore();
    }

    QColor fill = color_from_argb(eval_text_color(layer, t));
    fill.setAlphaF(std::clamp((double)fill.alphaF(), 0.0, 1.0));
    const RichTextDocument stroke_model =
        rich_text_model_for_source_time(layer, t);
    const std::vector<RichTextStrokeRenderGroup> stroke_groups =
        rich_text_stroke_render_groups(stroke_model);

    auto draw_text_fill = [&]() {
        std::unique_ptr<QTextDocument> fill_document =
            rich_text_fill_only_document(isolated_document);
        painter.save();
        painter.setOpacity(fill.alphaF());
        draw_positioned_rich_text_document(
            painter, *fill_document, layer, text_rect, t);
        painter.restore();
    };
    auto draw_stroke_phase = [&](bool on_front) {
        for (const RichTextStrokeRenderGroup &group : stroke_groups) {
            if (group.on_front != on_front)
                continue;
            const QImage stroke_layer = render_rich_text_stroke_group(
                isolated_document, stroke_model, layer, text_rect, t,
                render_bounds.size(), render_bounds.topLeft(), group, &range);
            if (!stroke_layer.isNull())
                painter.drawImage(QPointF(render_bounds.topLeft()),
                                  stroke_layer);
        }
    };

    draw_stroke_phase(false);
    draw_text_fill();
    draw_stroke_phase(true);

    painter.restore();
    painter.end();
    return unit_image;
}

static bool layer_has_non_transform_animation(const Layer &layer);

struct IsolatedTextUnitCache {
    struct BlurredVariant {
        int radius = 0;
        QImage crop;
        QRect bounds;
    };

    QString key;
    QString rejected_key;
    // Store only each unit's non-transparent crop. Keeping full layer-sized
    // images per character can retain hundreds of megabytes.
    QVector<TextTransitionUnitRange> ranges;
    QVector<QImage> source_crops;
    QVector<QRect> source_bounds;
    QVector<QPointF> placement_offsets;
    QVector<QVector<BlurredVariant>> blurred_variants;
    qsizetype source_bytes = 0;
    qsizetype blurred_bytes = 0;
    BlurredVariant scratch_variant;
    uint64_t last_used = 0;
};

static double source_frame_duration();

struct IsolatedTextUnitCachePool {
    std::unordered_map<std::string, IsolatedTextUnitCache> entries;
    uint64_t tick = 0;
};

static qsizetype isolated_text_unit_cache_bytes(const IsolatedTextUnitCache &cache)
{
    return cache.source_bytes + cache.blurred_bytes + cache.scratch_variant.crop.sizeInBytes();
}

static void evict_isolated_text_unit_caches(IsolatedTextUnitCachePool &pool,
                                             const std::string &protected_cache_id)
{
    constexpr size_t kMaxCachedTextLayers = 8;
    constexpr qsizetype kMaxTextTransitionCacheBytes = 128 * 1024 * 1024;
    qsizetype total_bytes = 0;
    for (const auto &entry : pool.entries)
        total_bytes += isolated_text_unit_cache_bytes(entry.second);

    while ((pool.entries.size() > kMaxCachedTextLayers ||
            total_bytes > kMaxTextTransitionCacheBytes) &&
           pool.entries.size() > 1) {
        auto oldest = pool.entries.end();
        for (auto it = pool.entries.begin(); it != pool.entries.end(); ++it) {
            if (it->first == protected_cache_id)
                continue;
            if (oldest == pool.entries.end() ||
                it->second.last_used < oldest->second.last_used)
                oldest = it;
        }
        if (oldest == pool.entries.end())
            break;
        total_bytes -= isolated_text_unit_cache_bytes(oldest->second);
        pool.entries.erase(oldest);
    }
}

static bool apply_isolated_text_layer_transition(QImage &image,
                                                 const QImage &static_background,
                                                 const Layer &layer,
                                                 double title_time,
                                                 const QString &text,
                                                 const QFont &font,
                                                 const QRectF &text_rect,
                                                 double t,
                                                 const ShadowRenderParams &shadow_params,
                                                 bool render_shadow)
{
    const LayerTransition *transition = active_text_layer_transition(layer, title_time);
    if (!transition || image.isNull() || layer.type == LayerType::Ticker)
        return false;

    // A single thread-local cache caused simultaneous text transitions on two
    // layers to evict each other every draw and rebuild all glyph surfaces on
    // every frame. Keep a small byte-bounded LRU pool keyed by layer id.
    static thread_local IsolatedTextUnitCachePool cache_pool;
    const std::string cache_id = layer.id.empty()
        ? std::string("__anonymous_text_transition") : layer.id;
    auto [cache_it, inserted] = cache_pool.entries.try_emplace(cache_id);
    (void)inserted;
    IsolatedTextUnitCache &cache = cache_it->second;
    cache.last_used = ++cache_pool.tick;
    // Oversized variants are scratch-only and must not remain resident after a
    // completed frame.
    cache.scratch_variant = {};
    evict_isolated_text_unit_caches(cache_pool, cache_id);

    // Hashing the complete layer-sized QImage on every transition frame was a
    // major memory-bandwidth regression (several MB per layer per frame). The
    // store revision invalidates static style changes, while the evaluated
    // time component covers animated text/style properties. Text and layout
    // values keep live-cue changes independent from unrelated store updates.
    const bool time_varying_source = layer_has_non_transform_animation(layer);
    const double frame_duration = std::max(1.0 / 240.0, source_frame_duration());
    const qlonglong evaluated_frame = time_varying_source
        ? static_cast<qlonglong>(std::llround(t / frame_duration)) : 0;
    const QByteArray text_digest = QCryptographicHash::hash(
        text.toUtf8(), QCryptographicHash::Sha1).toHex();
    const QString cache_key = QStringLiteral("%1|rev=%2|unit=%3|%4x%5|text=%6|font=%7|rect=%8,%9,%10,%11|shadow=%12|frame=%13")
        .arg(QString::fromStdString(layer.id))
        .arg(static_cast<qulonglong>(TitleDataStore::instance().revision()))
        .arg(static_cast<int>(transition->unit))
        .arg(image.width())
        .arg(image.height())
        .arg(QString::fromLatin1(text_digest))
        .arg(font.toString())
        .arg(text_rect.x(), 0, 'f', 3)
        .arg(text_rect.y(), 0, 'f', 3)
        .arg(text_rect.width(), 0, 'f', 3)
        .arg(text_rect.height(), 0, 'f', 3)
        .arg(render_shadow ? 1 : 0)
        .arg(evaluated_frame);

    constexpr qsizetype kMaxIsolatedTextUnits = 512;
    constexpr qsizetype kMaxSourceCropBytes = 48 * 1024 * 1024;
    if (cache.rejected_key == cache_key)
        return false;

    if (cache.key != cache_key || cache.source_crops.size() != cache.ranges.size()) {
        cache.key.clear();
        cache.ranges = text_transition_unit_ranges(text, transition->unit);
        cache.source_crops.clear();
        cache.source_bounds.clear();
        cache.placement_offsets.clear();
        cache.blurred_variants.clear();
        cache.source_bytes = 0;
        cache.blurred_bytes = 0;
        if (cache.ranges.isEmpty() || cache.ranges.size() > kMaxIsolatedTextUnits) {
            cache.ranges.clear();
            cache.rejected_key = cache_key;
            evict_isolated_text_unit_caches(cache_pool, cache_id);
            return false;
        }

        cache.source_crops.reserve(cache.ranges.size());
        cache.source_bounds.reserve(cache.ranges.size());
        cache.placement_offsets.reserve(cache.ranges.size());

        const std::unique_ptr<QTextDocument> shaped_document =
            rich_text_document_for_layer(layer, font, text_rect, t, nullptr);
        qsizetype source_crop_bytes = 0;
        for (const TextTransitionUnitRange &range : cache.ranges) {
            const std::unique_ptr<QTextDocument> isolated_document =
                rich_text_document_for_layer(layer, font, text_rect, t, &range);
            const QPointF placement_offset = shaped_document && isolated_document
                ? rich_text_transition_unit_kerning_offset(
                      *shaped_document, *isolated_document, layer, text_rect, t, range)
                : QPointF();
            const QRect render_bounds = shaped_document
                ? text_transition_unit_render_bounds(
                      *shaped_document, layer, text_rect, t, range,
                      shadow_params, render_shadow, image.rect())
                : image.rect();
            QImage unit_image = isolated_document
                ? render_rich_text_transition_unit(
                      *isolated_document, layer, t, font, text_rect, render_bounds,
                      shadow_params, render_shadow, range)
                : QImage();
            const QRect local_bounds = transition_image_alpha_bounds(unit_image)
                .intersected(unit_image.rect());
            const QRect bounds = local_bounds.isEmpty()
                ? QRect()
                : local_bounds.translated(render_bounds.topLeft());
            QImage source_crop = local_bounds.isEmpty()
                ? QImage() : unit_image.copy(local_bounds);
            source_crop_bytes += source_crop.sizeInBytes();
            if (source_crop_bytes > kMaxSourceCropBytes) {
                cache.key.clear();
                cache.rejected_key = cache_key;
                cache.ranges.clear();
                cache.source_crops.clear();
                cache.source_bounds.clear();
                cache.placement_offsets.clear();
                cache.blurred_variants.clear();
                cache.source_bytes = 0;
                cache.blurred_bytes = 0;
                evict_isolated_text_unit_caches(cache_pool, cache_id);
                return false;
            }
            cache.source_bounds.push_back(bounds);
            cache.placement_offsets.push_back(placement_offset);
            cache.source_crops.push_back(std::move(source_crop));
        }
        cache.source_bytes = source_crop_bytes;
        cache.key = cache_key;
        cache.rejected_key.clear();
        cache.blurred_variants.clear();
        cache.blurred_variants.resize(cache.ranges.size());
        cache.blurred_bytes = 0;
        cache.scratch_variant = {};
        evict_isolated_text_unit_caches(cache_pool, cache_id);
    }

    const QVector<TextTransitionUnitRange> &ranges = cache.ranges;
    if (ranges.isEmpty())
        return false;

    const bool blur_transition =
        transition->type == LayerTransitionType::TextBlur ||
        transition->type == LayerTransitionType::TextBlurSlide;

    auto blurred_variant_for = [&image, &cache, &cache_id](int index, int radius)
        -> const IsolatedTextUnitCache::BlurredVariant * {
        if (radius <= 0 || index < 0 || index >= cache.source_crops.size())
            return nullptr;

        QVector<IsolatedTextUnitCache::BlurredVariant> &variants =
            cache.blurred_variants[index];
        for (const auto &variant : variants) {
            if (variant.radius == radius)
                return &variant;
        }

        const QImage &source_crop = cache.source_crops[index];
        const QRect source_bounds = cache.source_bounds[index];
        if (source_crop.isNull() || source_bounds.isEmpty())
            return nullptr;

        // Blur only the current unit and expand its temporary image by the
        // current radius. The radius changes from blur_amount to zero as the
        // unit progresses, so the glyph itself becomes progressively sharper.
        const int blur_pad = radius * 3 + 2;
        QImage padded(source_crop.width() + blur_pad * 2,
                      source_crop.height() + blur_pad * 2,
                      QImage::Format_ARGB32_Premultiplied);
        padded.fill(Qt::transparent);
        {
            QPainter padded_painter(&padded);
            padded_painter.drawImage(QPoint(blur_pad, blur_pad), source_crop);
        }

        QImage blurred = blurred_transition_image(padded, radius);
        const QRect local_bounds = transition_image_alpha_bounds(blurred);
        if (local_bounds.isEmpty())
            return nullptr;

        const QPoint padded_origin = source_bounds.topLeft() - QPoint(blur_pad, blur_pad);
        const QRect global_bounds(padded_origin + local_bounds.topLeft(),
                                  local_bounds.size());
        const QRect clipped_bounds = global_bounds.intersected(image.rect());
        if (clipped_bounds.isEmpty())
            return nullptr;

        const QRect local_clip(clipped_bounds.topLeft() - global_bounds.topLeft(),
                               clipped_bounds.size());
        const QRect blurred_crop_rect(
            local_bounds.topLeft() + local_clip.topLeft(), local_clip.size());

        QImage cropped_blur = blurred.copy(blurred_crop_rect);
        const qsizetype variant_bytes = cropped_blur.sizeInBytes();

        // A per-unit count alone still allowed long strings to retain hundreds
        // of MB. Bound both the number of radii per unit and the total cache.
        constexpr int kMaxBlurVariantsPerUnit = 4;
        constexpr qsizetype kMaxBlurCacheBytes = 48 * 1024 * 1024;
        if (variant_bytes > kMaxBlurCacheBytes) {
            cache.scratch_variant = {radius, std::move(cropped_blur), clipped_bounds};
            return &cache.scratch_variant;
        }
        if (cache.blurred_bytes + variant_bytes > kMaxBlurCacheBytes) {
            for (auto &unit_variants : cache.blurred_variants)
                unit_variants.clear();
            cache.blurred_bytes = 0;
        }
        while (variants.size() >= kMaxBlurVariantsPerUnit) {
            cache.blurred_bytes -= variants.front().crop.sizeInBytes();
            variants.removeAt(0);
        }
        variants.push_back({radius, std::move(cropped_blur), clipped_bounds});
        cache.blurred_bytes += variants.back().crop.sizeInBytes();
        evict_isolated_text_unit_caches(cache_pool, cache_id);
        return &variants.back();
    };

    if (static_background.isNull())
        image.fill(Qt::transparent);
    else
        image = static_background.copy();
    const double global_progress = layer_transition_progress(
        *transition, layer.in_time, layer.out_time, title_time);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const int count = static_cast<int>(ranges.size());
    for (int index = 0; index < count; ++index) {
        const int order = transition->reverse_order ? count - 1 - index : index;
        const double delay = count <= 1 ? 0.0
            : transition->stagger * static_cast<double>(order) / static_cast<double>(count - 1);
        const double span = std::max(0.05, 1.0 - transition->stagger);
        double local = 1.0;
        if (transition->edge == LayerTransitionEdge::In)
            local = std::clamp((global_progress - delay) / span, 0.0, 1.0);
        else {
            const double out_phase = 1.0 - global_progress;
            local = 1.0 - std::clamp((out_phase - delay) / span, 0.0, 1.0);
        }
        local = layer_transition_ease(local, transition->easing);
        if (local <= 0.0001)
            continue;

        const QImage &unit_crop = cache.source_crops[index];
        const QRect source_bounds = cache.source_bounds[index];
        const QPointF placement_offset = cache.placement_offsets.value(index);
        if (source_bounds.isEmpty() || unit_crop.isNull())
            continue;

        const QPointF source_origin = QPointF(source_bounds.topLeft()) + placement_offset;
        const QRectF unit_bounds(source_origin, QSizeF(source_bounds.size()));
        const QPointF center = unit_bounds.center();

        double dx = 0.0;
        double dy = 0.0;
        if (transition->type == LayerTransitionType::TextSlide ||
            transition->type == LayerTransitionType::TextBlurSlide) {
            const double hidden = 1.0 - local;
            switch (transition->direction) {
            case LayerTransitionDirection::Right: dx = transition->offset * hidden; break;
            case LayerTransitionDirection::Up: dy = -transition->offset * hidden; break;
            case LayerTransitionDirection::Down: dy = transition->offset * hidden; break;
            case LayerTransitionDirection::Left:
            case LayerTransitionDirection::None:
            default: dx = -transition->offset * hidden; break;
            }
        }
        const double scale = transition->type == LayerTransitionType::TextScale
            ? transition->scale_from + (1.0 - transition->scale_from) * local : 1.0;

        QRectF visible = unit_bounds;
        if (transition->type == LayerTransitionType::TextWipe) {
            switch (transition->direction) {
            case LayerTransitionDirection::Right:
                visible.setLeft(unit_bounds.right() - unit_bounds.width() * local);
                break;
            case LayerTransitionDirection::Up:
                visible.setTop(unit_bounds.bottom() - unit_bounds.height() * local);
                break;
            case LayerTransitionDirection::Down:
                visible.setHeight(unit_bounds.height() * local);
                break;
            case LayerTransitionDirection::Left:
            case LayerTransitionDirection::None:
            default:
                visible.setWidth(unit_bounds.width() * local);
                break;
            }
        }

        painter.save();
        painter.translate(center.x() + dx, center.y() + dy);
        painter.scale(scale, scale);
        painter.translate(-center.x(), -center.y());
        if (transition->type == LayerTransitionType::TextWipe)
            painter.setClipRect(visible);
        const int current_blur_radius = blur_transition
            ? animated_text_blur_radius(transition->blur_amount, local)
            : 0;
        const IsolatedTextUnitCache::BlurredVariant *blurred_variant =
            blurred_variant_for(index, current_blur_radius);

        // Draw one representation of the glyph: blurred while radius > 0,
        // sharp only when the radius reaches zero. Drawing both simultaneously
        // produces a sharp core with a halo, i.e. a glow rather than blur.
        painter.setOpacity(local);
        if (blurred_variant && !blurred_variant->crop.isNull()) {
            painter.drawImage(QPointF(blurred_variant->bounds.topLeft()) + placement_offset,
                              blurred_variant->crop);
        } else {
            painter.drawImage(source_origin, unit_crop);
        }
        painter.restore();
    }

    painter.end();
    cache.scratch_variant = {};
    return true;
}

static void apply_text_layer_transition(QImage &image,
                                        const Layer &layer,
                                        double title_time,
                                        const QString &text,
                                        const QFont &font,
                                        const QRectF &text_rect)
{
    const LayerTransition *transition = active_text_layer_transition(layer, title_time);
    if (!transition || image.isNull())
        return;

    QVector<QRectF> raw_units = text_transition_unit_rects(
        text, font, text_rect, layer, transition->unit);
    // The precise isolated path is bounded to 512 units. When it declines a
    // pathological text block, progressively coarsen the fallback grouping
    // rather than issuing thousands of full-surface draw calls in one frame.
    constexpr int kMaxFlattenedTransitionUnits = 512;
    if (raw_units.size() > kMaxFlattenedTransitionUnits) {
        raw_units = text_transition_unit_rects(
            text, font, text_rect, layer, LayerTransitionUnit::Word);
        if (raw_units.size() > kMaxFlattenedTransitionUnits)
            raw_units = text_transition_unit_rects(
                text, font, text_rect, layer, LayerTransitionUnit::Sentence);
        if (raw_units.size() > kMaxFlattenedTransitionUnits)
            raw_units = {text_rect};
    }
    if (raw_units.isEmpty())
        return;

    const double global_progress = layer_transition_progress(
        *transition, layer.in_time, layer.out_time, title_time);
    const bool blur_transition =
        transition->type == LayerTransitionType::TextBlur ||
        transition->type == LayerTransitionType::TextBlurSlide;
    const QImage source = image.copy();
    QVector<int> cached_blur_radii;
    QVector<QImage> cached_blur_images;
    const qsizetype source_bytes = source.sizeInBytes();
    constexpr qsizetype kMaxFlattenedBlurWorkingSet = 64 * 1024 * 1024;
    const bool use_single_flattened_blur =
        blur_transition && source_bytes > kMaxFlattenedBlurWorkingSet / 4;
    const int single_flattened_blur_radius = use_single_flattened_blur
        ? animated_text_blur_radius(transition->blur_amount, global_progress) : 0;
    QImage single_flattened_blur;
    if (use_single_flattened_blur && source_bytes <= kMaxFlattenedBlurWorkingSet)
        single_flattened_blur = blurred_transition_image(source, single_flattened_blur_radius);

    auto image_for_blur_radius = [&source, &cached_blur_radii, &cached_blur_images,
                                  &single_flattened_blur, use_single_flattened_blur](int radius)
        -> const QImage & {
        if (use_single_flattened_blur)
            return single_flattened_blur.isNull() ? source : single_flattened_blur;
        for (int i = 0; i < cached_blur_radii.size(); ++i) {
            if (cached_blur_radii.at(i) == radius)
                return cached_blur_images.at(i);
        }
        // The precise fallback is used only for modest surfaces. Still cap the
        // number of retained full-size blur levels to avoid a 12x image cache.
        constexpr int kMaxFlattenedBlurLevels = 3;
        if (cached_blur_images.size() >= kMaxFlattenedBlurLevels) {
            cached_blur_radii.removeFirst();
            cached_blur_images.removeFirst();
        }
        cached_blur_radii.push_back(radius);
        cached_blur_images.push_back(blurred_transition_image(source, radius));
        return cached_blur_images.back();
    };
    const QRectF image_bounds(QPointF(0.0, 0.0), QSizeF(image.size()));
    // Keep a little glyph-bearing/antialiasing slack around each logical unit.
    // The clip itself is moved below together with slide transitions; keeping it
    // at the final unit position is what used to crop horizontally moving glyphs.
    const qreal margin = 1.5;

    QVector<QRectF> units;
    units.reserve(raw_units.size());
    for (const QRectF &unit : raw_units) {
        const QRectF expanded = unit.adjusted(-margin, -margin, margin, margin).intersected(image_bounds);
        if (!expanded.isEmpty())
            units.push_back(expanded);
    }
    if (units.isEmpty())
        return;

    QPainter clear_painter(&image);
    clear_painter.setCompositionMode(QPainter::CompositionMode_Clear);
    for (const QRectF &unit : units)
        clear_painter.fillRect(unit, Qt::transparent);
    clear_painter.end();

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const int count = static_cast<int>(units.size());
    for (int index = 0; index < count; ++index) {
        const int order = transition->reverse_order ? count - 1 - index : index;
        const double delay = count <= 1 ? 0.0
            : transition->stagger * static_cast<double>(order) / static_cast<double>(count - 1);
        const double span = std::max(0.05, 1.0 - transition->stagger);
        double local = 1.0;
        if (transition->edge == LayerTransitionEdge::In)
            local = std::clamp((global_progress - delay) / span, 0.0, 1.0);
        else {
            const double out_phase = 1.0 - global_progress;
            local = 1.0 - std::clamp((out_phase - delay) / span, 0.0, 1.0);
        }
        local = layer_transition_ease(local, transition->easing);
        if (local <= 0.0001)
            continue;

        const QRectF unit = units[index];
        double dx = 0.0;
        double dy = 0.0;
        if (transition->type == LayerTransitionType::TextSlide ||
            transition->type == LayerTransitionType::TextBlurSlide) {
            const double hidden = 1.0 - local;
            switch (transition->direction) {
            case LayerTransitionDirection::Right: dx = transition->offset * hidden; break;
            case LayerTransitionDirection::Up: dy = -transition->offset * hidden; break;
            case LayerTransitionDirection::Down: dy = transition->offset * hidden; break;
            case LayerTransitionDirection::Left:
            case LayerTransitionDirection::None:
            default: dx = -transition->offset * hidden; break;
            }
        }
        const double scale = transition->type == LayerTransitionType::TextScale
            ? transition->scale_from + (1.0 - transition->scale_from) * local : 1.0;

