#include "title-gpu-text-sdf.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace gsp::gpu_text {

TextStrokeCoverageExtents text_stroke_coverage_extents(float width,
                                                       int alignment)
{
    width = std::max(0.0f, width);
    switch (std::clamp(alignment, 0, 2)) {
    case 0: return {width, 0.0f};
    case 2: return {0.0f, width};
    case 1:
    default: return {width * 0.5f, width * 0.5f};
    }
}

int text_stroke_draw_phase(bool on_front)
{
    return on_front ? 2 : 0;
}

namespace {

constexpr float kInfinity = 1.0e20f;

void distance_transform_1d(const float *input, float *output,
                           int count, std::vector<int> &sites,
                           std::vector<float> &boundaries)
{
    int k = -1;
    for (int q = 0; q < count; ++q) {
        if (input[q] >= kInfinity * 0.5f)
            continue;
        float intersection = -kInfinity;
        while (k >= 0) {
            const int p = sites[k];
            const float qf = static_cast<float>(q);
            const float pf = static_cast<float>(p);
            intersection = ((input[q] + qf * qf) -
                            (input[p] + pf * pf)) /
                           (2.0f * static_cast<float>(q - p));
            if (intersection > boundaries[k])
                break;
            --k;
        }
        ++k;
        sites[k] = q;
        boundaries[k] = k == 0 ? -kInfinity : intersection;
        boundaries[k + 1] = kInfinity;
    }
    if (k < 0) {
        std::fill(output, output + count, kInfinity);
        return;
    }
    int site = 0;
    for (int q = 0; q < count; ++q) {
        while (boundaries[site + 1] < static_cast<float>(q))
            ++site;
        const float delta = static_cast<float>(q - sites[site]);
        output[q] = delta * delta + input[sites[site]];
    }
}

std::vector<float> squared_distance_field(const std::vector<uint8_t> &mask,
                                          int width, int height,
                                          bool feature_is_inside)
{
    const size_t count = static_cast<size_t>(width) * height;
    std::vector<float> first(count, kInfinity);
    std::vector<float> second(count, kInfinity);
    const int maximum = std::max(width, height);
    std::vector<float> input(maximum, kInfinity);
    std::vector<float> output(maximum, kInfinity);
    std::vector<int> sites(maximum, 0);
    std::vector<float> boundaries(static_cast<size_t>(maximum) + 1, 0.0f);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool inside = mask[static_cast<size_t>(y) * width + x] >= 128;
            input[x] = inside == feature_is_inside ? 0.0f : kInfinity;
        }
        distance_transform_1d(input.data(), output.data(), width,
                              sites, boundaries);
        for (int x = 0; x < width; ++x)
            first[static_cast<size_t>(y) * width + x] = output[x];
    }

    for (int x = 0; x < width; ++x) {
        for (int y = 0; y < height; ++y)
            input[y] = first[static_cast<size_t>(y) * width + x];
        distance_transform_1d(input.data(), output.data(), height,
                              sites, boundaries);
        for (int y = 0; y < height; ++y)
            second[static_cast<size_t>(y) * width + x] = output[y];
    }
    return second;
}

} // namespace

std::vector<uint8_t> build_glyph_sdf(const uint8_t *source, int source_width,
                                     int source_height, int source_stride,
                                     int spread, int &output_width,
                                     int &output_height)
{
    output_width = 0;
    output_height = 0;
    if (!source || source_width <= 0 || source_height <= 0 ||
        source_stride < source_width || spread <= 0)
        return {};

    constexpr int64_t kGuardPixels = 2;
    constexpr size_t kMaximumGlyphPixels = 64u * 1024u * 1024u;
    const int64_t padding64 = static_cast<int64_t>(spread) + kGuardPixels;
    const int64_t output_width64 = static_cast<int64_t>(source_width) +
                                   padding64 * 2;
    const int64_t output_height64 = static_cast<int64_t>(source_height) +
                                    padding64 * 2;
    if (padding64 > std::numeric_limits<int>::max() ||
        output_width64 > std::numeric_limits<int>::max() ||
        output_height64 > std::numeric_limits<int>::max())
        return {};

    const int padding = static_cast<int>(padding64);
    output_width = static_cast<int>(output_width64);
    output_height = static_cast<int>(output_height64);
    const size_t output_width_size = static_cast<size_t>(output_width);
    const size_t output_height_size = static_cast<size_t>(output_height);
    if (output_width_size >
        std::numeric_limits<size_t>::max() / output_height_size ||
        output_width_size * output_height_size > kMaximumGlyphPixels) {
        output_width = 0;
        output_height = 0;
        return {};
    }
    std::vector<uint8_t> alpha(output_width_size * output_height_size, 0);
    for (int y = 0; y < source_height; ++y) {
        const uint8_t *row = source + static_cast<size_t>(y) * source_stride;
        std::copy(row, row + source_width,
                  alpha.begin() +
                      static_cast<size_t>(y + padding) * output_width + padding);
    }

    const std::vector<float> distance_inside = squared_distance_field(
        alpha, output_width, output_height, true);
    const std::vector<float> distance_outside = squared_distance_field(
        alpha, output_width, output_height, false);
    std::vector<uint8_t> sdf(alpha.size(), 0);
    const float denominator = static_cast<float>(spread) * 2.0f;
    for (size_t i = 0; i < sdf.size(); ++i) {
        float signed_distance = std::sqrt(distance_outside[i]) -
                                std::sqrt(distance_inside[i]);
        signed_distance += static_cast<float>(alpha[i]) / 255.0f - 0.5f;
        const float normalized = std::clamp(
            0.5f + signed_distance / denominator, 0.0f, 1.0f);
        sdf[i] = static_cast<uint8_t>(std::lround(normalized * 255.0f));
    }
    return sdf;
}

} // namespace gsp::gpu_text
