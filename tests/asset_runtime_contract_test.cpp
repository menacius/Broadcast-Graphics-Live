#include "asset-runtime.h"
#include "title-data.h"

#include <cmath>
#include <iostream>
#include <memory>

namespace {
bool near(double actual, double expected, const char *label)
{
    if (std::abs(actual - expected) <= 0.0001)
        return true;
    std::cerr << label << ": expected " << expected << ", got " << actual << "\n";
    return false;
}
}

int main()
{
    bool ok = true;

    Layer asset;
    asset.id = "asset-layer";
    asset.type = LayerType::Asset;
    asset.asset_animated = true;
    asset.asset_playback_mode = 1;
    asset.asset_duration = 10.0;
    asset.asset_loop = false;

    asset.asset_source_playback_mode = 2;
    asset.asset_source_pause_time = 3.0;
    asset.asset_pause_duration = 2.0;
    ok &= near(bgs::asset_runtime::map_elapsed_to_local_time(asset, 2.0), 2.0,
               "pause mode before marker");
    ok &= near(bgs::asset_runtime::map_elapsed_to_local_time(asset, 4.0), 3.0,
               "pause mode hold");
    ok &= near(bgs::asset_runtime::map_elapsed_to_local_time(asset, 6.0), 4.0,
               "pause mode resume");

    asset.asset_source_playback_mode = 1;
    asset.asset_source_loop_type = 0;
    asset.asset_source_loop_start = 2.0;
    asset.asset_source_loop_end = 4.0;
    asset.asset_loop_count = 3;
    ok &= near(bgs::asset_runtime::map_elapsed_to_local_time(asset, 6.5), 2.5,
               "restart finite loop");
    ok &= near(bgs::asset_runtime::map_elapsed_to_local_time(asset, 9.0), 5.0,
               "restart loop outro");

    asset.asset_source_loop_type = 1;
    asset.asset_loop_count = 2;
    ok &= near(bgs::asset_runtime::map_elapsed_to_local_time(asset, 4.5), 3.5,
               "ping-pong reverse leg");
    ok &= near(bgs::asset_runtime::map_elapsed_to_local_time(asset, 6.5), 2.5,
               "ping-pong final forward leg");
    ok &= near(bgs::asset_runtime::map_elapsed_to_local_time(asset, 9.0), 5.0,
               "ping-pong outro");

    asset.asset_source_playback_mode = 0;
    asset.asset_loop = true;
    ok &= near(bgs::asset_runtime::map_elapsed_to_local_time(asset, 12.5), 2.5,
               "whole animation loop");

    Title static_title;
    static_title.duration = 5.0;
    auto static_layer = std::make_shared<Layer>();
    static_layer->type = LayerType::Clock;
    static_layer->out_time = 5.0;
    static_title.layers.push_back(static_layer);
    if (bgs::asset_runtime::title_has_timeline_animation(static_title)) {
        std::cerr << "clock-only asset must not expose timeline playback controls\n";
        ok = false;
    }

    static_layer->position.keyframes.push_back({0.0, {0.0, 0.0}});
    static_layer->position.keyframes.push_back({1.0, {100.0, 0.0}});
    if (!bgs::asset_runtime::title_has_timeline_animation(static_title)) {
        std::cerr << "keyframed asset must expose timeline playback controls\n";
        ok = false;
    }

    Title instance_title;
    instance_title.duration = 10.0;
    auto root_asset = std::make_shared<Layer>();
    root_asset->id = "root-asset";
    root_asset->type = LayerType::Asset;
    root_asset->asset_duration = 10.0;
    root_asset->out_time = 10.0;
    auto nested_child = std::make_shared<Layer>();
    nested_child->id = "nested-child";
    nested_child->asset_owner_id = root_asset->id;
    nested_child->out_time = 10.0;
    nested_child->opacity.keyframes.push_back({0.0, 0.0});
    nested_child->opacity.keyframes.push_back({1.0, 1.0});
    instance_title.layers = {nested_child, root_asset};
    if (!bgs::asset_runtime::asset_layer_has_timeline_animation(instance_title,
                                                                 *root_asset)) {
        std::cerr << "nested keyframes must mark the Asset Layer as animated\n";
        ok = false;
    }

    return ok ? 0 : 1;
}
