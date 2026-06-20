#pragma once

#include "layer-model.h"

#include <QSize>

#include <string>

namespace gsp {

/* Returns the natural pixel size of a bitmap or SVG asset. Results are cached
 * against the file timestamp/size because live cue variants can query the same
 * image repeatedly while prerendering. */
QSize image_intrinsic_size_for_path(const std::string &path);

/* Apply an exposed image cue value and make the layer's image-size model match
 * the replacement asset. Static image sizes reset to the asset's natural
 * dimensions. Animated image-size tracks are rebased from the old asset's
 * dimensions so their animation is retained with the new aspect ratio. */
bool apply_exposed_image_cue_value(Layer &layer, const std::string &path);

} // namespace gsp
