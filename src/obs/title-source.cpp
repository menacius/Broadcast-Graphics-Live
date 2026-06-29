/*
 * title-source.cpp
 *
 * Unified GPU compositor for OBS live output, editor preview and final cache
 * readback. Supported Text/Clock layers are composed from persistent SDF glyph
 * atlases and GPU quads; compatibility text transitions, vector/image adapters
 * and unsupported color fonts rasterize only when their content changes.
 * Transforms, masks, effects, blending and temporal motion blur remain
 * GPU-resident through presentation.
 *
 * Build dependency: cairo, pango, pangocairo (compatibility raster adapters)
 */

#include "cache-manager.h"
#include "cache-frame-payload.h"
#include "cache-tile-payload.h"
#include "title-cache-policy.h"
#include "title-source.h"
#include "style-presets.h"
#include "title-data.h"
#include "live-text-cue-utils.h"
#include "title-snapshot.h"
#include "plugin-main.h"
#include "title-localization.h"
#include "title-effect-registry.h"
#include "extensions/effect-extension-catalog.h"
#include "effects/effect-animation-utils.h"
#include "title-gpu-text-renderer.h"
#include "title-preferences.h"
#include "title-logger.h"
#include "ticker-runtime.h"
#include "asset-runtime.h"
#include "image-layer-utils.h"
#include "title-text-layout.h"

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <graphics/matrix4.h>
#include <graphics/image-file.h>
#include <util/threading.h>

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QSvgRenderer>
#include <QString>
#include <QStringList>
#include <QLocale>
#include <QPointF>
#include <QPainter>
#include <QBrush>
#include <QPainterPath>
#include <QtGlobal>
#include <QFont>
#include <QFontMetrics>
#include <QFontDatabase>
#include <QTextLayout>
#include <QTextDocument>
#include <QTextBlock>
#include <QAbstractTextDocumentLayout>
#include <QTextOption>
#include <QTextCursor>
#include <QTextBoundaryFinder>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include <QTransform>
#include <QColor>
#include <QLinearGradient>
#include <QRadialGradient>
#include <QConicalGradient>
#include <QCryptographicHash>
#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QMetaObject>
#include <QObject>

#include <memory>
#include <string>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>
#include <algorithm>
#include <atomic>
#include <array>
#include <mutex>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <sstream>
#include <iomanip>
#include <functional>
#include <utility>
#include "path-geometry.h"

using bgs::live_text::exposed_text_layers;

/* Implemented after all split title-source function continuations have closed.
 * The declaration must remain visible to the cache API in
 * source-lifecycle-playback.inc. */
static bool alias_global_gpu_frame_locked(
    const std::string &cache_key, const std::string &canonical_cache_key);


/* Ordered implementation modules. Keep this list in source order. */
#include "title-source/source-runtime.inc"
#include "title-source/scene-masks-properties.inc"
#include "title-source/layer-evaluation-layout.inc"
#include "title-source/compatibility-text-rendering.inc"
#include "title-source/compatibility-layer-raster.inc"
#include "title-source/compatibility-effects-compositor.inc"
#include "title-source/gpu-resources-primitives.inc"
#include "title-source/gpu-effects-transitions.inc"
#include "title-source/gpu-masks-groups-cache.inc"
#include "title-source/gpu-presentation-readback.inc"
#include "title-source/gpu-session-lifecycle.inc"
#include "title-source/source-lifecycle-playback.inc"
#include "title-source/source-registration.inc"
/* The preceding three files contain split function bodies and must remain
 * contiguous. Definitions inserted between them become illegal block-scope
 * functions on MSVC. */
#include "title-source/gpu-frame-cache-alias.inc"
