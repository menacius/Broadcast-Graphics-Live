#include "animation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {

constexpr double kAnimationEpsilon = 1e-10;

static bool finite_vec(const Vec2Value &value)
{
    return std::isfinite(value.x) && std::isfinite(value.y);
}

static Vec2Value add(const Vec2Value &a, const Vec2Value &b)
{
    return {a.x + b.x, a.y + b.y};
}

static Vec2Value subtract(const Vec2Value &a, const Vec2Value &b)
{
    return {a.x - b.x, a.y - b.y};
}

static Vec2Value multiply(const Vec2Value &value, double scalar)
{
    return {value.x * scalar, value.y * scalar};
}

static Vec2Value lerp(const Vec2Value &a, const Vec2Value &b, double progress)
{
    return {
        a.x + (b.x - a.x) * progress,
        a.y + (b.y - a.y) * progress,
    };
}

static double length(const Vec2Value &value)
{
    return std::hypot(value.x, value.y);
}

static Vec2Value cubic_bezier(const Vec2Value &p0, const Vec2Value &p1,
                              const Vec2Value &p2, const Vec2Value &p3,
                              double progress)
{
    const double u = 1.0 - progress;
    const double b0 = u * u * u;
    const double b1 = 3.0 * u * u * progress;
    const double b2 = 3.0 * u * progress * progress;
    const double b3 = progress * progress * progress;
    return {
        b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x,
        b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y,
    };
}

static double cubic_scalar(double p0, double p1, double p2, double p3, double u)
{
    const double inv = 1.0 - u;
    return inv * inv * inv * p0 + 3.0 * inv * inv * u * p1 +
           3.0 * inv * u * u * p2 + u * u * u * p3;
}

static double cubic_scalar_derivative(double p0, double p1, double p2,
                                      double p3, double u)
{
    const double inv = 1.0 - u;
    return 3.0 * inv * inv * (p1 - p0) +
           6.0 * inv * u * (p2 - p1) +
           3.0 * u * u * (p3 - p2);
}

static double legacy_bezier_y(double x, double cx1, double cy1,
                              double cx2, double cy2)
{
    x = std::clamp(x, 0.0, 1.0);
    cx1 = std::clamp(cx1, 0.0, 1.0);
    cx2 = std::clamp(cx2, 0.0, 1.0);
    double parameter = x;
    for (int i = 0; i < 8; ++i) {
        const double dx = cubic_scalar(0.0, cx1, cx2, 1.0, parameter) - x;
        const double derivative = cubic_scalar_derivative(0.0, cx1, cx2, 1.0, parameter);
        if (std::abs(dx) < 1e-7 || std::abs(derivative) < 1e-9) break;
        parameter = std::clamp(parameter - dx / derivative, 0.0, 1.0);
    }
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 24; ++i) {
        const double bx = cubic_scalar(0.0, cx1, cx2, 1.0, parameter);
        if (std::abs(bx - x) < 1e-9) break;
        if (bx < x) lo = parameter; else hi = parameter;
        parameter = 0.5 * (lo + hi);
    }
    return cubic_scalar(0.0, cy1, cy2, 1.0, parameter);
}

static double legacy_ease_value(double x, EasingType easing,
                                float cx1, float cy1, float cx2, float cy2)
{
    x = std::clamp(x, 0.0, 1.0);
    switch (easing) {
    case EasingType::EaseIn: return x * x;
    case EasingType::EaseOut: return x * (2.0 - x);
    case EasingType::EaseInOut:
        return x < 0.5 ? 2.0 * x * x : -1.0 + (4.0 - 2.0 * x) * x;
    case EasingType::Bezier:
        return legacy_bezier_y(x, cx1, cy1, cx2, cy2);
    case EasingType::Hold: return 0.0;
    default: return x;
    }
}

static void normalized_influences(const TemporalBezierSegment &segment,
                                  double *outgoing, double *incoming)
{
    double out = std::clamp(std::isfinite(segment.outgoing_influence)
                                ? segment.outgoing_influence : 1.0 / 3.0,
                            0.0, 1.0);
    double in = std::clamp(std::isfinite(segment.incoming_influence)
                               ? segment.incoming_influence : 1.0 / 3.0,
                           0.0, 1.0);
    /* Cubic temporal control x coordinates remain a valid single-valued
     * easing function when each influence is in [0, 1], even when the two
     * handles cross in graph space. Keep authored handle lengths unchanged so
     * the Value/Speed Graph control points exactly match final evaluation. */
    *outgoing = out;
    *incoming = in;
}

static double temporal_parameter_for_time(const TemporalBezierSegment &segment,
                                          double time)
{
    const double span = segment.end_time - segment.start_time;
    if (!(span > kAnimationEpsilon)) return 0.0;
    const double x = std::clamp((time - segment.start_time) / span, 0.0, 1.0);
    double out = 0.0, in = 0.0;
    normalized_influences(segment, &out, &in);
    const double x1 = out;
    const double x2 = 1.0 - in;
    double u = x;
    for (int i = 0; i < 8; ++i) {
        const double dx = cubic_scalar(0.0, x1, x2, 1.0, u) - x;
        const double slope = cubic_scalar_derivative(0.0, x1, x2, 1.0, u);
        if (std::abs(dx) < 1e-9 || std::abs(slope) < 1e-10) break;
        u = std::clamp(u - dx / slope, 0.0, 1.0);
    }
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 32; ++i) {
        const double bx = cubic_scalar(0.0, x1, x2, 1.0, u);
        if (std::abs(bx - x) < 1e-11) break;
        if (bx < x) lo = u; else hi = u;
        u = 0.5 * (lo + hi);
    }
    return u;
}

static double finite_or_zero(double value)
{
    return std::isfinite(value) ? value : 0.0;
}

} // namespace

double evaluate_temporal_segment(const TemporalBezierSegment &segment, double time)
{
    const double span = segment.end_time - segment.start_time;
    if (!(span > kAnimationEpsilon))
        return time < segment.end_time ? segment.start_value : segment.end_value;
    if (time <= segment.start_time) return segment.start_value;
    if (time >= segment.end_time) return segment.end_value;
    if (segment.hold) return segment.start_value;
    const double x = (time - segment.start_time) / span;
    if (segment.legacy) {
        const double mix = legacy_ease_value(x, segment.legacy_easing,
                                             segment.legacy_cx1, segment.legacy_cy1,
                                             segment.legacy_cx2, segment.legacy_cy2);
        return segment.start_value +
               (segment.end_value - segment.start_value) * mix;
    }
    double out = 0.0, in = 0.0;
    normalized_influences(segment, &out, &in);
    const double y1 = segment.start_value +
        finite_or_zero(segment.outgoing_speed) * span * out;
    const double y2 = segment.end_value -
        finite_or_zero(segment.incoming_speed) * span * in;
    const double u = temporal_parameter_for_time(segment, time);
    return cubic_scalar(segment.start_value, y1, y2, segment.end_value, u);
}

double evaluate_temporal_segment_velocity(const TemporalBezierSegment &segment,
                                          double time)
{
    const double span = segment.end_time - segment.start_time;
    if (!(span > kAnimationEpsilon) || segment.hold) return 0.0;
    if (segment.legacy) {
        const double h = std::max(span * 1e-6, 1e-9);
        const double a = std::max(segment.start_time, time - h);
        const double b = std::min(segment.end_time, time + h);
        if (!(b > a)) return 0.0;
        return (evaluate_temporal_segment(segment, b) -
                evaluate_temporal_segment(segment, a)) / (b - a);
    }
    double out = 0.0, in = 0.0;
    normalized_influences(segment, &out, &in);
    const double x1 = out;
    const double x2 = 1.0 - in;
    const double y1 = segment.start_value +
        finite_or_zero(segment.outgoing_speed) * span * out;
    const double y2 = segment.end_value -
        finite_or_zero(segment.incoming_speed) * span * in;
    const double u = temporal_parameter_for_time(segment,
        std::clamp(time, segment.start_time, segment.end_time));
    const double dx_du = span * cubic_scalar_derivative(0.0, x1, x2, 1.0, u);
    const double dy_du = cubic_scalar_derivative(segment.start_value, y1, y2,
                                                 segment.end_value, u);
    if (std::abs(dx_du) < 1e-12) return 0.0;
    return dy_du / dx_du;
}

/* ══════════════════════════════════════════════════════════════════
 *  AnimatedProperty::evaluate
 * ══════════════════════════════════════════════════════════════════ */
double AnimatedProperty::automatic_temporal_speed(size_t keyframe_index) const
{
    if (keyframe_index >= keyframes.size() || keyframes.size() < 2) return 0.0;
    if (keyframe_index == 0) {
        const double dt = keyframes[1].time - keyframes[0].time;
        return dt > kAnimationEpsilon
            ? (keyframes[1].value - keyframes[0].value) / dt : 0.0;
    }
    if (keyframe_index + 1 >= keyframes.size()) {
        const double dt = keyframes[keyframe_index].time - keyframes[keyframe_index - 1].time;
        return dt > kAnimationEpsilon
            ? (keyframes[keyframe_index].value - keyframes[keyframe_index - 1].value) / dt : 0.0;
    }
    const double dt = keyframes[keyframe_index + 1].time - keyframes[keyframe_index - 1].time;
    return dt > kAnimationEpsilon
        ? (keyframes[keyframe_index + 1].value - keyframes[keyframe_index - 1].value) / dt : 0.0;
}

TemporalBezierSegment AnimatedProperty::temporal_segment(size_t segment_index) const
{
    TemporalBezierSegment segment;
    if (segment_index + 1 >= keyframes.size()) return segment;
    const Keyframe &left = keyframes[segment_index];
    const Keyframe &right = keyframes[segment_index + 1];
    segment.start_time = left.time;
    segment.end_time = right.time;
    segment.start_value = left.value;
    segment.end_value = right.value;

    if (!left.temporal_velocity_explicit && !right.temporal_velocity_explicit) {
        segment.legacy = true;
        segment.hold = left.easing == EasingType::Hold;
        segment.legacy_easing = left.easing;
        segment.legacy_cx1 = left.cx1;
        segment.legacy_cy1 = left.cy1;
        segment.legacy_cx2 = left.cx2;
        segment.legacy_cy2 = left.cy2;
        return segment;
    }

    const double span = right.time - left.time;
    const double linear_speed = span > kAnimationEpsilon
        ? (right.value - left.value) / span : 0.0;
    auto speed_for = [&](size_t index, bool incoming) {
        const Keyframe &key = keyframes[index];
        if (!key.temporal_velocity_explicit) {
            if (left.easing == EasingType::Bezier) {
                if (incoming) {
                    const double influence = std::max(1e-9, 1.0 - (double)left.cx2);
                    return (right.value - left.value) * (1.0 - (double)left.cy2) /
                           (std::max(span, kAnimationEpsilon) * influence);
                }
                const double influence = std::max(1e-9, (double)left.cx1);
                return (right.value - left.value) * (double)left.cy1 /
                       (std::max(span, kAnimationEpsilon) * influence);
            }
            return linear_speed;
        }
        switch (key.temporal_mode) {
        case TemporalInterpolationMode::Linear: return linear_speed;
        case TemporalInterpolationMode::AutoBezier:
            return automatic_temporal_speed(index);
        case TemporalInterpolationMode::Hold: return 0.0;
        case TemporalInterpolationMode::ContinuousBezier:
        case TemporalInterpolationMode::ManualBezier:
        default:
            return finite_or_zero(incoming ? key.incoming_speed : key.outgoing_speed);
        }
    };
    auto influence_for = [&](const Keyframe &key, bool incoming) {
        if (!key.temporal_velocity_explicit && left.easing == EasingType::Bezier)
            return incoming ? 100.0 * (1.0 - (double)left.cx2)
                            : 100.0 * (double)left.cx1;
        if (!key.temporal_velocity_explicit ||
            key.temporal_mode == TemporalInterpolationMode::Linear ||
            key.temporal_mode == TemporalInterpolationMode::AutoBezier)
            return 33.3333333333;
        return incoming ? key.incoming_influence : key.outgoing_influence;
    };

    segment.hold = left.temporal_velocity_explicit
        ? left.temporal_mode == TemporalInterpolationMode::Hold
        : left.easing == EasingType::Hold;
    segment.outgoing_influence = influence_for(left, false) / 100.0;
    segment.incoming_influence = influence_for(right, true) / 100.0;
    segment.outgoing_speed = speed_for(segment_index, false);
    segment.incoming_speed = speed_for(segment_index + 1, true);
    return segment;
}

double AnimatedProperty::evaluate(double t) const
{
    if (!std::isfinite(t)) return static_value;
    if (keyframes.empty()) return static_value;
    if (keyframes.size() == 1) return keyframes.front().value;
    if (t <= keyframes.front().time) return keyframes.front().value;
    if (t >= keyframes.back().time) return keyframes.back().value;
    for (size_t i = 0; i + 1 < keyframes.size(); ++i)
        if (t >= keyframes[i].time && t <= keyframes[i + 1].time)
            return evaluate_temporal_segment(temporal_segment(i), t);
    return keyframes.back().value;
}

double AnimatedProperty::velocity(double t) const
{
    if (keyframes.size() < 2 || !std::isfinite(t)) return 0.0;
    if (t <= keyframes.front().time) t = keyframes.front().time;
    if (t >= keyframes.back().time) t = keyframes.back().time;
    for (size_t i = 0; i + 1 < keyframes.size(); ++i)
        if (t >= keyframes[i].time && t <= keyframes[i + 1].time)
            return evaluate_temporal_segment_velocity(temporal_segment(i), t);
    return 0.0;
}

void AnimatedProperty::set_temporal_mode(size_t keyframe_index,
                                         TemporalInterpolationMode mode)
{
    if (keyframe_index >= keyframes.size()) return;
    Keyframe &key = keyframes[keyframe_index];
    if (keyframe_index > 0) {
        const TemporalBezierSegment before = temporal_segment(keyframe_index - 1);
        key.incoming_influence = before.incoming_influence * 100.0;
        key.incoming_speed = before.incoming_speed;
    }
    if (keyframe_index + 1 < keyframes.size()) {
        const TemporalBezierSegment after = temporal_segment(keyframe_index);
        key.outgoing_influence = after.outgoing_influence * 100.0;
        key.outgoing_speed = after.outgoing_speed;
    }
    key.temporal_velocity_explicit = true;
    key.temporal_mode = mode;
    key.temporal_tangents_linked = mode == TemporalInterpolationMode::AutoBezier ||
                                   mode == TemporalInterpolationMode::ContinuousBezier;
    key.easing = mode == TemporalInterpolationMode::Hold ? EasingType::Hold
               : mode == TemporalInterpolationMode::Linear ? EasingType::Linear
               : EasingType::Bezier;
    if (mode == TemporalInterpolationMode::AutoBezier) {
        key.incoming_speed = key.outgoing_speed = automatic_temporal_speed(keyframe_index);
        key.incoming_influence = key.outgoing_influence = 33.3333333333;
    } else if (mode == TemporalInterpolationMode::ContinuousBezier) {
        const double common = 0.5 * (key.incoming_speed + key.outgoing_speed);
        key.incoming_speed = key.outgoing_speed = common;
    }
}

void AnimatedProperty::set_temporal_handle(size_t keyframe_index, bool incoming,
                                           double influence_percent, double speed,
                                           bool preserve_opposite)
{
    if (keyframe_index >= keyframes.size()) return;
    const bool was_linked = keyframes[keyframe_index].temporal_tangents_linked;
    set_temporal_mode(keyframe_index, TemporalInterpolationMode::ManualBezier);
    Keyframe &key = keyframes[keyframe_index];
    key.temporal_tangents_linked = was_linked && !preserve_opposite;
    influence_percent = std::clamp(std::isfinite(influence_percent)
                                       ? influence_percent : 33.3333333333,
                                   0.0, 100.0);
    speed = finite_or_zero(speed);
    if (incoming) {
        key.incoming_influence = influence_percent;
        key.incoming_speed = speed;
        if (key.temporal_tangents_linked && !preserve_opposite) {
            key.outgoing_influence = influence_percent;
            key.outgoing_speed = speed;
        }
    } else {
        key.outgoing_influence = influence_percent;
        key.outgoing_speed = speed;
        if (key.temporal_tangents_linked && !preserve_opposite) {
            key.incoming_influence = influence_percent;
            key.incoming_speed = speed;
        }
    }
    if (preserve_opposite) key.temporal_tangents_linked = false;
}

void AnimatedProperty::apply_easy_ease(size_t keyframe_index,
                                       bool ease_in, bool ease_out)
{
    if (keyframe_index >= keyframes.size()) return;
    set_temporal_mode(keyframe_index, TemporalInterpolationMode::ManualBezier);
    Keyframe &key = keyframes[keyframe_index];
    if (ease_in) { key.incoming_influence = 33.3333333333; key.incoming_speed = 0.0; }
    if (ease_out) { key.outgoing_influence = 33.3333333333; key.outgoing_speed = 0.0; }
    key.temporal_tangents_linked = ease_in && ease_out;
    key.easing = ease_in && ease_out ? EasingType::EaseInOut
               : ease_in ? EasingType::EaseIn : EasingType::EaseOut;
}

Vec2Value AnimatedVec2Property::automatic_tangent(size_t keyframe_index,
                                                   bool incoming) const
{
    if (keyframe_index >= keyframes.size() || keyframes.size() < 2)
        return {};

    const Vec2Value current = keyframes[keyframe_index].value;
    Vec2Value direction;
    if (keyframe_index == 0) {
        direction = subtract(keyframes[1].value, current);
    } else if (keyframe_index + 1 >= keyframes.size()) {
        direction = subtract(current, keyframes[keyframe_index - 1].value);
    } else {
        direction = multiply(
            subtract(keyframes[keyframe_index + 1].value,
                     keyframes[keyframe_index - 1].value),
            0.5);
    }

    /* One third of the local secant gives deterministic C1 cubic controls.
     * Endpoints use the adjacent segment; interior keys use the centred
     * Catmull-Rom derivative. */
    const Vec2Value tangent = multiply(direction, 1.0 / 3.0);
    return incoming ? multiply(tangent, -1.0) : tangent;
}

Vec2Value AnimatedVec2Property::resolved_incoming_tangent(size_t keyframe_index) const
{
    if (keyframe_index >= keyframes.size()) return {};
    const VectorKeyframe &keyframe = keyframes[keyframe_index];
    if (keyframe.spatial_mode == SpatialInterpolationMode::AutoBezier)
        return automatic_tangent(keyframe_index, true);
    if (keyframe.spatial_mode == SpatialInterpolationMode::Linear)
        return {};
    return finite_vec(keyframe.incoming_tangent) ? keyframe.incoming_tangent : Vec2Value{};
}

Vec2Value AnimatedVec2Property::resolved_outgoing_tangent(size_t keyframe_index) const
{
    if (keyframe_index >= keyframes.size()) return {};
    const VectorKeyframe &keyframe = keyframes[keyframe_index];
    if (keyframe.spatial_mode == SpatialInterpolationMode::AutoBezier)
        return automatic_tangent(keyframe_index, false);
    if (keyframe.spatial_mode == SpatialInterpolationMode::Linear)
        return {};
    return finite_vec(keyframe.outgoing_tangent) ? keyframe.outgoing_tangent : Vec2Value{};
}

double AnimatedVec2Property::spatial_progress_for_segment(
    size_t segment_index, double temporal_progress) const
{
    if (segment_index + 1 >= keyframes.size()) return 1.0;
    const TemporalBezierSegment segment = temporal_segment(segment_index);
    const double time = segment.start_time +
        temporal_progress * (segment.end_time - segment.start_time);
    return evaluate_temporal_segment(segment, time);
}

Vec2Value AnimatedVec2Property::evaluate_spatial_segment(size_t segment_index,
                                                          double progress) const
{
    if (keyframes.empty()) return static_value;
    if (segment_index + 1 >= keyframes.size()) return keyframes.back().value;

    if (!std::isfinite(progress)) progress = 0.0;
    const VectorKeyframe &k0 = keyframes[segment_index];
    const VectorKeyframe &k1 = keyframes[segment_index + 1];

    /* Old files deserialize every position keyframe as Linear. Preserve their
     * exact historical straight-line interpolation rather than approximating
     * it with cubic control points. */
    if (k0.spatial_mode == SpatialInterpolationMode::Linear &&
        k1.spatial_mode == SpatialInterpolationMode::Linear) {
        return {
            k0.value.x + progress * (k1.value.x - k0.value.x),
            k0.value.y + progress * (k1.value.y - k0.value.y),
        };
    }

    const Vec2Value p0 = k0.value;
    const Vec2Value p1 = add(p0, resolved_outgoing_tangent(segment_index));
    const Vec2Value p3 = k1.value;
    const Vec2Value p2 = add(p3, resolved_incoming_tangent(segment_index + 1));
    return cubic_bezier(p0, p1, p2, p3, progress);
}

void AnimatedVec2Property::set_spatial_mode(
    size_t keyframe_index, SpatialInterpolationMode mode)
{
    if (keyframe_index >= keyframes.size())
        return;

    VectorKeyframe &keyframe = keyframes[keyframe_index];
    Vec2Value resolved_in = resolved_incoming_tangent(keyframe_index);
    Vec2Value resolved_out = resolved_outgoing_tangent(keyframe_index);
    const bool resolved_handles_empty =
        length(resolved_in) < kAnimationEpsilon &&
        length(resolved_out) < kAnimationEpsilon;
    if ((mode == SpatialInterpolationMode::ManualBezier ||
         mode == SpatialInterpolationMode::ContinuousBezier) &&
        resolved_handles_empty) {
        const SpatialInterpolationMode previous_mode = keyframe.spatial_mode;
        keyframe.spatial_mode = SpatialInterpolationMode::AutoBezier;
        resolved_in = resolved_incoming_tangent(keyframe_index);
        resolved_out = resolved_outgoing_tangent(keyframe_index);
        keyframe.spatial_mode = previous_mode;
    }

    switch (mode) {
    case SpatialInterpolationMode::Linear:
        keyframe.spatial_mode = mode;
        break;
    case SpatialInterpolationMode::AutoBezier:
        keyframe.spatial_mode = mode;
        keyframe.spatial_tangents_linked = true;
        break;
    case SpatialInterpolationMode::ContinuousBezier:
        keyframe.incoming_tangent = resolved_in;
        keyframe.outgoing_tangent = resolved_out;
        keyframe.spatial_mode = SpatialInterpolationMode::ContinuousBezier;
        set_spatial_tangents_linked(keyframe_index, true);
        break;
    case SpatialInterpolationMode::ManualBezier:
        keyframe.incoming_tangent = resolved_in;
        keyframe.outgoing_tangent = resolved_out;
        keyframe.spatial_mode = mode;
        keyframe.spatial_tangents_linked = false;
        break;
    }
    recalculate_rove_times();
}

void AnimatedVec2Property::set_spatial_tangents_linked(
    size_t keyframe_index, bool linked)
{
    if (keyframe_index >= keyframes.size())
        return;

    VectorKeyframe &keyframe = keyframes[keyframe_index];
    Vec2Value incoming = resolved_incoming_tangent(keyframe_index);
    Vec2Value outgoing = resolved_outgoing_tangent(keyframe_index);
    if (!finite_vec(incoming)) incoming = {};
    if (!finite_vec(outgoing)) outgoing = {};
    if (length(incoming) < kAnimationEpsilon &&
        length(outgoing) < kAnimationEpsilon) {
        const SpatialInterpolationMode previous_mode = keyframe.spatial_mode;
        keyframe.spatial_mode = SpatialInterpolationMode::AutoBezier;
        incoming = resolved_incoming_tangent(keyframe_index);
        outgoing = resolved_outgoing_tangent(keyframe_index);
        keyframe.spatial_mode = previous_mode;
    }

    if (linked) {
        double outgoing_length = length(outgoing);
        double incoming_length = length(incoming);
        Vec2Value direction = outgoing;
        double direction_length = outgoing_length;
        if (direction_length < kAnimationEpsilon && incoming_length >= kAnimationEpsilon) {
            direction = multiply(incoming, -1.0);
            direction_length = incoming_length;
        }
        if (direction_length < kAnimationEpsilon) {
            const SpatialInterpolationMode previous_mode = keyframe.spatial_mode;
            keyframe.spatial_mode = SpatialInterpolationMode::AutoBezier;
            incoming = resolved_incoming_tangent(keyframe_index);
            outgoing = resolved_outgoing_tangent(keyframe_index);
            keyframe.spatial_mode = previous_mode;
            outgoing_length = length(outgoing);
            incoming_length = length(incoming);
            direction = outgoing;
            direction_length = outgoing_length;
        }
        if (direction_length >= kAnimationEpsilon) {
            direction = multiply(direction, 1.0 / direction_length);
            if (outgoing_length < kAnimationEpsilon)
                outgoing_length = incoming_length;
            if (incoming_length < kAnimationEpsilon)
                incoming_length = outgoing_length;
            keyframe.outgoing_tangent = multiply(direction, outgoing_length);
            keyframe.incoming_tangent = multiply(direction, -incoming_length);
        } else {
            keyframe.incoming_tangent = {};
            keyframe.outgoing_tangent = {};
        }
    } else {
        keyframe.incoming_tangent = incoming;
        keyframe.outgoing_tangent = outgoing;
    }

    keyframe.spatial_tangents_linked = linked;
    keyframe.spatial_mode = linked
        ? SpatialInterpolationMode::ContinuousBezier
        : SpatialInterpolationMode::ManualBezier;
    recalculate_rove_times();
}

double AnimatedVec2Property::spatial_segment_length(size_t segment_index) const
{
    if (segment_index + 1 >= keyframes.size())
        return 0.0;
    constexpr int kSamples = 32;
    Vec2Value previous = evaluate_spatial_segment(segment_index, 0.0);
    double total = 0.0;
    for (int sample = 1; sample <= kSamples; ++sample) {
        const double progress = static_cast<double>(sample) / kSamples;
        const Vec2Value current = evaluate_spatial_segment(segment_index, progress);
        total += length(subtract(current, previous));
        previous = current;
    }
    return std::isfinite(total) ? total : 0.0;
}

void AnimatedVec2Property::recalculate_rove_times()
{
    if (keyframes.empty())
        return;

    keyframes.front().rove_across_time = false;
    keyframes.back().rove_across_time = false;
    if (keyframes.size() < 3)
        return;

    size_t anchor = 0;
    while (anchor + 1 < keyframes.size()) {
        size_t end = anchor + 1;
        while (end + 1 < keyframes.size() && keyframes[end].rove_across_time)
            ++end;
        if (end == anchor + 1 || !keyframes[anchor + 1].rove_across_time) {
            anchor = end;
            continue;
        }

        const double start_time = keyframes[anchor].time;
        const double end_time = keyframes[end].time;
        if (!(end_time > start_time + kAnimationEpsilon)) {
            anchor = end;
            continue;
        }

        std::vector<double> segment_lengths;
        segment_lengths.reserve(end - anchor);
        double total_length = 0.0;
        for (size_t segment = anchor; segment < end; ++segment) {
            const double segment_length = spatial_segment_length(segment);
            segment_lengths.push_back(segment_length);
            total_length += segment_length;
        }

        double accumulated = 0.0;
        for (size_t index = anchor + 1; index < end; ++index) {
            accumulated += segment_lengths[index - anchor - 1];
            double fraction;
            if (total_length > kAnimationEpsilon) {
                fraction = accumulated / total_length;
            } else {
                fraction = static_cast<double>(index - anchor) /
                    static_cast<double>(end - anchor);
            }
            keyframes[index].time = start_time +
                std::clamp(fraction, 0.0, 1.0) * (end_time - start_time);
        }
        anchor = end;
    }
}

void AnimatedVec2Property::set_rove_across_time(size_t keyframe_index,
                                                 bool enabled)
{
    if (keyframe_index >= keyframes.size())
        return;
    if (keyframe_index == 0 || keyframe_index + 1 == keyframes.size())
        enabled = false;
    keyframes[keyframe_index].rove_across_time = enabled;
    recalculate_rove_times();
}

size_t AnimatedVec2Property::split_spatial_segment(
    size_t segment_index, double temporal_progress, double spatial_progress)
{
    if (segment_index + 1 >= keyframes.size())
        return keyframes.size();
    temporal_progress = std::clamp(temporal_progress, 0.0, 1.0);
    spatial_progress = std::clamp(spatial_progress, 0.0, 1.0);
    if (temporal_progress <= 1e-5 || temporal_progress >= 1.0 - 1e-5 ||
        spatial_progress <= 1e-5 || spatial_progress >= 1.0 - 1e-5)
        return keyframes.size();

    VectorKeyframe start = keyframes[segment_index];
    VectorKeyframe end = keyframes[segment_index + 1];
    const double inserted_time = start.time +
        temporal_progress * (end.time - start.time);

    if (start.spatial_mode == SpatialInterpolationMode::Linear &&
        end.spatial_mode == SpatialInterpolationMode::Linear) {
        VectorKeyframe inserted = start;
        inserted.time = inserted_time;
        inserted.value = lerp(start.value, end.value, spatial_progress);
        inserted.spatial_mode = SpatialInterpolationMode::Linear;
        inserted.incoming_tangent = {};
        inserted.outgoing_tangent = {};
        inserted.spatial_tangents_linked = true;
        inserted.rove_across_time = false;
        keyframes.insert(keyframes.begin() + static_cast<std::ptrdiff_t>(segment_index + 1),
                         inserted);
        recalculate_rove_times();
        return segment_index + 1;
    }

    const Vec2Value p0 = start.value;
    const Vec2Value p1 = add(p0, resolved_outgoing_tangent(segment_index));
    const Vec2Value p3 = end.value;
    const Vec2Value p2 = add(p3, resolved_incoming_tangent(segment_index + 1));
    const Vec2Value q0 = lerp(p0, p1, spatial_progress);
    const Vec2Value q1 = lerp(p1, p2, spatial_progress);
    const Vec2Value q2 = lerp(p2, p3, spatial_progress);
    const Vec2Value r0 = lerp(q0, q1, spatial_progress);
    const Vec2Value r1 = lerp(q1, q2, spatial_progress);
    const Vec2Value split = lerp(r0, r1, spatial_progress);

    start.incoming_tangent = resolved_incoming_tangent(segment_index);
    start.outgoing_tangent = subtract(q0, p0);
    start.spatial_mode = SpatialInterpolationMode::ManualBezier;
    start.spatial_tangents_linked = false;

    end.incoming_tangent = subtract(q2, p3);
    end.outgoing_tangent = resolved_outgoing_tangent(segment_index + 1);
    end.spatial_mode = SpatialInterpolationMode::ManualBezier;
    end.spatial_tangents_linked = false;

    VectorKeyframe inserted = start;
    inserted.time = inserted_time;
    inserted.value = split;
    inserted.incoming_tangent = subtract(r0, split);
    inserted.outgoing_tangent = subtract(r1, split);
    inserted.spatial_mode = SpatialInterpolationMode::ContinuousBezier;
    inserted.spatial_tangents_linked = true;
    inserted.rove_across_time = false;

    keyframes[segment_index] = start;
    keyframes[segment_index + 1] = end;
    keyframes.insert(keyframes.begin() + static_cast<std::ptrdiff_t>(segment_index + 1),
                     inserted);
    recalculate_rove_times();
    return segment_index + 1;
}

double AnimatedVec2Property::automatic_temporal_speed(size_t keyframe_index) const
{
    if (keyframe_index >= keyframes.size() || keyframes.size() < 2) return 0.0;
    if (keyframe_index == 0) {
        const double dt = keyframes[1].time - keyframes[0].time;
        return dt > kAnimationEpsilon
            ? spatial_segment_length(0) / dt : 0.0;
    }
    if (keyframe_index + 1 >= keyframes.size()) {
        const double dt = keyframes[keyframe_index].time - keyframes[keyframe_index - 1].time;
        return dt > kAnimationEpsilon
            ? spatial_segment_length(keyframe_index - 1) / dt : 0.0;
    }
    const double dt = keyframes[keyframe_index + 1].time - keyframes[keyframe_index - 1].time;
    return dt > kAnimationEpsilon
        ? (spatial_segment_length(keyframe_index - 1) +
           spatial_segment_length(keyframe_index)) / dt : 0.0;
}

TemporalBezierSegment AnimatedVec2Property::temporal_segment(size_t segment_index) const
{
    TemporalBezierSegment segment;
    if (segment_index + 1 >= keyframes.size()) return segment;
    const VectorKeyframe &left = keyframes[segment_index];
    const VectorKeyframe &right = keyframes[segment_index + 1];
    segment.start_time = left.time;
    segment.end_time = right.time;
    segment.start_value = 0.0;
    segment.end_value = 1.0;
    if (!left.temporal_velocity_explicit && !right.temporal_velocity_explicit) {
        segment.legacy = true;
        segment.hold = left.easing == EasingType::Hold;
        segment.legacy_easing = left.easing;
        segment.legacy_cx1 = left.cx1; segment.legacy_cy1 = left.cy1;
        segment.legacy_cx2 = left.cx2; segment.legacy_cy2 = left.cy2;
        return segment;
    }
    const double span = right.time - left.time;
    const double path_length = std::max(spatial_segment_length(segment_index), 1e-9);
    const double linear_speed = span > kAnimationEpsilon ? path_length / span : 0.0;
    auto path_speed = [&](size_t index, bool incoming) {
        const VectorKeyframe &key = keyframes[index];
        if (!key.temporal_velocity_explicit) return linear_speed;
        switch (key.temporal_mode) {
        case TemporalInterpolationMode::Linear: return linear_speed;
        case TemporalInterpolationMode::AutoBezier: return automatic_temporal_speed(index);
        case TemporalInterpolationMode::Hold: return 0.0;
        default: return finite_or_zero(incoming ? key.incoming_speed : key.outgoing_speed);
        }
    };
    auto influence = [](const VectorKeyframe &key, bool incoming) {
        if (!key.temporal_velocity_explicit ||
            key.temporal_mode == TemporalInterpolationMode::Linear ||
            key.temporal_mode == TemporalInterpolationMode::AutoBezier)
            return 33.3333333333;
        return incoming ? key.incoming_influence : key.outgoing_influence;
    };
    segment.hold = left.temporal_velocity_explicit
        ? left.temporal_mode == TemporalInterpolationMode::Hold
        : left.easing == EasingType::Hold;
    segment.outgoing_influence = influence(left, false) / 100.0;
    segment.incoming_influence = influence(right, true) / 100.0;
    segment.outgoing_speed = path_speed(segment_index, false) / path_length;
    segment.incoming_speed = path_speed(segment_index + 1, true) / path_length;
    return segment;
}

void AnimatedVec2Property::set_temporal_mode(size_t keyframe_index,
                                             TemporalInterpolationMode mode)
{
    if (keyframe_index >= keyframes.size()) return;
    VectorKeyframe &key = keyframes[keyframe_index];
    if (keyframe_index > 0) {
        const TemporalBezierSegment before = temporal_segment(keyframe_index - 1);
        const double length_scale = std::max(spatial_segment_length(keyframe_index - 1), 1e-9);
        key.incoming_influence = before.incoming_influence * 100.0;
        key.incoming_speed = before.incoming_speed * length_scale;
    }
    if (keyframe_index + 1 < keyframes.size()) {
        const TemporalBezierSegment after = temporal_segment(keyframe_index);
        const double length_scale = std::max(spatial_segment_length(keyframe_index), 1e-9);
        key.outgoing_influence = after.outgoing_influence * 100.0;
        key.outgoing_speed = after.outgoing_speed * length_scale;
    }
    key.temporal_velocity_explicit = true;
    key.temporal_mode = mode;
    key.temporal_tangents_linked = mode == TemporalInterpolationMode::AutoBezier ||
                                   mode == TemporalInterpolationMode::ContinuousBezier;
    key.easing = mode == TemporalInterpolationMode::Hold ? EasingType::Hold
               : mode == TemporalInterpolationMode::Linear ? EasingType::Linear
               : EasingType::Bezier;
    if (mode == TemporalInterpolationMode::AutoBezier) {
        key.incoming_speed = key.outgoing_speed = automatic_temporal_speed(keyframe_index);
        key.incoming_influence = key.outgoing_influence = 33.3333333333;
    } else if (mode == TemporalInterpolationMode::ContinuousBezier) {
        const double common = 0.5 * (key.incoming_speed + key.outgoing_speed);
        key.incoming_speed = key.outgoing_speed = common;
    }
}

void AnimatedVec2Property::set_temporal_handle(size_t keyframe_index, bool incoming,
                                               double influence_percent, double speed_value,
                                               bool preserve_opposite)
{
    if (keyframe_index >= keyframes.size()) return;
    const bool was_linked = keyframes[keyframe_index].temporal_tangents_linked;
    set_temporal_mode(keyframe_index, TemporalInterpolationMode::ManualBezier);
    VectorKeyframe &key = keyframes[keyframe_index];
    key.temporal_tangents_linked = was_linked && !preserve_opposite;
    influence_percent = std::clamp(std::isfinite(influence_percent)
                                       ? influence_percent : 33.3333333333,
                                   0.0, 100.0);
    speed_value = finite_or_zero(speed_value);
    if (incoming) {
        key.incoming_influence = influence_percent; key.incoming_speed = speed_value;
        if (key.temporal_tangents_linked && !preserve_opposite) {
            key.outgoing_influence = influence_percent; key.outgoing_speed = speed_value;
        }
    } else {
        key.outgoing_influence = influence_percent; key.outgoing_speed = speed_value;
        if (key.temporal_tangents_linked && !preserve_opposite) {
            key.incoming_influence = influence_percent; key.incoming_speed = speed_value;
        }
    }
    if (preserve_opposite) key.temporal_tangents_linked = false;
}

void AnimatedVec2Property::apply_easy_ease(size_t keyframe_index,
                                           bool ease_in, bool ease_out)
{
    if (keyframe_index >= keyframes.size()) return;
    set_temporal_mode(keyframe_index, TemporalInterpolationMode::ManualBezier);
    VectorKeyframe &key = keyframes[keyframe_index];
    if (ease_in) { key.incoming_influence = 33.3333333333; key.incoming_speed = 0.0; }
    if (ease_out) { key.outgoing_influence = 33.3333333333; key.outgoing_speed = 0.0; }
    key.temporal_tangents_linked = ease_in && ease_out;
    key.easing = ease_in && ease_out ? EasingType::EaseInOut
               : ease_in ? EasingType::EaseIn : EasingType::EaseOut;
}

Vec2Value AnimatedVec2Property::evaluate(double t) const
{
    if (!std::isfinite(t)) return static_value;
    if (keyframes.empty()) return static_value;
    if (keyframes.size() == 1) return keyframes.front().value;
    if (t <= keyframes.front().time) return keyframes.front().value;
    if (t >= keyframes.back().time) return keyframes.back().value;
    for (size_t i = 0; i + 1 < keyframes.size(); ++i) {
        if (t >= keyframes[i].time && t <= keyframes[i + 1].time) {
            const TemporalBezierSegment segment = temporal_segment(i);
            const double progress = evaluate_temporal_segment(segment, t);
            // The temporal result is the spatial progress for the shared path evaluator.
            return evaluate_spatial_segment(i, progress);
        }
    }
    return keyframes.back().value;
}

Vec2Value AnimatedVec2Property::velocity(double t) const
{
    if (keyframes.size() < 2 || !std::isfinite(t)) return {};
    const double total_span = std::max(keyframes.back().time - keyframes.front().time, 1e-6);
    const double h = std::max(1e-7, total_span * 1e-6);
    const double a = std::max(keyframes.front().time, t - h);
    const double b = std::min(keyframes.back().time, t + h);
    if (!(b > a)) return {};
    const Vec2Value va = evaluate(a);
    const Vec2Value vb = evaluate(b);
    return {(vb.x - va.x) / (b - a), (vb.y - va.y) / (b - a)};
}

double AnimatedVec2Property::speed(double t) const
{
    return length(velocity(t));
}

double AnimatedVec2Property::path_value(double t) const
{
    if (keyframes.empty()) return 0.0;
    if (keyframes.size() == 1 || t <= keyframes.front().time) return 0.0;
    double distance = 0.0;
    for (size_t i = 0; i + 1 < keyframes.size(); ++i) {
        if (t >= keyframes[i + 1].time) {
            distance += spatial_segment_length(i);
            continue;
        }
        const TemporalBezierSegment temporal = temporal_segment(i);
        const double progress = evaluate_temporal_segment(temporal, t);
        const int samples = 24;
        Vec2Value previous = evaluate_spatial_segment(i, 0.0);
        const double end_progress = progress;
        for (int sample = 1; sample <= samples; ++sample) {
            const double u = end_progress * (double)sample / (double)samples;
            const Vec2Value current = evaluate_spatial_segment(i, u);
            distance += length(subtract(current, previous));
            previous = current;
        }
        return distance;
    }
    return distance;
}

double AnimatedProperty::ease(double x, EasingType easing,
                               float cx1, float cy1,
                               float cx2, float cy2)
{
    return legacy_ease_value(x, easing, cx1, cy1, cx2, cy2);
}

// Spatial contract equivalent after temporal evaluation:
// return evaluate_spatial_segment(i, spatial_progress)

/* Legacy cubic-bezier helper retained for preset/file compatibility. */
double AnimatedProperty::bezierY(double x, float cx1, float cy1,
                                 float cx2, float cy2)
{
    return legacy_bezier_y(x, cx1, cy1, cx2, cy2);
}
