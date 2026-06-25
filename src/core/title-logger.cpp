#include "title-logger.h"

#include "title-preferences.h"

#include <obs-module.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>

namespace {

QMutex g_log_mutex;
QString g_session_file_path;
QString g_session_stamp;
bool g_session_started = false;

const QVector<TitleLogCategory> &all_categories()
{
    static const QVector<TitleLogCategory> categories = {
        {QStringLiteral("General"), QStringLiteral("General"),
         QStringLiteral("Unclassified application messages."), true},
        {QStringLiteral("Plugin"), QStringLiteral("Plugin lifecycle"),
         QStringLiteral("Module load, unload and OBS frontend events."), true},
        {QStringLiteral("Source"), QStringLiteral("OBS sources"),
         QStringLiteral("Source creation, activation, visibility and presentation state."), true},
        {QStringLiteral("TitleStore"), QStringLiteral("Titles and persistence"),
         QStringLiteral("Title loading, saving, revision and project storage."), true},
        {QStringLiteral("Dock"), QStringLiteral("Dock"),
         QStringLiteral("Dock lifecycle, title selection and cue controls."), true},
        {QStringLiteral("Editor"), QStringLiteral("Editor"),
         QStringLiteral("Editor lifecycle, title changes and preview synchronization."), true},
        {QStringLiteral("Canvas"), QStringLiteral("Canvas"),
         QStringLiteral("Canvas presentation, interactions and display rendering."), true},
        {QStringLiteral("Properties"), QStringLiteral("Properties"),
         QStringLiteral("Properties-panel edits and live visual updates."), true},
        {QStringLiteral("Layers"), QStringLiteral("Layers"),
         QStringLiteral("Layer creation, deletion, ordering, visibility and parenting."), true},
        {QStringLiteral("Text"), QStringLiteral("Text model"),
         QStringLiteral("Rich-text layout, inline editing and text model changes."), true},
        {QStringLiteral("GpuPipeline"), QStringLiteral("GPU compositor"),
         QStringLiteral("GPU graph updates, frame publication and draw failures."), true},
        {QStringLiteral("GpuText"), QStringLiteral("GPU text"),
         QStringLiteral("Glyph layout, atlas rendering and text-raster readiness."), true},
        {QStringLiteral("Effects"), QStringLiteral("Effects"),
         QStringLiteral("Effect compilation, passes and effect-stack changes."), true},
        {QStringLiteral("Masks"), QStringLiteral("Masks and mattes"),
         QStringLiteral("Layer masks, track mattes and OBS scene-mask composition."), true},
        {QStringLiteral("Cache"), QStringLiteral("Cache"),
         QStringLiteral("General frame-cache state, invalidation and diagnostics."), true},
        {QStringLiteral("CacheQueue"), QStringLiteral("Cache queue"),
         QStringLiteral("Prerender queue scheduling, cancellation and retries."), true},
        {QStringLiteral("CachePlayback"), QStringLiteral("Cache playback (verbose)"),
         QStringLiteral("Per-frame cache lookup and presentation decisions."), false},
        {QStringLiteral("RamCache"), QStringLiteral("RAM cache"),
         QStringLiteral("GPU/RAM cache publication, eviction and memory use."), true},
        {QStringLiteral("DiskCache"), QStringLiteral("Disk cache"),
         QStringLiteral("Disk-cache restore, compression, writes and failures."), true},
        {QStringLiteral("Prerender"), QStringLiteral("Prerender pipeline"),
         QStringLiteral("GPU readback submission, resolution and frame publication."), true},
        {QStringLiteral("LiveCue"), QStringLiteral("Live text cues"),
         QStringLiteral("Live-cue state, payload and render lifecycle."), true},
        {QStringLiteral("LiveCueUI"), QStringLiteral("Live cue UI"),
         QStringLiteral("Live-cue table editing and user-interface state."), true},
        {QStringLiteral("Playlist"), QStringLiteral("Playlist"),
         QStringLiteral("Playlist transitions, timing and cue selection."), true},
        {QStringLiteral("Timeline"), QStringLiteral("Timeline"),
         QStringLiteral("Timeline edits, playback, work area and keyframes."), true},
        {QStringLiteral("Animation"), QStringLiteral("Animation"),
         QStringLiteral("Animation evaluation, interpolation and transitions."), true},
        {QStringLiteral("Ticker"), QStringLiteral("Ticker playback"),
         QStringLiteral("Ticker pause, resume, stop, cue gates and adaptive auto-pause."), true},
        {QStringLiteral("Transitions"), QStringLiteral("Transitions"),
         QStringLiteral("Layer and title transition setup and evaluation."), true},
        {QStringLiteral("ImportExport"), QStringLiteral("Import and export"),
         QStringLiteral("Title/template import, append and export operations."), true},
        {QStringLiteral("Assets"), QStringLiteral("Assets"),
         QStringLiteral("Images, fonts, media and external asset loading."), true},
        {QStringLiteral("Preferences"), QStringLiteral("Preferences"),
         QStringLiteral("Application preference changes and configuration."), true},
        {QStringLiteral("Performance"), QStringLiteral("Performance (verbose)"),
         QStringLiteral("Render timing, resource and high-frequency diagnostics."), false},
    };
    return categories;
}

bool category_default_enabled(const QString &key)
{
    for (const auto &category : all_categories()) {
        if (category.key.compare(key, Qt::CaseInsensitive) == 0)
            return category.default_enabled;
    }
    return true;
}

QString normalized_category(const char *category)
{
    const QString clean = QString::fromUtf8(category ? category : "General").trimmed();
    return clean.isEmpty() ? QStringLiteral("General") : clean;
}

QString ensure_session_path_locked()
{
    if (!g_session_started) {
        g_session_stamp = QDateTime::currentDateTime().toString(
            QStringLiteral("yyyy-MM-dd_HH-mm-ss-zzz"));
        g_session_started = true;
    }
    if (g_session_file_path.isEmpty()) {
        const QString directory = TitlePreferences::logging_directory();
        QDir().mkpath(directory);
        g_session_file_path = QDir(directory).filePath(
            QStringLiteral("broadcast-graphics-live_%1.log").arg(g_session_stamp));
    }
    return g_session_file_path;
}

int obs_level_for_title_level(TitleLogLevel level)
{
    switch (level) {
    case TitleLogLevel::Error:
        return LOG_ERROR;
    case TitleLogLevel::Warning:
        return LOG_WARNING;
    case TitleLogLevel::Info:
        return LOG_INFO;
    case TitleLogLevel::Debug:
    case TitleLogLevel::Trace:
        return LOG_DEBUG;
    case TitleLogLevel::Off:
    default:
        return LOG_INFO;
    }
}

void append_line_locked(const QString &path, const QString &line)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QTextStream stream(&file);
    stream << line << '\n';
}

} // namespace

namespace TitleLogger {

QString levelName(TitleLogLevel level)
{
    switch (level) {
    case TitleLogLevel::Error:
        return QStringLiteral("ERROR");
    case TitleLogLevel::Warning:
        return QStringLiteral("WARN");
    case TitleLogLevel::Info:
        return QStringLiteral("INFO");
    case TitleLogLevel::Debug:
        return QStringLiteral("DEBUG");
    case TitleLogLevel::Trace:
        return QStringLiteral("TRACE");
    case TitleLogLevel::Off:
    default:
        return QStringLiteral("OFF");
    }
}

void startSession()
{
    QMutexLocker lock(&g_log_mutex);
    if (g_session_started)
        return;
    ensure_session_path_locked();
    if (TitlePreferences::logging_enabled()) {
        append_line_locked(g_session_file_path,
            QStringLiteral("%1 [INFO] [Plugin] Logging session started: %2")
                .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                     g_session_file_path));
    }
}

void endSession()
{
    QMutexLocker lock(&g_log_mutex);
    if (!g_session_started)
        return;
    if (TitlePreferences::logging_enabled() && !g_session_file_path.isEmpty()) {
        append_line_locked(g_session_file_path,
            QStringLiteral("%1 [INFO] [Plugin] Logging session ended")
                .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs)));
    }
    g_session_started = false;
    g_session_file_path.clear();
    g_session_stamp.clear();
}

QString currentSessionFilePath()
{
    QMutexLocker lock(&g_log_mutex);
    return ensure_session_path_locked();
}

bool relocateCurrentSession(const QString &directory)
{
    const QString requested_directory = directory.trimmed();
    if (requested_directory.isEmpty())
        return false;
    const QString clean_directory = QDir::cleanPath(requested_directory);

    QMutexLocker lock(&g_log_mutex);
    const QString old_path = ensure_session_path_locked();
    if (!QDir().mkpath(clean_directory))
        return false;
    const QString new_path = QDir(clean_directory).filePath(QFileInfo(old_path).fileName());
    if (QFileInfo(old_path).absoluteFilePath() == QFileInfo(new_path).absoluteFilePath())
        return true;

    bool moved = true;
    if (QFile::exists(old_path)) {
        QFile::remove(new_path);
        moved = QFile::rename(old_path, new_path);
        if (!moved) {
            moved = QFile::copy(old_path, new_path);
            if (moved)
                QFile::remove(old_path);
        }
    }
    if (moved)
        g_session_file_path = new_path;
    return moved;
}

QVector<TitleLogCategory> categories()
{
    return all_categories();
}

bool categoryEnabled(const QString &category)
{
    const QString clean = category.trimmed().isEmpty()
        ? QStringLiteral("General") : category.trimmed();
    return TitlePreferences::logging_category_enabled(
        clean, category_default_enabled(clean));
}

void log(TitleLogLevel level, const char *category, const QString &message)
{
    if (level == TitleLogLevel::Off || !TitlePreferences::logging_enabled())
        return;
    if ((int)level > (int)TitlePreferences::logging_level())
        return;

    const QString clean_category = normalized_category(category);
    if (!categoryEnabled(clean_category))
        return;

    const QString line = QStringLiteral("%1 [%2] [%3] %4")
                             .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                                  levelName(level), clean_category, message);

    {
        QMutexLocker lock(&g_log_mutex);
        append_line_locked(ensure_session_path_locked(), line);
    }

    if (TitlePreferences::logging_mirror_to_obs())
        blog(obs_level_for_title_level(level), "[Broadcast Graphics Live] %s", line.toUtf8().constData());
}

void error(const char *category, const QString &message)
{
    log(TitleLogLevel::Error, category, message);
}

void warning(const char *category, const QString &message)
{
    log(TitleLogLevel::Warning, category, message);
}

void info(const char *category, const QString &message)
{
    log(TitleLogLevel::Info, category, message);
}

void debug(const char *category, const QString &message)
{
    log(TitleLogLevel::Debug, category, message);
}

void trace(const char *category, const QString &message)
{
    log(TitleLogLevel::Trace, category, message);
}

} // namespace TitleLogger
