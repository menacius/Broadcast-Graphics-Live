#pragma once

#include <QString>

enum class TitleLogLevel {
    Off = 0,
    Error = 1,
    Warning = 2,
    Info = 3,
    Debug = 4,
    Trace = 5
};

namespace TitleLogger {

void log(TitleLogLevel level, const char *category, const QString &message);
void error(const char *category, const QString &message);
void warning(const char *category, const QString &message);
void info(const char *category, const QString &message);
void debug(const char *category, const QString &message);
void trace(const char *category, const QString &message);
QString levelName(TitleLogLevel level);

}

#define BGL_LOG_ERROR(category, message) ::TitleLogger::error(category, message)
#define BGL_LOG_WARNING(category, message) ::TitleLogger::warning(category, message)
#define BGL_LOG_INFO(category, message) ::TitleLogger::info(category, message)
#define BGL_LOG_DEBUG(category, message) ::TitleLogger::debug(category, message)
#define BGL_LOG_TRACE(category, message) ::TitleLogger::trace(category, message)
