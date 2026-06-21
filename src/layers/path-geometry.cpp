#include "path-geometry.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QLineF>

#include <algorithm>
#include <cmath>

namespace gsp {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kEpsilon = 1e-9;

QPointF normalized_to_local(double x, double y, const QRectF &rect)
{
    return QPointF(rect.left() + x * rect.width(),
                   rect.top() + y * rect.height());
}

BezierPathPoint normalized_point(const QPointF &point, const QRectF &rect)
{
    const double width = std::max(kEpsilon, rect.width());
    const double height = std::max(kEpsilon, rect.height());
    BezierPathPoint result;
    result.x = (point.x() - rect.left()) / width;
    result.y = (point.y() - rect.top()) / height;
    result.in_x = result.out_x = result.x;
    result.in_y = result.out_y = result.y;
    return result;
}

bool same_point(const QPointF &a, const QPointF &b)
{
    return QLineF(a, b).length() <= 1e-6;
}

QPointF normalized_vector(const QPointF &value)
{
    const double length = std::hypot(value.x(), value.y());
    return length > kEpsilon ? value / length : QPointF();
}

std::vector<QPointF> primitive_vertices(const Layer &layer, const QRectF &rect)
{
    std::vector<QPointF> vertices;
    switch (layer.shape_type) {
    case ShapeType::Rectangle:
    case ShapeType::RoundedRectangle:
        vertices = {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
        break;
    case ShapeType::Triangle:
    case ShapeType::Polygon:
    case ShapeType::Diamond: {
        const int sides = layer.shape_type == ShapeType::Triangle
            ? 3
            : (layer.shape_type == ShapeType::Diamond ? 4 : std::clamp(layer.shape_sides, 3, 64));
        const QPointF center = rect.center();
        const double rx = rect.width() / 2.0;
        const double ry = rect.height() / 2.0;
        vertices.reserve((size_t)sides);
        for (int i = 0; i < sides; ++i) {
            const double angle = -kPi / 2.0 + 2.0 * kPi * i / sides;
            vertices.emplace_back(center.x() + std::cos(angle) * rx,
                                  center.y() + std::sin(angle) * ry);
        }
        break;
    }
    case ShapeType::Star: {
        const QPointF center = rect.center();
        const double rx = rect.width() / 2.0;
        const double ry = rect.height() / 2.0;
        const int points = std::clamp(layer.shape_points, 3, 64);
        const double inner = std::clamp((double)layer.shape_inner_radius, 0.0, 1.0) * 2.0;
        const double outer = std::clamp((double)layer.shape_outer_radius, 0.0, 1.0) * 2.0;
        vertices.reserve((size_t)points * 2);
        for (int i = 0; i < points * 2; ++i) {
            const double factor = (i % 2 == 0) ? outer : inner;
            const double angle = -kPi / 2.0 + kPi * i / points;
            vertices.emplace_back(center.x() + std::cos(angle) * rx * factor,
                                  center.y() + std::sin(angle) * ry * factor);
        }
        break;
    }
    default:
        break;
    }
    return vertices;
}

std::vector<double> primitive_corner_radii(const Layer &layer, size_t count)
{
    std::vector<double> radii(count, std::max(0.0, (double)layer.shape_roundness));
    if (layer.shape_type == ShapeType::Star) {
        for (size_t i = 0; i < radii.size(); ++i)
            radii[i] = std::max(0.0, (double)(i % 2 == 0 ? layer.shape_roundness
                                                         : layer.shape_inner_roundness));
    }
    if (layer.shape_type == ShapeType::Rectangle || layer.shape_type == ShapeType::RoundedRectangle) {
        radii.assign({std::max(0.0, (double)layer.corner_radius_tl),
                      std::max(0.0, (double)layer.corner_radius_tr),
                      std::max(0.0, (double)layer.corner_radius_br),
                      std::max(0.0, (double)layer.corner_radius_bl)});
        radii.resize(count, 0.0);
    }
    return radii;
}

struct CornerData {
    QPointF previous;
    QPointF anchor;
    QPointF next;
    QPointF incoming;
    QPointF outgoing;
    double requested = 0.0;
    double radius = 0.0;
    double max_radius = 0.0;
    bool rounded = false;
};

CornerData make_corner(const QPointF &previous, const QPointF &anchor, const QPointF &next,
                       double requested, bool eligible)
{
    CornerData corner;
    corner.previous = previous;
    corner.anchor = anchor;
    corner.next = next;
    const double incoming_length = QLineF(anchor, previous).length();
    const double outgoing_length = QLineF(anchor, next).length();
    corner.max_radius = std::max(0.0, std::min(incoming_length, outgoing_length) * 0.5);
    corner.requested = std::max(0.0, requested);
    corner.radius = eligible ? std::clamp(corner.requested, 0.0, corner.max_radius) : 0.0;
    const QPointF toward_previous = normalized_vector(previous - anchor);
    const QPointF toward_next = normalized_vector(next - anchor);
    corner.incoming = anchor + toward_previous * corner.radius;
    corner.outgoing = anchor + toward_next * corner.radius;
    corner.rounded = corner.radius > 1e-6;
    return corner;
}

void add_corner_transition(QPainterPath &path, const CornerData &corner, CornerType type)
{
    if (!corner.rounded) {
        path.lineTo(corner.anchor);
        return;
    }
    switch (type) {
    case CornerType::Straight:
        path.lineTo(corner.outgoing);
        break;
    case CornerType::Concave: {
        const QPointF bisector = normalized_vector((corner.previous - corner.anchor) +
                                                    (corner.next - corner.anchor));
        const QPointF notch = corner.anchor + bisector * corner.radius;
        path.quadTo(notch, corner.outgoing);
        break;
    }
    case CornerType::Cutout: {
        const QPointF bisector = normalized_vector((corner.previous - corner.anchor) +
                                                    (corner.next - corner.anchor));
        const QPointF notch = corner.anchor + bisector * corner.radius;
        path.lineTo(notch);
        path.lineTo(corner.outgoing);
        break;
    }
    case CornerType::Round:
    default:
        path.quadTo(corner.anchor, corner.outgoing);
        break;
    }
}

QPainterPath rounded_vertices_path(const std::vector<QPointF> &vertices,
                                   const std::vector<double> &requested_radii,
                                   CornerType type)
{
    QPainterPath path;
    if (vertices.size() < 3)
        return path;
    std::vector<CornerData> corners;
    corners.reserve(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        const size_t previous = (i + vertices.size() - 1) % vertices.size();
        const size_t next = (i + 1) % vertices.size();
        const double requested = i < requested_radii.size() ? requested_radii[i] : 0.0;
        corners.push_back(make_corner(vertices[previous], vertices[i], vertices[next], requested, true));
    }

    path.moveTo(corners.front().outgoing);
    for (size_t i = 0; i < corners.size(); ++i) {
        const size_t next = (i + 1) % corners.size();
        path.lineTo(corners[next].incoming);
        add_corner_transition(path, corners[next], type);
    }
    path.closeSubpath();
    return path;
}

QPainterPath primitive_shape_path(const Layer &layer, const QRectF &rect)
{
    QPainterPath path;
    switch (layer.shape_type) {
    case ShapeType::Ellipse:
        path.addEllipse(rect);
        return path;
    case ShapeType::Line:
        path.moveTo(rect.left(), rect.center().y());
        path.lineTo(rect.right(), rect.center().y());
        return path;
    case ShapeType::Rectangle:
    case ShapeType::RoundedRectangle:
    case ShapeType::Triangle:
    case ShapeType::Star:
    case ShapeType::Polygon:
    case ShapeType::Diamond: {
        const std::vector<QPointF> vertices = primitive_vertices(layer, rect);
        return rounded_vertices_path(vertices, primitive_corner_radii(layer, vertices.size()), layer.corner_type);
    }
    case ShapeType::Path:
        return path;
    }
    return path;
}

std::vector<BezierPathPoint> painter_path_to_points(const QPainterPath &path,
                                                    const QRectF &rect,
                                                    bool closed)
{
    std::vector<BezierPathPoint> points;
    if (path.elementCount() == 0)
        return points;

    QPointF current;
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element element = path.elementAt(i);
        const QPointF point(element.x, element.y);
        if (element.isMoveTo()) {
            points.push_back(normalized_point(point, rect));
            current = point;
            continue;
        }
        if (element.isLineTo()) {
            if (closed && !points.empty() && same_point(point, QPointF(path.elementAt(0).x, path.elementAt(0).y)))
                continue;
            points.push_back(normalized_point(point, rect));
            current = point;
            continue;
        }
        if (element.type == QPainterPath::CurveToElement && i + 2 < path.elementCount()) {
            const QPointF control1(element.x, element.y);
            const QPainterPath::Element control2_element = path.elementAt(i + 1);
            const QPainterPath::Element end_element = path.elementAt(i + 2);
            const QPointF control2(control2_element.x, control2_element.y);
            const QPointF end(end_element.x, end_element.y);
            if (points.empty())
                points.push_back(normalized_point(current, rect));

            BezierPathPoint &start = points.back();
            const BezierPathPoint normalized_control1 = normalized_point(control1, rect);
            start.has_out = !same_point(current, control1);
            start.out_x = normalized_control1.x;
            start.out_y = normalized_control1.y;

            const bool closes_to_first = closed && !points.empty() &&
                same_point(end, QPointF(path.elementAt(0).x, path.elementAt(0).y));
            BezierPathPoint *destination = nullptr;
            if (closes_to_first) {
                destination = &points.front();
            } else {
                points.push_back(normalized_point(end, rect));
                destination = &points.back();
            }
            const BezierPathPoint normalized_control2 = normalized_point(control2, rect);
            destination->has_in = !same_point(control2, end);
            destination->in_x = normalized_control2.x;
            destination->in_y = normalized_control2.y;
            current = end;
            i += 2;
        }
    }

    for (BezierPathPoint &point : points) {
        if (!point.has_in) {
            point.in_x = point.x;
            point.in_y = point.y;
        }
        if (!point.has_out) {
            point.out_x = point.x;
            point.out_y = point.y;
        }
        if (point.has_in && point.has_out) {
            const QPointF anchor(point.x, point.y);
            const QPointF incoming(point.in_x, point.in_y);
            const QPointF outgoing(point.out_x, point.out_y);
            const QPointF a = incoming - anchor;
            const QPointF b = outgoing - anchor;
            const double cross = std::abs(a.x() * b.y() - a.y() * b.x());
            point.smooth = cross <= 1e-5 * std::max(1.0, QLineF(anchor, incoming).length() +
                                                        QLineF(anchor, outgoing).length());
        }
    }
    return points;
}

struct CubicSegment {
    QPointF p0;
    QPointF p1;
    QPointF p2;
    QPointF p3;
    bool curved = false;
};

struct EditableCornerData {
    CornerData corner;
    bool eligible = false;
    double incoming_t = 1.0;
    double outgoing_t = 0.0;
};

QPointF lerp_point(const QPointF &a, const QPointF &b, double t)
{
    return a + (b - a) * t;
}

QPointF cubic_point(const CubicSegment &segment, double t)
{
    const double u = 1.0 - t;
    return segment.p0 * (u * u * u) +
           segment.p1 * (3.0 * u * u * t) +
           segment.p2 * (3.0 * u * t * t) +
           segment.p3 * (t * t * t);
}

void split_cubic(const CubicSegment &segment, double t,
                 CubicSegment &left, CubicSegment &right)
{
    t = std::clamp(t, 0.0, 1.0);
    const QPointF p01 = lerp_point(segment.p0, segment.p1, t);
    const QPointF p12 = lerp_point(segment.p1, segment.p2, t);
    const QPointF p23 = lerp_point(segment.p2, segment.p3, t);
    const QPointF p012 = lerp_point(p01, p12, t);
    const QPointF p123 = lerp_point(p12, p23, t);
    const QPointF p0123 = lerp_point(p012, p123, t);

    left = {segment.p0, p01, p012, p0123, segment.curved};
    right = {p0123, p123, p23, segment.p3, segment.curved};
}

CubicSegment cubic_subsegment(const CubicSegment &segment, double t0, double t1)
{
    t0 = std::clamp(t0, 0.0, 1.0);
    t1 = std::clamp(t1, t0, 1.0);
    if (t0 <= kEpsilon && t1 >= 1.0 - kEpsilon)
        return segment;

    CubicSegment before_start;
    CubicSegment from_start;
    split_cubic(segment, t0, before_start, from_start);
    if (t1 >= 1.0 - kEpsilon)
        return from_start;

    const double denominator = std::max(kEpsilon, 1.0 - t0);
    const double relative_end = std::clamp((t1 - t0) / denominator, 0.0, 1.0);
    CubicSegment selected;
    CubicSegment after_end;
    split_cubic(from_start, relative_end, selected, after_end);
    return selected;
}

double cubic_length_between(const CubicSegment &segment, double t0, double t1)
{
    t0 = std::clamp(t0, 0.0, 1.0);
    t1 = std::clamp(t1, t0, 1.0);
    if (t1 - t0 <= kEpsilon)
        return 0.0;

    constexpr int kLengthSteps = 48;
    double length = 0.0;
    QPointF previous = cubic_point(segment, t0);
    for (int i = 1; i <= kLengthSteps; ++i) {
        const double t = t0 + (t1 - t0) * ((double)i / kLengthSteps);
        const QPointF current = cubic_point(segment, t);
        length += QLineF(previous, current).length();
        previous = current;
    }
    return length;
}

double cubic_length(const CubicSegment &segment)
{
    return cubic_length_between(segment, 0.0, 1.0);
}

double cubic_parameter_at_distance(const CubicSegment &segment, double distance,
                                   bool from_start, double total)
{
    if (total <= kEpsilon)
        return from_start ? 0.0 : 1.0;

    const double target = std::clamp(from_start ? distance : total - distance, 0.0, total);
    if (target <= kEpsilon)
        return 0.0;
    if (target >= total - kEpsilon)
        return 1.0;

    constexpr int kSearchSamples = 64;
    double accumulated = 0.0;
    QPointF previous = segment.p0;
    double previous_t = 0.0;
    for (int i = 1; i <= kSearchSamples; ++i) {
        const double current_t = (double)i / kSearchSamples;
        const QPointF current = cubic_point(segment, current_t);
        const double piece = QLineF(previous, current).length();
        if (accumulated + piece >= target) {
            const double fraction = piece > kEpsilon
                ? std::clamp((target - accumulated) / piece, 0.0, 1.0)
                : 0.0;
            return previous_t + (current_t - previous_t) * fraction;
        }
        accumulated += piece;
        previous = current;
        previous_t = current_t;
    }
    return 1.0;
}

QPointF start_tangent_away(const CubicSegment &segment)
{
    QPointF tangent = segment.p1 - segment.p0;
    if (std::hypot(tangent.x(), tangent.y()) <= kEpsilon)
        tangent = segment.p2 - segment.p0;
    if (std::hypot(tangent.x(), tangent.y()) <= kEpsilon)
        tangent = segment.p3 - segment.p0;
    return normalized_vector(tangent);
}

QPointF end_tangent_away(const CubicSegment &segment)
{
    QPointF tangent = segment.p2 - segment.p3;
    if (std::hypot(tangent.x(), tangent.y()) <= kEpsilon)
        tangent = segment.p1 - segment.p3;
    if (std::hypot(tangent.x(), tangent.y()) <= kEpsilon)
        tangent = segment.p0 - segment.p3;
    return normalized_vector(tangent);
}

std::vector<CubicSegment> editable_path_segments(const Layer &layer, const QRectF &rect)
{
    std::vector<CubicSegment> segments;
    const size_t point_count = layer.path_points.size();
    if (point_count < 2)
        return segments;

    const size_t segment_count = layer.path_closed ? point_count : point_count - 1;
    segments.reserve(segment_count);
    for (size_t i = 0; i < segment_count; ++i) {
        const size_t next = (i + 1) % point_count;
        const BezierPathPoint &start = layer.path_points[i];
        const BezierPathPoint &end = layer.path_points[next];
        const QPointF p0 = path_point_to_local(start, rect);
        const QPointF p3 = path_point_to_local(end, rect);
        const QPointF p1 = start.has_out ? path_out_handle_to_local(start, rect) : p0;
        const QPointF p2 = end.has_in ? path_in_handle_to_local(end, rect) : p3;
        segments.push_back({p0, p1, p2, p3, start.has_out || end.has_in});
    }
    return segments;
}

std::vector<EditableCornerData> editable_path_corner_data(const Layer &layer,
                                                           const QRectF &rect)
{
    std::vector<EditableCornerData> corners;
    const size_t point_count = layer.path_points.size();
    if (point_count < 2)
        return corners;

    const std::vector<CubicSegment> segments = editable_path_segments(layer, rect);
    if (segments.empty())
        return corners;

    std::vector<double> segment_lengths;
    segment_lengths.reserve(segments.size());
    for (const CubicSegment &segment : segments)
        segment_lengths.push_back(cubic_length(segment));

    corners.resize(point_count);
    constexpr double kCornerAngleEpsilon = 0.5 * kPi / 180.0;
    const double max_collinear_dot = std::cos(kCornerAngleEpsilon);

    for (size_t i = 0; i < point_count; ++i) {
        const QPointF anchor = path_point_to_local(layer.path_points[i], rect);
        EditableCornerData &data = corners[i];
        data.corner.anchor = anchor;
        data.corner.previous = anchor;
        data.corner.next = anchor;
        data.corner.incoming = anchor;
        data.corner.outgoing = anchor;
        data.corner.requested = std::max(0.0, layer.path_points[i].corner_radius);

        const bool endpoint = !layer.path_closed && (i == 0 || i + 1 == point_count);
        if (endpoint)
            continue;

        const size_t previous_segment_index = i == 0 ? segments.size() - 1 : i - 1;
        const size_t next_segment_index = i % segments.size();
        const CubicSegment &previous_segment = segments[previous_segment_index];
        const CubicSegment &next_segment = segments[next_segment_index];
        const QPointF toward_previous = end_tangent_away(previous_segment);
        const QPointF toward_next = start_tangent_away(next_segment);
        const double previous_tangent_length = std::hypot(toward_previous.x(), toward_previous.y());
        const double next_tangent_length = std::hypot(toward_next.x(), toward_next.y());
        const double previous_length = segment_lengths[previous_segment_index];
        const double next_length = segment_lengths[next_segment_index];
        if (previous_tangent_length <= kEpsilon || next_tangent_length <= kEpsilon ||
            previous_length <= kEpsilon || next_length <= kEpsilon)
            continue;

        const double dot = std::clamp(QPointF::dotProduct(toward_previous, toward_next), -1.0, 1.0);
        /* Illustrator exposes Live Corners on geometric corner anchors, even
         * when one or both adjacent path segments are cubic. Smooth tangencies
         * (180 degrees) and folded zero-angle degeneracies are intentionally
         * omitted because there is no stable corner wedge for a widget. */
        const bool has_corner_angle = dot > -max_collinear_dot && dot < max_collinear_dot;
        if (!has_corner_angle)
            continue;

        data.eligible = true;
        data.corner.previous = anchor + toward_previous * previous_length;
        data.corner.next = anchor + toward_next * next_length;
        data.corner.max_radius = std::max(0.0, std::min(previous_length, next_length) * 0.5);
        data.corner.radius = std::clamp(data.corner.requested, 0.0, data.corner.max_radius);
        data.corner.rounded = data.corner.radius > 1e-6;
        if (data.corner.rounded) {
            data.incoming_t = cubic_parameter_at_distance(previous_segment,
                                                           data.corner.radius, false,
                                                           previous_length);
            data.outgoing_t = cubic_parameter_at_distance(next_segment,
                                                           data.corner.radius, true,
                                                           next_length);
            data.corner.incoming = cubic_point(previous_segment, data.incoming_t);
            data.corner.outgoing = cubic_point(next_segment, data.outgoing_t);
        }
    }
    return corners;
}

void append_trimmed_segment(QPainterPath &path, const CubicSegment &segment,
                            double t0, double t1)
{
    t0 = std::clamp(t0, 0.0, 1.0);
    t1 = std::clamp(t1, t0, 1.0);
    if (t1 - t0 <= kEpsilon)
        return;

    const CubicSegment trimmed = cubic_subsegment(segment, t0, t1);
    if (segment.curved)
        path.cubicTo(trimmed.p1, trimmed.p2, trimmed.p3);
    else
        path.lineTo(trimmed.p3);
}

QPainterPath editable_path(const Layer &layer, const QRectF &rect)
{
    QPainterPath path;
    const auto &points = layer.path_points;
    if (points.empty())
        return path;

    const std::vector<CubicSegment> segments = editable_path_segments(layer, rect);
    const std::vector<EditableCornerData> corners = editable_path_corner_data(layer, rect);
    if (segments.empty() || corners.size() != points.size())
        return path;

    if (!layer.path_closed) {
        path.moveTo(path_point_to_local(points.front(), rect));
        for (size_t i = 0; i < segments.size(); ++i) {
            const size_t next = i + 1;
            const double start_t = corners[i].corner.rounded ? corners[i].outgoing_t : 0.0;
            const double end_t = corners[next].corner.rounded ? corners[next].incoming_t : 1.0;
            append_trimmed_segment(path, segments[i], start_t, end_t);
            if (corners[next].corner.rounded)
                add_corner_transition(path, corners[next].corner, layer.corner_type);
        }
        return path;
    }

    path.moveTo(corners.front().corner.rounded
                    ? corners.front().corner.outgoing
                    : path_point_to_local(points.front(), rect));
    for (size_t i = 0; i < segments.size(); ++i) {
        const size_t next = (i + 1) % points.size();
        const double start_t = corners[i].corner.rounded ? corners[i].outgoing_t : 0.0;
        const double end_t = corners[next].corner.rounded ? corners[next].incoming_t : 1.0;
        append_trimmed_segment(path, segments[i], start_t, end_t);
        if (corners[next].corner.rounded)
            add_corner_transition(path, corners[next].corner, layer.corner_type);
    }
    path.closeSubpath();
    return path;
}

} // namespace

QPointF path_point_to_local(const BezierPathPoint &point, const QRectF &rect)
{
    return normalized_to_local(point.x, point.y, rect);
}

QPointF path_in_handle_to_local(const BezierPathPoint &point, const QRectF &rect)
{
    return normalized_to_local(point.in_x, point.in_y, rect);
}

QPointF path_out_handle_to_local(const BezierPathPoint &point, const QRectF &rect)
{
    return normalized_to_local(point.out_x, point.out_y, rect);
}

QPainterPath layer_shape_path(const Layer &layer, const QRectF &rect)
{
    if (layer.shape_type != ShapeType::Path || layer.path_points.empty())
        return primitive_shape_path(layer, rect);
    return editable_path(layer, rect);
}

std::vector<LiveCornerGeometry> layer_live_corners(const Layer &layer, const QRectF &rect)
{
    std::vector<LiveCornerGeometry> result;
    if (layer.shape_type == ShapeType::Ellipse || layer.shape_type == ShapeType::Line)
        return result;

    if (layer.shape_type != ShapeType::Path) {
        const std::vector<QPointF> vertices = primitive_vertices(layer, rect);
        const std::vector<double> radii = primitive_corner_radii(layer, vertices.size());
        result.reserve(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            const size_t previous = (i + vertices.size() - 1) % vertices.size();
            const size_t next = (i + 1) % vertices.size();
            const CornerData corner = make_corner(vertices[previous], vertices[i], vertices[next],
                                                  i < radii.size() ? radii[i] : 0.0, true);
            result.push_back({(int)i, corner.previous, corner.anchor, corner.next,
                              corner.radius, corner.max_radius});
        }
        return result;
    }

    const std::vector<EditableCornerData> corners = editable_path_corner_data(layer, rect);
    for (size_t i = 0; i < corners.size(); ++i) {
        if (!corners[i].eligible)
            continue;
        const CornerData &corner = corners[i].corner;
        result.push_back({(int)i, corner.previous, corner.anchor, corner.next,
                          corner.radius, corner.max_radius});
    }
    return result;
}

bool ensure_editable_path(Layer &layer)
{
    if (layer.type != LayerType::Shape)
        return false;
    if (layer.shape_type == ShapeType::Path)
        return layer.path_points.size() >= 2;

    const QRectF rect(0.0, 0.0, std::max(1.0f, layer.rect_width),
                      std::max(1.0f, layer.rect_height));
    const ShapeType source_type = layer.shape_type;
    const bool closed = source_type != ShapeType::Line;
    std::vector<BezierPathPoint> points;

    const std::vector<QPointF> vertices = primitive_vertices(layer, rect);
    if (!vertices.empty()) {
        const std::vector<double> radii = primitive_corner_radii(layer, vertices.size());
        points.reserve(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
            BezierPathPoint point = normalized_point(vertices[i], rect);
            point.corner_radius = i < radii.size() ? radii[i] : 0.0;
            points.push_back(point);
        }
    } else {
        points = painter_path_to_points(primitive_shape_path(layer, rect), rect, closed);
    }
    if (points.size() < 2)
        return false;

    layer.path_points = std::move(points);
    layer.path_closed = closed;
    layer.shape_type = ShapeType::Path;
    return true;
}

QString path_geometry_signature(const Layer &layer)
{
    if (layer.shape_type != ShapeType::Path)
        return QString::number((int)layer.shape_type);

    QByteArray bytes;
    bytes.reserve((int)layer.path_points.size() * 96 + 8);
    bytes.append(layer.path_closed ? '1' : '0');
    for (const BezierPathPoint &point : layer.path_points) {
        const QString row = QStringLiteral("|%1,%2,%3,%4,%5,%6,%7,%8,%9,%10")
            .arg(point.x, 0, 'g', 12)
            .arg(point.y, 0, 'g', 12)
            .arg(point.in_x, 0, 'g', 12)
            .arg(point.in_y, 0, 'g', 12)
            .arg(point.out_x, 0, 'g', 12)
            .arg(point.out_y, 0, 'g', 12)
            .arg(point.has_in ? 1 : 0)
            .arg(point.has_out ? 1 : 0)
            .arg(point.smooth ? 1 : 0)
            .arg(point.corner_radius, 0, 'g', 12);
        bytes.append(row.toUtf8());
    }
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex());
}

} // namespace gsp
