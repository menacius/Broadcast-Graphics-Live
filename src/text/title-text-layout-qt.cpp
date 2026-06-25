#include "title-text-layout.h"

#include <QByteArray>
#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QPointF>
#include <QRawFont>
#include <QRectF>
#include <QString>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {

struct ParagraphSpan {
    size_t start = 0;
    size_t length = 0;
};

static std::vector<ParagraphSpan> paragraph_spans(const std::string &text)
{
    std::vector<ParagraphSpan> spans;
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            spans.push_back({start, i - start});
            start = i + 1;
        }
    }
    if (spans.empty())
        spans.push_back({0, 0});
    return spans;
}

static int qtext_position_from_byte_offset(const QString &text, size_t byte_offset)
{
    const QByteArray utf8 = text.toUtf8();
    const size_t clamped = std::min(byte_offset, static_cast<size_t>(utf8.size()));
    int units = 0;
    size_t bytes_seen = 0;
    const int text_size = static_cast<int>(text.size());
    for (int i = 0; i < text_size; ++i) {
        const ushort u = text.at(i).unicode();
        const bool high = u >= 0xD800 && u <= 0xDBFF && i + 1 < text_size;
        const int char_units = high ? 2 : 1;
        const size_t chunk_bytes = static_cast<size_t>(text.mid(i, char_units).toUtf8().size());
        if (bytes_seen + chunk_bytes > clamped)
            break;
        bytes_seen += chunk_bytes;
        units += char_units;
        if (high)
            ++i;
    }
    return units;
}

static size_t byte_offset_from_qtext_position(const QString &text, int qpos)
{
    qpos = std::clamp(qpos, 0, static_cast<int>(text.size()));
    return static_cast<size_t>(text.left(qpos).toUtf8().size());
}

static QFont font_from_rich_format(const RichTextCharFormat &format,
                                   float device_scale)
{
    QFont font(QString::fromStdString(format.font_family));
    if (!format.font_style.empty())
        font.setStyleName(QString::fromStdString(format.font_style));
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(
        static_cast<double>(format.font_size) * device_scale))));
    font.setBold(format.bold);
    font.setItalic(format.italic);
    font.setUnderline(format.underline);
    font.setStrikeOut(format.strikethrough);
    font.setKerning(format.kerning_mode != 2 && format.kerning);
    font.setLetterSpacing(QFont::AbsoluteSpacing,
                          (format.tracking +
                           (format.kerning_mode == 2 ? format.manual_kerning : 0.0f)) *
                              device_scale);
    const RichTextFontScaleMetrics scale =
        rich_text_font_scale_metrics(format.scale_x, format.scale_y);
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(
        static_cast<double>(font.pixelSize()) * scale.vertical_factor))));
    font.setStretch(scale.horizontal_stretch_percent);
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    font.setFeature("kern", format.kerning_mode != 2 && format.kerning ? 1 : 0);
    font.setFeature("liga", format.ligatures ? 1 : 0);
    font.setFeature("clig", format.ligatures ? 1 : 0);
    font.setFeature("salt", format.stylistic_alternates ? 1 : 0);
    font.setFeature("frac", format.fractions ? 1 : 0);
    font.setFeature("calt", format.opentype_features ? 1 : 0);
#endif
    font.setCapitalization(QFont::MixedCase);
    if (format.text_style == 1)
        font.setCapitalization(QFont::AllUppercase);
    else if (format.text_style == 2)
        font.setCapitalization(QFont::SmallCaps);
    if (format.text_style == 3 || format.text_style == 4)
        font.setPixelSize(std::max(1, static_cast<int>(std::lround(font.pixelSize() * 0.65))));
    return font;
}

static QTextCharFormat qtext_format(const RichTextCharFormat &format,
                                    float device_scale)
{
    QTextCharFormat result;
    result.setFont(font_from_rich_format(format, device_scale));
    result.setFontUnderline(format.underline);
    result.setFontStrikeOut(format.strikethrough);
    result.setFontKerning(format.kerning_mode != 2 && format.kerning);
    result.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    result.setFontLetterSpacing(
        (format.tracking +
         (format.kerning_mode == 2 ? format.manual_kerning : 0.0f)) *
        device_scale);
    if (format.text_style == 3)
        result.setVerticalAlignment(QTextCharFormat::AlignSuperScript);
    else if (format.text_style == 4)
        result.setVerticalAlignment(QTextCharFormat::AlignSubScript);
    else
        result.setVerticalAlignment(QTextCharFormat::AlignNormal);
    return result;
}

static TextLayoutShapingStyle
shaping_style(const RichTextCharFormat &format)
{
    TextLayoutShapingStyle style;
    style.font_family = format.font_family;
    style.font_style = format.font_style;
    style.font_size = format.font_size;
    style.bold = format.bold;
    style.italic = format.italic;
    style.kerning = format.kerning;
    style.kerning_mode = format.kerning_mode;
    style.manual_kerning = format.manual_kerning;
    style.tracking = format.tracking;
    style.scale_x = format.scale_x;
    style.scale_y = format.scale_y;
    style.baseline_shift = format.baseline_shift;
    style.text_style = format.text_style;
    style.ligatures = format.ligatures;
    style.stylistic_alternates = format.stylistic_alternates;
    style.fractions = format.fractions;
    style.opentype_features = format.opentype_features;
    return style;
}

static Qt::Alignment qt_alignment(int align_h)
{
    if (align_h == 1 || align_h == 4)
        return Qt::AlignHCenter;
    if (align_h == 2 || align_h == 5)
        return Qt::AlignRight;
    if (align_h >= 3)
        return Qt::AlignJustify;
    return Qt::AlignLeft;
}

static void hash_bytes(uint64_t &hash, const QByteArray &bytes)
{
    for (unsigned char value : bytes) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
}

static TextLayoutFontKey font_key(const QRawFont &font)
{
    TextLayoutFontKey key;
    key.family = font.familyName().toStdString();
    key.style = font.styleName().toStdString();
    key.pixel_size = static_cast<float>(font.pixelSize());
    uint64_t hash = 1469598103934665603ULL;
    hash_bytes(hash, QByteArray::fromStdString(key.family));
    hash_bytes(hash, QByteArray("\n", 1));
    hash_bytes(hash, QByteArray::fromStdString(key.style));
    /* Family/style names are not unique enough for a persistent atlas. The
     * stable face tables distinguish fallback fonts and separate files that
     * expose the same display name. */
    hash_bytes(hash, font.fontTable("head"));
    hash_bytes(hash, font.fontTable("maxp"));
    hash_bytes(hash, font.fontTable("name"));
    uint32_t size_bits = 0;
    static_assert(sizeof(size_bits) == sizeof(key.pixel_size), "float size");
    std::memcpy(&size_bits, &key.pixel_size, sizeof(size_bits));
    hash ^= size_bits;
    hash *= 1099511628211ULL;
    key.fingerprint = hash;
    return key;
}

static void translate_line(TextLayoutData &data, TextLayoutLine &line,
                           float dx, float dy)
{
    line.x += dx;
    line.y += dy;
    line.baseline += dy;
    for (uint32_t i = 0; i < line.glyph_count; ++i) {
        TextLayoutGlyph &glyph = data.glyphs[line.glyph_begin + i];
        glyph.x += dx;
        glyph.y += dy;
        glyph.ink_x += dx;
        glyph.ink_y += dy;
    }
    for (uint32_t i = 0; i < line.cluster_count; ++i) {
        TextLayoutCluster &cluster = data.clusters[line.cluster_begin + i];
        cluster.x += dx;
        cluster.y += dy;
        if (cluster.boundary_begin <= data.cursor_boundaries.size() &&
            cluster.boundary_count <=
                data.cursor_boundaries.size() - cluster.boundary_begin) {
            for (uint32_t boundary = 0; boundary < cluster.boundary_count;
                 ++boundary) {
                data.cursor_boundaries[cluster.boundary_begin + boundary].x += dx;
            }
        }
    }
    for (uint32_t i = 0; i < line.run_count; ++i) {
        TextLayoutRun &run = data.runs[line.run_begin + i];
        run.clip_x += dx;
        run.clip_y += dy;
    }
}

static void scale_line_x(TextLayoutData &data, TextLayoutLine &line,
                         float origin_x, float factor)
{
    for (uint32_t i = 0; i < line.glyph_count; ++i) {
        TextLayoutGlyph &glyph = data.glyphs[line.glyph_begin + i];
        glyph.x = origin_x + (glyph.x - origin_x) * factor;
        glyph.advance_x *= factor;
        glyph.ink_x = origin_x + (glyph.ink_x - origin_x) * factor;
        glyph.ink_width *= factor;
    }
    for (uint32_t i = 0; i < line.cluster_count; ++i) {
        TextLayoutCluster &cluster = data.clusters[line.cluster_begin + i];
        cluster.x = origin_x + (cluster.x - origin_x) * factor;
        cluster.width *= factor;
        if (cluster.boundary_begin <= data.cursor_boundaries.size() &&
            cluster.boundary_count <=
                data.cursor_boundaries.size() - cluster.boundary_begin) {
            for (uint32_t boundary = 0; boundary < cluster.boundary_count;
                 ++boundary) {
                TextLayoutCursorBoundary &cursor =
                    data.cursor_boundaries[cluster.boundary_begin + boundary];
                cursor.x = origin_x + (cursor.x - origin_x) * factor;
            }
        }
    }
    for (uint32_t i = 0; i < line.run_count; ++i) {
        TextLayoutRun &run = data.runs[line.run_begin + i];
        run.clip_x = origin_x + (run.clip_x - origin_x) * factor;
        run.clip_width *= factor;
    }
    line.width *= factor;
}

static std::vector<size_t> format_breaks(const RichTextDocument &doc,
                                         const ParagraphSpan &paragraph)
{
    const size_t paragraph_end = paragraph.start + paragraph.length;
    std::vector<size_t> breaks{paragraph.start, paragraph_end};
    for (const RichTextRange &range : doc.ranges) {
        if ((range.mask & RichTextCharShapingMask) == 0)
            continue;
        const size_t range_end = range.start + range.length;
        if (range_end <= paragraph.start || range.start >= paragraph_end)
            continue;
        breaks.push_back(std::max(range.start, paragraph.start));
        breaks.push_back(std::min(range_end, paragraph_end));
    }
    std::sort(breaks.begin(), breaks.end());
    breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());
    return breaks;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
using LayoutFormatList = QList<QTextLayout::FormatRange>;
#else
using LayoutFormatList = QVector<QTextLayout::FormatRange>;
#endif

static LayoutFormatList formats_for_paragraph(const RichTextDocument &doc,
                                              const ParagraphSpan &paragraph,
                                              const QString &text,
                                              float device_scale)
{
    LayoutFormatList formats;
    const std::vector<size_t> breaks = format_breaks(doc, paragraph);
    for (size_t i = 0; i + 1 < breaks.size(); ++i) {
        const size_t begin = breaks[i];
        const size_t end = breaks[i + 1];
        if (end <= begin)
            continue;
        QTextLayout::FormatRange range;
        range.start = qtext_position_from_byte_offset(text, begin - paragraph.start);
        const int qend = qtext_position_from_byte_offset(text, end - paragraph.start);
        range.length = std::max(0, qend - range.start);
        range.format = qtext_format(rich_text_format_at(doc, begin), device_scale);
        if (range.length > 0)
            formats.push_back(range);
    }
    return formats;
}

static void append_empty_line(TextLayoutData &data,
                              const RichTextDocument &doc,
                              const ParagraphSpan &paragraph,
                              uint32_t paragraph_index, float y,
                              float device_scale)
{
    const RichTextCharFormat format =
        rich_text_format_at(doc, std::min(paragraph.start, doc.plain_text.size()));
    const QFontMetricsF metrics(font_from_rich_format(format, device_scale));
    TextLayoutLine line;
    line.byte_start = paragraph.start;
    line.byte_length = 0;
    line.paragraph_index = paragraph_index;
    line.x = 0.0f;
    line.y = y;
    line.width = 0.0f;
    line.height = static_cast<float>(metrics.height());
    line.ascent = static_cast<float>(metrics.ascent());
    line.descent = static_cast<float>(metrics.descent());
    line.baseline = y + line.ascent;
    data.lines.push_back(line);
}

/* QGlyphRun boundaries are local to each shaping/style run. The final cluster
 * of a run must end at the next logical cluster on the line, not at the end of
 * the complete line. Using a per-run list made style-boundary clusters overlap
 * all following styled text, so selection rectangles acquired apparent extra
 * character padding/margins. Finalize every run against one line-wide logical
 * boundary table and rebuild cursor geometry from that exact table. */
static void finalize_line_cluster_boundaries(
    TextLayoutData &data, TextLayoutLine &line,
    const ParagraphSpan &paragraph, const QString &paragraph_text,
    const QTextLine &qline)
{
    const uint32_t cluster_end_index =
        static_cast<uint32_t>(data.clusters.size());
    if (line.cluster_begin >= cluster_end_index)
        return;

    const size_t line_end = line.byte_start + line.byte_length;
    std::vector<size_t> logical_starts;
    logical_starts.reserve(cluster_end_index - line.cluster_begin + 1);
    for (uint32_t index = line.cluster_begin; index < cluster_end_index; ++index) {
        const size_t start = std::clamp(data.clusters[index].byte_start,
                                        line.byte_start, line_end);
        logical_starts.push_back(start);
    }
    logical_starts.push_back(line_end);
    std::sort(logical_starts.begin(), logical_starts.end());
    logical_starts.erase(
        std::unique(logical_starts.begin(), logical_starts.end()),
        logical_starts.end());

    for (uint32_t index = line.cluster_begin; index < cluster_end_index; ++index) {
        TextLayoutCluster &cluster = data.clusters[index];
        const size_t cluster_start = std::clamp(cluster.byte_start,
                                                 line.byte_start, line_end);
        const auto next = std::upper_bound(logical_starts.begin(),
                                           logical_starts.end(), cluster_start);
        const size_t cluster_end = next == logical_starts.end()
            ? line_end : *next;
        cluster.byte_start = cluster_start;
        cluster.byte_length = cluster_end - cluster_start;

        const int cluster_qstart = qtext_position_from_byte_offset(
            paragraph_text, cluster_start - paragraph.start);
        const int cluster_qend = qtext_position_from_byte_offset(
            paragraph_text, cluster_end - paragraph.start);
        const qreal cursor_start = qline.cursorToX(cluster_qstart);
        const qreal cursor_end = qline.cursorToX(cluster_qend);
        cluster.x = line.x + static_cast<float>(
            std::min(cursor_start, cursor_end));
        cluster.y = line.y;
        cluster.width = std::max(
            0.0f, static_cast<float>(std::abs(cursor_end - cursor_start)));
        cluster.height = line.height;

        cluster.boundary_begin = static_cast<uint32_t>(
            data.cursor_boundaries.size());
        size_t boundary_offset = cluster_start;
        while (true) {
            const int boundary_qpos = qtext_position_from_byte_offset(
                paragraph_text, boundary_offset - paragraph.start);
            data.cursor_boundaries.push_back({
                boundary_offset,
                line.x + static_cast<float>(qline.cursorToX(boundary_qpos))});
            if (boundary_offset >= cluster_end)
                break;
            size_t following = rich_text_utf8_next_boundary(
                data.text, boundary_offset + 1);
            if (following <= boundary_offset || following > cluster_end)
                following = cluster_end;
            boundary_offset = following;
        }
        cluster.boundary_count = static_cast<uint32_t>(
            data.cursor_boundaries.size()) - cluster.boundary_begin;
    }

    const uint32_t run_end_index = static_cast<uint32_t>(data.runs.size());
    for (uint32_t run_index = line.run_begin; run_index < run_end_index;
         ++run_index) {
        TextLayoutRun &run = data.runs[run_index];
        size_t run_start = line_end;
        size_t run_end = line.byte_start;
        const uint32_t last_cluster = std::min<uint32_t>(
            run.cluster_begin + run.cluster_count, cluster_end_index);
        for (uint32_t cluster_index = run.cluster_begin;
             cluster_index < last_cluster; ++cluster_index) {
            const TextLayoutCluster &cluster = data.clusters[cluster_index];
            run_start = std::min(run_start, cluster.byte_start);
            run_end = std::max(run_end,
                               cluster.byte_start + cluster.byte_length);
        }
        if (run_end >= run_start) {
            run.byte_start = run_start;
            run.byte_length = run_end - run_start;
        }
    }
}

} // namespace

ImmutableTextLayout build_text_layout(const TextLayoutRequest &source_request)
{
    TextLayoutRequest request = source_request;
    request.document.normalize();
    request.device_scale = std::clamp(request.device_scale, 0.01f, 64.0f);
    request.minimum_horizontal_fit =
        std::clamp(request.minimum_horizontal_fit, 0.01f, 1.0f);
    request.max_width = std::max(0.0f, request.max_width);
    request.max_height = std::max(0.0f, request.max_height);

    auto data = std::make_shared<TextLayoutData>();
    data->key = text_layout_key(request);
    data->text = request.document.plain_text;

    const std::vector<ParagraphSpan> paragraphs = paragraph_spans(data->text);
    float cursor_y = 0.0f;
    float natural_width = 0.0f;

    for (uint32_t paragraph_index = 0;
         paragraph_index < static_cast<uint32_t>(paragraphs.size());
         ++paragraph_index) {
        const ParagraphSpan paragraph = paragraphs[paragraph_index];
        const RichTextParagraphFormat paragraph_format =
            rich_text_paragraph_format_at(request.document, paragraph.start);
        cursor_y += std::max(0.0f, paragraph_format.space_before) *
                    request.device_scale;

        const QString paragraph_text = QString::fromUtf8(
            data->text.data() + paragraph.start,
            static_cast<int>(paragraph.length));
        if (paragraph_text.isEmpty()) {
            append_empty_line(*data, request.document, paragraph,
                              paragraph_index, cursor_y,
                              request.device_scale);
            cursor_y += data->lines.back().height;
            cursor_y += std::max(0.0f, paragraph_format.space_after) *
                        request.device_scale;
            continue;
        }

        const RichTextCharFormat base_format =
            rich_text_format_at(request.document, paragraph.start);
        QTextLayout layout(paragraph_text,
                           font_from_rich_format(base_format,
                                                 request.device_scale));
        layout.setFormats(formats_for_paragraph(request.document, paragraph,
                                                paragraph_text,
                                                request.device_scale));
        QTextOption option;
        option.setUseDesignMetrics(true);
        option.setWrapMode(request.overflow_mode == 0
                               ? (paragraph_format.hyphenate
                                      ? QTextOption::WrapAnywhere
                                      : QTextOption::WrapAtWordBoundaryOrAnywhere)
                               : QTextOption::NoWrap);
        option.setAlignment(request.overflow_mode == 2
                                ? Qt::AlignLeft
                                : qt_alignment(paragraph_format.align_h));
        layout.setTextOption(option);

        const float left_indent =
            std::max(0.0f, paragraph_format.indent_left) * request.device_scale;
        const float right_indent =
            std::max(0.0f, paragraph_format.indent_right) * request.device_scale;
        const float first_indent = paragraph_format.indent_first_line *
                                   request.device_scale;
        const float available_base = request.max_width > 0.0f
                                         ? std::max(1.0f, request.max_width -
                                                              left_indent -
                                                              right_indent)
                                         : 10000000.0f;

        layout.beginLayout();
        uint32_t paragraph_line_index = 0;
        while (true) {
            QTextLine qline = layout.createLine();
            if (!qline.isValid())
                break;
            const float line_indent = paragraph_line_index == 0 ? first_indent : 0.0f;
            const float available = std::max(1.0f, available_base - line_indent);
            qline.setLineWidth(request.overflow_mode == 0 || request.max_width > 0.0f
                                   ? available
                                   : 10000000.0f);
            qline.setPosition(QPointF(left_indent + line_indent, cursor_y));

            TextLayoutLine line;
            line.byte_start = paragraph.start +
                              byte_offset_from_qtext_position(paragraph_text,
                                                              qline.textStart());
            const size_t line_end = paragraph.start +
                                    byte_offset_from_qtext_position(
                                        paragraph_text,
                                        qline.textStart() + qline.textLength());
            line.byte_length = line_end - line.byte_start;
            line.paragraph_index = paragraph_index;
            line.x = static_cast<float>(qline.position().x());
            line.y = static_cast<float>(qline.position().y());
            line.width = static_cast<float>(qline.naturalTextWidth());
            line.height = static_cast<float>(qline.height());
            line.ascent = static_cast<float>(qline.ascent());
            line.descent = static_cast<float>(qline.descent());
            line.baseline = line.y + line.ascent;
            line.run_begin = static_cast<uint32_t>(data->runs.size());
            line.cluster_begin = static_cast<uint32_t>(data->clusters.size());
            line.glyph_begin = static_cast<uint32_t>(data->glyphs.size());

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
            const auto glyph_runs = layout.glyphRuns(
                qline.textStart(), qline.textLength(),
                QTextLayout::RetrieveGlyphIndexes |
                    QTextLayout::RetrieveGlyphPositions |
                    QTextLayout::RetrieveStringIndexes);
#else
            const auto glyph_runs =
                layout.glyphRuns(qline.textStart(), qline.textLength());
#endif
            for (const QGlyphRun &glyph_run : glyph_runs) {
                const auto glyph_ids = glyph_run.glyphIndexes();
                const auto positions = glyph_run.positions();
                if (glyph_ids.isEmpty() || glyph_ids.size() != positions.size())
                    continue;
                const int glyph_count = static_cast<int>(glyph_ids.size());

                TextLayoutRun run;
                run.glyph_begin = static_cast<uint32_t>(data->glyphs.size());
                run.cluster_begin = static_cast<uint32_t>(data->clusters.size());
                run.line_index = static_cast<uint32_t>(data->lines.size());
                run.right_to_left = glyph_run.isRightToLeft();
                run.split_ligature =
                    glyph_run.flags().testFlag(QGlyphRun::SplitLigature);
                const QRectF run_clip = glyph_run.boundingRect();
                run.clip_x = static_cast<float>(run_clip.x());
                run.clip_y = static_cast<float>(run_clip.y());
                run.clip_width = static_cast<float>(run_clip.width());
                run.clip_height = static_cast<float>(run_clip.height());
                const QRawFont raw_font = glyph_run.rawFont();
                run.font = font_key(raw_font);

                std::vector<size_t> cluster_starts;
                cluster_starts.reserve(static_cast<size_t>(glyph_count));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
                const auto string_indexes = glyph_run.stringIndexes();
#endif
                for (int i = 0; i < glyph_count; ++i) {
                    int qindex = qline.textStart();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
                    if (i < static_cast<int>(string_indexes.size()))
                        qindex = static_cast<int>(string_indexes[i]);
                    else
#endif
                    {
                        const qreal relative_x =
                            positions[i].x() - qline.position().x();
                        qindex = qline.xToCursor(
                            relative_x, QTextLine::CursorBetweenCharacters);
                    }
                    qindex = std::clamp(qindex, qline.textStart(),
                                        qline.textStart() + qline.textLength());
                    cluster_starts.push_back(
                        paragraph.start + byte_offset_from_qtext_position(
                                              paragraph_text, qindex));
                }

                size_t run_start = line.byte_start + line.byte_length;
                size_t run_end = line.byte_start;
                for (size_t start : cluster_starts) {
                    run_start = std::min(run_start, start);
                    run_end = std::max(run_end, start);
                }
                if (run_start > line.byte_start + line.byte_length)
                    run_start = line.byte_start;
                std::vector<size_t> logical_starts = cluster_starts;
                logical_starts.push_back(line.byte_start + line.byte_length);
                std::sort(logical_starts.begin(), logical_starts.end());
                logical_starts.erase(
                    std::unique(logical_starts.begin(), logical_starts.end()),
                    logical_starts.end());
                auto cluster_end_for = [&](size_t start) {
                    const auto it = std::upper_bound(logical_starts.begin(),
                                                     logical_starts.end(), start);
                    return it == logical_starts.end()
                               ? line.byte_start + line.byte_length
                               : *it;
                };
                run_end = cluster_end_for(run_end);
                run.byte_start = run_start;
                run.byte_length = run_end - run_start;
                const RichTextCharFormat run_format =
                    rich_text_format_at(request.document, run_start);
                run.shaping_style = shaping_style(run_format);
                run.clip_y -= run_format.baseline_shift *
                              request.device_scale;

                const auto advances = raw_font.advancesForGlyphIndexes(glyph_ids);
                std::vector<uint32_t> glyph_indices;
                glyph_indices.reserve(static_cast<size_t>(glyph_count));
                for (int i = 0; i < glyph_count; ++i) {
                    TextLayoutGlyph glyph;
                    glyph.glyph_id = glyph_ids[i];
                    glyph.run_index = static_cast<uint32_t>(data->runs.size());
                    glyph.x = static_cast<float>(positions[i].x());
                    glyph.y = static_cast<float>(positions[i].y() -
                                                  run_format.baseline_shift *
                                                      request.device_scale);
                    if (i < static_cast<int>(advances.size())) {
                        glyph.advance_x = static_cast<float>(advances[i].x());
                        glyph.advance_y = static_cast<float>(advances[i].y());
                    }
                    const QRectF ink = raw_font.boundingRect(glyph_ids[i]);
                    glyph.ink_x = glyph.x + static_cast<float>(ink.x());
                    glyph.ink_y = glyph.y + static_cast<float>(ink.y());
                    glyph.ink_width = static_cast<float>(ink.width());
                    glyph.ink_height = static_cast<float>(ink.height());
                    glyph_indices.push_back(
                        static_cast<uint32_t>(data->glyphs.size()));
                    data->glyphs.push_back(glyph);
                }

                std::vector<size_t> visual_clusters;
                for (size_t start : cluster_starts) {
                    if (std::find(visual_clusters.begin(), visual_clusters.end(),
                                  start) == visual_clusters.end())
                        visual_clusters.push_back(start);
                }
                for (size_t cluster_start : visual_clusters) {
                    TextLayoutCluster cluster;
                    cluster.byte_start = cluster_start;
                    cluster.byte_length =
                        cluster_end_for(cluster_start) - cluster_start;
                    cluster.run_index = static_cast<uint32_t>(data->runs.size());
                    cluster.line_index = static_cast<uint32_t>(data->lines.size());
                    cluster.right_to_left = run.right_to_left;
                    cluster.glyph_begin = std::numeric_limits<uint32_t>::max();
                    for (size_t i = 0; i < cluster_starts.size(); ++i) {
                        if (cluster_starts[i] != cluster_start)
                            continue;
                        const uint32_t glyph_index = glyph_indices[i];
                        TextLayoutGlyph &glyph = data->glyphs[glyph_index];
                        if (cluster.glyph_begin ==
                            std::numeric_limits<uint32_t>::max())
                            cluster.glyph_begin = glyph_index;
                        ++cluster.glyph_count;
                        glyph.cluster_index =
                            static_cast<uint32_t>(data->clusters.size());
                    }
                    if (cluster.glyph_begin ==
                        std::numeric_limits<uint32_t>::max())
                        cluster.glyph_begin = run.glyph_begin;
                    const int cluster_qstart =
                        qtext_position_from_byte_offset(
                            paragraph_text, cluster_start - paragraph.start);
                    const int cluster_qend =
                        qtext_position_from_byte_offset(
                            paragraph_text,
                            cluster_end_for(cluster_start) - paragraph.start);
                    const qreal cursor_start =
                        qline.cursorToX(cluster_qstart);
                    const qreal cursor_end = qline.cursorToX(cluster_qend);
                    const float advance_left = line.x + static_cast<float>(
                        std::min(cursor_start, cursor_end));
                    const float advance_right = line.x + static_cast<float>(
                        std::max(cursor_start, cursor_end));
                    cluster.x = advance_left;
                    cluster.y = line.y;
                    cluster.width = std::max(0.0f,
                                             advance_right - advance_left);
                    cluster.height = line.height;
                    /* Cursor boundaries are finalized after every glyph
                     * run on this line has contributed its logical starts. */
                    cluster.boundary_begin = 0;
                    cluster.boundary_count = 0;
                    /* Keep ink extents on the glyph records. Cluster bounds
                     * deliberately use cursor advances so spaces, combining
                     * marks, ligatures and RTL selections remain hittable. */
                    data->clusters.push_back(cluster);
                }

                run.glyph_count = static_cast<uint32_t>(data->glyphs.size()) -
                                  run.glyph_begin;
                run.cluster_count =
                    static_cast<uint32_t>(data->clusters.size()) -
                    run.cluster_begin;
                data->runs.push_back(std::move(run));
            }

            finalize_line_cluster_boundaries(*data, line, paragraph,
                                             paragraph_text, qline);
            line.run_count = static_cast<uint32_t>(data->runs.size()) -
                             line.run_begin;
            line.cluster_count = static_cast<uint32_t>(data->clusters.size()) -
                                 line.cluster_begin;
            line.glyph_count = static_cast<uint32_t>(data->glyphs.size()) -
                               line.glyph_begin;
            data->lines.push_back(line);
            natural_width = std::max(natural_width,
                                     line.x + line.width + right_indent);
            cursor_y += line.height;
            ++paragraph_line_index;
            if (request.overflow_mode != 0)
                break;
            if (qline.textStart() + qline.textLength() <
                static_cast<int>(paragraph_text.size()))
                cursor_y += paragraph_format.line_spacing * request.device_scale;
        }
        layout.endLayout();
        cursor_y += std::max(0.0f, paragraph_format.space_after) *
                    request.device_scale;
    }

    data->natural_width = std::max(0.0f, natural_width);
    data->natural_height = std::max(0.0f, cursor_y);
    data->horizontal_fit = 1.0f;

    if (request.overflow_mode == 2 && request.max_width > 0.0f &&
        data->natural_width > request.max_width) {
        data->horizontal_fit = std::clamp(
            request.max_width / std::max(1.0f, data->natural_width),
            request.minimum_horizontal_fit, 1.0f);
        for (TextLayoutLine &line : data->lines) {
            const RichTextParagraphFormat paragraph_format =
                rich_text_paragraph_format_at(request.document,
                                              line.byte_start);
            const float origin_x = line.x;
            scale_line_x(*data, line, origin_x, data->horizontal_fit);
            const float available = request.max_width;
            float shift = 0.0f;
            if (paragraph_format.align_h == 1 || paragraph_format.align_h == 4)
                shift = (available - line.width) * 0.5f - line.x;
            else if (paragraph_format.align_h == 2 ||
                     paragraph_format.align_h == 5)
                shift = available - line.width - line.x;
            if (shift != 0.0f)
                translate_line(*data, line, shift, 0.0f);
        }
    }

    const int align_v = request.document.default_paragraph_format.align_v;
    float vertical_shift = 0.0f;
    if (request.max_height > data->natural_height && align_v != 3) {
        if (align_v == 1)
            vertical_shift = (request.max_height - data->natural_height) * 0.5f;
        else if (align_v == 2)
            vertical_shift = request.max_height - data->natural_height;
    }
    if (vertical_shift != 0.0f) {
        for (TextLayoutLine &line : data->lines)
            translate_line(*data, line, 0.0f, vertical_shift);
    } else if (align_v == 3 && data->lines.size() > 1 &&
               request.max_height > data->natural_height) {
        const float gap = (request.max_height - data->natural_height) /
                          static_cast<float>(data->lines.size() - 1);
        for (size_t i = 0; i < data->lines.size(); ++i)
            translate_line(*data, data->lines[i], 0.0f,
                           gap * static_cast<float>(i));
    }

    data->width = request.max_width > 0.0f
                      ? request.max_width
                      : data->natural_width * data->horizontal_fit;
    data->height = request.max_height > 0.0f
                       ? request.max_height
                       : data->natural_height;
    data->valid = true;
    return data;
}
