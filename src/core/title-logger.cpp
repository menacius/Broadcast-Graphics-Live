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

void log(TitleLogLevel level, const char *category, const QString &message)
{
    if (level == TitleLogLevel::Off || !TitlePreferences::logging_enabled())
        return;
    if ((int)level > (int)TitlePreferences::logging_level())
        return;

    const QString clean_category = QString::fromUtf8(category ? category : "General");
    const QString line = QStringLiteral("%1 [%2] [%3] %4")
                             .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                                  levelName(level),
                                  clean_category,
                                  message);

    {
        QMutexLocker lock(&g_log_mutex);
        const QString path = TitlePreferences::logging_file_path();
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << line << '\n';
        }
    }

    if (TitlePreferences::logging_mirror_to_obs())
        blog(obs_level_for_title_level(level), "[OBS Graphics Studio Pro] %s", line.toUtf8().constData());
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
