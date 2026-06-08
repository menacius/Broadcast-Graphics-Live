#include "animation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

/* ══════════════════════════════════════════════════════════════════
 *  AnimatedProperty::evaluate
 * ══════════════════════════════════════════════════════════════════ */
double AnimatedProperty::evaluate(double t) const
{
    if (!std::isfinite(t)) return static_value;
    if (keyframes.empty()) return static_value;
    if (keyframes.size() == 1) return keyframes.front().value;
    if (t <= keyframes.front().time) return keyframes.front().value;
    if (t >= keyframes.back().time)  return keyframes.back().value;

    /* Find surrounding pair */
    for (size_t i = 0; i + 1 < keyframes.size(); ++i) {
        const auto &k0 = keyframes[i];
        const auto &k1 = keyframes[i + 1];
        if (t >= k0.time && t <= k1.time) {
            double span = k1.time - k0.time;
            if (span < 1e-10) return k0.value;
            double x = (t - k0.time) / span;  // 0..1

            if (k0.easing == EasingType::Hold) return k0.value;

            double y = ease(x, k0.easing,
                            k0.cx1, k0.cy1, k0.cx2, k0.cy2);
            return k0.value + y * (k1.value - k0.value);
        }
    }
    return keyframes.back().value;
}

double AnimatedProperty::ease(double x, EasingType e,
                               float cx1, float cy1,
                               float cx2, float cy2)
{
    switch (e) {
    case EasingType::Linear:   return x;
    case EasingType::EaseIn:   return x * x;
    case EasingType::EaseOut:  return x * (2.0 - x);
    case EasingType::EaseInOut:
        return x < 0.5 ? 2.0 * x * x : -1.0 + (4.0 - 2.0 * x) * x;
    case EasingType::Bezier:
        return bezierY(x, cx1, cy1, cx2, cy2);
    default: return x;
    }
    (void)cx1; (void)cx2; // used by full bezier solver if needed
}

/* Cubic-bezier Y for a given X using P0=(0,0), P3=(1,1). */
double AnimatedProperty::bezierY(double x, float cx1, float cy1,
                                 float cx2, float cy2)
{
    auto sample = [](double t, double p1, double p2) {
        double inv = 1.0 - t;
        return 3.0 * inv * inv * t * p1 +
               3.0 * inv * t * t * p2 +
               t * t * t;
    };
    auto slope = [](double t, double p1, double p2) {
        double inv = 1.0 - t;
        return 3.0 * inv * inv * p1 +
               6.0 * inv * t * (p2 - p1) +
               3.0 * t * t * (1.0 - p2);
    };

    x = std::clamp(x, 0.0, 1.0);
    cx1 = std::clamp(cx1, 0.0f, 1.0f);
    cx2 = std::clamp(cx2, 0.0f, 1.0f);

    double t = x;
    for (int i = 0; i < 8; ++i) {
        double dx = sample(t, cx1, cx2) - x;
        double d = slope(t, cx1, cx2);
        if (std::abs(dx) < 1e-6) break;
        if (std::abs(d) < 1e-6) break;
        t = std::clamp(t - dx / d, 0.0, 1.0);
    }

    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 12; ++i) {
        double bx = sample(t, cx1, cx2);
        if (std::abs(bx - x) < 1e-6) break;
        if (bx < x) lo = t; else hi = t;
        t = 0.5 * (lo + hi);
    }

    return std::clamp(sample(t, cy1, cy2), 0.0, 1.0);
}
