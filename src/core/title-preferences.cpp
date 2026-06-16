#include "title-preferences.h"

#include "title-data.h"

#include <QSettings>

#include <atomic>
#include <mutex>
#include <string>

namespace {

constexpr const char *kSettingsOrg = "OBSGraphicsStudioPro";
constexpr const char *kSettingsApp = "Dock";
constexpr const char *kSettingsGroup = "Rendering";
constexpr const char *kTimelineColorGroup = "TimelineColors";
constexpr const char *kAppearanceGroup = "Appearance";
constexpr const char *kUseGpuKey = "useGpu";
constexpr const char *kCacheEnabledKey = "cacheEnabled";
constexpr const char *kSceneMaskColorKey = "sceneMaskColor";
std::atomic_bool g_gpu_available{true};
std::mutex g_gpu_reason_mutex;
std::string g_gpu_unavailable_reason;

QString timeline_color_key(TitlePreferences::TimelineColorRole role)
{
    switch (role) {
    case TitlePreferences::TimelineColorRole::TextLayer:
        return QStringLiteral("textLayer");
    case TitlePreferences::TimelineColorRole::ClockLayer:
        return QStringLiteral("clockLayer");
    case TitlePreferences::TimelineColorRole::TickerLayer:
        return QStringLiteral("tickerLayer");
    case TitlePreferences::TimelineColorRole::ObjectLayer:
        return QStringLiteral("objectLayer");
    case TitlePreferences::TimelineColorRole::ImageLayer:
        return QStringLiteral("imageLayer");
    case TitlePreferences::TimelineColorRole::Current:
        return QStringLiteral("current");
    case TitlePreferences::TimelineColorRole::Pause:
        return QStringLiteral("pause");
    case TitlePreferences::TimelineColorRole::Loop:
        return QStringLiteral("loop");
    }
    return QStringLiteral("textLayer");
}

} // namespace

namespace TitlePreferences {

bool use_gpu()
{
    QSettings settings(QString::fromUtf8(kSettingsOrg), QString::fromUtf8(kSettingsApp));
    settings.beginGroup(QString::fromUtf8(kSettingsGroup));
    const bool enabled = settings.value(QString::fromUtf8(kUseGpuKey), false).toBool();
    settings.endGroup();
    return enabled;
}

void set_use_gpu(bool enabled)
{
    QSettings settings(QString::fromUtf8(kSettingsOrg), QString::fromUtf8(kSettingsApp));
    settings.beginGroup(QString::fromUtf8(kSettingsGroup));
    settings.setValue(QString::fromUtf8(kUseGpuKey), enabled);
    settings.endGroup();
    settings.sync();
    if (enabled)
        set_gpu_available(true);
}

bool cache_enabled()
{
    QSettings settings(QString::fromUtf8(kSettingsOrg), QString::fromUtf8(kSettingsApp));
    settings.beginGroup(QString::fromUtf8(kSettingsGroup));
    const bool enabled = settings.value(QString::fromUtf8(kCacheEnabledKey), true).toBool();
    settings.endGroup();
    return enabled;
}

void set_cache_enabled(bool enabled)
{
    QSettings settings(QString::fromUtf8(kSettingsOrg), QString::fromUtf8(kSettingsApp));
    settings.beginGroup(QString::fromUtf8(kSettingsGroup));
    settings.setValue(QString::fromUtf8(kCacheEnabledKey), enabled);
    settings.endGroup();
    settings.sync();
    notify_changed(nullptr);
}

bool gpu_available()
{
    return g_gpu_available.load();
}

void set_gpu_available(bool available, const char *reason)
{
    const bool previous = g_gpu_available.exchange(available);
    {
        std::lock_guard<std::mutex> lock(g_gpu_reason_mutex);
        g_gpu_unavailable_reason = available ? std::string() : (reason ? reason : "GPU effects unavailable");
    }
    if (previous != available)
        notify_changed(nullptr);
}

const char *gpu_unavailable_reason()
{
    std::lock_guard<std::mutex> lock(g_gpu_reason_mutex);
    return g_gpu_unavailable_reason.empty() ? "GPU effects unavailable" : g_gpu_unavailable_reason.c_str();
}

QColor default_timeline_color(TimelineColorRole role)
{
    switch (role) {
    case TimelineColorRole::TextLayer:
        return QColor(0xb4, 0x5a, 0xa0);
    case TimelineColorRole::ClockLayer:
        return QColor(0x4b, 0x9a, 0xc8);
    case TimelineColorRole::TickerLayer:
        return QColor(0xd8, 0x8a, 0x30);
    case TimelineColorRole::ObjectLayer:
        return QColor(0x4f, 0x8f, 0x58);
    case TimelineColorRole::ImageLayer:
        return QColor(0x7d, 0x8b, 0x7f);
    case TimelineColorRole::Current:
        return QColor(0xff, 0x44, 0x44);
    case TimelineColorRole::Pause:
        return QColor(0xff, 0xc8, 0x32);
    case TimelineColorRole::Loop:
        return QColor(0x20, 0xa0, 0xff);
    }
    return QColor(0x65, 0x8a, 0xc8);
}

QColor timeline_color(TimelineColorRole role)
{
    QSettings settings(QString::fromUtf8(kSettingsOrg), QString::fromUtf8(kSettingsApp));
    settings.beginGroup(QString::fromUtf8(kTimelineColorGroup));
    const QColor fallback = default_timeline_color(role);
    const QColor color = settings.value(timeline_color_key(role), fallback).value<QColor>();
    settings.endGroup();
    return color.isValid() ? color : fallback;
}

void set_timeline_color(TimelineColorRole role, const QColor &color)
{
    if (!color.isValid())
        return;

    QSettings settings(QString::fromUtf8(kSettingsOrg), QString::fromUtf8(kSettingsApp));
    settings.beginGroup(QString::fromUtf8(kTimelineColorGroup));
    settings.setValue(timeline_color_key(role), color);
    settings.endGroup();
    settings.sync();
    notify_changed(nullptr);
}

QColor default_scene_mask_color()
{
    return QColor(255, 0, 200, 240);
}

QColor scene_mask_color()
{
    QSettings settings(QString::fromUtf8(kSettingsOrg), QString::fromUtf8(kSettingsApp));
    settings.beginGroup(QString::fromUtf8(kAppearanceGroup));
    const QColor fallback = default_scene_mask_color();
    const QColor color = settings.value(QString::fromUtf8(kSceneMaskColorKey), fallback).value<QColor>();
    settings.endGroup();
    return color.isValid() ? color : fallback;
}

void set_scene_mask_color(const QColor &color)
{
    if (!color.isValid())
        return;

    QSettings settings(QString::fromUtf8(kSettingsOrg), QString::fromUtf8(kSettingsApp));
    settings.beginGroup(QString::fromUtf8(kAppearanceGroup));
    settings.setValue(QString::fromUtf8(kSceneMaskColorKey), color);
    settings.endGroup();
    settings.sync();
    notify_changed(nullptr);
}

void notify_changed(QObject *)
{
    TitleDataStore::instance().touch_runtime_change();
}

} // namespace TitlePreferences
