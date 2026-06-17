#pragma once

#include <QColor>
#include <QString>

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
int cache_ram_limit_mb();
void set_cache_ram_limit_mb(int megabytes);
QString cache_disk_location();
void set_cache_disk_location(const QString &path);
bool clear_cache_on_exit();
void set_clear_cache_on_exit(bool enabled);
bool logging_enabled();
void set_logging_enabled(bool enabled);
int logging_level();
void set_logging_level(int level);
bool logging_mirror_to_obs();
void set_logging_mirror_to_obs(bool enabled);
QString logging_file_path();
void set_logging_file_path(const QString &path);
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
