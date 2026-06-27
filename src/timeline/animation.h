#pragma once

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
    Bezier,     /* uses cx1/cy1/cx2/cy2 control points */
    Hold,       /* no interpolation – jump cut */
};

struct Keyframe {
    double   time   = 0.0;   /* seconds from clip start */
    double   value  = 0.0;

    EasingType easing = EasingType::EaseInOut;

    /* Bezier control points (normalised 0-1 both axes) */
    float cx1 = 0.333f, cy1 = 0.0f;
    float cx2 = 0.667f, cy2 = 1.0f;
};

struct Vec2Value {
    double x = 0.0;
    double y = 0.0;
};

struct VectorKeyframe {
    double time = 0.0;   /* seconds from clip start */
    Vec2Value value;

    EasingType easing = EasingType::EaseInOut;

    /* Bezier control points (normalised 0-1 both axes) */
    float cx1 = 0.333f, cy1 = 0.0f;
    float cx2 = 0.667f, cy2 = 1.0f;
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

    /* Evaluate the property at time t (seconds). */
    double evaluate(double t) const;

private:
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

    /* Evaluate the property at time t (seconds). */
    Vec2Value evaluate(double t) const;
};
