#pragma once

#include "title-data.h"

#include <memory>

/* Title contains shared layer pointers. Rendering/cache jobs require an
 * immutable deep snapshot so UI edits cannot mutate a frame while it is being
 * rendered. Keep this copy contract in one place for both cache workers and GPU
 * presentation sessions. */
inline Title clone_title_snapshot(const Title &title)
{
    Title snapshot = title;
    snapshot.layers.clear();
    snapshot.layers.reserve(title.layers.size());
    for (const auto &layer : title.layers) {
        if (layer)
            snapshot.layers.push_back(std::make_shared<Layer>(*layer));
    }
    return snapshot;
}

inline std::shared_ptr<Title> clone_title_snapshot(
    const std::shared_ptr<Title> &title)
{
    return title ? std::make_shared<Title>(clone_title_snapshot(*title))
                 : nullptr;
}
