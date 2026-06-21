#pragma once

#include "layer-model.h"

#include <QPainterPath>
#include <QRectF>
#include <QString>

#include <vector>

namespace gsp {

struct LiveCornerGeometry {
    int point_index = -1;
    QPointF previous;
    QPointF anchor;
    QPointF next;
    double radius = 0.0;
    double max_radius = 0.0;
};

/* Build a drawable path for either a parametric primitive or a Pen-tool path.
 * The supplied rectangle is the layer's local editable box. */
QPainterPath layer_shape_path(const Layer &layer, const QRectF &rect);

/* Return editable geometric corners in layer-local coordinates. Cubic
 * free-form segments are supported; open endpoints and truly smooth tangent
 * anchors are intentionally omitted. */
std::vector<LiveCornerGeometry> layer_live_corners(const Layer &layer, const QRectF &rect);

/* Convert a primitive into an equivalent editable cubic Bézier path. Existing
 * Pen paths are left untouched. Returns true when the layer can be edited. */
bool ensure_editable_path(Layer &layer);

/* Coordinate helpers shared by the renderer and Direct Selection tool. */
QPointF path_point_to_local(const BezierPathPoint &point, const QRectF &rect);
QPointF path_in_handle_to_local(const BezierPathPoint &point, const QRectF &rect);
QPointF path_out_handle_to_local(const BezierPathPoint &point, const QRectF &rect);

/* Stable, compact identity used by cached shadows and other geometry caches. */
QString path_geometry_signature(const Layer &layer);

} // namespace gsp
