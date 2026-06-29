#pragma once

#include "title-data.h"
#include "ticker-runtime.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

struct TitleDynamicLayerAnalysis {
    bool has_dynamic_layers = false;
    std::size_t first_dynamic_layer = 0;
    bool has_cacheable_prefix = false;
};

/*
 * Runtime-dynamic layers cannot be baked into the frame cache.  Clock and
 * clocks and runtime-driven ticker layers are the direct dynamic sources. Custom-playback tickers are timeline-deterministic and cacheable.  Dynamic state also propagates
 * to layers whose output depends on a dynamic parent transform/opacity or a
 * dynamic track matte.
 *
 * The current partial-cache renderer caches the largest z-order-safe prefix
 * below the first dynamic output.  Layers from that point upward remain live,
 * which preserves masks, parenting and blend modes without flattening a live
 * layer above artwork that should cover it.
 */
inline TitleDynamicLayerAnalysis analyze_title_dynamic_layers(const Title &title)
{
    TitleDynamicLayerAnalysis analysis;
    const std::size_t count = title.layers.size();
    analysis.first_dynamic_layer = count;
    if (count == 0)
        return analysis;

    std::unordered_map<std::string, std::size_t> index_by_id;
    index_by_id.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto &layer = title.layers[i];
        if (layer && !layer->id.empty())
            index_by_id[layer->id] = i;
    }

    std::vector<bool> dynamic(count, false);
    for (std::size_t i = 0; i < count; ++i) {
        const auto &layer = title.layers[i];
        if (!layer)
            continue;
        bool independent_asset_time = layer->type == LayerType::Asset &&
                                      layer->asset_animated &&
                                      layer->asset_playback_mode == 1;
        if (!independent_asset_time && !layer->asset_owner_id.empty()) {
            const auto owner_it = index_by_id.find(layer->asset_owner_id);
            if (owner_it != index_by_id.end()) {
                const auto &owner = title.layers[owner_it->second];
                independent_asset_time = owner && owner->type == LayerType::Asset &&
                                         owner->asset_animated &&
                                         owner->asset_playback_mode == 1;
            }
        }
        dynamic[i] = independent_asset_time ||
                     layer->type == LayerType::Clock ||
                     (layer->type == LayerType::Ticker &&
                      layer->ticker_playback_mode != static_cast<int>(TickerPlaybackMode::CustomPlayback));
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (std::size_t i = 0; i < count; ++i) {
            const auto &layer = title.layers[i];
            if (!layer || dynamic[i])
                continue;

            const auto group_parent = index_by_id.find(layer->parent_id);
            const auto transform_parent = index_by_id.find(layer->transform_parent_id);
            const bool dynamic_parent =
                (group_parent != index_by_id.end() && dynamic[group_parent->second]) ||
                (transform_parent != index_by_id.end() && dynamic[transform_parent->second]);

            const auto mask = index_by_id.find(layer->mask_source_id);
            const bool dynamic_mask = layer->mask_mode != MaskMode::None &&
                                      mask != index_by_id.end() && dynamic[mask->second];

            if (dynamic_parent || dynamic_mask) {
                dynamic[i] = true;
                changed = true;
            }
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (!dynamic[i])
            continue;
        analysis.has_dynamic_layers = true;
        analysis.first_dynamic_layer = i;
        break;
    }

    analysis.has_cacheable_prefix = analysis.has_dynamic_layers &&
                                    analysis.first_dynamic_layer > 0 &&
                                    analysis.first_dynamic_layer < count;
    return analysis;
}
