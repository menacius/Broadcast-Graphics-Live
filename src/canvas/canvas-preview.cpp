#include "title-editor-internal.h"
#include "effect-preset-catalog.h"
#include "title-localization.h"
#include "cache-manager.h"
#include "title-cache-policy.h"
#include "title-source.h"
#include "title-preferences.h"
#include "title-logger.h"
#include "ticker-runtime.h"
#include "asset-runtime.h"

#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>

#include <QMetaObject>
#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QImage>
#include <QMimeData>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <QScreen>
#include <QPaintEngine>

#include <algorithm>
#include <map>
#include <mutex>
#include <cmath>
#include <chrono>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#endif
#if defined(ENABLE_WAYLAND) && QT_VERSION < QT_VERSION_CHECK(6, 9, 0) && \
    __has_include(<qpa/qplatformnativeinterface.h>)
#define OBS_BGS_HAS_QPA_NATIVE_INTERFACE 1
#include <qpa/qplatformnativeinterface.h>
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

// Free Transform helpers are used before their definitions later in this
// translation unit. Declare them in the same global scope as their definitions
// to avoid creating duplicate anonymous-namespace overload candidates.
static Vec2Value editor_quad_value(const Layer &layer, const AnimatedVec2Property &prop,
                                   float legacy_x, float legacy_y, double playhead);
static void editor_set_quad_value(Layer &layer, AnimatedVec2Property &prop,
                                  float &legacy_x, float &legacy_y, double playhead,
                                  const Vec2Value &value);


/* Ordered implementation modules. Keep this list in source order. */
#include "canvas-preview/preview-cache-view.inc"
#include "canvas-preview/geometry-selection.inc"
#include "canvas-preview/transform-snap.inc"
#include "canvas-preview/path-gradient-tools.inc"
#include "canvas-preview/pointer-events.inc"
#include "canvas-preview/keyboard-wheel-events.inc"
#include "canvas-preview/canvas-overlay-paint.inc"
#include "canvas-preview/gpu-frame-rendering.inc"
