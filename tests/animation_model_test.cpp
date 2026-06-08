#include "animation.h"

#include <cassert>
#include <cmath>
#include <iostream>

static bool near(double actual, double expected)
{
    return std::abs(actual - expected) < 1e-9;
}

int main()
{
    AnimatedProperty opacity{"opacity", 0.25};
    assert(near(opacity.evaluate(1.0), 0.25));

    opacity.keyframes.push_back({0.0, 0.0, EasingType::Linear});
    opacity.keyframes.push_back({2.0, 1.0, EasingType::Linear});
    assert(near(opacity.evaluate(-1.0), 0.0));
    assert(near(opacity.evaluate(1.0), 0.5));
    assert(near(opacity.evaluate(3.0), 1.0));

    opacity.keyframes[0].easing = EasingType::Hold;
    assert(near(opacity.evaluate(1.0), 0.0));

    opacity.keyframes[0].easing = EasingType::EaseInOut;
    double eased_midpoint = opacity.evaluate(1.0);
    assert(eased_midpoint > 0.0 && eased_midpoint < 1.0);

    std::cout << "animation model static, linear, hold, boundary, and eased interpolation passed\n";
    return 0;
}
