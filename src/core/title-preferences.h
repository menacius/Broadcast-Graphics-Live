#pragma once

#include <QColor>

class QObject;

namespace TitlePreferences {

enum class TimelineColorRole {
    TextLayer,
    ClockLayer,
    TickerLayer,
    ObjectLayer,
    ImageLayer,
    Current,
    Pause,
    Loop
};

bool use_gpu();
void set_use_gpu(bool enabled);
bool cache_enabled();
void set_cache_enabled(bool enabled);
bool gpu_available();
void set_gpu_available(bool available, const char *reason = nullptr);
const char *gpu_unavailable_reason();
QColor timeline_color(TimelineColorRole role);
void set_timeline_color(TimelineColorRole role, const QColor &color);
QColor default_timeline_color(TimelineColorRole role);
QColor scene_mask_color();
void set_scene_mask_color(const QColor &color);
QColor default_scene_mask_color();
void notify_changed(QObject *sender = nullptr);

}
