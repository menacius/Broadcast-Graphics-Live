#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

/* ══════════════════════════════════════════════════════════════════
 *  Timeline & Animation primitives
 * ══════════════════════════════════════════════════════════════════ */
enum class EasingType {
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
    Bezier,     /* legacy segment-level cubic controls */
    Hold,       /* no interpolation – jump cut */
};

/* Temporal interpolation is authored independently on the incoming and
 * outgoing side of every keyframe.  temporal_velocity_explicit=false keeps
 * the legacy EasingType/cx/cy segment representation byte-for-byte compatible
 * until a velocity handle or an AE-style command is used. */
enum class TemporalInterpolationMode {
    Linear = 0,
    Hold,
    AutoBezier,
    ContinuousBezier,
    ManualBezier,
};

struct TemporalBezierSegment {
    double start_time = 0.0;
    double end_time = 0.0;
    double start_value = 0.0;
    double end_value = 0.0;
    double outgoing_influence = 1.0 / 3.0; /* normalized 0..1 time span */
    double incoming_influence = 1.0 / 3.0;
    double outgoing_speed = 0.0;           /* property units / second */
    double incoming_speed = 0.0;
    bool hold = false;
    bool legacy = false;
    EasingType legacy_easing = EasingType::Linear;
    float legacy_cx1 = 0.333f, legacy_cy1 = 0.0f;
    float legacy_cx2 = 0.667f, legacy_cy2 = 1.0f;
};

/* Stable cubic evaluation shared by editor, cached-frame rendering and OBS.
 * Only the time-domain influences are constrained to the segment. Values and
 * velocities are deliberately not clamped, allowing negative values and
 * overshoot. */
double evaluate_temporal_segment(const TemporalBezierSegment &segment, double time);
double evaluate_temporal_segment_velocity(const TemporalBezierSegment &segment, double time);

struct Keyframe {
    double   time   = 0.0;   /* seconds from clip start */
    double   value  = 0.0;

    EasingType easing = EasingType::EaseInOut;

    /* Legacy segment-level Bezier control points (normalised 0-1). */
    float cx1 = 0.333f, cy1 = 0.0f;
    float cx2 = 0.667f, cy2 = 1.0f;

    TemporalInterpolationMode temporal_mode = TemporalInterpolationMode::AutoBezier;
    double incoming_influence = 33.3333333333; /* percent */
    double outgoing_influence = 33.3333333333;
    double incoming_speed = 0.0;               /* property units / second */
    double outgoing_speed = 0.0;
    bool temporal_tangents_linked = true;
    bool temporal_velocity_explicit = false;
};

struct Vec2Value {
    double x = 0.0;
    double y = 0.0;
};

/* Spatial interpolation is independent from temporal easing. Temporal easing
 * maps time to a normalized progress value; spatial interpolation maps that
 * progress onto the position/path geometry in layer-local coordinates. */
enum class SpatialInterpolationMode {
    Linear = 0,
    AutoBezier,
    ContinuousBezier,
    ManualBezier,
};

struct VectorKeyframe {
    double time = 0.0;   /* seconds from clip start */
    Vec2Value value;

    EasingType easing = EasingType::EaseInOut;

    /* Legacy temporal Bezier control points (normalised 0-1). */
    float cx1 = 0.333f, cy1 = 0.0f;
    float cx2 = 0.667f, cy2 = 1.0f;

    TemporalInterpolationMode temporal_mode = TemporalInterpolationMode::AutoBezier;
    double incoming_influence = 33.3333333333; /* percent */
    double outgoing_influence = 33.3333333333;
    double incoming_speed = 0.0;               /* local path units / second */
    double outgoing_speed = 0.0;
    bool temporal_tangents_linked = true;
    bool temporal_velocity_explicit = false;

    /* Spatial tangents are offsets from value, stored in layer-local units.
     * incoming_tangent points from the keyframe toward the previous segment;
     * outgoing_tangent points from the keyframe toward the next segment. */
    Vec2Value incoming_tangent;
    Vec2Value outgoing_tangent;
    SpatialInterpolationMode spatial_mode = SpatialInterpolationMode::Linear;
    bool spatial_tangents_linked = true;
    bool rove_across_time = false;
};

struct AnimatedProperty {
    std::string name;
    double      static_value = 0.0;
    std::vector<Keyframe> keyframes;   /* sorted by time */

    AnimatedProperty() = default;
    AnimatedProperty(std::string property_name, double value,
                     std::vector<Keyframe> keys = {})
        : name(std::move(property_name)), static_value(value),
          keyframes(std::move(keys))
    {
    }

    bool is_animated() const { return !keyframes.empty(); }

    /* Evaluate the property and its final temporal velocity at time t. */
    double evaluate(double t) const;
    double velocity(double t) const;
    TemporalBezierSegment temporal_segment(size_t segment_index) const;
    void set_temporal_mode(size_t keyframe_index, TemporalInterpolationMode mode);
    void set_temporal_handle(size_t keyframe_index, bool incoming,
                             double influence_percent, double speed,
                             bool preserve_opposite = false);
    void apply_easy_ease(size_t keyframe_index, bool ease_in, bool ease_out);

private:
    double automatic_temporal_speed(size_t keyframe_index) const;
    static double ease(double x, EasingType e,
                       float cx1, float cy1, float cx2, float cy2);
    static double bezierY(double x,
                          float cx1, float cy1,
                          float cx2, float cy2);

    friend struct AnimatedVec2Property;
};

struct AnimatedVec2Property {
    std::string name;
    Vec2Value static_value;
    std::vector<VectorKeyframe> keyframes;   /* sorted by time */

    AnimatedVec2Property() = default;
    AnimatedVec2Property(std::string property_name, Vec2Value value,
                         std::vector<VectorKeyframe> keys = {})
        : name(std::move(property_name)), static_value(value),
          keyframes(std::move(keys))
    {
    }

    bool is_animated() const { return !keyframes.empty(); }

    /* Evaluate the property at time t (seconds). The returned coordinate is
     * always layer-local; parent/group/nested-composition transforms are
     * applied only after this common evaluation step. */
    Vec2Value evaluate(double t) const;
    Vec2Value velocity(double t) const;
    double speed(double t) const;
    double path_value(double t) const;
    TemporalBezierSegment temporal_segment(size_t segment_index) const;
    void set_temporal_mode(size_t keyframe_index, TemporalInterpolationMode mode);
    void set_temporal_handle(size_t keyframe_index, bool incoming,
                             double influence_percent, double speed,
                             bool preserve_opposite = false);
    void apply_easy_ease(size_t keyframe_index, bool ease_in, bool ease_out);

    /* Deterministic helpers shared by editor, prerender, and OBS output. */
    Vec2Value resolved_incoming_tangent(size_t keyframe_index) const;
    Vec2Value resolved_outgoing_tangent(size_t keyframe_index) const;
    double spatial_progress_for_segment(size_t segment_index,
                                        double temporal_progress) const;
    Vec2Value evaluate_spatial_segment(size_t segment_index, double progress) const;
    void set_spatial_mode(size_t keyframe_index, SpatialInterpolationMode mode);
    void set_spatial_tangents_linked(size_t keyframe_index, bool linked);
    void set_rove_across_time(size_t keyframe_index, bool enabled);
    void recalculate_rove_times();

    /* Split one spatial segment without changing its authored curve. Returns
     * the inserted keyframe index, or keyframes.size() when the request is
     * invalid. temporal_progress is the time-domain fraction of the segment;
     * spatial_progress is the eased geometric fraction used by the cubic. */
    size_t split_spatial_segment(size_t segment_index,
                                 double temporal_progress,
                                 double spatial_progress);

private:
    Vec2Value automatic_tangent(size_t keyframe_index, bool incoming) const;
    double automatic_temporal_speed(size_t keyframe_index) const;
    double spatial_segment_length(size_t segment_index) const;
};
